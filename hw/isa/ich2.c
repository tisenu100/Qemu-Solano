/*
 * Intel 82801BA ISA Bridge
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
#include "hw/acpi/acpi.h"
#include "hw/acpi/ich9_tco.h"
#include "hw/southbridge/ich2.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/intc/ioapic.h"
#include "hw/isa/apm.h"
#include "hw/isa/isa.h"
#include "system/runstate.h"

static void pm_tmr_timer(ACPIREGS *ar)
{
    ICH2State *d = container_of(ar, ICH2State, ar);
    acpi_update_sci(&d->ar, d->sci_irq);
}

static void apm_ctrl_changed(uint32_t val, void *opaque)
{
    ICH2State *s = opaque;

    acpi_pm1_cnt_update(&s->ar, val == 0xf1, val == 0xf0);
    if (val == 0xf1 || val == 0xf0) {
        return;
    }

    if (s->smi[0] & 0x20) {
        qemu_printf("Intel ICH2: An APMC SMI was provoked\n");
        s->smi[4] |= 0x20;
        qemu_irq_raise(s->smi_irq);
    }
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    ACPIREGS *ar = opaque;
    ICH2State *d = container_of(ar, ICH2State, ar);

    acpi_gpe_ioport_writeb(ar, addr, val);
    acpi_update_sci(ar, d->smi_irq); /* The BIOS want to generate a wake event via this */
}

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned len)
{
    ACPIREGS *ar = opaque;
    return acpi_gpe_ioport_readb(ar, addr);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void smi_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    ICH2State *d = opaque;

    if(addr > 3)
        d->smi[addr] &= ~val;
    else
        d->smi[addr] = val;
}

static uint64_t smi_read(void *opaque, hwaddr addr, unsigned len)
{
    ICH2State *d = opaque;

    return d->smi[addr];
}

static const MemoryRegionOps smi_ops = {
    .read = smi_read,
    .write = smi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void ich2_update_acpi(ICH2State *s)
{
    PCIDevice *pci_dev = PCI_DEVICE(s);
    uint16_t addr = pci_get_word(pci_dev->config + 0x40) & 0xffc0;
    bool enable = !!(pci_get_byte(pci_dev->config + 0x44) & 0x10);
    int sci_num = pci_get_byte(pci_dev->config + 0x44) & 7;

    memory_region_transaction_begin();

    memory_region_set_enabled(&s->acpi_io, false);

    if(enable && (addr != 0)) {
        memory_region_set_address(&s->acpi_io, addr);
        memory_region_set_enabled(&s->acpi_io, true);
        qemu_printf("Intel ICH2: ACPI was enabled at address 0x%04x\n", addr);
    }

    memory_region_transaction_commit();

    switch(sci_num) {
        case 1: case 2:
            sci_num = 10 + (sci_num & 1);
        break;

        case 4: case 5: case 6: case 7:
            sci_num = 20 + (sci_num & 3);
        break;

        default:
            sci_num = 9;
        break;
    }

    qemu_printf("Intel ICH2: SCI IRQ was set to %d\n", sci_num);
    s->sci_irq = s->isa_irqs_in[sci_num];
}

static int ich2_get_pirq(PCIDevice *pci_dev, int pirq)
{
    uint8_t val = 0;
    int irq = 0;

    if(pirq > 3)
        val = pci_get_byte(pci_dev->config + 0x68 + (pirq - 3));
    else
        val = pci_get_byte(pci_dev->config + 0x60 + pirq);
    
    if(val & 0x80) /* Forward from APIC  */
        irq = 16 + pirq;
    else           /* Forward from i8259 */
        irq = val & 0x0f;

    return irq;
}

static void ich2_update_pirq(void *opaque, int pirq, int level)
{
    ICH2State *s = opaque;
    PCIDevice *pci_dev = PCI_DEVICE(s);
    int irq = ich2_get_pirq(pci_dev, pirq);

    qemu_set_irq(s->isa_irqs_in[irq], level);
}

static PCIINTxRoute ich2_route_intx_pin_to_irq(void *opaque, int pirq)
{
    PCIDevice *pci_dev = opaque;
    int irq = ich2_get_pirq(pci_dev, pirq);
    PCIINTxRoute route;

    if (irq < IOAPIC_NUM_PINS) {
        route.mode = PCI_INTX_ENABLED;
        route.irq = irq;
    } else {
        route.mode = PCI_INTX_DISABLED;
        route.irq = -1;
    }
    return route;
}

static void ich2_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    ICH2State *s = ICH2_PCI_DEVICE(dev);

    pci_default_write_config(dev, address, val, len);

    switch(address) {
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44:
            ich2_update_acpi(s);
        break;

        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x68: case 0x69: case 0x6a: case 0x6b:
            pci_bus_fire_intx_routing_notifier(pci_get_bus(&s->dev));
        break;
    }
}

static void rcr_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    ICH2State *d = opaque;

    if (val & 4) {
        qemu_printf("Intel ICH2: Reset triggered by RCR\n");
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    d->rcr = val & 2; /* keep System Reset type only */
}

static uint64_t rcr_read(void *opaque, hwaddr addr, unsigned len)
{
    ICH2State *d = opaque;

    return d->rcr;
}

static const MemoryRegionOps rcr_ops = {
    .read = rcr_read,
    .write = rcr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void ich2_reset(DeviceState *dev)
{
    ICH2State *d = ICH2_PCI_DEVICE(dev);
    PCIDevice *pci_dev = PCI_DEVICE(d);
    PCIBus *pci_bus = pci_get_bus(pci_dev);

    pci_set_word(pci_dev->config + PCI_COMMAND, PCI_COMMAND_SPECIAL | PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    pci_set_word(pci_dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);
    pci_set_long(pci_dev->config + 0x40, 0x00000001);
    pci_set_byte(pci_dev->config + 0x44, 0x00);
    pci_set_long(pci_dev->config + 0x58, 0x00000001);
    pci_set_long(pci_dev->config + 0x60, 0x80808080);
    pci_set_byte(pci_dev->config + 0x54, 0x10);
    pci_set_long(pci_dev->config + 0x68, 0x80808080);
    pci_set_long(pci_dev->config + 0xd4, 0x00000f00);
    pci_set_byte(pci_dev->config + 0xe1, 0xff);
    pci_set_long(pci_dev->config + 0xe8, 0x00112233);
    pci_set_word(pci_dev->config + 0xee, 0x5678);
    pci_set_byte(pci_dev->config + 0xf2, 0x0f);

    ich2_update_acpi(d);
    acpi_pm1_evt_reset(&d->ar);
    acpi_pm1_cnt_reset(&d->ar);
    acpi_pm_tmr_reset(&d->ar);
    acpi_gpe_reset(&d->ar);
    acpi_update_sci(&d->ar, d->sci_irq);

    pci_bus_fire_intx_routing_notifier(pci_bus);
}

static void pci_ich2_realize(PCIDevice *dev, Error **errp)
{
    ICH2State *d = ICH2_PCI_DEVICE(dev);
    PCIBus *pci_bus = pci_get_bus(dev);
    ISABus *isa_bus;
    uint32_t irq;

    isa_bus = isa_bus_new(DEVICE(d), pci_address_space(dev), pci_address_space_io(dev), errp);

    if (!isa_bus) {
        return;
    }

    qemu_printf("Intel ICH2: Setup RCR\n");
    memory_region_init_io(&d->rcr_mem, OBJECT(dev), &rcr_ops, d, "reset-control", 1);
    memory_region_add_subregion_overlap(pci_address_space_io(dev), 0xcf9, &d->rcr_mem, 1);

    qemu_printf("Intel ICH2: Setup LPC bus\n");
    isa_bus_register_input_irqs(isa_bus, d->isa_irqs_in);

    qdev_prop_set_int32(DEVICE(&d->rtc), "base_year", 2000);
    if (!qdev_realize(DEVICE(&d->rtc), BUS(isa_bus), errp)) {
        return;
    }
    irq = object_property_get_uint(OBJECT(&d->rtc), "irq", &error_fatal);
    isa_connect_gpio_out(ISA_DEVICE(&d->rtc), 0, irq);

    pci_bus_irqs(pci_bus, ich2_update_pirq, d, 8);
    pci_bus_set_route_irq_fn(pci_bus, ich2_route_intx_pin_to_irq);

    qemu_printf("Intel ICH2: Setup ACPI\n");
    memory_region_init(&d->acpi_io, OBJECT(d), "ich2-acpi", 128);
    memory_region_set_enabled(&d->acpi_io, false);
    memory_region_add_subregion(pci_address_space_io(dev), 0, &d->acpi_io);

    acpi_pm_tmr_init(&d->ar, pm_tmr_timer, &d->acpi_io);
    acpi_pm1_evt_init(&d->ar, pm_tmr_timer, &d->acpi_io);
    acpi_pm1_cnt_init(&d->ar, &d->acpi_io, 0, 0, 6, 1);
    acpi_gpe_init(&d->ar, 4);
    acpi_pm_tco_init(&d->tco, &d->acpi_io);

    memory_region_init_io(&d->gpio_io, OBJECT(dev), &gpio_ops, &d->ar, "gpio", 8);
    memory_region_add_subregion_overlap(&d->acpi_io, 0x28, &d->gpio_io, 1);

    memory_region_init_io(&d->smi_io, OBJECT(dev), &smi_ops, d, "smi-control", 8);
    memory_region_add_subregion_overlap(&d->acpi_io, 0x30, &d->smi_io, 1);

    apm_init(dev, &d->apm, apm_ctrl_changed, d);
}

static void pci_ich2_init(Object *obj)
{
    ICH2State *d = ICH2_PCI_DEVICE(obj);

    qdev_init_gpio_out_named(DEVICE(obj), d->isa_irqs_in, "isa-irqs", IOAPIC_NUM_PINS);
    qdev_init_gpio_out(DEVICE(obj), &d->sci_irq, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &d->smi_irq, "smi-irq", 1);

    object_initialize_child(obj, "rtc", &d->rtc, TYPE_MC146818_RTC);
}

static void pci_ich2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize= pci_ich2_realize;
    k->config_write = ich2_write_config;
    device_class_set_legacy_reset(dc, ich2_reset);
    dc->desc        = "Intel ICH2";
    dc->hotpluggable   = false;
    k->vendor_id    = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_ICH2_LPC;
    k->revision = 0x01;
    k->class_id     = PCI_CLASS_BRIDGE_ISA;
    dc->user_creatable = false;
}

static const TypeInfo ich2_type_info = {
    .name = TYPE_ICH2_PCI_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ICH2State),
    .instance_init = pci_ich2_init,
    .class_init = pci_ich2_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ich2_register_types(void)
{
    type_register_static(&ich2_type_info);
}

type_init(ich2_register_types)
