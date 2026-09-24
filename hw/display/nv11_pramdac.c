/*
 * Nvidia Geforce2 MX400 (NV11B) PRAMDAC / PLL
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
#include "nv11.h"
#include "trace.h"

#define NV11_PRAMDAC_SIZE (NV11_PRAMDAC0_END - NV11_PRAMDAC0_OFF)

static bool nv11_pramdac_idx_valid(uint32_t idx)
{
    return idx < (NV11_PRAMDAC_SIZE) / 4;
}

static bool nv11_pramdac_is_pll(uint32_t reg)
{
    return reg == NV11_PRAMDAC_NVPLL ||
           reg == NV11_PRAMDAC_MPLL ||
           reg == NV11_PRAMDAC_VPLL ||
           reg == NV11_PRAMDAC_VPLL2;
}

uint64_t nv11_pramdac_read(NV11State *s, int head, hwaddr offset, unsigned size)
{
    uint32_t reg = (uint32_t)offset;
    uint32_t idx = reg / 4;
    uint32_t eip = nv11_get_eip();
    uint32_t val;

    if (reg + size > NV11_PRAMDAC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PRAMDAC%d] read beyond window at 0x%x\n",
                      head, reg);
        return 0;
    }
    if (!nv11_pramdac_idx_valid(idx)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PRAMDAC%d] bad read index at 0x%x\n", head, reg);
        return 0;
    }

    val = s->pramdac[head][idx];

    switch (size) {
    case 1:
        val = (val >> (8 * (reg % 4))) & 0xFF;
        break;
    case 2:
        val = (val >> (8 * (reg % 4))) & 0xFFFF;
        break;
    case 4:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PRAMDAC%d] bad read size %u at 0x%x\n",
                      head, size, reg);
        return 0;
    }

    trace_nv11_pramdac_read(eip, head, reg, size, val);
    return val;
}

void nv11_pramdac_write(NV11State *s, int head, hwaddr offset,
                        uint64_t val, unsigned size)
{
    uint32_t reg = (uint32_t)offset;
    uint32_t idx = reg / 4;
    uint32_t eip = nv11_get_eip();

    trace_nv11_pramdac_write(eip, head, reg, size, (uint32_t)val);

    if (reg + size > NV11_PRAMDAC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PRAMDAC%d] write beyond window at 0x%x\n",
                      head, reg);
        return;
    }
    if (!nv11_pramdac_idx_valid(idx)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PRAMDAC%d] bad write index at 0x%x\n", head, reg);
        return;
    }

    switch (size) {
    case 1:
        s->pramdac[head][idx] &= ~(0xFFu << (8 * (reg % 4)));
        s->pramdac[head][idx] |= ((uint32_t)val & 0xFFu) << (8 * (reg % 4));
        break;
    case 2:
        s->pramdac[head][idx] &= ~(0xFFFFu << (8 * (reg % 4)));
        s->pramdac[head][idx] |= ((uint32_t)val & 0xFFFFu) << (8 * (reg % 4));
        break;
    case 4:
        s->pramdac[head][idx] = (uint32_t)val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PRAMDAC%d] bad write size %u at 0x%x\n",
                      head, size, reg);
        return;
    }

    if (reg == NV11_PRAMDAC_CUR_POS) {
        s->cur_pos = s->pramdac[head][idx];
    }

    if (nv11_pramdac_is_pll(reg)) {
        trace_nv11_pramdac_pll(eip, head, reg,
                               s->pramdac[head][idx] & NV11_PLL_M1_MASK,
                               (s->pramdac[head][idx] & NV11_PLL_N1_MASK) >>
                                   NV11_PLL_N1_SHIFT,
                               (s->pramdac[head][idx] & NV11_PLL_P_MASK) >>
                                   NV11_PLL_P_SHIFT);
    }
}

void nv11_pramdac_reset(NV11State *s)
{
    int head;

    memset(s->pramdac, 0, sizeof(s->pramdac));

    /*
     * Plausible single-stage PLL defaults so some drivers first read-back
     * is never garbage: NVPLL/MPLL/VPLL/VPLL2 all get M1=1 with a sane N1,
     * and PLL_SEL routes head 0 to the NVPLL (VGA/analog CRT).
     */
    for (head = 0; head < 2; head++) {
        s->pramdac[head][NV11_PRAMDAC_NVPLL / 4] = 0x00000D01;
        s->pramdac[head][NV11_PRAMDAC_MPLL / 4] = 0x00000C01;
        s->pramdac[head][NV11_PRAMDAC_VPLL / 4] = 0x00000A01;
        s->pramdac[head][NV11_PRAMDAC_VPLL2 / 4] = 0x00000A01;
        s->pramdac[head][NV11_PRAMDAC_PLL_SEL / 4] =
            NV11_PRAMDAC_PLL_SEL_SRC_NVPLL;
        /* General control: the VBIOS POST leaves the analog CRT path
         * enabled (PIXMIX on, VGA state selected, 8-bit DAC).  nvcore reads
         * 0x680600 dozens of times but never writes it; a 0 read-back would
         * make it believe no output path exists and stall the mode-set. */
        s->pramdac[head][NV11_PRAMDAC_GEN_CTL / 4] =
            NV11_PRAMDAC_GEN_CTL_CRT_ON;
    }

    s->cur_pos = 0;
    s->last_cur_pos = 0;

    trace_nv11_pramdac_reset();
}

void nv11_pramdac_init(NV11State *s)
{
    nv11_pramdac_reset(s);

    trace_nv11_pramdac_init();
}