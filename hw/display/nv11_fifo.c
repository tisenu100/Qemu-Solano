/*
 * Nvidia Geforce2 MX400 (NV11B) FIFO Window
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

static void nv11_fifo_shadow_write(NV11State *s, uint32_t chan, uint32_t reg,
                                   uint64_t val, unsigned size)
{
    uint8_t *shadow = (uint8_t *)s->fifo[chan].methods;

    if (reg + size > NV11_FIFO_CHAN_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [FIFO] subch%u write beyond channel at 0x%x\n",
                      chan, reg);
        return;
    }
    switch (size) {
    case 1: shadow[reg] = (uint8_t)val; break;
    case 2: stw_le_p(shadow + reg, (uint16_t)val); break;
    case 4: stl_le_p(shadow + reg, (uint32_t)val); break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [FIFO] subch%u bad write size %u at 0x%x\n",
                      chan, size, reg);
        break;
    }
}

static uint32_t nv11_fifo_shadow_read(NV11State *s, uint32_t chan, uint32_t reg,
                                      unsigned size)
{
    uint8_t *shadow = (uint8_t *)s->fifo[chan].methods;
    uint32_t val = 0;

    if (reg + size > NV11_FIFO_CHAN_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [FIFO] subch%u read beyond channel at 0x%x\n",
                      chan, reg);
        return 0;
    }
    switch (size) {
    case 1: val = shadow[reg]; break;
    case 2: val = lduw_le_p(shadow + reg); break;
    case 4: val = ldl_le_p(shadow + reg); break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [FIFO] subch%u bad read size %u at 0x%x\n",
                      chan, size, reg);
        break;
    }
    return val;
}

static void nv11_fifo_drain(void *opaque)
{
    NV11State *s = opaque;
    uint32_t eip = nv11_get_eip();
    int i;

    for (i = 0; i < NV11_FIFO_CHANNELS; i++) {
        if (s->fifo[i].pending) {
            trace_nv11_fifo_drain(eip, i, s->fifo[i].pending);
            s->fifo[i].pending = 0;
            s->fifo[i].fifo_free = NV11_FIFO_FULL;
        }
    }

    if (s->pgraph_busy) {
        s->pgraph_busy = false;
        trace_nv11_pgraph_idle(eip);
    }
}

/* Read a dword from VRAM at byte offset (for the DMA pusher ring). */
static inline uint32_t nv11_vram_read_dword(NV11State *s, uint32_t off)
{
    if (off + 4 > s->vga.vram_size) {
        return 0;
    }
    return ldl_le_p(s->vram_ptr + off);
}

/*
 * Execute the DMA pushbuffer ring after the driver writes DMA_PUT.
 * Base = VRAM FbUsableSize (emu.cache for a 64MB VRAM card = 64MB - 128K).
 * Ring is 32 KB; headers: NOP=0, JUMP=0x20000000, methods (count<<18)|(ch<<13)|reg.
 */
static void nv11_fifo_dma_push(NV11State *s, uint32_t chan, uint32_t put)
{
    uint32_t eip = nv11_get_eip();
    uint32_t ring_base = s->vga.vram_size - 128 * 1024;
    uint32_t get = s->fifo[chan].dma_get;
    uint32_t i;

    trace_nv11_fifo_dma_push(eip, chan, put, ring_base, get);

    /* The ring holds ring_size/4 dwords; each iteration consumes exactly
     * one dword, so this bounds the walk. */
    for (i = 0; get != put && i < NV11_DMA_RING_SIZE / 4; i++) {
        uint32_t hdr = nv11_vram_read_dword(s, ring_base + (get & NV11_DMA_RING_MASK));
        uint32_t count, sub, reg, j;

        get += 4;

        if (hdr == 0) {
            continue;                   /* NOP */
        }
        if (hdr == 0x20000000) {
            get = 0;                    /* JUMP to ring start */
            continue;
        }

        count = (hdr >> 18) & 0x3F;     /* Data dwords */
        sub   = (hdr >> 13) & 0x7;      /* Subchannel 0..7 */
        reg   = hdr & 0x1FFF;           /* Method byte offset */

        for (j = 0; j < count; j++) {
            uint32_t val = nv11_vram_read_dword(s,
                ring_base + (get & NV11_DMA_RING_MASK));
            get += 4;
            if (reg == 0 && (val & 0x80000000)) {
                trace_nv11_fifo_dma_bind(eip, sub, val);
                continue;
            }
            trace_nv11_fifo_dma_method(eip, sub, reg + j * 4, val);
            nv11_2d_method(s, sub, reg + j * 4, val);
        }
    }

    s->fifo[chan].dma_get = get;
    trace_nv11_fifo_dma_idle(eip, chan, get);
}

uint64_t nv11_fifo_read(NV11State *s, hwaddr offset, unsigned size)
{
    uint32_t off = (uint32_t)offset;
    uint32_t chan = off / NV11_FIFO_CHAN_SIZE;
    uint32_t reg  = off % NV11_FIFO_CHAN_SIZE;
    uint32_t eip = nv11_get_eip();
    uint64_t val = 0;

    if (chan >= NV11_FIFO_CHANNELS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [FIFO] read beyond channels at 0x%x\n", off);
        return 0;
    }

    if (reg >= NV11_FIFO_FREE_OFF && reg < NV11_FIFO_FREE_OFF + 4) {
        switch (size) {
        case 1:
            val = (s->fifo[chan].fifo_free >>
                   (8 * (reg - NV11_FIFO_FREE_OFF))) & 0xFF;
            break;
        case 2:
            val = (s->fifo[chan].fifo_free >>
                   (8 * (reg - NV11_FIFO_FREE_OFF))) & 0xFFFF;
            break;
        case 4:
            val = s->fifo[chan].fifo_free;   /* Upper 16 bits = NOP = 0 */
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "NV11: [FIFO] subch%u bad FifoFree read size %u\n",
                          chan, size);
            return 0;
        }
        trace_nv11_fifo_read(eip, off, size);
        trace_nv11_fifo_free(eip, chan, s->fifo[chan].fifo_free);
        return val;
    }

    val = nv11_fifo_shadow_read(s, chan, reg, size);

    if (reg == NV11_FIFO_DMA_GET_OFF && size == 4) {
        val = s->fifo[chan].dma_get;
        trace_nv11_fifo_dma_get(eip, chan, val);
    }

    trace_nv11_fifo_read(eip, off, size);
    return val;
}

void nv11_fifo_write(NV11State *s, hwaddr offset, uint64_t val, unsigned size)
{
    uint32_t off = (uint32_t)offset;
    uint32_t chan = off / NV11_FIFO_CHAN_SIZE;
    uint32_t reg  = off % NV11_FIFO_CHAN_SIZE;
    uint32_t eip = nv11_get_eip();

    trace_nv11_fifo_write(eip, off, size, (uint32_t)val);

    if (chan >= NV11_FIFO_CHANNELS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [FIFO] write beyond channels at 0x%x\n", off);
        return;
    }

    /* FIFOFree is read-only; NOP is a no-op */
    if (reg >= NV11_FIFO_FREE_OFF && reg < NV11_FIFO_FREE_OFF + 4) {
        return;
    }

    /* Context bindings (e.g. 0x80000000 subch0 -> RAMIN0) are stored,
     * not executed. */
    if (reg < NV11_FIFO_METHOD_OFF) {
        if (reg == NV11_FIFO_CONTEXT_OFF && size == 4) {
            trace_nv11_fifo_context(eip, chan, (uint32_t)val);
        }
        if (reg == NV11_FIFO_DMA_PUT_OFF && size == 4) {
            nv11_fifo_shadow_write(s, chan, reg, val, size);
            nv11_fifo_dma_push(s, chan, (uint32_t)val);
            return;
        }
        nv11_fifo_shadow_write(s, chan, reg, val, size);
        return;
    }

    /* Method write: retain, consume FIFO, knock PGRAPH busy */
    nv11_fifo_shadow_write(s, chan, reg, val, size);
    trace_nv11_fifo_method(eip, chan, reg, (uint32_t)val);

    s->fifo[chan].pending += (size + 3) / 4;
    if (s->fifo[chan].fifo_free >= (uint16_t)size) {
        s->fifo[chan].fifo_free -= size;
    } else {
        s->fifo[chan].fifo_free = 0;
    }
    trace_nv11_fifo_free(eip, chan, s->fifo[chan].fifo_free);

    s->pgraph_busy = true;
    trace_nv11_fifo_busy_set(eip);

    nv11_2d_method(s, chan, reg, (uint32_t)val);

    timer_mod(s->fifo_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NV11_FIFO_DRAIN_NS);
}

void nv11_fifo_reset(NV11State *s)
{
    int i;

    for (i = 0; i < NV11_FIFO_CHANNELS; i++) {
        memset(s->fifo[i].methods, 0, sizeof(s->fifo[i].methods));
        s->fifo[i].fifo_free = NV11_FIFO_FULL;
        s->fifo[i].pending = 0;
        s->fifo[i].dma_get = 0;
    }
    s->pgraph_busy = false;

    nv11_2d_reset(s);

    if (s->fifo_timer) {
        timer_del(s->fifo_timer);
    }
}

void nv11_fifo_init(NV11State *s)
{
    nv11_fifo_reset(s);

    s->fifo_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nv11_fifo_drain, s);

    trace_nv11_fifo_init();
}