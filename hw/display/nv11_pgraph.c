/*
 * Nvidia Geforce2 MX400 (NV11B) PGRAPH
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

static bool nv11_pgraph_idx_valid(uint32_t idx)
{
    return idx < (NV11_PGRAPH_END - NV11_PGRAPH_OFF) / 4;
}

uint64_t nv11_pgraph_read(NV11State *s, hwaddr offset, unsigned size)
{
    uint32_t reg = (uint32_t)offset;
    uint32_t idx = reg / 4;
    uint32_t eip = nv11_get_eip();
    uint32_t val;

    if (reg + size > NV11_PGRAPH_END - NV11_PGRAPH_OFF) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PGRAPH] read beyond window at 0x%x\n", reg);
        return 0;
    }
    if (!nv11_pgraph_idx_valid(idx)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PGRAPH] bad read index at 0x%x\n", reg);
        return 0;
    }

    val = s->pgraph_scratch[idx];

    if (reg == NV11_PGRAPH_STATUS) {
        /* bit0 = Busy: set when FIFO methods are queued, cleared on drain */
        val = (val & ~0x1u) | (s->pgraph_busy ? 1 : 0);
        trace_nv11_pgraph_status(eip, s->pgraph_busy);
    }

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
                      "NV11: [PGRAPH] bad read size %u at 0x%x\n", size, reg);
        return 0;
    }

    trace_nv11_pgraph_read(eip, reg, size);
    return val;
}

void nv11_pgraph_write(NV11State *s, hwaddr offset, uint64_t val, unsigned size)
{
    uint32_t reg = (uint32_t)offset;
    uint32_t idx = reg / 4;
    uint32_t eip = nv11_get_eip();

    trace_nv11_pgraph_write(eip, reg, size, (uint32_t)val);

    if (reg + size > NV11_PGRAPH_END - NV11_PGRAPH_OFF) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PGRAPH] write beyond window at 0x%x\n", reg);
        return;
    }
    if (!nv11_pgraph_idx_valid(idx)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PGRAPH] bad write index at 0x%x\n", reg);
        return;
    }

    switch (size) {
    case 1:
        s->pgraph_scratch[idx] &= ~(0xFFu << (8 * (reg % 4)));
        s->pgraph_scratch[idx] |= ((uint32_t)val & 0xFFu) << (8 * (reg % 4));
        break;
    case 2:
        s->pgraph_scratch[idx] &= ~(0xFFFFu << (8 * (reg % 4)));
        s->pgraph_scratch[idx] |= ((uint32_t)val & 0xFFFFu) << (8 * (reg % 4));
        break;
    case 4:
        s->pgraph_scratch[idx] = (uint32_t)val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [PGRAPH] bad write size %u at 0x%x\n", size, reg);
        return;
    }
}

void nv11_pgraph_reset(NV11State *s)
{
    memset(s->pgraph_scratch, 0, sizeof(s->pgraph_scratch));
    s->pgraph_busy = false;
}

void nv11_pgraph_init(NV11State *s)
{
    nv11_pgraph_reset(s);

    trace_nv11_pgraph_init();
}