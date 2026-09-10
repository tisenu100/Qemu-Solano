/*
 * ATI Radeon DDR (R100)
 *
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
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "r100.h"
#include "r100_regs.h"

static void r100_realize(PCIDevice *dev, Error **errp)
{
    R100State *s = R100(dev);

    /* 32 and 64 are the only sizes the VBIOS's own RAM-autodetect table offers */
    if (s->vram_size_mb != 32 && s->vram_size_mb != 64) {
        s->vram_size_mb = 64;
    }

    r100_mmio_init(s, OBJECT(s));
    r100_vga_init(s, dev, errp);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vga.vram);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io_bar);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void r100_reset(DeviceState *dev)
{
    R100State *s = R100(dev);
    uint32_t bios_scratch[(R100_BIOS_SCRATCH_END - R100_BIOS_SCRATCH_BASE)
                          >> 2];

    trace_r100_reset();

    memcpy(bios_scratch, &s->regs[R100_BIOS_SCRATCH_BASE >> 2],
           sizeof(bios_scratch));

    vga_common_reset(&s->vga);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->pll_regs, 0, sizeof(s->pll_regs));
    s->mm_index = 0;
    s->clock_cntl_index = 0;
    s->crtc_gen_cntl = 0;
    s->dac_cntl = 0;

    s->pll_regs[0x2A] = 0x3C;
    s->pll_regs[0x4A] = 0x3C;

    memcpy(&s->regs[R100_BIOS_SCRATCH_BASE >> 2], bios_scratch,
           sizeof(bios_scratch));

    /*
     * The gate byte must be correct immediately after reset, before POST
     * even runs: bit 0x80 set.
     */
    s->io_bar_gate = R100_IO_GATE_BIT;
}

static const Property r100_properties[] = {
    DEFINE_PROP_UINT32("vram_size_mb", R100State, vram_size_mb, 64),
};

static void r100_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, r100_reset);
    device_class_set_props(dc, r100_properties);
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    k->vendor_id = PCI_VENDOR_ID_ATI;
    k->device_id = PCI_DEVICE_ID_RADEON_DDR;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->realize = r100_realize;
}

static const TypeInfo r100_type_info = {
    .name = TYPE_R100,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(R100State),
    .class_init = r100_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void r100_register_types(void)
{
    type_register_static(&r100_type_info);
}

type_init(r100_register_types)
