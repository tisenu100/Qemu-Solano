/*
 * Nvidia Geforce2 MX400 (NV11B) 2D/D2D engine
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
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "nv11.h"
#include "trace.h"

typedef struct Nv11Surf {
    int32_t w, h;
    int32_t bpp;          /* Bits per pixel (8/16/32) */
    int32_t bpr;          /* Bytes per row (pitch) */
    uint32_t off;         /* Byte offset of surface in vram */
    uint32_t fmt;         /* Format nibble of 0x724 */
} Nv11Surf;

static void nv11_2d_surface(NV11State *s, Nv11Surf *sf)
{
    uint32_t fmt = s->pgraph_scratch[NV11_2D_SURF_FMT / 4] & 0xF;

    switch (fmt) {
    case 0x1: sf->bpp = 8;  break;
    case 0x2:
    case 0x5: sf->bpp = 16; break;
    case 0x7: sf->bpp = 32; break;
    default:  sf->bpp = 16; break;
    }
    sf->fmt = fmt;
    sf->off = s->pgraph_scratch[NV11_2D_SURF_OFF_0 / 4];
    sf->bpr = s->pgraph_scratch[NV11_2D_SURF_PITCH_0 / 4];
    nv11_get_resolution(&s->vga, &sf->w, &sf->h);
    if (sf->w <= 0 || sf->w > 0x4000) {
        sf->w = 1024;
    }
    if (!sf->bpr) {
        sf->bpr = (sf->w * sf->bpp) / 8;
    }
    if (sf->bpr > 0) {
        uint32_t avail = (sf->off < s->vga.vram_size)
                         ? (s->vga.vram_size - sf->off) : 0;
        uint32_t maxr = avail / sf->bpr;

        sf->h = (maxr > 0) ? MIN(maxr, 0x4000) : 768;
    } else {
        sf->h = 768;
    }
}

static inline uint8_t *nv11_2d_pix(NV11State *s, const Nv11Surf *sf,
                                   int32_t x, int32_t y)
{
    uint8_t *fb = memory_region_get_ram_ptr(&s->vga.vram);
    uint32_t b = sf->bpp / 8;
    uint32_t a;

    if (x < 0 || y < 0) {
        return NULL;
    }
    a = sf->off + (uint32_t)y * sf->bpr + (uint32_t)x * b;
    if (a + b > s->vga.vram_size) {
        return NULL;
    }
    return fb + a;
}

static inline uint32_t nv11_2d_load(const uint8_t *p, int32_t bpp)
{
    switch (bpp) {
    case 8:  return *p;
    case 32: return ldl_le_p(p);
    default: return lduw_le_p(p);
    }
}

static inline void nv11_2d_store(uint8_t *p, int32_t bpp, uint32_t v)
{
    switch (bpp) {
    case 8:  *p = (uint8_t)v; break;
    case 32: stl_le_p(p, v); break;
    default: stw_le_p(p, (uint16_t)v); break;
    }
}

static void nv11_2d_dirty(NV11State *s, const Nv11Surf *sf,
                          int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t b = sf->bpp / 8;
    uint32_t off0, off1;

    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    off0 = sf->off + (uint32_t)y0 * sf->bpr + (uint32_t)x0 * b;
    off1 = sf->off + (uint32_t)(y1 - 1) * sf->bpr + (uint32_t)(x1 - 1) * b + b;
    if (off1 > s->vga.vram_size) {
        off1 = s->vga.vram_size;
    }
    if (off0 >= s->vga.vram_size) {
        return;
    }
    memory_region_set_dirty(&s->vga.vram, off0, off1 - off0);
}

static inline uint32_t nv11_2d_rop3(uint32_t rop, uint32_t dst,
                                    uint32_t src, uint32_t pat)
{
    uint32_t r = 0;
    int i;

    for (i = 0; i < 32; i++) {
        int idx = ((dst >> i) & 1)
                | (((src >> i) & 1) << 1)
                | (((pat >> i) & 1) << 2);
        if ((rop >> idx) & 1) {
            r |= (uint32_t)1 << i;
        }
    }
    return r;
}

static bool nv11_2d_rop_uses_pat(uint32_t rop)
{
    int d, s;

    for (d = 0; d < 2; d++) {
        for (s = 0; s < 2; s++) {
            int i0 = d | (s << 1);
            int i1 = d | (s << 1) | 4;
            if (((rop >> i0) & 1) != ((rop >> i1) & 1)) {
                return true;
            }
        }
    }
    return false;
}

static inline int nv11_2d_pattern_bit(NV11State *s, int32_t x, int32_t y)
{
    int bit = ((y & 7) << 3) | (x & 7);
    uint32_t w = (bit < 32) ? s->d2d_pat_mono[0] : s->d2d_pat_mono[1];

    return (w >> (bit & 31)) & 1;
}

static void nv11_2d_fill_rect(NV11State *s, int32_t x, int32_t y,
                              int32_t w, int32_t h)
{
    Nv11Surf sf;
    int32_t x0, y0, x1, y1, px, py;
    uint32_t eip = nv11_get_eip();
    bool use_pat = nv11_2d_rop_uses_pat(s->d2d_rop3);

    if (w <= 0 || h <= 0) {
        return;
    }
    nv11_2d_surface(s, &sf);

    x0 = MAX(x, 0); y0 = MAX(y, 0);
    x1 = MIN(x + w, sf.w); y1 = MIN(y + h, sf.h);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    trace_nv11_2d_fill(eip, x0, y0, x1 - x0, y1 - y0, sf.bpp, s->d2d_rop3);

    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            uint8_t *p = nv11_2d_pix(s, &sf, px, py);
            uint32_t dst, src, pat, r;

            if (!p) {
                continue;
            }
            dst = nv11_2d_load(p, sf.bpp);
            src = s->d2d_col1a;
            if (use_pat) {
                pat = nv11_2d_pattern_bit(s, px, py) ? s->d2d_pat_col1
                                                     : s->d2d_pat_col0;
            } else {
                pat = 0;
            }
            r = nv11_2d_rop3(s->d2d_rop3, dst, src, pat);
            if (use_pat && sf.bpp == 32 && ((r >> 24) & 0xFF) == 0) {
                continue;   /* transparent stipple (32bpp alpha) */
            }
            nv11_2d_store(p, sf.bpp, r);
        }
    }
    nv11_2d_dirty(s, &sf, x0, y0, x1, y1);
}

static void nv11_2d_blit(NV11State *s, int32_t sx, int32_t sy,
                         int32_t dx, int32_t dy, int32_t w, int32_t h)
{
    Nv11Surf sf;
    int32_t c0, c1, r0, r1, i, j;
    uint32_t eip = nv11_get_eip();
    bool rev_y, rev_x;

    if (w <= 0 || h <= 0) {
        return;
    }
    nv11_2d_surface(s, &sf);

    /* Clip in dst space, translate back to src */
    c0 = MAX(dx, s->d2d_clip_tl & 0xFFFF);
    c1 = MIN(dx + w, (s->d2d_clip_tl & 0xFFFF) + (s->d2d_clip_wh & 0xFFFF));
    r0 = MAX(dy, s->d2d_clip_tl >> 16);
    r1 = MIN(dy + h, (s->d2d_clip_tl >> 16) + (s->d2d_clip_wh >> 16));
    c0 = MAX(c0, 0); r0 = MAX(r0, 0);
    c1 = MIN(c1, sf.w); r1 = MIN(r1, sf.h);
    if (c0 >= c1 || r0 >= r1) {
        return;
    }

    trace_nv11_2d_blit(eip, sx + (c0 - dx), sy + (r0 - dy), c0, r0,
                       c1 - c0, r1 - r0);

    /* Widen the src region we step over to the clipped dst rectangle */
    sx += c0 - dx;
    sy += r0 - dy;
    w = c1 - c0;
    h = r1 - r0;

    /* Avoid overlap corruption: iterate away from the shift direction */
    rev_y = sy < dy;
    rev_x = (sy == dy) && (sx < dx);

    for (i = 0; i < h; i++) {
        int32_t rr = rev_y ? (h - 1 - i) : i;
        for (j = 0; j < w; j++) {
            int32_t cc = rev_x ? (w - 1 - j) : j;
            const uint8_t *ps = nv11_2d_pix(s, &sf, sx + cc, sy + rr);
            uint8_t *pd = nv11_2d_pix(s, &sf, dx + c0 - dx + cc,
                                      dy + r0 - dy + rr);
            uint32_t dst, r;

            if (!ps || !pd) {
                continue;
            }
            dst = nv11_2d_load(ps, sf.bpp);
            r = nv11_2d_rop3(s->d2d_rop3, nv11_2d_load(pd, sf.bpp),
                             dst, 0);
            nv11_2d_store(pd, sf.bpp, r);
        }
    }
    nv11_2d_dirty(s, &sf, dx, dy, dx + w, dy + h);
}

static void nv11_2d_line(NV11State *s, int32_t x0, int32_t y0,
                         int32_t x1, int32_t y1)
{
    Nv11Surf sf;
    int32_t dx, dy, sx, sy, err, e2, x, y;
    uint32_t eip = nv11_get_eip();

    nv11_2d_surface(s, &sf);
    trace_nv11_2d_line(eip, x0, y0, x1, y1, s->d2d_line_color);

    dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;
    x = x0;
    y = y0;

    for (;;) {
        int32_t cl = s->d2d_clip_tl & 0xFFFF;
        int32_t ct = s->d2d_clip_tl >> 16;

        if (x >= cl && y >= ct &&
            x < cl + (s->d2d_clip_wh & 0xFFFF) &&
            y < ct + (s->d2d_clip_wh >> 16) &&
            x >= 0 && y >= 0 && x < sf.w && y < sf.h) {
            uint8_t *p = nv11_2d_pix(s, &sf, x, y);
            if (p) {
                uint32_t dst = nv11_2d_load(p, sf.bpp);
                uint32_t r = nv11_2d_rop3(s->d2d_rop3, dst,
                                          s->d2d_line_color, 0);
                nv11_2d_store(p, sf.bpp, r);
            }
        }
        if (x == x1 && y == y1) {
            break;
        }
        e2 = 2 * err;
        if (e2 > -(dy)) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
    nv11_2d_dirty(s, &sf, MIN(x0, x1), MIN(y0, y1),
                  MAX(x0, x1) + 1, MAX(y0, y1) + 1);
}

static void nv11_2d_expand_begin(NV11State *s, bool opaque,
                                 uint32_t point, uint32_t wh)
{
    uint32_t eip = nv11_get_eip();

    s->d2d_exp_active = true;
    s->d2d_exp_opaque = opaque;
    s->d2d_exp_x = point & 0xFFFF;
    s->d2d_exp_y = point >> 16;
    s->d2d_exp_h = wh >> 16;
    s->d2d_exp_bw = wh & 0xFFFF;
    s->d2d_exp_bw32 = (int)((s->d2d_exp_bw + 31) >> 5);
    s->d2d_exp_row = 0;
    s->d2d_exp_dw = 0;

    if (!s->d2d_exp_bw32 || !s->d2d_exp_h) {
        s->d2d_exp_active = false;
        return;
    }
    if (s->d2d_exp_bw32 > NV11_2D_EXP_BUF_DWORDS) {
        s->d2d_exp_bw32 = NV11_2D_EXP_BUF_DWORDS;
    }

    trace_nv11_2d_expand(eip, s->d2d_exp_x, s->d2d_exp_y,
                         s->d2d_exp_bw, s->d2d_exp_h, opaque);
}

static void nv11_2d_expand_row(NV11State *s)
{
    Nv11Surf sf;
    int32_t px, py, left, right, cl, ct;
    int32_t r = s->d2d_exp_row;
    int32_t xbase = s->d2d_exp_x;

    if (r >= (int32_t)s->d2d_exp_h) {
        return;
    }
    nv11_2d_surface(s, &sf);

    cl = s->d2d_clip_tl & 0xFFFF;
    ct = s->d2d_clip_tl >> 16;

    left = MAX(xbase, s->d2d_exp_opaque ? (s->d2d_clip_e_tl & 0xFFFF)
                                        : (s->d2d_clip_c_tl & 0xFFFF));
    right = MIN(xbase + (int32_t)s->d2d_exp_bw,
                s->d2d_exp_opaque ? (s->d2d_clip_e_br & 0xFFFF)
                                  : (s->d2d_clip_c_br & 0xFFFF));
    left = MAX(left, cl);
    right = MIN(right, cl + (s->d2d_clip_wh & 0xFFFF));
    left = MAX(left, 0);
    right = MIN(right, sf.w);

    py = s->d2d_exp_y + r;
    if (py < ct || py >= ct + (s->d2d_clip_wh >> 16)) {
        return;
    }
    if (py < 0 || py >= sf.h) {
        return;
    }

    for (px = left; px < right; px++) {
        int32_t b = px - xbase;
        int bit;
        uint32_t col, dst, rp;
        uint8_t *p;

        if (b < 0 || b >= (int32_t)s->d2d_exp_bw) {
            continue;
        }
        bit = (s->d2d_exp_buf[b >> 5] >> (b & 31)) & 1;
        if (!bit && !s->d2d_exp_opaque) {
            continue;
        }
        col = (bit) ? s->d2d_exp_fg : s->d2d_exp_bg;

        p = nv11_2d_pix(s, &sf, px, py);
        if (!p) {
            continue;
        }
        dst = nv11_2d_load(p, sf.bpp);
        rp = nv11_2d_rop3(s->d2d_rop3, dst, col,
                          bit ? 0xFFFFFFFFu : 0);
        nv11_2d_store(p, sf.bpp, rp);
    }
    if (py < s->d2d_exp_y + (int32_t)s->d2d_exp_h) {
        nv11_2d_dirty(s, &sf, left, py, right, py + 1);
    }
}

static void nv11_2d_expand_data(NV11State *s, uint32_t val)
{
    if (!s->d2d_exp_active) {
        return;
    }
    if (s->d2d_exp_dw >= s->d2d_exp_bw32) {
        return;
    }
    s->d2d_exp_buf[s->d2d_exp_dw++] = val;
    if (s->d2d_exp_dw != s->d2d_exp_bw32) {
        return;
    }
    nv11_2d_expand_row(s);
    s->d2d_exp_row++;
    s->d2d_exp_dw = 0;
    if (s->d2d_exp_row >= (int32_t)s->d2d_exp_h) {
        s->d2d_exp_active = false;
    }
}

static void nv11_2d_bitmap_method(NV11State *s, uint32_t reg, uint32_t val)
{
    switch (reg) {
    case NV11_2D_BITMAP_COLOR1A:
        s->d2d_col1a = val;
        break;
    case NV11_2D_BITMAP_RECT_TL:
        s->d2d_rect_tl = val;
        break;
    case NV11_2D_BITMAP_RECT_WH:
        nv11_2d_fill_rect(s, s->d2d_rect_tl >> 16, s->d2d_rect_tl & 0xFFFF,
                          val >> 16, val & 0xFFFF);
        break;
    case NV11_2D_BITMAP_CLIPC_TL:
        s->d2d_clip_c_tl = val;
        break;
    case NV11_2D_BITMAP_CLIPC_BR:
        s->d2d_clip_c_br = val;
        break;
    case NV11_2D_BITMAP_COLOR1C:
        s->d2d_exp_fg = val;
        break;
    case NV11_2D_BITMAP_WHC:
        s->d2d_exp_wh = val;
        break;
    case NV11_2D_BITMAP_POINTC:
        nv11_2d_expand_begin(s, false, val, s->d2d_exp_wh);
        break;
    case NV11_2D_BITMAP_CLIPE_TL:
        s->d2d_clip_e_tl = val;
        break;
    case NV11_2D_BITMAP_CLIPE_BR:
        s->d2d_clip_e_br = val;
        break;
    case NV11_2D_BITMAP_COLOR0E:
        s->d2d_exp_bg = val;
        break;
    case NV11_2D_BITMAP_COLOR1E:
        s->d2d_exp_fg = val;
        break;
    case NV11_2D_BITMAP_WHINE:
    case NV11_2D_BITMAP_WHOUTE:
        s->d2d_exp_wh = val;
        break;
    case NV11_2D_BITMAP_POINTE:
        nv11_2d_expand_begin(s, true, val, s->d2d_exp_wh);
        break;
    default:
        if (reg >= NV11_2D_BITMAP_MONOC &&
            reg < NV11_2D_BITMAP_MONOE) {
            nv11_2d_expand_data(s, val);
        } else if (reg >= NV11_2D_BITMAP_MONOE &&
                   reg < NV11_FIFO_CHAN_SIZE) {
            nv11_2d_expand_data(s, val);
        }
        break;
    }
}

static void nv11_2d_rect_nv4_method(NV11State *s, uint32_t reg, uint32_t val)
{
    switch (reg) {
    case NV11_2D_RECT_FMT:
        /* colour depth is implied by the bound surface */
        break;
    case NV11_2D_RECT_SOLID_COLOR:
        s->d2d_col1a = val;
        break;
    case NV11_2D_RECT_SOLID_TL:
        s->d2d_rect_tl = val;
        break;
    case NV11_2D_RECT_SOLID_WH:
        nv11_2d_fill_rect(s, s->d2d_rect_tl >> 16, s->d2d_rect_tl & 0xFFFF,
                          val >> 16, val & 0xFFFF);
        break;
    case NV11_2D_RECT_X0:      /* one-colour clip point 0 (top-left) */
        s->d2d_clip_c_tl = val;
        break;
    case NV11_2D_RECT_X1:      /* one-colour clip point 1 (exclusive) */
        s->d2d_clip_c_br = val;
        break;
    case NV11_2D_RECT_XCOLOR:
        s->d2d_exp_fg = val;
        break;
    case NV11_2D_RECT_XSIZE:
        s->d2d_exp_wh = val;
        break;
    case NV11_2D_RECT_XPOINT:
        nv11_2d_expand_begin(s, false, val, s->d2d_exp_wh);
        break;
    case NV11_2D_RECT_Y0:      /* two-colour clip point 0 */
        s->d2d_clip_e_tl = val;
        break;
    case NV11_2D_RECT_Y1:      /* two-colour clip point 1 (exclusive) */
        s->d2d_clip_e_br = val;
        break;
    case NV11_2D_RECT_YBG:
        s->d2d_exp_bg = val;
        break;
    case NV11_2D_RECT_YFG:
        s->d2d_exp_fg = val;
        break;
    case NV11_2D_RECT_YSIZE_IN:
    case NV11_2D_RECT_YSIZE_OUT:
        s->d2d_exp_wh = val;
        break;
    case NV11_2D_RECT_YPOINT:
        nv11_2d_expand_begin(s, true, val, s->d2d_exp_wh);
        break;
    default:
        if (reg >= NV11_2D_RECT_XDATA && reg < NV11_2D_RECT_XDATA_END) {
            nv11_2d_expand_data(s, val);
        } else if (reg >= NV11_2D_RECT_YDATA &&
                   reg < NV11_2D_RECT_YDATA_END) {
            nv11_2d_expand_data(s, val);
        }
        break;
    }
}

static void nv11_2d_ifc_method(NV11State *s, uint32_t reg, uint32_t val)
{
    switch (reg) {
    case NV11_2D_IFC_OPER:
        s->d2d_ifc_op = val;
        break;
    case NV11_2D_IFC_FMT:
        s->d2d_ifc_fmt = val;
        break;
    case NV11_2D_IFC_POINT:
        nv11_2d_expand_begin(s, false, val, s->d2d_exp_wh);
        break;
    case NV11_2D_IFC_SIZE_OUT:
        s->d2d_exp_wh = val;
        break;
    case NV11_2D_IFC_SIZE_IN:
        break;
    default:
        if (reg >= NV11_2D_IFC_COLOR && reg < NV11_FIFO_CHAN_SIZE) {
            nv11_2d_expand_data(s, val);
        }
        break;
    }
}

static void nv11_2d_opsrc_method(NV11State *s, uint32_t reg, uint32_t val)
{
    switch (reg) {
    case NV11_2D_OP_OPER:
        s->d2d_ifc_op = val;
        break;
    case NV11_2D_OP_COLOR:
        s->d2d_col1a = val;
        break;
    case NV11_2D_OP_P1:
        s->d2d_rect_tl = val;
        break;
    case NV11_2D_OP_P2:
        nv11_2d_fill_rect(s, s->d2d_rect_tl & 0xFFFF, s->d2d_rect_tl >> 16,
                          (val & 0xFFFF) - (s->d2d_rect_tl & 0xFFFF),
                          (val >> 16) - (s->d2d_rect_tl >> 16));
        break;
    default:
        break;
    }
}

/* BT.601 YUV -> 32-bit XRGB. */
static inline uint32_t nv11_2d_yuv_to_rgb32(uint8_t y, uint8_t u, uint8_t v)
{
    int32_t c = (int32_t)y - 16, d = (int32_t)u - 128, e = (int32_t)v - 128;
    int32_t r = (298 * c + 409 * e + 128) >> 8;
    int32_t g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int32_t b = (298 * c + 516 * d + 128) >> 8;

    if (r < 0) { r = 0; } else if (r > 255) { r = 255; }
    if (g < 0) { g = 0; } else if (g > 255) { g = 255; }
    if (b < 0) { b = 0; } else if (b > 255) { b = 255; }
    return (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
           (uint32_t)b;
}

/* Fit a 32-bit XRGB pixel into the destination surface's colour depth. */
static inline uint32_t nv11_2d_rgb_fit(uint32_t rgb, const Nv11Surf *sf)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;

    switch (sf->fmt) {
    case 0x1:                       /* 8 bpp: simple luma */
        return (r * 77 + g * 150 + b * 29) >> 8;
    case 0x2:                       /* 15 bpp RGB555 */
        return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    case 0x5:                       /* 16 bpp RGB565 */
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    default:
        return rgb;
    }
}

/*
 * Fetch one source pixel for the stretch blit. Sources are byte-aligned
 * in VRAM: X8R8G8B8 is a straight 32-bit read, YUV 4:2:2 is packed two
 * pixels per dword (YUYV or UYVY ordering of the chroma dword).
 */
static inline uint32_t nv11_2d_sifm_src(NV11State *s, uint32_t fmt,
                                        uint32_t off, uint32_t pitch,
                                        int32_t sx, int32_t sy)
{
    uint8_t *fb = memory_region_get_ram_ptr(&s->vga.vram);
    uint32_t base = off + (uint32_t)sy * pitch;
    uint32_t w;
    uint8_t y, u, v;

    if (fmt == NV11_2D_SIFM_FMT_X8R8G8B8) {
        if (base > s->vga.vram_size ||
            base + (uint32_t)sx * 4 + 4 > s->vga.vram_size) {
            return 0;
        }
        return ldl_le_p(fb + base + (uint32_t)sx * 4);
    }
    base = off + (uint32_t)sy * pitch + (uint32_t)(sx & ~1) * 2;
    if (base + 4 > s->vga.vram_size) {
        return 0;
    }
    w = ldl_le_p(fb + base);
    if (fmt == NV11_2D_SIFM_FMT_UYVY) {
        u = w & 0xFF;
        y = (sx & 1) ? ((w >> 24) & 0xFF) : ((w >> 8) & 0xFF);
        v = (w >> 16) & 0xFF;
    } else {                        /* YUYV */
        y = (sx & 1) ? ((w >> 16) & 0xFF) : (w & 0xFF);
        u = (w >> 8) & 0xFF;
        v = (w >> 24) & 0xFF;
    }
    return nv11_2d_yuv_to_rgb32(y, u, v);
}

static void nv11_2d_sifm_run(NV11State *s)
{
    Nv11Surf sf;
    uint32_t eip = nv11_get_eip();
    uint32_t fmt = s->d2d_sifm_fmt & NV11_2D_SIFM_FMT_MASK;
    int32_t src_w = s->d2d_sifm_src_wh & 0xFFFF;
    int32_t src_h = s->d2d_sifm_src_wh >> 16;
    uint32_t src_pitch = s->d2d_sifm_src_fmt & 0xFFFF;
    int32_t dx0 = s->d2d_sifm_dst & 0xFFFF;
    int32_t dy0 = s->d2d_sifm_dst >> 16;
    int32_t dw = s->d2d_sifm_dst_wh & 0xFFFF;
    int32_t dh = s->d2d_sifm_dst_wh >> 16;
    int32_t c0, c1, r0, r1, x, y;
    uint64_t u16, v16, usx, vsy;

    if (src_w <= 0 || src_h <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    if (fmt != NV11_2D_SIFM_FMT_YUYV &&
        fmt != NV11_2D_SIFM_FMT_UYVY &&
        fmt != NV11_2D_SIFM_FMT_X8R8G8B8) {
        return;
    }
    nv11_2d_surface(s, &sf);

    c0 = MAX(dx0, s->d2d_clip_tl & 0xFFFF);
    c1 = MIN(dx0 + dw, (s->d2d_clip_tl & 0xFFFF) + (s->d2d_clip_wh & 0xFFFF));
    r0 = MAX(dy0, s->d2d_clip_tl >> 16);
    r1 = MIN(dy0 + dh, (s->d2d_clip_tl >> 16) + (s->d2d_clip_wh >> 16));
    c0 = MAX(c0, s->d2d_sifm_clip_tl & 0xFFFF);
    c1 = MIN(c1, (s->d2d_sifm_clip_tl & 0xFFFF) +
                 (s->d2d_sifm_clip_wh & 0xFFFF));
    r0 = MAX(r0, s->d2d_sifm_clip_tl >> 16);
    r1 = MIN(r1, (s->d2d_sifm_clip_tl >> 16) + (s->d2d_sifm_clip_wh >> 16));
    c0 = MAX(c0, 0); r0 = MAX(r0, 0);
    c1 = MIN(c1, sf.w); r1 = MIN(r1, sf.h);
    if (c0 >= c1 || r0 >= r1) {
        return;
    }

    trace_nv11_2d_sifm(eip, c0, r0, c1 - c0, r1 - r0, src_w, src_h, fmt);

    u16 = (uint64_t)(s->d2d_sifm_src_point & 0xFFFF) << 12;
    v16 = (uint64_t)(s->d2d_sifm_src_point >> 16) << 12;
    usx = (uint64_t)s->d2d_sifm_dudx >> 4;
    vsy = (uint64_t)s->d2d_sifm_dvdy >> 4;

    /* Slide the source origin to the clipped top-left corner */
    u16 += (uint64_t)(c0 - dx0) * usx;
    v16 += (uint64_t)(r0 - dy0) * vsy;

    for (y = r0; y < r1; y++) {
        uint64_t uu = u16;

        for (x = c0; x < c1; x++) {
            int32_t sx = (int32_t)((uu + 0x8000) >> 16);
            int32_t sy = (int32_t)((v16 + 0x8000) >> 16);
            uint8_t *p = nv11_2d_pix(s, &sf, x, y);

            if (sx < 0) { sx = 0; }
            if (sy < 0) { sy = 0; }
            if (sx >= src_w) { sx = src_w - 1; }
            if (sy >= src_h) { sy = src_h - 1; }
            if (p) {
                uint32_t rgb = nv11_2d_sifm_src(s, fmt, s->d2d_sifm_src_off,
                                                src_pitch, sx, sy);
                nv11_2d_store(p, sf.bpp, nv11_2d_rgb_fit(rgb, &sf));
            }
            uu += usx;
        }
        v16 += vsy;
    }
    nv11_2d_dirty(s, &sf, c0, r0, c1, r1);
}

static void nv11_2d_sifm_method(NV11State *s, uint32_t reg, uint32_t val)
{
    switch (reg) {
    case NV11_2D_SIFM_FMT:
        s->d2d_sifm_fmt = val;
        break;
    case NV11_2D_SIFM_OPER:
        break;                      /* COPY is the only op we model */
    case NV11_2D_SIFM_CLIP_TL:
        s->d2d_sifm_clip_tl = val;
        break;
    case NV11_2D_SIFM_CLIP_WH:
        s->d2d_sifm_clip_wh = val;
        break;
    case NV11_2D_SIFM_DST_TL:
        s->d2d_sifm_dst = val;
        break;
    case NV11_2D_SIFM_DST_WH:
        s->d2d_sifm_dst_wh = val;
        break;
    case NV11_2D_SIFM_DUDX:
        s->d2d_sifm_dudx = val;
        break;
    case NV11_2D_SIFM_DVDY:
        s->d2d_sifm_dvdy = val;
        break;
    case NV11_2D_SIFM_SRC_WH:
        s->d2d_sifm_src_wh = val;
        break;
    case NV11_2D_SIFM_SRC_FMT:
        s->d2d_sifm_src_fmt = val;
        break;
    case NV11_2D_SIFM_SRC_OFF:
        s->d2d_sifm_src_off = val;
        break;
    case NV11_2D_SIFM_SRC_POINT:
        s->d2d_sifm_src_point = val;
        nv11_2d_sifm_run(s);
        break;
    default:
        break;
    }
}

static void nv11_2d_m2mf_run(NV11State *s)
{
    uint32_t eip = nv11_get_eip();
    uint8_t *fb = memory_region_get_ram_ptr(&s->vga.vram);
    uint32_t src, dst, len = s->d2d_m2mf_len;
    uint32_t lines = s->d2d_m2mf_lines;
    uint32_t psin = s->d2d_m2mf_pitch_in;
    uint32_t psout = s->d2d_m2mf_pitch_out;
    uint32_t i;

    if (!len || !lines || len > s->vga.vram_size) {
        return;
    }
    src = nv11_fifo_dma_frame(s, s->d2d_m2mf_in) + s->d2d_m2mf_off_in;
    dst = nv11_fifo_dma_frame(s, s->d2d_m2mf_out) + s->d2d_m2mf_off_out;
    if (src >= s->vga.vram_size ||
        dst  >  s->vga.vram_size || dst > s->vga.vram_size - len) {
        return;
    }

    trace_nv11_2d_m2mf(eip, src, dst, len, lines, psin, psout);

    for (i = 0; i < lines; i++) {
        uint32_t so = src + i * psin;
        uint32_t doff = dst + i * psout;

        if (so >= s->vga.vram_size || doff > s->vga.vram_size - len) {
            return;
        }
        memmove(fb + doff, fb + so, len);
    }
    memory_region_set_dirty(&s->vga.vram, dst, len);
}

static void nv11_2d_m2mf_method(NV11State *s, uint32_t reg, uint32_t val)
{
    switch (reg) {
    case NV11_2D_M2MF_DMA_NOTIFY:
        s->d2d_m2mf_notify = val;
        break;
    case NV11_2D_M2MF_DMA_IN:
        s->d2d_m2mf_in = val;
        break;
    case NV11_2D_M2MF_DMA_OUT:
        s->d2d_m2mf_out = val;
        break;
    case NV11_2D_M2MF_OFF_IN:
        s->d2d_m2mf_off_in = val;
        break;
    case NV11_2D_M2MF_OFF_OUT:
        s->d2d_m2mf_off_out = val;
        break;
    case NV11_2D_M2MF_PITCH_IN:
        s->d2d_m2mf_pitch_in = val;
        break;
    case NV11_2D_M2MF_PITCH_OUT:
        s->d2d_m2mf_pitch_out = val;
        break;
    case NV11_2D_M2MF_LINE_LEN:
        s->d2d_m2mf_len = val;
        break;
    case NV11_2D_M2MF_LINE_COUNT:
        s->d2d_m2mf_lines = val;
        nv11_2d_m2mf_run(s);
        break;
    case NV11_2D_M2MF_FORMAT:
        s->d2d_m2mf_fmt = val;
        break;
    case NV11_2D_M2MF_BUF_NOTIFY:
        break;
    default:
        break;
    }
}

/* NOP / NOTIFY completion semantics: writing the 0x104 method with the
 * notify-object handle makes the engine drop a STATUS_COMPLETED word into
 * the notifier once the earlier methods have drained; we are synchronous,
 * so the writeback happens immediately. */
static void nv11_2d_notify_method(NV11State *s, uint32_t reg, uint32_t val)
{
    uint8_t *fb;
    uint32_t base;

    if (reg != NV11_2D_NOTIFY_METHOD) {
        return;
    }
    fb = memory_region_get_ram_ptr(&s->vga.vram);
    base = nv11_fifo_dma_frame(s, val);
    if (base + NV11_NOTIFY_STATUS_OFF + 4 > s->vga.vram_size) {
        return;
    }
    trace_nv11_2d_notify(nv11_get_eip(), base + NV11_NOTIFY_STATUS_OFF);
    stl_le_p(fb + base + NV11_NOTIFY_STATUS_OFF, NV11_NOTIFY_STATUS_DONE);
}

static void nv11_2d_line_method(NV11State *s, uint32_t reg, uint32_t val)
{
    if (reg == NV11_2D_LINE_COLOR) {
        s->d2d_line_color = val;
    } else if (reg == NV11_2D_LINE_P0) {
        s->d2d_line_p0 = val;
    } else if (reg == NV11_2D_LINE_P1) {
        nv11_2d_line(s, s->d2d_line_p0 & 0xFFFF, s->d2d_line_p0 >> 16,
                     val & 0xFFFF, val >> 16);
    } else if (reg == NV11_2D_LINE_P0B) {
        s->d2d_line_p0b = val;
    } else if (reg == NV11_2D_LINE_P1B) {
        nv11_2d_line(s, s->d2d_line_p0b & 0xFFFF,
                     s->d2d_line_p0b >> 16, val & 0xFFFF, val >> 16);
    }
}

static void nv11_2d_rop_method(NV11State *s, uint32_t reg, uint32_t val)
{
    if (reg == NV11_2D_ROP3) {
        s->d2d_rop3 = val & 0xFF;
    }
}

static void nv11_2d_clip_method(NV11State *s, uint32_t reg, uint32_t val)
{
    if (reg == NV11_2D_CLIP_TL) {
        s->d2d_clip_tl = val;
    } else if (reg == NV11_2D_CLIP_WH) {
        s->d2d_clip_wh = val;
    }
}

static void nv11_2d_patt_method(NV11State *s, uint32_t reg, uint32_t val)
{
    if (reg == NV11_2D_PATT_SHAPE) {
        s->d2d_pat_shape = val;
    } else if (reg == NV11_2D_PATT_COLOR0) {
        s->d2d_pat_col0 = val;
    } else if (reg == NV11_2D_PATT_COLOR1) {
        s->d2d_pat_col1 = val;
    } else if (reg == NV11_2D_PATT_MONO0) {
        s->d2d_pat_mono[0] = val;
    } else if (reg == NV11_2D_PATT_MONO1) {
        s->d2d_pat_mono[1] = val;
    }
}

static void nv11_2d_blt_method(NV11State *s, uint32_t reg, uint32_t val)
{
    if (reg == NV11_2D_BLT_TL_SRC) {
        s->d2d_blt_src = val;
    } else if (reg == NV11_2D_BLT_TL_DST) {
        s->d2d_blt_dst = val;
    } else if (reg == NV11_2D_BLT_WH) {
        int32_t sx = s->d2d_blt_src & 0xFFFF;
        int32_t sy = s->d2d_blt_src >> 16;
        int32_t dx = s->d2d_blt_dst & 0xFFFF;
        int32_t dy = s->d2d_blt_dst >> 16;
        nv11_2d_blit(s, sx, sy, dx, dy, val & 0xFFFF, val >> 16);
    }
}

static void nv11_2d_surface_method(NV11State *s, uint32_t reg, uint32_t val)
{
    switch (reg) {
    case NV11_2D_SURF_FMT_M:
        switch (val & 0x7) {
        case 0x1: val = 0x1; break;   /* 8 bpp  */
        case 0x2: val = 0x2; break;   /* 15 bpp */
        case 0x4: val = 0x5; break;   /* 16 bpp */
        default:  val = 0x7; break;   /* 24/32  */
        }
        s->pgraph_scratch[NV11_2D_SURF_FMT / 4] = val;
        break;
    case NV11_2D_SURF_PITCH_M:
        s->pgraph_scratch[NV11_2D_SURF_PITCH_0 / 4] = val & 0xFFFF;
        break;
    case NV11_2D_SURF_OFF_M:
        s->pgraph_scratch[NV11_2D_SURF_OFF_0 / 4] = val;
        break;
    case NV11_2D_SURF_OFFDST_M:
        s->pgraph_scratch[NV11_2D_SURF_OFF_0 / 4] = val;
        break;
    }
}

void nv11_2d_method(NV11State *s, uint32_t chan, uint32_t reg, uint32_t val)
{
    uint32_t eip = nv11_get_eip();
    uint16_t cls = s->ch_class[chan];

    trace_nv11_2d_method(eip, chan, reg, val);

    switch (cls) {
    case NV11_CLASS_ROP:
        nv11_2d_rop_method(s, reg, val);
        break;
    case NV11_CLASS_CLIP:
        nv11_2d_clip_method(s, reg, val);
        break;
    case NV11_CLASS_PATT:
    case NV11_CLASS_PATT_NV4:
        nv11_2d_patt_method(s, reg, val);
        break;
    case NV11_CLASS_SURF:
        nv11_2d_surface_method(s, reg, val);
        break;
    case NV11_CLASS_BLT:
    case NV11_CLASS_BLT_NV15:
        nv11_2d_blt_method(s, reg, val);
        break;
    case NV11_CLASS_GDI:
        nv11_2d_bitmap_method(s, reg, val);
        break;
    case NV11_CLASS_RECT_NV4:
        nv11_2d_rect_nv4_method(s, reg, val);
        break;
    case NV11_CLASS_SIFM:
        nv11_2d_sifm_method(s, reg, val);
        break;
    case NV11_CLASS_SIFC:
        nv11_2d_sifm_method(s, reg, val);
        break;
    case NV11_CLASS_IFC_NV4:
        nv11_2d_ifc_method(s, reg, val);
        break;
    case NV11_CLASS_OP_SRCCOPY:
        nv11_2d_opsrc_method(s, reg, val);
        break;
    case NV11_CLASS_M2MF:
        nv11_2d_m2mf_method(s, reg, val);
        break;
    case NV11_CLASS_NOTIFY:
        nv11_2d_notify_method(s, reg, val);
        break;
    case NV11_CLASS_NOP:
        break;                      /* always-idle engine */
    case NV11_CLASS_LINE:
    case NV11_CLASS_LINE_NV4:
    case NV11_CLASS_LIN:
        nv11_2d_line_method(s, reg, val);
        break;
    default:
        trace_nv11_2d_ignored(eip, chan, reg, val, cls);
        break;
    }
}

void nv11_2d_reset(NV11State *s)
{
    memset(&s->d2d_rop3, 0, sizeof(NV11State) - offsetof(NV11State, d2d_rop3));

    s->d2d_rop3 = 0xCC;              /* Default: Copy Source */
    s->d2d_clip_tl = 0;              /* Clip = Full (0x7FFF x 0x7FFF) */
    s->d2d_clip_wh = (0x7FFF << 16) | 0x7FFF;

    trace_nv11_2d_reset();
}

void nv11_2d_init(NV11State *s)
{
    nv11_2d_reset(s);
}