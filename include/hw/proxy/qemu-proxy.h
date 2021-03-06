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

#ifndef QEMU_PROXY_H
#define QEMU_PROXY_H

#include <linux/kvm.h>

#include "io/proxy-link.h"
#include "hw/proxy/memory-sync.h"
#include "qemu/event_notifier.h"
#include "hw/pci/pci.h"
#include "block/qdict.h"

#define TYPE_PCI_PROXY_DEV "pci-proxy-dev"

#define PCI_PROXY_DEV(obj) \
            OBJECT_CHECK(PCIProxyDev, (obj), TYPE_PCI_PROXY_DEV)

#define PCI_PROXY_DEV_CLASS(klass) \
            OBJECT_CLASS_CHECK(PCIProxyDevClass, (klass), TYPE_PCI_PROXY_DEV)

#define PCI_PROXY_DEV_GET_CLASS(obj) \
            OBJECT_GET_CLASS(PCIProxyDevClass, (obj), TYPE_PCI_PROXY_DEV)

typedef struct PCIProxyDev {
    PCIDevice parent_dev;

    int n_mr_sections;
    MemoryRegionSection *mr_sections;

    ProxyLinkState *proxy_link;

    RemoteMemSync *sync;
    struct kvm_irqfd irqfd;

    EventNotifier intr;
    EventNotifier resample;

    pid_t remote_pid;
    char *rid;

    QLIST_ENTRY(PCIProxyDev) next;

    void (*set_remote_opts) (PCIDevice *dev, QDict *qdict, unsigned int cmd);
    void (*proxy_ready) (PCIDevice *dev);

} PCIProxyDev;

typedef struct PCIProxyDevClass {
    PCIDeviceClass parent_class;

    void (*realize)(PCIProxyDev *dev, Error **errp);

    char *command;
} PCIProxyDevClass;

typedef struct PCIProxyDevList {
    QLIST_HEAD(, PCIProxyDev) devices;
} proxy_dev_list_t;

extern QemuMutex proxy_list_lock;
extern proxy_dev_list_t proxy_dev_list;

void proxy_default_bar_write(PCIProxyDev *dev, MemoryRegion *mr, hwaddr addr,
                             uint64_t val, unsigned size, bool memory);

uint64_t proxy_default_bar_read(PCIProxyDev *dev, MemoryRegion *mr, hwaddr addr,
                                unsigned size, bool memory);

#endif /* QEMU_PROXY_H */
