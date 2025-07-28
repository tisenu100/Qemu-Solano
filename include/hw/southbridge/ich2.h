/*
 * Intel Corporation 82801BA ISA Bridge
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2018 Hervé Poussineau
 * Copyright (c) 2025 Tisenu100
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_SOUTHBRIDGE_ICH2_H
#define HW_SOUTHBRIDGE_ICH2_H

#include "hw/pci/pci_device.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/ich9_tco.h"
#include "hw/i2c/pm_smbus.h"
#include "hw/intc/ioapic.h"
#include "hw/isa/apm.h"
#include "hw/rtc/mc146818rtc.h"

struct ICH2State {
    PCIDevice dev;

    uint64_t pic_levels;

    qemu_irq cpu_intr;
    qemu_irq isa_irqs_in[IOAPIC_NUM_PINS];
    int32_t pci_irq_levels_vmstate[8];

    MC146818RtcState rtc;

    uint8_t rcr;
    MemoryRegion rcr_mem;

    ACPIREGS ar;
    APMState apm;
    MemoryRegion acpi_io;
    uint8_t smi[8];
    MemoryRegion gpio_io;
    MemoryRegion smi_io;
    TCOIORegs tco;
    qemu_irq sci_irq;
    qemu_irq smi_irq;
};

#define TYPE_ICH2_PCI_DEVICE "ich2"
OBJECT_DECLARE_SIMPLE_TYPE(ICH2State, ICH2_PCI_DEVICE)

#define TYPE_ICH2_IDE_PCI_DEVICE "ich2-ide"

struct ICH2SMBState {
    PCIDevice dev;

    PMSMBus smb;
};

#define TYPE_ICH2_SMBUS_PCI_DEVICE "ich2-smbus"
OBJECT_DECLARE_SIMPLE_TYPE(ICH2SMBState, ICH2_SMBUS_PCI_DEVICE)

#endif
