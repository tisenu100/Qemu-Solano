/*
 * Nvidia Geforce2 MX400 (NV11B) PCRTC Extended Registers
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
#include "hw/display/vga.h"
#include "vga_regs.h"
#include "nv11.h"
#include "trace.h"

static void nv11_crtc_index_data_ports(NV11State *s, uint32_t *pi,
                                       uint32_t *pd)
{
    if (s->vga.msr & VGA_MIS_COLOR) {
        *pi = 0x3D4;
        *pd = 0x3D5;
    } else {
        *pi = 0x3B4;
        *pd = 0x3B5;
    }
}

void nv11_pcrtc_write(NV11State *s, uint8_t index, uint8_t value)
{
    uint32_t eip = nv11_get_eip();
    trace_nv11_pcrtc_crtc_write(eip, index, value);

    if (index < 0x19) {
        uint32_t pi, pd;
        nv11_crtc_index_data_ports(s, &pi, &pd);
        vga_ioport_write(&s->vga, pi, index);
        vga_ioport_write(&s->vga, pd, value);
        return;
    }

    s->nv_crtc_reg[index] = value;

    switch (index) {
    case NV11_CRTC_WIN_OP:
        s->win_op = value & 7;
        trace_nv11_window_opcode(eip, value, s->win_op);
        break;
    case 0x2E:
        trace_nv11_pcrtc_strap_write(eip, index, value);
        break;
    case 0x3C:
        trace_nv11_pcrtc_strap_write(eip, index, value);
        break;
    case 0x44:
        trace_nv11_pcrtc_strap_write(eip, index, value);
        break;
    default:
        break;
    }
}

uint8_t nv11_pcrtc_read(NV11State *s, uint8_t index)
{
    if (index < 0x19) {
        uint32_t pi, pd;
        nv11_crtc_index_data_ports(s, &pi, &pd);
        vga_ioport_write(&s->vga, pi, index);
        return vga_ioport_read(&s->vga, pd);
    }

    switch (index) {
    case 0x2E:
        return 0x44;
    case 0x3C:
        return 0x01;
    case 0x44:
        return 0xB2;
    case 0x38:
        return s->win_op;
    default:
        break;
    }

    return s->nv_crtc_reg[index];
}

void nv11_pcrtc_init(NV11State *s)
{
    s->pcrtc_scratch[0][NV11_PCRTC_INTR / 4] = 0;
    s->pcrtc_scratch[1][NV11_PCRTC_INTR / 4] = 0;

    s->nv_crtc_reg[0x2E] = 0x44;
    s->nv_crtc_reg[0x3C] = 0x01;
    s->nv_crtc_reg[0x44] = 0xB2;
    s->nv_crtc_reg[0x38] = 0x00;

    trace_nv11_pcrtc_init();
}
