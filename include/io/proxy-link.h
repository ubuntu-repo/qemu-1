/*
 * Communication channel between QEMU and remote device process
 *
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

#ifndef PROXY_LINK_H
#define PROXY_LINK_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>

#include "qemu/osdep.h"
#include "qom/object.h"
#include "qemu/thread.h"
#include "exec/cpu-common.h"
#include "exec/hwaddr.h"

typedef struct ProxyLinkState ProxyLinkState;

#define TYPE_PROXY_LINK "proxy-link"
#define PROXY_LINK(obj) \
    OBJECT_CHECK(ProxyLinkState, (obj), TYPE_PROXY_LINK)

#define REMOTE_MAX_FDS 8

#define PROC_HDR_SIZE offsetof(ProcMsg, data1.u64)

/*
 * proc_cmd_t enum type to specify the command to be executed on the remote
 * device
 *
 * Following commands are supported:
 * CONF_READ        PCI config. space read
 * CONF_WRITE       PCI config. space write
 * SYNC_SYSMEM      Shares QEMU's RAM with remote device's RAM
 * BAR_WRITE        Writes to PCI BAR region
 * BAR_READ         Reads from PCI BAR region
 * SET_IRQFD        Sets the IRQFD to be used to raise interrupts directly
 *                  from remote device
 * DEVICE_ADD       QMP/HMP command to hotplug device
 * DEVICE_DEL       QMP/HMP command to hot-unplug device
 * DRIVE_ADD        HMP command to hotplug drive
 * DRIVE_DEL        HMP command to hot-unplug drive
 * BLOCK_RESIZE     QMP/HMP command to resize block backend
 *
 */
typedef enum {
    INIT = 0,
    CONF_READ,
    CONF_WRITE,
    SYNC_SYSMEM,
    BAR_WRITE,
    BAR_READ,
    SET_IRQFD,
    DEV_OPTS,
    DRIVE_OPTS,
    DEVICE_ADD,
    DEVICE_DEL,
    DRIVE_ADD,
    DRIVE_DEL,
    PROXY_PING,
    BLOCK_RESIZE,
    MAX,
} proc_cmd_t;

/*
 * ProcMsg Format of the message sent to the remote device from QEMU
 *
 * cmd         The remote command
 * bytestream  Indicates if the data to be shared is structured (data1)
 *             or unstructured (data2)
 * size        Size of the data to be shared
 * data1       Structured data
 * fds         File descriptors to be shared with remote device
 * data2       Unstructured data
 *
 */
typedef struct {
    hwaddr gpas[REMOTE_MAX_FDS];
    uint64_t sizes[REMOTE_MAX_FDS];
    ram_addr_t offsets[REMOTE_MAX_FDS];
} sync_sysmem_msg_t;

typedef struct {
    hwaddr addr;
    uint64_t val;
    unsigned size;
    bool memory;
} bar_access_msg_t;

typedef struct {
    int intx;
} set_irqfd_msg_t;

typedef struct {
    proc_cmd_t cmd;
    int bytestream;
    size_t size;

    union {
        uint64_t u64;
        sync_sysmem_msg_t sync_sysmem;
        bar_access_msg_t bar_access;
        set_irqfd_msg_t set_irqfd;
    } data1;

    int fds[REMOTE_MAX_FDS];
    int num_fds;

    uint8_t *data2;
} ProcMsg;

struct conf_data_msg {
    uint32_t addr;
    uint32_t val;
    int l;
};

typedef void (*proxy_link_callback)(GIOCondition cond);

typedef struct ProxySrc {
    GSource gsrc;
    GPollFD gpfd;
} ProxySrc;

/*
 * ProxyLinkState Instance info. of the communication
 * link between QEMU and remote process
 *
 * ctx        GMainContext to be used for communication
 * loop       Main loop that would be used to poll for incoming data
 * src        Source fds to poll on, and which events to poll on
 * sock       Unix socket used for the link
 * lock       Lock to synchronize access to the link
 *
 */
struct ProxyLinkState {
    Object obj;

    GMainContext *ctx;
    GMainLoop *loop;
    ProxySrc *src;

    int sock;
    QemuMutex lock;

    proxy_link_callback callback;
};

#define GET_REMOTE_WAIT eventfd(0, 0)
#define PUT_REMOTE_WAIT(wait) close(wait)
#define PROXY_LINK_WAIT_DONE 1

ProxyLinkState *proxy_link_create(void);
void proxy_link_finalize(ProxyLinkState *s);

void proxy_proc_send(ProxyLinkState *s, ProcMsg *msg);
int proxy_proc_recv(ProxyLinkState *s, ProcMsg *msg);
uint64_t wait_for_remote(int efd);
void notify_proxy(int fd, uint64_t val);

void proxy_link_set_sock(ProxyLinkState *s, int fd);
void proxy_link_set_callback(ProxyLinkState *s, proxy_link_callback callback);
void start_handler(ProxyLinkState *s);

#endif
