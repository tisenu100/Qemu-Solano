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
#include "r100.h"
#include "r100_regs.h"

uint32_t r100_pll_read(R100State *s)
{
    return s->pll_regs[s->clock_cntl_index & 0x3F];
}

void r100_pll_write(R100State *s, uint32_t val)
{
    s->pll_regs[s->clock_cntl_index & 0x3F] = val;
}

uint32_t r100_crtc_gen_cntl_read(R100State *s)
{
    return s->crtc_gen_cntl;
}

void r100_crtc_gen_cntl_write(R100State *s, uint32_t val)
{
    s->crtc_gen_cntl = val;
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
    return s->io_bar_gate | R100_IO_GATE_BIT;
}

void r100_io_gate_write(R100State *s, uint32_t val)
{
    s->io_bar_gate = val;
}
