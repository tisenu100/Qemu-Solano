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
#include "hw/i386/x86.h"
#include "hw/i386/pc.h"
#include "hw/i386/pc_solano.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci-host/brookdale.h"
#include "hw/i2c/smbus_eeprom.h"
#include "system/memory.h"
#include "migration/global_state.h"
#include "migration/misc.h"
#include "system/runstate.h"
#include "target/i386/cpu.h"

/* Board IRQ table used by the ABit AB-BD7 */
/* To add a device: -device rtl8139,bus=pci.2,addr=01.0 will place an RTL8139 on Slot 1 */
static int pci_slots_get_pirq(PCIDevice *pci_dev, int pin)
{
    int ret = 0;

    switch (PCI_SLOT(pci_dev->devfn)) {
        case 0x01: /* Slot 1 */
            ret = (0x0321 >> (pin * 4)) & 7;
        break;

        case 0x02: /* Slot 2 */
            ret = (0x7654 >> (pin * 4)) & 7;
        break;

        case 0x03: /* Slot 3 */
            ret = (0x6547 >> (pin * 4)) & 7;
        break;

        case 0x04: /* Slot 4 */
            ret = (0x5476 >> (pin * 4)) & 7;
        break;
    
        case 0x05: /* Slot 5 */
            ret = (0x4765 >> (pin * 4)) & 7;
        break;

        case 0x06: /* RAID Controller port (Unused) */
            ret = (0x2301 >> (pin * 4)) & 7;
        break;

        case 0x07: /* CNR Port. Better avoid using */
            ret = (0x4531 >> (pin * 4)) & 7;
        break;

        case 0x08: /* Occupied for the internal network controller (Unused) */
            ret = (0x7654 >> (pin * 4)) & 7;
        break;

        default:
            ret = (0x3210 >> (pin * 4)) & 7;
        break;
    }

    return ret;
}

static void pc_init(MachineState *machine)
{

    /*
        Brookdale Board:

        Chipset: Intel 845 (Brookdale DDR)
        Southbridge: Intel ICH2

        Notes:

        Memory: The Intel 845 chipset can assign 3GB maximum

        Top of Memory

        On the Intel 845 it is configured according to the TOM register (C4h)
        400h = 4000_0000

        The ABit board gives this result
        pci_cfg_write i845 00:00.0 @0xc4 <- 0x2000
        
        2000h = 2_0000_0000

        TOM output may be different for each board
    */

    pc_solano_init(machine, TYPE_I845_PCI_HOST_BRIDGE, TYPE_I845_PCI_DEVICE, pci_slots_get_pirq, PCI_DEVICE_ID_INTEL_I845_AGP, 32 * MiB, 3 * GiB, 0x200000000, 0x414c, 0x4710, DDR, w83627hf_create);
}

#define DEFINE_BROOKDALE_MACHINE(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_brookdale, "pc-brookdale", pc_init, false, NULL, major, minor);

#define DEFINE_BROOKDALE_MACHINE_AS_LATEST(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_brookdale, "pc-brookdale", pc_init, true, "brookdale", major, minor);

static void pc_brookdale_machine_options(MachineClass *m)
{
    pc_solano_common_machine_options(m);

    m->family = "pc_brookdale";
    m->desc = "Standard PC (i845 + ICH2, 2001)";
    m->max_cpus = 2; /* 1 CPU + 1 Thread -smp cores=1,threads=1 */
    m->default_cpu_type = X86_CPU_TYPE_NAME("willamette");
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L1D] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L1I] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L2] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L3] = false;
}

static void pc_brookdale_machine_11_0_options(MachineClass *m)
{
    pc_brookdale_machine_options(m);
}

DEFINE_BROOKDALE_MACHINE_AS_LATEST(11, 0);
