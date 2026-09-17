/*
 * Nvidia Geforce2 MX400 (NV11B) PTIMER
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
#include "qemu/timer.h"
#include "nv11.h"
#include "trace.h"

/* PTIMER runs at 31.25 MHz (32 ns per tick) */
#define NV11_PTIMER_TICK_NS 32

static uint64_t nv11_ptimer_counter(NV11State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t delta = now - s->ptimer_base_ns;

    if (delta < 0) {
        delta = 0;
    }
    return s->ptimer_base + (uint64_t)delta / NV11_PTIMER_TICK_NS;
}

static uint32_t nv11_ptimer_time_low(NV11State *s)
{
    return (uint32_t)(nv11_ptimer_counter(s) & 0xFFFFFFFFu);
}

static uint32_t nv11_ptimer_time_high(NV11State *s)
{
    return (uint32_t)((nv11_ptimer_counter(s) >> 32) & 0xFFFFFFFFu);
}

static void nv11_ptimer_update_intr(NV11State *s)
{
    if ((s->ptimer_intr_en & 0x1) &&
        (int32_t)(nv11_ptimer_time_low(s) - s->ptimer_alarm) >= 0) {
        s->ptimer_intr |= 0x1;
    }
}

static uint32_t nv11_ptimer_reg_load(NV11State *s, uint32_t reg)
{
    switch (reg) {
    case NV11_PTIMER_INTR:
        nv11_ptimer_update_intr(s);
        return s->ptimer_intr;
    case NV11_PTIMER_INTR_EN:
        return s->ptimer_intr_en;
    case NV11_PTIMER_NUMERATOR:
        return s->ptimer_num;
    case NV11_PTIMER_DENOMINATOR:
        return s->ptimer_denom;
    case NV11_PTIMER_TIME_LOW:
        return nv11_ptimer_time_low(s);
    case NV11_PTIMER_TIME_HIGH:
        return nv11_ptimer_time_high(s);
    case NV11_PTIMER_ALARM:
        return s->ptimer_alarm;
    default:
        return 0;
    }
}

uint64_t nv11_ptimer_read(NV11State *s, hwaddr offset, unsigned size)
{
    uint32_t reg = (uint32_t)offset;
    uint32_t eip = nv11_get_eip();
    uint32_t val;

    if (reg + size > NV11_PTMR_END - NV11_PTMR_OFF) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PTIMER] read beyond window at 0x%x\n", reg);
        return 0;
    }

    val = nv11_ptimer_reg_load(s, reg & ~0x3u);

    switch (size) {
    case 1:
        val = (val >> (8 * (reg & 0x3))) & 0xFF;
        break;
    case 2:
        val = (val >> (8 * (reg & 0x3))) & 0xFFFF;
        break;
    case 4:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PTIMER] bad read size %u at 0x%x\n", size, reg);
        return 0;
    }

    trace_nv11_ptimer_read(eip, reg, size);
    return val;
}

void nv11_ptimer_write(NV11State *s, hwaddr offset, uint64_t val, unsigned size)
{
    uint32_t reg = (uint32_t)offset;
    uint32_t eip = nv11_get_eip();
    uint32_t aligned = reg & ~0x3u;
    unsigned shift = 8 * (reg & 0x3);
    uint32_t mask;
    uint64_t now;
    uint64_t cur;

    trace_nv11_ptimer_write(eip, reg, size, (uint32_t)val);

    if (reg + size > NV11_PTMR_END - NV11_PTMR_OFF) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PTIMER] write beyond window at 0x%x\n", reg);
        return;
    }

    switch (size) {
    case 1:
        mask = 0xFFu << shift;
        val = ((uint32_t)val & 0xFFu) << shift;
        break;
    case 2:
        if (reg & 0x1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "NV11: [PTIMER] unaligned 2-byte write at 0x%x\n",
                          reg);
            return;
        }
        mask = 0xFFFFu << shift;
        val = ((uint32_t)val & 0xFFFFu) << shift;
        break;
    case 4:
        if (reg & 0x3) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "NV11: [PTIMER] unaligned 4-byte write at 0x%x\n",
                          reg);
            return;
        }
        mask = 0xFFFFFFFFu;
        val &= 0xFFFFFFFFu;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PTIMER] bad write size %u at 0x%x\n", size, reg);
        return;
    }

    switch (aligned) {
    case NV11_PTIMER_INTR:
        /* Write-1-to-clear. */
        s->ptimer_intr &= ~(uint32_t)val;
        break;
    case NV11_PTIMER_INTR_EN:
        s->ptimer_intr_en =
            (s->ptimer_intr_en & ~mask) | ((uint32_t)val & mask);
        nv11_ptimer_update_intr(s);
        break;
    case NV11_PTIMER_NUMERATOR:
        s->ptimer_num =
            (s->ptimer_num & ~mask) | ((uint32_t)val & mask);
        break;
    case NV11_PTIMER_DENOMINATOR:
        s->ptimer_denom =
            (s->ptimer_denom & ~mask) | ((uint32_t)val & mask);
        break;
    case NV11_PTIMER_TIME_LOW:
    case NV11_PTIMER_TIME_HIGH:
        /*
         * TIME is writable (nouveau sets it on init/resume: HIGH then
         * LOW). Rebase the free-running counter so the combined 64-bit
         * value equals the written value from now on.
         */
        now = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        cur = nv11_ptimer_counter(s);
        if (aligned == NV11_PTIMER_TIME_LOW) {
            cur = (cur & 0xFFFFFFFF00000000ull) |
                  ((cur & 0xFFFFFFFFull & ~(uint64_t)mask) |
                   ((uint64_t)val & mask));
        } else {
            cur = (cur & 0xFFFFFFFFull) |
                  (((cur >> 32 & 0xFFFFFFFFull & ~(uint64_t)mask) |
                    ((uint64_t)val & mask)) << 32);
        }
        s->ptimer_base = cur;
        s->ptimer_base_ns = (int64_t)now;
        break;
    case NV11_PTIMER_ALARM:
        s->ptimer_alarm =
            (s->ptimer_alarm & ~mask) | ((uint32_t)val & mask);
        nv11_ptimer_update_intr(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PTIMER] write to unhandled reg 0x%x\n", reg);
        break;
    }
}

void nv11_ptimer_reset(NV11State *s)
{
    s->ptimer_intr = 0;
    s->ptimer_intr_en = 0;
    s->ptimer_num = 1;
    s->ptimer_denom = 1;
    s->ptimer_alarm = 0xFFFFFFFFu;
    s->ptimer_base = 0;
    s->ptimer_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

void nv11_ptimer_init(NV11State *s)
{
    nv11_ptimer_reset(s);

    trace_nv11_ptimer_init();
}
