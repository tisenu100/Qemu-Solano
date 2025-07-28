/*
 * Intel Corporation 82801BA ISA Bridge
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2018 Hervé Poussineau
 * Copyright (c) 2025 Tisenu100
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

#include "qemu/osdep.h"
#include "qemu/range.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "hw/i2c/pm_smbus.h"
#include "hw/southbridge/ich2.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/isa/isa.h"
#include "system/runstate.h"
#include "migration/vmstate.h"

static void ich2_smbus_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    pci_default_write_config(dev, address, val, len);

    if((address == 0x20) && (pci_get_byte(dev->config + 0x04) & 1))
        qemu_printf("Intel ICH2 SMBus: SMBus has been updated to 0x%04x\n", pci_get_word(dev->config + 0x20) & 0xfff0);
}

static const VMStateDescription vmstate_ich2_smbus = {
    .name = "Intel ICH2 SMBus",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, ICH2SMBState),
        VMSTATE_END_OF_LIST()
    },
};

static void pci_ich2_smbus_realize(PCIDevice *dev, Error **errp)
{
    ICH2SMBState *s = ICH2_SMBUS_PCI_DEVICE(dev);

    qemu_printf("Intel ICH2 SMBus: Setup SMBus\n");
    pm_smbus_init(DEVICE(dev), &s->smb, 0);
    pci_register_bar(dev, 4, 1, &s->smb.io);
}

static void pci_ich2_smbus_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize= pci_ich2_smbus_realize;
    k->config_write = ich2_smbus_write_config;
    dc->desc        = "Intel ICH2 SMBus";
    dc->hotpluggable   = false;
    dc->vmsd = &vmstate_ich2_smbus;
    k->vendor_id    = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_ICH2_SMBUS;
    k->revision = 0x01;
    k->class_id     = PCI_CLASS_SERIAL_SMBUS;
    dc->user_creatable = false;
}

static const TypeInfo ich2_smbus_type_info = {
    .name = TYPE_ICH2_SMBUS_PCI_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ICH2SMBState),
    .class_init = pci_ich2_smbus_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ich2_smbus_register_types(void)
{
    type_register_static(&ich2_smbus_type_info);
}

type_init(ich2_smbus_register_types)
