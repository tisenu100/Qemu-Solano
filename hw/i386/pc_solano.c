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
#include "hw/pci-host/solano.h"
#include "hw/i2c/smbus_eeprom.h"
#include "system/memory.h"
#include "migration/global_state.h"
#include "migration/misc.h"
#include "system/runstate.h"
#include "target/i386/cpu.h"

/* Board IRQ table used by the AOpen AX3S Pro */
/* To add a device: -device rtl8139,bus=pci.2,addr=04.0 will place an RTL8139 on Slot 1 */
static int pci_slots_get_pirq(PCIDevice *pci_dev, int pin)
{
    int ret = 0;

    switch (PCI_SLOT(pci_dev->devfn)) {
        case 0x04:
            ret = (0x5432 >> (pin * 4)) & 7;
        break;

        case 0x05:
            ret = (0x6543 >> (pin * 4)) & 7;
        break;

        case 0x07:
            ret = (0x1765 >> (pin * 4)) & 7;
        break;

        case 0x08:
            ret = (0x7654 >> (pin * 4)) & 7;
        break;

        case 0x09:
            ret = (0x1076 >> (pin * 4)) & 7;
        break;

        case 0x0a:
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
        Solano Board:

        Chipset: Intel 815EP (Solano-3)
        Southbridge: Intel ICH2

        Notes:

        Memory: The Intel 815 chipset can assign 512MB maximum
    
        Top of Memory

        As per Intel 815EP datasheet:
        The Top of memory is limited to 512 MB. All accesses to addresses within this range will be
        forwarded to the DRAM unless a hole in this range is created.
    
    */

    pc_solano_init(machine, TYPE_I815E_PCI_HOST_BRIDGE, TYPE_I815E_PCI_DEVICE, pci_slots_get_pirq, PCI_DEVICE_ID_INTEL_I815E_AGP, 32 * MiB, 512 * MiB, 512 * MiB, 0x414c, 0x4730, SDR, w83627hf_create);
}

#define DEFINE_SOLANO_MACHINE(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_solano, "pc-solano", pc_init, false, NULL, major, minor);

#define DEFINE_SOLANO_MACHINE_AS_LATEST(major, minor) \
    DEFINE_PC_VER_MACHINE(pc_solano, "pc-solano", pc_init, true, "solano", major, minor);

static void pc_solano_machine_options(MachineClass *m)
{
    pc_solano_common_machine_options(m);

    m->family = "pc_solano";
    m->desc = "Standard PC (i815E + ICH2, 2000)";
    m->max_cpus = 1;
    m->default_cpu_type = X86_CPU_TYPE_NAME("pentium3");
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L1D] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L1I] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L2] = true;
    m->smp_props.cache_supported[CACHE_LEVEL_AND_TYPE_L3] = false;
}

static void pc_solano_machine_11_0_options(MachineClass *m)
{
    pc_solano_machine_options(m);
}

DEFINE_SOLANO_MACHINE_AS_LATEST(11, 0);
