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

    switch (n->nv_crtc_reg[0x28]) {
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
    int bpp, width;

    bpp = nv11_get_bpp(s);
    if (bpp) {
        /* NV Desktop Mode: the scanout pitch is the width*depth product
         * (matches state->repaint0 in the X driver) and the base address
         * is the standard VGA display start. */
        width = (s->cr[VGA_CRTC_H_DISP] + 1) * 8;
        params->line_offset = (width * bpp) / 8;
        params->start_addr = s->cr[VGA_CRTC_START_LO] |
            (s->cr[VGA_CRTC_START_HI] << 8);
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
    *pwidth = (s->cr[VGA_CRTC_H_DISP] + 1) * 8;
    *pheight = s->cr[VGA_CRTC_V_DISP_END] |
        ((s->cr[VGA_CRTC_OVERFLOW] & 0x02) << 7) |
        ((s->cr[VGA_CRTC_OVERFLOW] & 0x40) << 3);
    *pheight = (*pheight + 1);
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
 * surface rows that Qemu's backend rebuilds each frame. */
void nv11_cursor_invalidate(VGACommonState *vga)
{
    NV11State *s = container_of(vga, NV11State, vga);

    if (s->last_cur_pos != s->cur_pos ||
        s->last_cur_img != s->cur_img ||
        s->last_cur_enabled != s->cur_enabled) {
        vga_invalidate_scanlines(vga, vga->hw_cursor_y,
                                 vga->hw_cursor_y + 64);
        vga->hw_cursor_x = s->cur_pos & 0xFFFF;
        vga->hw_cursor_y = s->cur_pos >> 16;
        s->last_cur_pos = s->cur_pos;
        s->last_cur_img = s->cur_img;
        s->last_cur_enabled = s->cur_enabled;
        if (s->cur_enabled) {
            vga_invalidate_scanlines(vga, vga->hw_cursor_y,
                                     vga->hw_cursor_y + 64);
        }
    }
}

void nv11_cursor_draw_line(VGACommonState *vga, uint8_t *d, int y)
{
    NV11State *s = container_of(vga, NV11State, vga);
    uint32_t *dp;
    uint32_t row;
    int cx, cy, i;

    if (!s->cur_enabled) {
        return;
    }
    cy = s->cur_pos >> 16;
    if (y < cy || y >= cy + 64) {
        return;
    }
    row = s->cur_img + (uint32_t)(y - cy) * 256;
    if (row + 256 > vga->vram_size) {
        return;
    }
    cx = s->cur_pos & 0xFFFF;
    if (cx >= vga->last_scr_width) {
        return;
    }
    dp = (uint32_t *)d + cx;
    for (i = 0; i < 64; i++) {
        uint32_t px;

        if (cx + i >= vga->last_scr_width) {
            break;
        }
        px = ldl_le_p(vga->vram_ptr + row + i * 4);
        if ((px >> 24) == 0) {
            continue;   /* Fully transparent */
        }
        dp[i] = nv11_cursor_blend(px, dp[i]);
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
    nv11_pgraph_reset(s);
    nv11_fifo_reset(s);
}

void nv11_vga_class_reset(ObjectClass *klass)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, nv11_vga_reset);
}