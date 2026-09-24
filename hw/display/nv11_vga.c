/*
 * Nvidia Geforce2 MX400 (NV11B) VGA Backend Handlers
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
#include "qemu/bswap.h"
#include "hw/display/vga.h"
#include "vga_regs.h"
#include "nv11.h"
#include "trace.h"

int nv11_get_bpp(VGACommonState *s)
{
    NV11State *n = container_of(s, NV11State, vga);

    switch (n->nv_crtc_reg[0x28] & 0x03) {
    case 1:
        return 8;
    case 2:
        return 16;
    case 3:
        return 32;
    default:
        return 0;
    }
}

void nv11_get_params(VGACommonState *s, VGADisplayParams *params)
{
    NV11State *n = container_of(s, NV11State, vga);
    int bpp, width;

    bpp = nv11_get_bpp(s);
    if (bpp) {
        /* NV Desktop Mode: the scanout pitch lives in CR13 + extensions
         * (CR19[7:5] = pitch/8 bits 10:8, CR42[6] = pitch/8 bit 11).
         * It is *not* width*bpp/8: the framebuffer may be wider than
         * the scanout (e.g. 1920-byte pitch showing 800px), so delivering
         * pitch from width skews every line on width changes.
         * Base is PCRTC START (byte address), not VGA CR0C/0D. */
        uint32_t pitch8, pcrtc_start;

        width = (s->cr[VGA_CRTC_H_DISP] + 1) * 8;
        if (n->nv_crtc_reg[0x2D] & 0x02) {
            width += 0x100 * 8;
        }
        pitch8 = (uint32_t)s->cr[VGA_CRTC_OFFSET] |
                 (((uint32_t)n->nv_crtc_reg[0x19] >> 5 & 0x7) << 8) |
                 (((uint32_t)n->nv_crtc_reg[0x42] >> 6 & 0x1) << 11);
        if (pitch8 == 0 ||
            pitch8 * 8u < (uint32_t)(width * bpp) / 8u) {
            /* Not programmed yet: fall back. */
            params->line_offset = (width * bpp) / 8;
        } else {
            params->line_offset = pitch8 * 8u;
        }
        pcrtc_start =
            ldl_le_p(n->bar0_flat + NV11_PCRTC0_OFF + 0x800);
        if (pcrtc_start + 4u <= (uint32_t)s->vram_size) {
            params->start_addr = pcrtc_start >> 2;
        } else {
            params->start_addr = s->cr[VGA_CRTC_START_LO] |
                (s->cr[VGA_CRTC_START_HI] << 8);
        }
        params->line_compare = 0xffff;
        params->hpel = 8; /* VGA_HPEL_NEUTRAL */
        params->hpel_split = false;
    } else {
        /* Standard VGA semantics (BIOS text and planar modes). */
        params->line_offset = s->cr[VGA_CRTC_OFFSET] << 3;
        params->start_addr = s->cr[VGA_CRTC_START_LO] |
            (s->cr[VGA_CRTC_START_HI] << 8);
        params->line_compare = s->cr[VGA_CRTC_LINE_COMPARE] |
            ((s->cr[VGA_CRTC_OVERFLOW] & 0x10) << 4) |
            ((s->cr[VGA_CRTC_MAX_SCAN] & 0x40) << 3);
        params->hpel = s->ar[VGA_ATC_PEL];
        params->hpel_split = s->ar[VGA_ATC_MODE] & 0x20;
    }
}

void nv11_get_resolution(VGACommonState *s, int *pwidth, int *pheight)
{
    NV11State *n = container_of(s, NV11State, vga);
    int width_chars = s->cr[VGA_CRTC_H_DISP] + 1;
    int height = s->cr[VGA_CRTC_V_DISP_END] |
        ((s->cr[VGA_CRTC_OVERFLOW] & 0x02) << 7) |
        ((s->cr[VGA_CRTC_OVERFLOW] & 0x40) << 3);

    /* NV extended overflow:
     * CRTC 0x2D bit1 = horizDisplay bit8, CRTC 0x25 bit1 =
     * vertDisplay bit10, CRTC 0x41 bit2 = vertDisplay bit11.
     * Without these any mode taller than 1024 is truncated. */
    if (n->nv_crtc_reg[0x2D] & 0x02) {
        width_chars += 0x100;
    }
    if (n->nv_crtc_reg[0x25] & 0x02) {
        height |= 0x400;
    }
    if (n->nv_crtc_reg[0x41] & 0x04) {
        height |= 0x800;
    }

    *pwidth = width_chars * 8;
    *pheight = height + 1;
}

static uint32_t nv11_cursor_blend(uint32_t fg, uint32_t bg)
{
    uint32_t a = fg >> 24;
    uint32_t ia = 255 - a;
    uint32_t r, g, b;

    if (a == 0) {
        return bg;
    }
    if (a == 255) {
        return 0xFF000000 | (fg & 0x00FFFFFF);
    }
    r = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * ia) / 255;
    g = (((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * ia) / 255;
    b = ((fg & 0xFF) * a + (bg & 0xFF) * ia) / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* The 64x64 ARGB cursor image is composited directly into the 32-bit shadow
 * surface rows that Qemu's backend rebuilds each frame.
 * Cursor X/Y are signed 16-bit (allows partial offscreen, e.g. 0xFFF9 = -7).
 */
static inline int nv11_cur_x(uint32_t pos)
{
    return (int16_t)(pos & 0xFFFF);
}

static inline int nv11_cur_y(uint32_t pos)
{
    return (int16_t)((pos >> 16) & 0xFFFF);
}

static void nv11_cursor_invalidate_range(VGACommonState *vga, uint32_t pos)
{
    int y = nv11_cur_y(pos);

    if (y < 0) {
        y = 0;
    }
    vga_invalidate_scanlines(vga, y, y + 64);
}

void nv11_cursor_invalidate(VGACommonState *vga)
{
    NV11State *s = container_of(vga, NV11State, vga);

    if (s->last_cur_pos != s->cur_pos ||
        s->last_cur_img != s->cur_img ||
        s->last_cur_enabled != s->cur_enabled) {
        nv11_cursor_invalidate_range(vga, s->last_cur_pos);
        vga->hw_cursor_x = s->cur_pos & 0xFFFF;
        vga->hw_cursor_y = s->cur_pos >> 16;
        s->last_cur_pos = s->cur_pos;
        s->last_cur_img = s->cur_img;
        s->last_cur_enabled = s->cur_enabled;
        if (s->cur_enabled) {
            nv11_cursor_invalidate_range(vga, s->cur_pos);
        }
    }
}

void nv11_cursor_draw_line(VGACommonState *vga, uint8_t *d, int y)
{
    NV11State *s = container_of(vga, NV11State, vga);
    uint32_t *dp;
    uint32_t row;
    int cx, cy, src_x0, dst_x, i;

    if (!s->cur_enabled) {
        return;
    }
    cx = nv11_cur_x(s->cur_pos);
    cy = nv11_cur_y(s->cur_pos);
    if (y < cy || y >= cy + 64) {
        return;
    }
    src_x0 = 0;
    dst_x = cx;
    if (dst_x < 0) {
        src_x0 = -dst_x;
        dst_x = 0;
    }
    if (dst_x >= vga->last_scr_width) {
        return;
    }
    row = s->cur_img + (uint32_t)(y - cy) * 256 + (uint32_t)src_x0 * 4;
    if (row + 256 > vga->vram_size) {
        return;
    }
    dp = (uint32_t *)d + dst_x;
    for (i = src_x0; i < 64; i++) {
        uint32_t px;
        int dst_i = dst_x + (i - src_x0);

        if (dst_i >= vga->last_scr_width) {
            break;
        }
        px = ldl_le_p(vga->vram_ptr + row + (uint32_t)(i - src_x0) * 4);
        if ((px >> 24) == 0) {
            continue;   /* Fully transparent */
        }
        dp[i - src_x0] = nv11_cursor_blend(px, dp[i - src_x0]);
    }
}

static uint32_t nv11_vga_ioport_read(void *opaque, uint32_t addr)
{
    NV11State *s = NV11(opaque);
    uint32_t eip = nv11_get_eip();
    uint32_t val;

    if (addr == 0x3B5 || addr == 0x3D5) {
        val = nv11_pcrtc_read(s, s->vga.cr_index);
    } else {
        val = vga_ioport_read(&s->vga, addr);
    }

    trace_nv11_vga_read(eip, addr, 1, val);
    return val;
}

static void nv11_vga_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    NV11State *s = NV11(opaque);
    uint32_t eip = nv11_get_eip();

    trace_nv11_vga_write(eip, addr, 1, val);

    if (addr == 0x3B5 || addr == 0x3D5) {
        nv11_pcrtc_write(s, s->vga.cr_index, (uint8_t)val);
    } else {
        vga_ioport_write(&s->vga, addr, val);
    }
}

/* Mirrored from Qemu's standard VGA code. So we can actually have control over the registers */
static const MemoryRegionPortio nv11_vga_portio_list[] = {
    { 0x04,  2, 1, .read = nv11_vga_ioport_read,
                    .write = nv11_vga_ioport_write }, /* 3b4-3b5 */
    { 0x0a,  1, 1, .read = nv11_vga_ioport_read,
                    .write = nv11_vga_ioport_write }, /* 3ba */
    { 0x10, 16, 1, .read = nv11_vga_ioport_read,
                    .write = nv11_vga_ioport_write }, /* 3c0-3cf */
    { 0x24,  2, 1, .read = nv11_vga_ioport_read,
                    .write = nv11_vga_ioport_write }, /* 3d4-3d5 */
    { 0x2a,  1, 1, .read = nv11_vga_ioport_read,
                    .write = nv11_vga_ioport_write }, /* 3da */
    PORTIO_END_OF_LIST(),
};

void nv11_vga_init(NV11State *s, PCIDevice *dev, Error **errp)
{
    portio_list_init(&s->vga.vga_port_list, OBJECT(dev), nv11_vga_portio_list,
                     s, "nv11-vga");
    portio_list_set_flush_coalesced(&s->vga.vga_port_list);
    portio_list_add(&s->vga.vga_port_list, pci_address_space_io(dev), 0x3B0);

    trace_nv11_vga_init();
}

static void nv11_vga_reset(DeviceState *dev)
{
    NV11State *s = NV11(dev);
    vga_common_reset(&s->vga);
    nv11_pramdac_reset(s);
    nv11_pgraph_reset(s);
    nv11_fifo_reset(s);
    nv11_i2c_reset(s);
    /* Clear PCRTC VBLANK latches so a stale retrace does not fake the
     * next mode probe; the 60Hz timer re-sets them. */
    stl_le_p(s->bar0_flat + NV11_PCRTC0_OFF + NV11_PCRTC_INTR, 0);
    stl_le_p(s->bar0_flat + NV11_PCRTC1_OFF + NV11_PCRTC_INTR, 0);
    nv11_update_irq(s);
}

void nv11_vga_class_reset(ObjectClass *klass)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, nv11_vga_reset);
}