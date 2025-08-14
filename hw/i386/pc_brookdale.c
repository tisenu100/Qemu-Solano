/*
 * QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#include CONFIG_DEVICES

#include "qemu/units.h"
#include "qemu/qemu-print.h"
#include "hw/char/parallel-isa.h"
#include "hw/dma/i8257.h"
#include "hw/timer/i8254.h"
#include "hw/loader.h"
#include "hw/i386/x86.h"
#include "hw/i386/pc.h"
#include "hw/i386/apic.h"
#include "hw/isa/superio.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci-host/brookdale.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/southbridge/ich2.h"
#include "hw/display/ramfb.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/usb.h"
#include "net/net.h"
#include "hw/ide/pci.h"
#include "hw/irq.h"
#include "system/kvm.h"
#include "hw/i386/kvm/clock.h"
#include "hw/sysbus.h"
#include "hw/i2c/smbus_eeprom.h"
#include "system/memory.h"
#include "hw/acpi/acpi.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/usb/hcd-uhci.h"
#include "migration/global_state.h"
#include "migration/misc.h"
#include "system/runstate.h"
#include "target/i386/cpu.h"

static int hub_get_pirq(PCIDevice *pci_dev, int pin)
{
    return (0x3210 >> (pin * 4)) & 7;
}

static int agp_slot_get_pirq(PCIDevice *pci_dev, int pin)
{
    return (0x3210 >> (pin * 4)) & 7;
}

static int pci_slots_get_pirq(PCIDevice *pci_dev, int pin)
{
    int ret = 0;

    switch (PCI_SLOT(pci_dev->devfn)) {
        case 0x01:
            ret = (0x0231 >> (pin * 4)) & 7;
        break;

        case 0x02:
            ret = (0x2301 >> (pin * 4)) & 7;
        break;

        case 0x03:
            ret = (0x2103 >> (pin * 4)) & 7;
        break;

        case 0x04:
            ret = (0x1032 >> (pin * 4)) & 7;
        break;

        case 0x05:
            ret = (0x0213 >> (pin * 4)) & 7;
        break;

        case 0x06:
            ret = (0x1032 >> (pin * 4)) & 7;
        break;

        case 0x07:
            ret = (0x2103 >> (pin * 4)) & 7;
        break;

        default:
            ret = (0x3210 >> (pin * 4)) & 7;
        break;
    }

    return ret;
}

/* PC hardware initialisation */
static void pc_init(MachineState *machine)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    X86MachineState *x86ms = X86_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *system_io = get_system_io();
    Object *phb = NULL;

    PCIDevice *lpc_pci_dev;
    DeviceState *lpc_dev;
    ISABus *isa_bus;
    MC146818RtcState *rtc;
    qemu_irq smi_irq;
    GSIState *gsi_state;

    PCIDevice *agp_bridge_dev;
    PCIBridge *agp_bridge;

    PCIDevice *pci_bridge_dev;
    PCIBridge *pci_bridge;

    PCIDevice *ide_pci_dev;

    PCIDevice *smb_pci_dev;
    DeviceState *smb_dev;

    PCIDevice *ac97;

    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory = NULL;
    MemoryRegion *rom_memory = system_memory;
    ram_addr_t lowmem;
    uint64_t hole64_size = 0;

    qemu_printf("PC: Setting up\n");

    ram_memory = machine->ram;
    if (!pcms->max_ram_below_4g) {
        pcms->max_ram_below_4g = 0xe0000000;
    }
    lowmem = pcms->max_ram_below_4g;
    if (machine->ram_size >= pcms->max_ram_below_4g) {
        if (pcmc->gigabyte_align) {
            if (lowmem > 0xc0000000) {
                lowmem = 0xc0000000;
            }
            if (lowmem & (1 * GiB - 1)) {
                warn_report("Large machine and max_ram_below_4g "
                            "(%" PRIu64 ") not a multiple of 1G; "
                            "possible bad performance.",
                            pcms->max_ram_below_4g);
            }
        }
    }

    if (machine->ram_size >= lowmem) {
        x86ms->above_4g_mem_size = machine->ram_size - lowmem;
        x86ms->below_4g_mem_size = lowmem;
    } else {
        x86ms->above_4g_mem_size = 0;
        x86ms->below_4g_mem_size = machine->ram_size;
    }

    pc_machine_init_sgx_epc(pcms);
    x86_cpus_init(x86ms, pcmc->default_cpu_version);

    if (kvm_enabled()) {
        kvmclock_create(pcmc->kvmclock_create_always);
    }

    qemu_printf("PC: Starting the PCI Host\n");
    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);
    rom_memory = pci_memory;

    phb = OBJECT(qdev_new(TYPE_I845_PCI_HOST_BRIDGE));
    object_property_add_child(OBJECT(machine), "i845", phb);
    object_property_set_link(phb, PCI_HOST_PROP_RAM_MEM, OBJECT(ram_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_PCI_MEM, OBJECT(pci_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_SYSTEM_MEM, OBJECT(system_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_IO_MEM, OBJECT(system_io), &error_fatal);
    object_property_set_uint(phb, PCI_HOST_BELOW_4G_MEM_SIZE, x86ms->below_4g_mem_size, &error_fatal);
    object_property_set_uint(phb, PCI_HOST_ABOVE_4G_MEM_SIZE, x86ms->above_4g_mem_size, &error_fatal);
    object_property_set_str(phb, I845_HOST_PROP_PCI_TYPE, TYPE_I845_PCI_DEVICE, &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(phb), &error_fatal);

    pcms->pcibus = PCI_BUS(qdev_get_child_bus(DEVICE(phb), "pci.0"));
    pci_bus_map_irqs(pcms->pcibus, hub_get_pirq);

    hole64_size = object_property_get_uint(phb, PCI_HOST_PROP_PCI_HOLE64_SIZE, &error_abort);

    pc_memory_init(pcms, system_memory, rom_memory, hole64_size);

    gsi_state = pc_gsi_create(&x86ms->gsi, pcmc->pci_enabled);

    qemu_printf("PC: Setting up the LPC Bridge\n");
    lpc_pci_dev = pci_new_multifunction(PCI_DEVFN(0x1f, 0), TYPE_ICH2_PCI_DEVICE);
    lpc_dev = DEVICE(lpc_pci_dev);
    for (int i = 0; i < IOAPIC_NUM_PINS; i++) {
        qdev_connect_gpio_out_named(lpc_dev, "isa-irqs", i, x86ms->gsi[i]);
    }
    pci_realize_and_unref(lpc_pci_dev, pcms->pcibus, &error_fatal);

    isa_bus = ISA_BUS(qdev_get_child_bus(lpc_dev, "isa.0"));

    i8254_pit_init(isa_bus, 0x40, 0, NULL);
    i8257_dma_init(OBJECT(lpc_dev), isa_bus, 1);
    rtc = mc146818_rtc_init(isa_bus, 2000, NULL);
    x86ms->rtc = ISA_DEVICE(rtc);
    smi_irq = qemu_allocate_irq(pc_acpi_smi_interrupt, first_cpu, 0);
    qdev_connect_gpio_out_named(lpc_dev, "smi-irq", 0, smi_irq);

    if (tcg_enabled()) {
        x86_register_ferr_irq(x86ms->gsi[13]);
    }

    pc_vga_init(isa_bus, pcms->pcibus);

    qemu_printf("PC: Setting up the Super I/O\n");
    pc_basic_device_init_simple(pcms, isa_bus, x86ms->gsi);
    isa_create_simple(isa_bus, TYPE_WINBOND_W83627HF);

    qemu_printf("PC: Setting up IDE\n");
    ide_pci_dev = pci_create_simple(pcms->pcibus, PCI_DEVFN(0x1f, 1), TYPE_ICH2_IDE_PCI_DEVICE);
    pci_ide_create_devs(ide_pci_dev);
    pcms->idebus[0] = qdev_get_child_bus(DEVICE(ide_pci_dev), "ide.0");
    pcms->idebus[1] = qdev_get_child_bus(DEVICE(ide_pci_dev), "ide.1");
    

    qemu_printf("PC: Setting up the SMBus\n");
    smb_pci_dev = pci_create_simple(pcms->pcibus, PCI_DEVFN(0x1f, 3), TYPE_ICH2_SMBUS_PCI_DEVICE);
    smb_dev = DEVICE(smb_pci_dev);

    pcms->smbus = I2C_BUS(qdev_get_child_bus(smb_dev, "i2c"));
    uint8_t *spd = spd_data_generate(SDR, machine->ram_size);

    smbus_eeprom_init_one(pcms->smbus, 0x50, spd);

    qemu_printf("PC: Setting up Bridges\n");
    agp_bridge_dev = pci_new(PCI_DEVFN(0x01, 0), "brookdale-agp-bridge");
    agp_bridge = PCI_BRIDGE(agp_bridge_dev);
    pci_bridge_map_irq(agp_bridge, "pci.1", agp_slot_get_pirq);
    pci_realize_and_unref(agp_bridge_dev, pcms->pcibus, &error_fatal);

    pci_bridge_dev = pci_new(PCI_DEVFN(0x1e, 0), "ich2-pci-bridge");
    pci_bridge = PCI_BRIDGE(pci_bridge_dev);
    pci_bridge_map_irq(pci_bridge, "pci.2", pci_slots_get_pirq);
    pci_realize_and_unref(pci_bridge_dev, pcms->pcibus, &error_fatal);

    qemu_printf("PC: Setting up USB\n");
    pci_create_simple(pcms->pcibus, PCI_DEVFN(0x1f, 2), TYPE_ICH2_USB_UHCI1);
    pci_create_simple(pcms->pcibus, PCI_DEVFN(0x1f, 4), TYPE_ICH2_USB_UHCI2);

    qemu_printf("PC: Setting up AC97\n");
    ac97 = pci_new(PCI_DEVFN(0x1f, 5), "AC97");

    /* Realtek ALC200 */
    qdev_prop_set_uint16(DEVICE(ac97), "ac97-vendor", 0x414c);
    qdev_prop_set_uint16(DEVICE(ac97), "ac97-device", 0x4710);

    pci_realize_and_unref(ac97, pcms->pcibus, &error_fatal);

    qemu_printf("PC: Setting up interrupts\n");
    pc_i8259_create(isa_bus, gsi_state->i8259_irq);
    ioapic_init_gsi(gsi_state, phb);

    qemu_printf("PC: Passing control to the BIOS\n");
}

#define DEFINE_BROOKDALE_MACHINE(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_brookdale, "pc-brookdale", pc_init, false, NULL, major, minor);

#define DEFINE_BROOKDALE_MACHINE_AS_LATEST(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_brookdale, "pc-brookdale", pc_init, true, "brookdale", major, minor);

static void pc_brookdale_machine_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
    pcmc->pci_enabled = true;
    pcmc->has_acpi_build = false;
    pcmc->smbios_defaults = false;
    pcmc->gigabyte_align = true;
    pcmc->has_reserved_memory = true;
    pcmc->enforce_amd_1tb_hole = false;
    pcmc->isa_bios_alias = false;
    pcmc->pvh_enabled = true;
    pcmc->kvmclock_create_always = true;

    m->family = "pc_brookdale";
    m->desc = "Standard PC (i845 + ICH2, 2001)";
    m->hotplug_allowed = false;
    m->auto_enable_numa_with_memhp = false;
    m->auto_enable_numa_with_memdev = false;
    m->has_hotpluggable_cpus = true;
    m->default_boot_order = "";
    m->max_cpus = 1;
    m->default_cpu_type = X86_CPU_TYPE_NAME("willamette");
    m->nvdimm_supported = false;
    m->smp_props.dies_supported = false;
    m->smp_props.modules_supported = false;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L1D] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L1I] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L2] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L3] = false;
}

static void pc_brookdale_machine_10_1_options(MachineClass *m)
{
    pc_brookdale_machine_options(m);
}

DEFINE_BROOKDALE_MACHINE_AS_LATEST(10, 1);
