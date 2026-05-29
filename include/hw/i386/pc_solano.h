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

#ifndef HW_PC_SOLANO_H
#define HW_PC_SOLANO_H

#include "qemu/osdep.h"
#include CONFIG_DEVICES

#include "qemu/units.h"
#include "hw/i386/pc.h"
#include "hw/isa/superio.h"
#include "hw/pci/pci.h"
#include "hw/i2c/smbus_eeprom.h"
#include "system/memory.h"

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
                    void(*sio_create)(ISABus *bus)); /* Super I/O                                */

void pc_solano_common_machine_options(MachineClass *m);

#endif
