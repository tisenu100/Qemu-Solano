/*
 * Intel 815 Chipset Host Bridge and Memory Controller Hub (Solano)
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2025 Tisenu100
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_PCI_I815E_H
#define HW_PCI_I815E_H

#include "hw/pci/pci_device.h"
#include "hw/pci-host/pam.h"
#include "qom/object.h"

#define I815E_HOST_PROP_PCI_TYPE "pci-type"

#define TYPE_I815E_PCI_HOST_BRIDGE "i815e-pcihost"
#define TYPE_I815E_PCI_DEVICE "i815e"

OBJECT_DECLARE_SIMPLE_TYPE(PCII815EState, I815E_PCI_DEVICE)

struct PCII815EState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    PAMMemoryRegion pam_regions[PAM_REGIONS_COUNT];
    MemoryRegion smram_region;
    MemoryRegion smram, low_smram, smbase;
};

#endif
