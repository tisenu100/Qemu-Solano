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
#include "qemu/bswap.h"
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

static uint16_t nv11_fifo_ramin_class(NV11State *s, uint32_t instance)
{
    uint32_t tab = NV11_RAMIN_INST_TABLE + ((instance & 0x7F) ^ 0x10) * 8;
    uint32_t obj;

    if (tab + 8 > NV11_BAR0_SIZE) {
        return 0;
    }
    obj = ldl_le_p(s->bar0_flat + tab + 4) & 0x7FFF;
    tab = NV11_RAMIN_OBJ_BASE + (obj << 4);
    if (tab + 4 > NV11_BAR0_SIZE ||
        !ldl_le_p(s->bar0_flat + tab)) {
        return 0;
    }
    /* 9 bits: catches the 0x100 NOP / 0x104 NOTIFY objects whose low
     * byte would otherwise collide with an unbound class. */
    return ldl_le_p(s->bar0_flat + tab) & 0x1FF;
}

/*
 * Resolve a context-style handle (e.g. a DMA_NOTIFY / DMA_BUFFER_IN /
 * DMA_BUFFER_OUT reference) to the base address of the DMA object it names.
 * The RAMIN instance table maps the handle to a word pointer, and the object
 * at that pointer holds {flags, limit, address, term}. The classic in-memory
 * FB map keep its address (frame) in dword[2]. Unknown handles yield 0,
 * which on a real NV11 is the start of VRAM as well, so callers clamp.
 */
uint32_t nv11_fifo_dma_frame(NV11State *s, uint32_t handle)
{
    uint32_t inst = handle & 0x7F;
    uint32_t tab = NV11_RAMIN_INST_TABLE + ((inst ^ 0x10) * 8);
    uint32_t obj, base;

    if (tab + 8 > NV11_BAR0_SIZE) {
        return 0;
    }
    obj = ldl_le_p(s->bar0_flat + tab + 4) & 0x7FFF;
    base = NV11_RAMIN_OBJ_BASE + (obj << 4);
    if (base + 12 > NV11_BAR0_SIZE ||
        !ldl_le_p(s->bar0_flat + base)) {
        return 0;
    }
    return ldl_le_p(s->bar0_flat + base + 8) & 0xFFFFF000;
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
    uint32_t get  = s->fifo[chan].dma_get & NV11_DMA_RING_MASK;
    uint32_t putm = put & NV11_DMA_RING_MASK;
    uint32_t i;

    trace_nv11_fifo_dma_push(eip, chan, put, ring_base, get);

    /* Raw ring peek: tells empty-ring (BAR1 coherency) apart from
     * header-decode mismatch. Throttled like the drain timer path. */
    {
        static uint32_t dbg_n;
        if ((dbg_n++ % 25) == 0) {
            uint32_t g = get;
            uint32_t d[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            for (i = 0; g != putm && i < 8; i++) {
                d[i] = nv11_vram_read_dword(s, ring_base + g);
                g = (g + 4) & NV11_DMA_RING_MASK;
            }
            trace_nv11_fifo_dma_ring0(eip, chan, get, putm,
                                      d[0], d[1], d[2], d[3]);
            trace_nv11_fifo_dma_ring1(eip, chan,
                                      d[4], d[5], d[6], d[7]);
        }
    }

    /* DMA_GET/DMA_PUT are byte offsets relative to the ring base and wrap
     * modulo the ring size. */
    for (i = 0; get != putm && i < NV11_DMA_RING_SIZE / 4; i++) {
        uint32_t hdr = nv11_vram_read_dword(s, ring_base + get);
        uint32_t count, sub, reg, j;

        get = (get + 4) & NV11_DMA_RING_MASK;

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
            uint32_t val = nv11_vram_read_dword(s, ring_base + get);
            get = (get + 4) & NV11_DMA_RING_MASK;
            if (reg == 0 && (val & 0x80000000)) {
                trace_nv11_fifo_dma_bind(eip, sub, val);
                s->ch_class[sub] = nv11_fifo_ramin_class(s, val & 0x7F);
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

    /* Context bindings (e.g. 0x80000000 subch0 -> RAMIN0) select the object
     * class for this subchannel; method writes dispatch on that class. */
    if (reg < NV11_FIFO_METHOD_OFF) {
        if (reg == NV11_FIFO_CONTEXT_OFF && size == 4) {
            uint32_t cv = (uint32_t)val;

            trace_nv11_fifo_context(eip, chan, cv);
            s->ch_class[chan] = (cv & 0x80000000)
                                ? nv11_fifo_ramin_class(s, cv & 0x7F) : 0;
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
        s->ch_class[i] = 0;
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