/*
 * ATI Radeon DDR (R100) Extended CRTC
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
#include "trace.h"
#include "vga_regs.h"
#include "r100.h"
#include "r100_regs.h"

uint32_t r100_pll_read(R100State *s)
{
    uint32_t idx = s->clock_cntl_index & 0xFF;

    trace_r100_pll_read(idx, s->pll_regs[idx]);
    return s->pll_regs[idx];
}

void r100_pll_write(R100State *s, uint32_t val)
{
    uint32_t idx = s->clock_cntl_index & 0xFF;

    trace_r100_pll_write(idx, val);
    s->pll_regs[idx] = val;
}

uint32_t r100_crtc_gen_cntl_read(R100State *s)
{
    return s->crtc_gen_cntl;
}

void r100_crtc_gen_cntl_write(R100State *s, uint32_t val)
{
    s->crtc_gen_cntl = val;
}

/*
 * Extended CRTC Mode Handler
 */
void r100_update_mode(R100State *s)
{
    VGACommonState *vga = &s->vga;
    uint32_t gen_cntl = r100_crtc_gen_cntl_read(s);
    uint32_t hdd = s->regs[R100_CRTC_H_TOTAL_DISP >> 2];
    uint32_t vdd = s->regs[R100_CRTC_V_TOTAL_DISP >> 2];
    uint32_t pitch_chars = s->regs[R100_CRTC_PITCH >> 2];
    uint32_t start = s->regs[R100_CRTC_START >> 2];
    int h, v, bpp;

    switch (gen_cntl & R100_CRTC_PIX_WIDTH_MASK) {
    case 0x100:
        bpp = 4;
        break;
    case 0x200:
        bpp = 8;
        break;
    case 0x300:
        bpp = 15;
        break;
    case 0x400:
        bpp = 16;
        break;
    case 0x500:
        bpp = 24;
        break;
    case 0x600:
        bpp = 32;
        break;
    default:
        if (vga->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED) {
            vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(vga, 0, VBE_DISPI_DISABLED);
        }
        return;
    }

    h = (((hdd >> 16) & 0xFF) + 1) * 8;
    v = ((vdd >> 16) & 0x3FF) + 1;

    if (!h || !v || !pitch_chars) {
        return;
    }

    if ((vga->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED) &&
        vga->vbe_regs[VBE_DISPI_INDEX_XRES] == h &&
        vga->vbe_regs[VBE_DISPI_INDEX_YRES] == v &&
        vga->vbe_regs[VBE_DISPI_INDEX_BPP] == bpp) {
        return;
    }

    vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
    vbe_ioport_write_data(vga, 0, VBE_DISPI_DISABLED);

    vga->vbe_regs[VBE_DISPI_INDEX_XRES] = h;
    vga->vbe_regs[VBE_DISPI_INDEX_YRES] = v;
    vga->vbe_regs[VBE_DISPI_INDEX_BPP] = bpp;
    vga->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = pitch_chars * 8;
    vga->vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
    vga->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;

    vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
    vbe_ioport_write_data(vga, 0, VBE_DISPI_ENABLED);

    vga->ar_index |= 0x20;
    vga->sr_vbe[VGA_SEQ_CLOCK_MODE] &= ~VGA_SR01_SCREEN_OFF;

    if (start < s->vga.vram_size) {
        vga->vbe_start_addr = start / 4;
    }

    trace_r100_mode_set(h, v, bpp, pitch_chars * 8, start);
}

uint32_t r100_dac_cntl_read(R100State *s)
{
    return s->dac_cntl;
}

void r100_dac_cntl_write(R100State *s, uint32_t val)
{
    s->dac_cntl = val;
}

uint8_t r100_io_gate_read(R100State *s)
{
    /*
     * Bit 0x80 is forced on every read regardless of stored content. If
     * this bit reads as 0 the VBIOS silently skips all >16-color SVGA
     * mode setup while still reporting mode-set success.
     */
    uint8_t val = s->io_bar_gate | R100_IO_GATE_BIT;

    trace_r100_io_gate_read(val);
    return val;
}

void r100_io_gate_write(R100State *s, uint32_t val)
{
    trace_r100_io_gate_write((uint8_t)val);
    s->io_bar_gate = val;
}
