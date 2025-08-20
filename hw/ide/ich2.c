/*
 * Intel 82801BAM IDE U100 Controller
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2006 Openedhand Ltd.
 * Copyright (c) 2025 Tisenu100.
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
 *
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/irq.h"
#include "hw/southbridge/ich2.h"
#include "hw/ide/pci.h"
#include "ide-internal.h"
#include "trace.h"

static uint64_t bmdma_read(void *opaque, hwaddr addr, unsigned size)
{
    BMDMAState *bm = opaque;
    uint32_t val;

    if (size != 1) {
        return ((uint64_t)1 << (size * 8)) - 1;
    }

    switch(addr & 3) {
    case 0:
        val = bm->cmd;
        break;
    case 2:
        val = bm->status;
        break;
    default:
        val = 0xff;
        break;
    }

    trace_bmdma_read(addr, val);
    return val;
}

static void bmdma_write(void *opaque, hwaddr addr,
                        uint64_t val, unsigned size)
{
    BMDMAState *bm = opaque;

    if (size != 1) {
        return;
    }

    trace_bmdma_write(addr, val);

    switch(addr & 3) {
    case 0:
        bmdma_cmd_writeb(bm, val);
        break;
    case 2:
        bmdma_status_writeb(bm, val);
        break;
    }
}

static const MemoryRegionOps ich2_bmdma_ops = {
    .read = bmdma_read,
    .write = bmdma_write,
};

static void bmdma_setup_bar(PCIIDEState *d)
{
    memory_region_init(&d->bmdma_bar, OBJECT(d), "ich2-bmdma-container", 16);
    for(int i = 0; i < 2; i++) {
        BMDMAState *bm = &d->bmdma[i];

        memory_region_init_io(&bm->extra_io, OBJECT(d), &ich2_bmdma_ops, bm, "ich2-bmdma", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8, &bm->extra_io);
        memory_region_init_io(&bm->addr_ioport, OBJECT(d), &bmdma_addr_ioport_ops, bm, "bmdma", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8 + 4, &bm->addr_ioport);
    }
}

static void ich2_ide_raise_irq(void *opaque, int n, int level)
{
    PCIIDEState *d = opaque;

    qemu_set_irq(d->isa_irq[n], level);
}

static void ich2_update_drives(PCIIDEState *d)
{
    PCIDevice *dev = PCI_DEVICE(d);
    uint32_t drive_stats = pci_get_long(dev->config + 0x40);

    if (d->bus[0].portio_list.owner && !(drive_stats & 0x00008000)) {
        portio_list_del(&d->bus[0].portio_list);
        portio_list_destroy(&d->bus[0].portio_list);
    }

    if (d->bus[0].portio2_list.owner && !(drive_stats & 0x00008000)) {
        portio_list_del(&d->bus[0].portio2_list);
        portio_list_destroy(&d->bus[0].portio2_list);
    }

    if (d->bus[1].portio_list.owner && !(drive_stats & 0x80000000)) {
        portio_list_del(&d->bus[1].portio_list);
        portio_list_destroy(&d->bus[1].portio_list);
    }

    if (d->bus[1].portio2_list.owner && !(drive_stats & 0x80000000)) {
        portio_list_del(&d->bus[1].portio2_list);
        portio_list_destroy(&d->bus[1].portio2_list);
    }

    if(drive_stats & 0x00008000) {
        if (!d->bus[0].portio_list.owner) {
            portio_list_init(&d->bus[0].portio_list, OBJECT(d), ide_portio_list, &d->bus[0], "ide");
            portio_list_add(&d->bus[0].portio_list, pci_address_space_io(dev), 0x1f0);
        }

        if (!d->bus[0].portio2_list.owner) {
            portio_list_init(&d->bus[0].portio2_list, OBJECT(d), ide_portio2_list, &d->bus[0], "ide");
            portio_list_add(&d->bus[0].portio2_list, pci_address_space_io(dev), 0x3f6);
        }
    }

    if(drive_stats & 0x80000000) {
        if (!d->bus[1].portio_list.owner) {
            portio_list_init(&d->bus[1].portio_list, OBJECT(d), ide_portio_list, &d->bus[1], "ide");
            portio_list_add(&d->bus[1].portio_list, pci_address_space_io(dev), 0x170);
        }

        if (!d->bus[1].portio2_list.owner) {
            portio_list_init(&d->bus[1].portio2_list, OBJECT(d), ide_portio2_list, &d->bus[1], "ide");
            portio_list_add(&d->bus[1].portio2_list, pci_address_space_io(dev), 0x376);
        }
    }
}

static void ich2_ide_config_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len)
{
    PCIIDEState *d = PCI_IDE(dev);

    pci_default_write_config(dev, addr, val, len);

    if((addr >= 0x40) && (addr <= 0x43))
        ich2_update_drives(d);
}

static void ich2_ide_reset(DeviceState *dev)
{
    PCIIDEState *d = PCI_IDE(dev);
    PCIDevice *pci_dev = PCI_DEVICE(d);

    for (int i = 0; i < 2; i++) {
        ide_bus_reset(&d->bus[i]);
        ide_bus_register_restart_cb(&d->bus[i]);
    }

    pci_set_word(pci_dev->config + PCI_COMMAND, 0x0000);
    pci_set_word(pci_dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);
    pci_set_byte(pci_dev->config + PCI_CLASS_PROG, 0x80);
    pci_set_long(pci_dev->config + 0x20, 0x0000001);

    ich2_update_drives(d);
}

static void ich2_ide_realize(PCIDevice *dev, Error **errp)
{
    PCIIDEState *d = PCI_IDE(dev);

    qdev_init_gpio_in(DEVICE(d), ich2_ide_raise_irq, 2);

    ide_bus_init(&d->bus[0], sizeof(d->bus[0]), DEVICE(d), 0, 2);
    ide_bus_init_output_irq(&d->bus[0], qdev_get_gpio_in(DEVICE(dev), 0));
    bmdma_init(&d->bus[0], &d->bmdma[0], d);

    ide_bus_init(&d->bus[1], sizeof(d->bus[1]), DEVICE(d), 1, 2);
    ide_bus_init_output_irq(&d->bus[1], qdev_get_gpio_in(DEVICE(dev), 1));
    bmdma_init(&d->bus[1], &d->bmdma[1], d);

    bmdma_setup_bar(d);
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &d->bmdma_bar);
}

static void ich2_ide_exitfn(PCIDevice *dev)
{
    PCIIDEState *d = PCI_IDE(dev);

    for (int i = 0; i < 2; ++i) {
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].extra_io);
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].addr_ioport);
    }
}

static void ich2_ide_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ich2_ide_reset);
    dc->vmsd = &vmstate_ide_pci;
    k->config_write = ich2_ide_config_write;
    k->realize = ich2_ide_realize;
    k->exit = ich2_ide_exitfn;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_ICH2_IDE;
    k->class_id = PCI_CLASS_STORAGE_IDE;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->user_creatable = false;
    dc->hotpluggable = false;
}

static const TypeInfo ich2_ide_info = {
    .name          = TYPE_ICH2_IDE_PCI_DEVICE,
    .parent        = TYPE_PCI_IDE,
    .class_init    = ich2_ide_class_init,
};

static void ich2_ide_register_types(void)
{
    type_register_static(&ich2_ide_info);
}

type_init(ich2_ide_register_types)
