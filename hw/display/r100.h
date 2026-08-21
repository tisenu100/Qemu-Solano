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

#ifndef HW_DISPLAY_R100_H
#define HW_DISPLAY_R100_H

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/display/vga_int.h"
#include "qom/object.h"
#include "qapi/error.h"

#define TYPE_R100 "r100"
OBJECT_DECLARE_SIMPLE_TYPE(R100State, R100)

#define R100_PLL_REG_COUNT 64
#define R100_MMIO_SIZE     0x4000
#define R100_IO_BAR_SIZE   0x100

/* Must move to pci_ids.h */
#define PCI_VENDOR_ID_ATI             0x1002
#define PCI_DEVICE_ID_RADEON_DDR      0x5144

typedef struct R100State {
    PCIDevice parent_obj;

    VGACommonState vga;

    MemoryRegion mmio;
    MemoryRegion io_bar;
    uint32_t regs[R100_MMIO_SIZE / 4];
    uint32_t mm_index;

    uint32_t clock_cntl_index;
    uint32_t pll_regs[R100_PLL_REG_COUNT];
    uint32_t crtc_gen_cntl;
    uint32_t dac_cntl;
    uint8_t  io_bar_gate;

    uint32_t vram_size_mb;
} R100State;

uint32_t r100_pll_read(R100State *s);
void     r100_pll_write(R100State *s, uint32_t val);
uint32_t r100_crtc_gen_cntl_read(R100State *s);
void     r100_crtc_gen_cntl_write(R100State *s, uint32_t val);
uint32_t r100_dac_cntl_read(R100State *s);
void     r100_dac_cntl_write(R100State *s, uint32_t val);
uint8_t  r100_io_gate_read(R100State *s);
void     r100_io_gate_write(R100State *s, uint32_t val);
void r100_mmio_init(R100State *s, Object *owner);
void r100_vga_init(R100State *s, PCIDevice *dev, Error **errp);

#endif
