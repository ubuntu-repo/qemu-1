/*
 * Copyright 2019, Oracle and/or its affiliates.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <linux/kvm.h>
#include <errno.h>

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "io/proxy-link.h"
#include "exec/memory.h"
#include "exec/cpu-common.h"
#include "exec/address-spaces.h"
#include "qemu/int128.h"
#include "qemu/range.h"
#include "hw/pci/pci.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qstring.h"
#include "sysemu/sysemu.h"
#include "hw/proxy/qemu-proxy.h"
#include "hw/proxy/memory-sync.h"
#include "qom/object.h"
#include "qemu/event_notifier.h"
#include "sysemu/kvm.h"
#include "util/event_notifier-posix.c"
#include "hw/i386/pc.h"
#include "hw/boards.h"
#include "include/qemu/log.h"

/*
 * TODO: kvm_vm_ioctl is only available for per-target objects (NEED_CPU_H).
 * The invocation of kvm_vm_ioctl should be moved to a per-target object. Until
 * the the following definition is necessary
 */
int kvm_vm_ioctl(KVMState *s, int type, ...);

QEMUTimer *hb_timer;
static void pci_proxy_dev_realize(PCIDevice *dev, Error **errp);
static void setup_irqfd(PCIProxyDev *dev);
static void pci_dev_exit(PCIDevice *dev);
static void start_heartbeat_timer(void);
static void stop_heartbeat_timer(void);
static void childsig_handler(int sig, siginfo_t *siginfo, void *ctx);
static void broadcast_msg(ProcMsg *msg, bool need_reply);

static void childsig_handler(int sig, siginfo_t *siginfo, void *ctx)
{
    /* TODO: Add proper handler. */
    printf("Child (pid %d) is dead? Signal is %d, Exit code is %d.\n",
           siginfo->si_pid, siginfo->si_signo, siginfo->si_code);
}

static void broadcast_msg(ProcMsg *msg, bool need_reply)
{
    PCIProxyDev *entry;
    unsigned int pid;
    int wait;

    QLIST_FOREACH(entry, &proxy_dev_list.devices, next) {
        if (need_reply) {
            wait = GET_REMOTE_WAIT;
            msg->num_fds = 1;
            msg->fds[0] = wait;
        }

        proxy_proc_send(entry->proxy_link, msg);
        if (need_reply) {
            pid = (uint32_t)wait_for_remote(wait);
            PUT_REMOTE_WAIT(wait);
            /* TODO: Add proper handling. */
            if (pid) {
                need_reply = 0;
            }
        }
    }
}

#define NOP_INTERVAL 1000000

static void remote_ping(void *opaque)
{
    ProcMsg msg;

    memset(&msg, 0, sizeof(ProcMsg));

    msg.num_fds = 0;
    msg.cmd = PROXY_PING;
    msg.bytestream = 0;
    msg.size = 0;

    broadcast_msg(&msg, true);
    timer_mod(hb_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + NOP_INTERVAL);

}

void start_heartbeat_timer(void)
{
    hb_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                            remote_ping,
                                            &proxy_dev_list);
    timer_mod(hb_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + NOP_INTERVAL);

}

static void stop_heartbeat_timer(void)
{
    timer_del(hb_timer);
    timer_free(hb_timer);
}

static void set_sigchld_handler(void)
{
    struct sigaction sa_sigterm;
    memset(&sa_sigterm, 0, sizeof(sa_sigterm));
    sa_sigterm.sa_sigaction = childsig_handler;
    sa_sigterm.sa_flags = SA_SIGINFO | SA_NOCLDWAIT | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_sigterm, NULL);
}

static void proxy_ready(PCIDevice *dev)
{
    PCIProxyDev *pdev = PCI_PROXY_DEV(dev);

    setup_irqfd(pdev);
    set_sigchld_handler();
    start_heartbeat_timer();
}

static void set_remote_opts(PCIDevice *dev, QDict *qdict, unsigned int cmd)
{
    QString *qstr;
    ProcMsg msg;
    const char *str;
    PCIProxyDev *pdev;

    pdev = PCI_PROXY_DEV(dev);

    qstr = qobject_to_json(QOBJECT(qdict));
    str = qstring_get_str(qstr);
    qemu_log_mask(LOG_REMOTE_DEBUG, "remote qdict in proxy: %s.\n", str);

    memset(&msg, 0, sizeof(ProcMsg));

    msg.data2 = (uint8_t *)str;
    msg.cmd = cmd;
    msg.bytestream = 1;
    msg.size = qstring_get_length(qstr) + 1;
    msg.num_fds = 0;

    proxy_proc_send(pdev->proxy_link, &msg);

    return;
}

static int config_op_send(PCIProxyDev *dev, uint32_t addr, uint32_t *val, int l,
                          unsigned int op)
{
    ProcMsg msg;
    struct conf_data_msg conf_data;
    int wait;

    memset(&msg, 0, sizeof(ProcMsg));
    conf_data.addr = addr;
    conf_data.val = (op == CONF_WRITE) ? *val : 0;
    conf_data.l = l;

    msg.data2 = (uint8_t *)malloc(sizeof(conf_data));
    if (!msg.data2) {
        qemu_log_mask(LOG_REMOTE_DEBUG, "Failed to allocate memory"
                      " for msg.data2.\n");
        return -ENOMEM;
    }

    memcpy(msg.data2, (const uint8_t *)&conf_data, sizeof(conf_data));
    msg.size = sizeof(conf_data);
    msg.cmd = op;
    msg.bytestream = 1;

    if (op == CONF_WRITE) {
        msg.num_fds = 0;
    } else {
        wait = GET_REMOTE_WAIT;
        msg.num_fds = 1;
        msg.fds[0] = wait;
    }

    proxy_proc_send(dev->proxy_link, &msg);

    if (op == CONF_READ) {
        *val = (uint32_t)wait_for_remote(wait);
        PUT_REMOTE_WAIT(wait);
    }

    free(msg.data2);

    return 0;
}

static uint32_t pci_proxy_read_config(PCIDevice *d, uint32_t addr, int len)
{
    uint32_t val;

    (void)pci_default_read_config(d, addr, len);

    config_op_send(PCI_PROXY_DEV(d), addr, &val, len, CONF_READ);

    return val;
}

static void pci_proxy_write_config(PCIDevice *d, uint32_t addr, uint32_t val,
                                   int l)
{
    pci_default_write_config(d, addr, val, l);

    config_op_send(PCI_PROXY_DEV(d), addr, &val, l, CONF_WRITE);
}

static void pci_proxy_dev_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pci_proxy_dev_realize;
    k->exit = pci_dev_exit;
    k->config_read = pci_proxy_read_config;
    k->config_write = pci_proxy_write_config;
}

static const TypeInfo pci_proxy_dev_type_info = {
    .name          = TYPE_PCI_PROXY_DEV,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIProxyDev),
    .abstract      = true,
    .class_size    = sizeof(PCIProxyDevClass),
    .class_init    = pci_proxy_dev_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pci_proxy_dev_register_types(void)
{
    type_register_static(&pci_proxy_dev_type_info);
}

type_init(pci_proxy_dev_register_types)

static void proxy_intx_update(PCIDevice *pci_dev)
{
    PCIProxyDev *dev = PCI_PROXY_DEV(pci_dev);
    PCIINTxRoute route;
    int pin = pci_get_byte(pci_dev->config + PCI_INTERRUPT_PIN) - 1;

    if (dev->irqfd.fd) {
        dev->irqfd.flags = KVM_IRQFD_FLAG_DEASSIGN;
        (void) kvm_vm_ioctl(kvm_state, KVM_IRQFD, &dev->irqfd);
        memset(&dev->irqfd, 0, sizeof(struct kvm_irqfd));
    }

    route = pci_device_route_intx_to_irq(pci_dev, pin);

    dev->irqfd.fd = event_notifier_get_fd(&dev->intr);
    dev->irqfd.resamplefd = event_notifier_get_fd(&dev->resample);
    dev->irqfd.gsi = route.irq;
    dev->irqfd.flags |= KVM_IRQFD_FLAG_RESAMPLE;
    (void) kvm_vm_ioctl(kvm_state, KVM_IRQFD, &dev->irqfd);
}

static void setup_irqfd(PCIProxyDev *dev)
{
    PCIDevice *pci_dev = PCI_DEVICE(dev);
    ProcMsg msg;

    event_notifier_init(&dev->intr, 0);
    event_notifier_init(&dev->resample, 0);

    memset(&msg, 0, sizeof(ProcMsg));
    msg.cmd = SET_IRQFD;
    msg.num_fds = 2;
    msg.fds[0] = event_notifier_get_fd(&dev->intr);
    msg.fds[1] = event_notifier_get_fd(&dev->resample);
    msg.data1.set_irqfd.intx =
        pci_get_byte(pci_dev->config + PCI_INTERRUPT_PIN) - 1;

    proxy_proc_send(dev->proxy_link, &msg);

    memset(&dev->irqfd, 0, sizeof(struct kvm_irqfd));

    proxy_intx_update(pci_dev);

    pci_device_set_intx_routing_notifier(pci_dev, proxy_intx_update);
}

static void init_emulation_process(PCIProxyDev *pdev, char *command, Error **errp)
{
    char *args[3];
    pid_t rpid;
    int fd[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) {
        error_setg(errp, "Unable to create unix socket.");
        return;
    }

    /* TODO: Restrict the forked process' permissions and capabilities. */
    rpid = qemu_fork(errp);

    if (rpid == -1) {
        error_setg(errp, "Unable to spawn emulation program.");
        close(fd[0]);
        close(fd[1]);
        return;
    }

    if (rpid == 0) {
        close(fd[0]);

        args[0] = g_strdup(command);
        args[1] = g_strdup_printf("%d", fd[1]);
        args[2] = NULL;

        execvp(args[0], (char *const *)args);
        exit(1);
    }

    pdev->proxy_link = proxy_link_create();
    pdev->remote_pid = rpid;

    if (!pdev->proxy_link) {
        error_setg(errp, "Failed to create proxy link");
        close(fd[0]);
        close(fd[1]);
        return;
    }

    proxy_link_set_sock(pdev->proxy_link, fd[0]);

    close(fd[1]);
}

static void pci_proxy_dev_realize(PCIDevice *device, Error **errp)
{
    PCIProxyDev *dev = PCI_PROXY_DEV(device);
    PCIProxyDevClass *k = PCI_PROXY_DEV_GET_CLASS(dev);
    Error *local_err = NULL;
    PCMachineState *pcms = PC_MACHINE(current_machine);
    DeviceState *d = DEVICE(dev);

    init_emulation_process(dev, k->command, errp);

    (void)g_hash_table_insert(pcms->remote_devs, (gpointer)d->id, (gpointer)dev);

    if (k->realize) {
        k->realize(dev, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
        }
    }

    dev->sync = REMOTE_MEM_SYNC(object_new(TYPE_MEMORY_LISTENER));

    configure_memory_sync(dev->sync, dev->proxy_link);
    dev->set_remote_opts = set_remote_opts;
    dev->proxy_ready = proxy_ready;

}

static void pci_dev_exit(PCIDevice *pdev)
{
    PCIProxyDev *entry, *sentry;
    PCIProxyDev *dev = PCI_PROXY_DEV(pdev);

    stop_heartbeat_timer();

    QLIST_FOREACH_SAFE(entry, &proxy_dev_list.devices, next, sentry) {
        if (entry->remote_pid == dev->remote_pid) {
            QLIST_REMOVE(entry, next);
        }
    }

    if (!QLIST_EMPTY(&proxy_dev_list.devices)) {
        start_heartbeat_timer();
    }
}

static void send_bar_access_msg(ProxyLinkState *proxy_link, MemoryRegion *mr,
                                bool write, hwaddr addr, uint64_t *val,
                                unsigned size, bool memory)
{
    ProcMsg msg;
    int wait;

    memset(&msg, 0, sizeof(ProcMsg));

    msg.bytestream = 0;
    msg.size = sizeof(msg.data1);
    msg.data1.bar_access.addr = mr->addr + addr;
    msg.data1.bar_access.size = size;
    msg.data1.bar_access.memory = memory;

    if (write) {
        msg.cmd = BAR_WRITE;
        msg.data1.bar_access.val = *val;
    } else {
        wait = GET_REMOTE_WAIT;

        msg.cmd = BAR_READ;
        msg.num_fds = 1;
        msg.fds[0] = wait;
    }

    proxy_proc_send(proxy_link, &msg);

    if (!write) {
        *val = wait_for_remote(wait);
        PUT_REMOTE_WAIT(wait);
    }
}

void proxy_default_bar_write(PCIProxyDev *dev, MemoryRegion *mr, hwaddr addr,
                             uint64_t val, unsigned size, bool memory)
{
    send_bar_access_msg(dev->proxy_link, mr, true, addr, &val, size, memory);
}

uint64_t proxy_default_bar_read(PCIProxyDev *dev, MemoryRegion *mr, hwaddr addr,
                                unsigned size, bool memory)
{
    uint64_t val;

    send_bar_access_msg(dev->proxy_link, mr, false, addr, &val, size, memory);

    return val;
}
