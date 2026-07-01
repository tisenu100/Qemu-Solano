/*
 * QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2026 Tisenu100
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
#include "hw/block/sst_lpc.h"
#include "hw/dma/i8257.h"
#include "hw/timer/i8254.h"
#include "hw/i386/x86.h"
#include "hw/i386/pc.h"
#include "hw/i386/pc_solano.h"
#include "hw/i386/apic.h"
#include "hw/intc/i8259.h"
#include "hw/isa/superio.h"
#include "hw/pci/pci_bridge.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/southbridge/ich2.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/usb/usb.h"
#include "hw/ide/pci.h"
#include "hw/core/irq.h"
#include "system/kvm.h"
#include "kvm/kvm_i386.h"
#include "hw/i386/kvm/clock.h"
#include "hw/core/sysbus.h"
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

/* Hub */
static int hub_get_pirq(PCIDevice *pci_dev, int pin)
{
    if(PCI_SLOT(pci_dev->devfn) == 0x1f)
        return (0x3710 >> (pin * 4)) & 7;
    else
        return (0x3210 >> (pin * 4)) & 7;
}

/* Dummy. Qemu has no AGP emulation */
static int agp_slot_get_pirq(PCIDevice *pci_dev, int pin)
{
    return (0x3210 >> (pin * 4)) & 7;
}

void pc_solano_init(MachineState *machine,                                                           \
                    const char   *pci_host,          /* Northbridge PCI Host                     */  \
                    const char   *pci_dev,           /* Northbridge PCI Device                   */  \
                    pci_map_irq_fn board_slots,      /* PCI slots assigned usually by the board  */  \
                    uint16_t agp_bridge_dev_id,      /* The AGP Bridge Device ID                 */  \
                    int min_assignable_memory,       /* Smallest mem size allowed by the chipset */  \
                    int max_assignable_memory,       /* Maximum mem size allowed by the chipset  */  \
                    ram_addr_t tom_config_lowmem,    /* TOM configuration                        */  \
                    uint16_t ac97_vendor,            /* AC97 Mixer Vendor                        */  \
                    uint16_t ac97_device,            /* AC97 Mixer Device                        */  \
                    enum sdram_type ram_arch,        /* RAM architecture                         */  \
                    void(*sio_create)(ISABus *bus))  /* Super I/O                                */
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
    qemu_irq *i8259;
    ISADevice *i8254;
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

    DeviceState *sst_flash;
    SSTState *sst;

    MemoryRegion *ram_memory = machine->ram;
    MemoryRegion *pci_memory = NULL;
    uint64_t hole64_size = 0;

    fprintf(stderr, "PC: Setting up\n");
    if (!pcms->max_ram_below_4g) {
        pcms->max_ram_below_4g = 4 * GiB;
    }

    if((machine->ram_size < min_assignable_memory) || (machine->ram_size > max_assignable_memory)) {
        error_printf("FATAL! Assigning memory %s %dMB\n", (machine->ram_size > max_assignable_memory) ? "beyond" : "below", \
                                                          (int)(((machine->ram_size > max_assignable_memory) ? max_assignable_memory : min_assignable_memory) >> 10));
        exit(EXIT_FAILURE);
    }

    if (machine->ram_size >= tom_config_lowmem) {
        x86ms->above_4g_mem_size = machine->ram_size - tom_config_lowmem;
        x86ms->below_4g_mem_size = tom_config_lowmem;
    } else {
        x86ms->above_4g_mem_size = 0;
        x86ms->below_4g_mem_size = machine->ram_size;
    }

    x86_cpus_init(x86ms, pcmc->default_cpu_version);

    if (kvm_enabled()) {
        kvmclock_create(pcmc->kvmclock_create_always);
    }

    fprintf(stderr, "PC: Starting the PCI Host\n");
    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);

    phb = OBJECT(qdev_new(pci_host));
    object_property_add_child(OBJECT(machine), pci_dev, phb);
    object_property_set_link(phb, PCI_HOST_PROP_RAM_MEM, OBJECT(ram_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_PCI_MEM, OBJECT(pci_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_SYSTEM_MEM, OBJECT(system_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_IO_MEM, OBJECT(system_io), &error_fatal);
    object_property_set_uint(phb, PCI_HOST_BELOW_4G_MEM_SIZE, x86ms->below_4g_mem_size, &error_fatal);
    object_property_set_uint(phb, PCI_HOST_ABOVE_4G_MEM_SIZE, x86ms->above_4g_mem_size, &error_fatal);
    object_property_set_str(phb, "pci-type", pci_dev, &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(phb), &error_fatal);

    pcms->pcibus = PCI_BUS(qdev_get_child_bus(DEVICE(phb), "pci.0"));
    pci_bus_map_irqs(pcms->pcibus, hub_get_pirq);

    hole64_size = object_property_get_uint(phb, PCI_HOST_PROP_PCI_HOLE64_SIZE, &error_abort);

    pc_memory_init(pcms, system_memory, pci_memory, hole64_size);

    gsi_state = pc_gsi_create(&x86ms->gsi, pcmc->pci_enabled);

    fprintf(stderr, "PC: Setting up the LPC Bridge\n");
    lpc_pci_dev = pci_new_multifunction(PCI_DEVFN(0x1f, 0), TYPE_ICH2_PCI_DEVICE);
    lpc_dev = DEVICE(lpc_pci_dev);
    for (int i = 0; i < IOAPIC_NUM_PINS; i++) {
        qdev_connect_gpio_out_named(lpc_dev, "isa-irqs", i, x86ms->gsi[i]);
    }
    pci_realize_and_unref(lpc_pci_dev, pcms->pcibus, &error_fatal);

    isa_bus = ISA_BUS(qdev_get_child_bus(lpc_dev, "isa.0"));

    i8257_dma_init(OBJECT(lpc_dev), isa_bus, 1);
    rtc = mc146818_rtc_init(isa_bus, 2000, NULL);
    x86ms->rtc = ISA_DEVICE(rtc);
    smi_irq = qemu_allocate_irq(pc_acpi_smi_interrupt, first_cpu, 0);
    qdev_connect_gpio_out_named(lpc_dev, "smi-irq", 0, smi_irq);

    if (tcg_enabled()) {
        x86_register_ferr_irq(x86ms->gsi[13]);
    }

    fprintf(stderr, "PC: Setting up the Super I/O\n");
    pc_basic_device_init_simple(pcms, isa_bus, x86ms->gsi);
    sio_create(isa_bus); /* The Super I/O init function. Referenced from the board setup */
    
    fprintf(stderr, "PC: Setting up IDE\n");
    ide_pci_dev = pci_create_simple(pcms->pcibus, PCI_DEVFN(0x1f, 1), TYPE_ICH2_IDE_PCI_DEVICE);
    qdev_connect_gpio_out_named(DEVICE(ide_pci_dev), "isa-irq", 0, x86ms->gsi[14]);
    qdev_connect_gpio_out_named(DEVICE(ide_pci_dev), "isa-irq", 1, x86ms->gsi[15]);
    pci_ide_create_devs(ide_pci_dev);
    pcms->idebus[0] = qdev_get_child_bus(DEVICE(ide_pci_dev), "ide.0");
    pcms->idebus[1] = qdev_get_child_bus(DEVICE(ide_pci_dev), "ide.1");
    
    fprintf(stderr, "PC: Setting up the SMBus\n");
    smb_pci_dev = pci_create_simple(pcms->pcibus, PCI_DEVFN(0x1f, 3), TYPE_ICH2_SMBUS_PCI_DEVICE);
    smb_dev = DEVICE(smb_pci_dev);

    pcms->smbus = I2C_BUS(qdev_get_child_bus(smb_dev, "i2c"));
    uint8_t *spd = spd_data_generate(ram_arch, machine->ram_size);

    smbus_eeprom_init_one(pcms->smbus, 0x50, spd);

    fprintf(stderr, "PC: Setting up Bridges\n");
    agp_bridge_dev = pci_new(PCI_DEVFN(0x01, 0), "i82801b11-bridge");
    agp_bridge = PCI_BRIDGE(agp_bridge_dev);
    pci_bridge_map_irq(agp_bridge, "pci.1", agp_slot_get_pirq);
    pci_realize_and_unref(agp_bridge_dev, pcms->pcibus, &error_fatal);

    /* Update Bridge vendor to match the AGP bridge */
    pci_set_word(agp_bridge_dev->config + 0x02, agp_bridge_dev_id);
    pci_set_byte(agp_bridge_dev->config + 0x04, 0x01);

    pci_bridge_dev = pci_new(PCI_DEVFN(0x1e, 0), "i82801b11-bridge");
    pci_bridge = PCI_BRIDGE(pci_bridge_dev);
    pci_bridge_map_irq(pci_bridge, "pci.2", board_slots);
    pci_realize_and_unref(pci_bridge_dev, pcms->pcibus, &error_fatal);

    /* Update Bridge vendor to match the PCI bridge */
    pci_set_word(pci_bridge_dev->config + 0x02, PCI_DEVICE_ID_INTEL_ICH2_PCI);
    pci_set_byte(pci_bridge_dev->config + 0x04, 0x01);

    fprintf(stderr, "PC: Setting up USB\n");
    pci_create_simple(pcms->pcibus, PCI_DEVFN(0x1f, 2), TYPE_ICH2_USB_UHCI1);
    pci_create_simple(pcms->pcibus, PCI_DEVFN(0x1f, 4), TYPE_ICH2_USB_UHCI2);

    fprintf(stderr, "PC: Setting up AC97\n");
    ac97 = pci_new(PCI_DEVFN(0x1f, 5), "AC97");

    qdev_prop_set_uint16(DEVICE(ac97), "ac97-vendor", ac97_vendor);
    qdev_prop_set_uint16(DEVICE(ac97), "ac97-device", ac97_device);

    pci_realize_and_unref(ac97, pcms->pcibus, &error_fatal);

    fprintf(stderr, "PC: Setting up Flash\n");
    sst_flash = qdev_new(TYPE_SST_LPC);
    sst = SST_LPC(sst_flash);
    sst_mount_flash(sst, pcms->flash[0]);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sst_flash), &error_fatal);

    fprintf(stderr, "PC: Setting up timers\n");
    if(kvm_enabled()) 
        i8254 = kvm_pit_init(isa_bus, 0x40); /* KVM 8254 PIT */
    else
        i8254 = i8254_pit_init(isa_bus, 0x40, 0, NULL); /* Legacy 8254 PIT */

    object_property_set_link(OBJECT(pcms->pcspk), "pit", OBJECT(i8254), &error_fatal);
    isa_realize_and_unref(pcms->pcspk, isa_bus, &error_fatal);
    ioapic_init_gsi(gsi_state, phb);

    fprintf(stderr, "PC: Setting up interrupts\n");
    i8259 = i8259_init(isa_bus, x86_allocate_cpu_irq());
    for (int i = 0; i < ISA_NUM_IRQS; i++) {
        gsi_state->i8259_irq[i] = i8259[i];
    }
    g_free(i8259);

    fprintf(stderr, "PC: Starting up VGA\n");
    pc_vga_init(isa_bus, pcms->pcibus);

    fprintf(stderr, "PC: Passing control to the BIOS\n");
}

void pc_solano_common_machine_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
    pcmc->pci_enabled = true;
    pcmc->has_acpi_build = false;
    pcmc->smbios_defaults = false;
    pcmc->gigabyte_align = true;
    pcmc->has_reserved_memory = true;
    pcmc->enforce_amd_1tb_hole = false;
    pcmc->isa_bios_alias = false;
    pcmc->kvmclock_create_always = true;

    m->hotplug_allowed = false;
    m->auto_enable_numa_with_memhp = false;
    m->auto_enable_numa_with_memdev = false;
    m->has_hotpluggable_cpus = true;
    m->default_boot_order = "";
    m->nvdimm_supported = false;
    m->smp_props.dies_supported = false;
    m->smp_props.modules_supported = false;
}

