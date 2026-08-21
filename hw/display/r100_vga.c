/*
 * ATI Radeon DDR (R100) VGA Backend Handlers
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
#include "ui/console.h"
#include "qemu/log.h"
#include "vga_regs.h"
#include "r100.h"

/* Radeon specific BPP handler. It's very crunchy for now */
static int r100_get_bpp(VGACommonState *vga)
{
    int shift_control = (vga->gr[VGA_GFX_MODE] >> 5) & 3;
    int char_width, bytes_per_pixel;

    if (shift_control != 2) {
        return 0;
    }

    char_width = vga->cr[VGA_CRTC_H_DISP] + 1;
    if (char_width == 0) {
        return 8;
    }

    /*
    *  Dirty hack to figure coloring because the VBIOS don't really set anything specific inside
    *  the SVGA tables.
    */
    bytes_per_pixel = (vga->cr[VGA_CRTC_OFFSET] + char_width / 2) / char_width;

    switch (bytes_per_pixel) {
    case 1:
        return 8;
    case 2:
        return 16;
    case 3:
        return 24;
    case 4:
        return 32;
    default:
        /* Fallback to 8-bit instant if unsupported */
        return 8;
    }
}

void r100_vga_init(R100State *s, PCIDevice *dev, Error **errp)
{
    VGACommonState *vga = &s->vga;

    vga->vram_size_mb = s->vram_size_mb;

    if (!vga_common_init(vga, OBJECT(s), errp)) {
        return;
    }
    vga_init(vga, OBJECT(s), pci_address_space(dev),
             pci_address_space_io(dev), true);

    vga->get_bpp = r100_get_bpp;

    vga->con = qemu_graphic_console_create(DEVICE(s), 0, vga->hw_ops, vga);
}
