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
#include "hw/pci/pci.h"
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

/* Resolve a bind handle to its object class through the RAMIN instance
 * table.  NV4-era drivers record every allocated handle as dword0 of a
 * table slot (0x9008, 0x9010, ...) with dword1 = 0x8000 | obj_index, but
 * do not arrange slots by the handle value, so the table is scanned.
 * The object class sits in the low 12 bits of the object's first word.
 * A null instance (dword1 obj == 0 with empty object at 0x700000, e.g.
 * placeholder 0x9020 -> 0x80000000) means unbound: return 0. */
static uint32_t nv11_fifo_find_obj(NV11State *s, uint32_t handle, int pass)
{
    uint32_t i;

    for (i = 0; i < 0x800; i++) {
        uint32_t tab = NV11_RAMIN_INST_TABLE + i * 8;
        uint32_t uh, objw;

        if (tab + 8 > NV11_BAR0_SIZE) {
            break;
        }
        uh = ldl_le_p(s->bar0_flat + tab);
        if (!uh || (pass == 0 ? uh != handle :
                                (uh & 0x7F) != (handle & 0x7F))) {
            continue;
        }
        objw = ldl_le_p(s->bar0_flat + tab + 4) & 0x1FFF;
        tab = NV11_RAMIN_OBJ_BASE + (objw << 4);
        if (tab + 4 > NV11_BAR0_SIZE) {
            return 0;
        }
        return tab;
    }
    return 0;
}

static uint16_t nv11_fifo_ramin_class(NV11State *s, uint32_t handle)
{
    int pass;
    uint32_t tab;

    if (!handle) {
        return 0;
    }

    /* First pass: exact handle match.  Second pass: PIO-style
     * instance-only binds (nv11_fifo_write passes cv & 0x7F). */
    for (pass = 0; pass < 2; pass++) {
        tab = nv11_fifo_find_obj(s, handle, pass);
        if (tab) {
            if (!ldl_le_p(s->bar0_flat + tab)) {
                return 0;
            }
            return ldl_le_p(s->bar0_flat + tab) & 0xFFF;
        }
    }
    return 0;
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
    /* Same linear instance-table scan as binds: the old
     * (inst ^ 0x10) direct index does not match the driver's
     * layout (e.g. 0x9002 at 0x710080, not at (0x02^0x10)*8). */
    uint32_t tab = nv11_fifo_find_obj(s, handle, 0);

    if (!tab || tab + 12 > NV11_BAR0_SIZE ||
        !ldl_le_p(s->bar0_flat + tab)) {
        return 0;
    }
    return ldl_le_p(s->bar0_flat + tab + 8) & 0xFFFFF000;
}

static void nv11_fifo_drain(void *opaque)
{
    NV11State *s = opaque;
    uint32_t eip = nv11_get_eip();
    int i;

    for (i = 0; i < NV11_FIFO_CHANNELS; i++) {
        if (s->fifo[i].pending) {
            s->fifo[i].pending = 0;
            s->fifo[i].fifo_free = NV11_FIFO_FULL;
        }
    }

    if (s->pgraph_busy) {
        s->pgraph_busy = false;
        trace_nv11_pgraph_idle(eip);
        /* A context switch only completes genuine executed work. A kick
         * with nothing buffered (the ISR's unconditional re-kick) must not
         * re-raise CS, or the driver would never exit its ISR. */
        if (s->pgraph_work) {
            s->pgraph_work = 0;
            nv11_pgraph_notify_cs(s);
        }
    }
}

/* PGRAPH_FIFO kick (bit0=1): begin executing the methods fed so far. If
 * nothing is buffered (busy or work leftover from a ring feed), the kick is
 * a no-op - exactly as hardware treats an empty FIFO. */
void nv11_fifo_kick(NV11State *s)
{
    uint32_t eip = nv11_get_eip();

    if (s->pgraph_busy || s->pgraph_work) {
        s->pgraph_busy = true;
        trace_nv11_fifo_busy_set(eip);
        timer_mod(s->fifo_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NV11_FIFO_DRAIN_NS);
    }
}

/* Read a dword from VRAM at byte offset (for the DMA pusher). */
static inline uint32_t nv11_vram_read_dword(NV11State *s, uint32_t off)
{
    if (off + 4 > s->vga.vram_size) {
        return 0;
    }
    return ldl_le_p(s->vram_ptr + off);
}

/* RAMFC base address: PFIFO[0x2214] << 8, within the BAR0 instance RAM
 * window the guest initialises its channel context into. */
static uint32_t nv11_fifo_ramfc_base(NV11State *s)
{
    uint32_t reg = ldl_le_p(s->bar0_flat + NV11_PFIFO_RAMFC);
    uint32_t base = reg << 8;

    if (base >= 0x100000 && base + 0x100 < NV11_BAR0_SIZE) {
        return base;
    }
    return 0;
}

/*
 * Resolve the channel's pushbuffer through the DMA object named by the
 * RAMFC/CACHE1 DMA_INSTANCE pointer: class 0x3D (NV_DMA_IN_MEMORY) or 0x02
 * (NV_CLASS_DMA_FROM_MEMORY):
 *   word0: bits 11:0 = class, bit12/13 = PT present/linear, bit14/15 =
 *          access, bits 17:16 = TARGET (0 NVM / 2 PCI / 3 AGP),
 *          bits 31:20 = base[11:0]
 *   word1: DMA_LIMIT (object size - 1)
 *   word2: base[31:12] (plus RW flag bit 1)
 * The pusher reads the pushbuffer at object base + dma_get.
 */
static void nv11_fifo_dma_object(NV11State *s, uint32_t chan)
{
    uint32_t eip = nv11_get_eip();
    uint32_t ramfc = nv11_fifo_ramfc_base(s);
    uint32_t inst = 0, pinst, obj, w0, w1, w2;

    if (ramfc) {
        inst = ldl_le_p(s->bar0_flat + ramfc + 0x0C);   /* RAMFC DMA_INSTANCE */
    }
    if (!inst) {
        inst = ldl_le_p(s->bar0_flat + NV11_PFIFO_CACHE1_DMA_INSTANCE);
    }
    s->fifo[chan].obj_inst = inst;

    s->fifo[chan].obj_valid  = true;
    s->fifo[chan].obj_target = 0;                 /* NVM */
    s->fifo[chan].obj_base   = 0;                 /* identity VRAM map */
    s->fifo[chan].obj_limit  = s->vga.vram_size - 1;

    if (!inst) {
        return;
    }

    pinst = (inst & 0xFFFF) << 4;
    obj = NV11_RAMIN_OBJ_BASE + pinst;
    if (obj + 12 > NV11_BAR0_SIZE) {
        return;
    }

    w0 = ldl_le_p(s->bar0_flat + obj);
    w1 = ldl_le_p(s->bar0_flat + obj + 4);
    w2 = ldl_le_p(s->bar0_flat + obj + 8);

    if ((w0 & 0xFFF) != NV11_CLASS_DMA_IN_MEMORY &&
        (w0 & 0xFFF) != NV11_CLASS_DMA_FROM_MEMORY) {
        return;
    }

    s->fifo[chan].obj_target = (w0 >> 16) & 3;
    s->fifo[chan].obj_base   = ((w0 >> 20) & 0xFFF) | (w2 & 0xFFFFF000);
    s->fifo[chan].obj_limit  = w1 ? w1 : s->vga.vram_size - 1;
    trace_nv11_fifo_dma_object(eip, chan, w0 & 0xFFFF,
                               s->fifo[chan].obj_target,
                               s->fifo[chan].obj_base,
                               s->fifo[chan].obj_limit);
}

/* Translate a dma_get through the pushbuffer object and load a word.
 * Target 0 reads VRAM (NVM identity); target 2/3 (PCI, PCI-no-snoop/AGP)
 * reads guest memory through the PCI DMA aperture. */
static uint32_t nv11_fifo_dma_read(NV11State *s, uint32_t chan, uint32_t addr)
{
    uint32_t base = s->fifo[chan].obj_base + addr;
    uint8_t b[4];

    switch (s->fifo[chan].obj_target) {
    case 0:
        return nv11_vram_read_dword(s, base);
    case 2:   /* NV_MEM_TARGET_PCI */
    case 3:   /* NV_MEM_TARGET_PCI_NOSNOOP */
        if (pci_dma_read(&s->parent_dev, base, b, 4)) {
            return 0;
        }
        return ldl_le_p(b);
    default:
        return 0;
    }
}

/* On the first submission after (re)initialisation, take the channel's
 * starting GET: first from the driver's RAMFC image (DMA_GET at +0x04),
 * else from CACHE1_GET, else from the ring start (0).  Seeding GET=PUT
 * would silently drop the first batch (0..PUT) if it holds real commands;
 * leading padding decodes as no-op increasing-methods headers anyway.
 * Empty submissions (put==0) never consume the seed. */
static void nv11_fifo_dma_seed(NV11State *s, uint32_t chan, uint32_t put)
{
    uint32_t ramfc, g = 0;

    if (s->fifo[chan].dma_seeded || !put) {
        return;
    }

    ramfc = nv11_fifo_ramfc_base(s);
    if (ramfc) {
        g = ldl_le_p(s->bar0_flat + ramfc + 0x04);
    }
    if (!g) {
        g = ldl_le_p(s->bar0_flat + NV11_PFIFO_CACHE1_DMA_GET);
        if (g == put) {
            g = 0;
        }
    }
    /* g==0 means ring start; a stale non-zero GET below PUT is honoured,
     * otherwise also start at 0 so the first batch is not skipped. */
    if (g && g != put) {
        s->fifo[chan].dma_get = g;
    } else {
        s->fifo[chan].dma_get = 0;
    }
    s->fifo[chan].dma_seeded = true;
}

/* Fault context: report the pusher error with the faulting address. */
static void nv11_dma_err_probe(NV11State *s, uint32_t eip, uint32_t chan,
                               uint32_t addr, uint32_t err)
{
    uint32_t d[6] = { 0 };

    trace_nv11_dma_err(eip, chan, err, addr, d[0], d[1], d[2], d[3],
                       d[4], d[5]);
}

/*
 * DMA pusher:
 * while dma_get != dma_put, read the command word from the pushbuffer DMA
 * object at dma_get (identity VRAM map -> VRAM[get]), then either consume a
 * data word of the active methods command or decode a new command:
 * old jump 0x20000000, new jump ..1, call ..2, return 0x00020000,
 * increasing methods (0x00000000) and non-increasing methods (0x40000000)
 * headers. Method 0 is the object-binding pulse. DMA_GET is
 * advanced after each word and written back, so the driver's DMA_GET
 * polls at 0x800044 see the pusher progress exactly as on hardware.
 */
void nv11_fifo_dma_push(NV11State *s, uint32_t chan, uint32_t put)
{
    uint32_t eip = nv11_get_eip();
    uint32_t get, st, sub, start_get;
    uint32_t iters = 0;

    /* Always re-resolve: the CACHE1 DMA_INSTANCE may be armed only after
     * the first (empty) submission pokes (< 0x322C write), and the object
     * must then be re-parsed rather than kept as the NVM-identity default. */
    nv11_fifo_dma_object(s, chan);
    nv11_fifo_dma_seed(s, chan, put);
    get = s->fifo[chan].dma_get;
    st  = s->fifo[chan].dma_state;
    sub = s->fifo[chan].subr_ret;
    start_get = get;

    trace_nv11_fifo_dma_push(eip, chan, put, s->fifo[chan].obj_base, get);

    if (put == 0) {
        /* An empty DMA_PUT write is the driver's idle/teardown poke.  With
         * get != put there is *nothing submitted*, and running the pusher
         * here would walk linearly past the end of the ring into unrelated
         * system memory, spinning forever decoding garbage "methods".
         * Real hardware idles instead. */
        trace_nv11_fifo_dma_push_end(eip, chan, get, 0, sub);
        return;
    }

    while (get != put && iters++ < NV11_DMA_MAX_ITERS) {
        uint32_t mthd, subc, mcnt, word;

        if (get >= s->fifo[chan].obj_limit) {
            st = (st & ~NV11_DMA_STATE_ERROR_MASK) |
                 (NV11_DMA_PUSHER_ERR_MEM_FAULT << NV11_DMA_STATE_ERROR_SHIFT);
            nv11_dma_err_probe(s, eip, chan, get,
                               NV11_DMA_PUSHER_ERR_MEM_FAULT);
            break;
        }

        word = nv11_fifo_dma_read(s, chan, get);
        get += 4;
        s->pgraph_work++;

        if (st & NV11_DMA_STATE_MCNT_MASK) {
            /* Data word of the active methods command. */
            mthd = (st & NV11_DMA_STATE_METHOD_MASK) >>
                   NV11_DMA_STATE_METHOD_SHIFT;
            subc = (st & NV11_DMA_STATE_SUBCH_MASK) >>
                   NV11_DMA_STATE_SUBCH_SHIFT;

            if (mthd == 0) {
                uint16_t cls = nv11_fifo_ramin_class(s, word);
                trace_nv11_fifo_dma_bind(eip, subc, word, cls);
                s->ch_class[subc] = cls;
            } else {
                trace_nv11_fifo_dma_method(eip, subc, mthd << 2, word);
                nv11_2d_method(s, subc, mthd << 2, word);
            }

            if (!(st & NV11_DMA_STATE_NONINC)) {
                st += 1 << NV11_DMA_STATE_METHOD_SHIFT;
            }
            st -= 1 << NV11_DMA_STATE_MCNT_SHIFT;
            continue;
        }

        /* First word of a new command. */
        if ((word & 0xE0000003) == 0x20000000) {
            get = word & 0x1FFFFFFF;                 /* old jump */
        } else if ((word & 3) == 1) {
            get = word & 0xFFFFFFFC;                 /* jump */
        } else if ((word & 3) == 2) {
            if (sub & NV11_SUBROUTINE_ACTIVE) {
                st = (st & ~NV11_DMA_STATE_ERROR_MASK) |
                     (NV11_DMA_PUSHER_ERR_CALL_SUBR  <<
                      NV11_DMA_STATE_ERROR_SHIFT);
                break;
            }
            sub = NV11_SUBROUTINE_ACTIVE | (get & 0xFFFFFFFC);
            get = word & 0xFFFFFFFC;                 /* call */
        } else if (word == 0x00020000) {
            if (!(sub & NV11_SUBROUTINE_ACTIVE)) {
                st = (st & ~NV11_DMA_STATE_ERROR_MASK) |
                     (NV11_DMA_PUSHER_ERR_RET_SUBR <<
                      NV11_DMA_STATE_ERROR_SHIFT);
                break;
            }
            get = sub & 0xFFFFFFFC;
            sub = 0;                                 /* return */
        } else if ((word & 0xE0030003) == 0) {
            mthd = (word >> 2) & 0x7FF;              /* increasing methods */
            subc = (word >> 13) & 7;
            mcnt = (word >> 18) & 0x7FF;
            st = (mcnt << NV11_DMA_STATE_MCNT_SHIFT) |
                 (subc << NV11_DMA_STATE_SUBCH_SHIFT) |
                 (mthd << NV11_DMA_STATE_METHOD_SHIFT);
        } else if ((word & 0xE0030003) == 0x40000000) {
            mthd = (word >> 2) & 0x7FF;              /* non-increasing */
            subc = (word >> 13) & 7;
            mcnt = (word >> 18) & 0x7FF;
            st = NV11_DMA_STATE_NONINC |
                 (mcnt << NV11_DMA_STATE_MCNT_SHIFT) |
                 (subc << NV11_DMA_STATE_SUBCH_SHIFT) |
                 (mthd << NV11_DMA_STATE_METHOD_SHIFT);
        } else {
            st = (st & ~NV11_DMA_STATE_ERROR_MASK) |
                 (NV11_DMA_PUSHER_ERR_INVALID_CMD <<
                  NV11_DMA_STATE_ERROR_SHIFT);
            nv11_dma_err_probe(s, eip, chan, get,
                               NV11_DMA_PUSHER_ERR_INVALID_CMD);
            break;
        }
    }

    s->fifo[chan].dma_get = get;
    s->fifo[chan].dma_put = put;
    s->fifo[chan].dma_state = st;
    s->fifo[chan].subr_ret = sub;
    stl_le_p(s->bar0_flat + NV11_PFIFO_CACHE1_DMA_GET, get);

    trace_nv11_fifo_dma_push_end(eip, chan, get,
                                 (st & NV11_DMA_STATE_ERROR_MASK) >>
                                 NV11_DMA_STATE_ERROR_SHIFT, sub);

    /* Keep the completion handshake that worked for PIO: the engine stays
     * busy until the drain timer clears it and raises the context-switch
     * interrupt the driver waits on after a submit. */
    if (get != start_get) {
        s->pgraph_busy = true;
        trace_nv11_fifo_busy_set(eip);

        timer_mod(s->fifo_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NV11_FIFO_DRAIN_NS);
    }
}

uint64_t nv11_fifo_read(NV11State *s, hwaddr offset, unsigned size)
{
    uint32_t off = (uint32_t)offset;
    uint32_t chan = off / NV11_FIFO_CHAN_SIZE;
    uint32_t reg  = off % NV11_FIFO_CHAN_SIZE;
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
        return val;
    }

    val = nv11_fifo_shadow_read(s, chan, reg, size);

    if (reg == NV11_FIFO_DMA_GET_OFF && size == 4) {
        val = s->fifo[chan].dma_get;
    }

    return val;
}

void nv11_fifo_write(NV11State *s, hwaddr offset, uint64_t val, unsigned size)
{
    uint32_t off = (uint32_t)offset;
    uint32_t chan = off / NV11_FIFO_CHAN_SIZE;
    uint32_t reg  = off % NV11_FIFO_CHAN_SIZE;
    uint32_t eip = nv11_get_eip();

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

    s->fifo[chan].pending += (size + 3) / 4;
    if (s->fifo[chan].fifo_free >= (uint16_t)size) {
        s->fifo[chan].fifo_free -= size;
    } else {
        s->fifo[chan].fifo_free = 0;
    }

    s->pgraph_busy = true;
    trace_nv11_fifo_busy_set(eip);
    s->pgraph_work++;

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
        s->fifo[i].dma_put = 0;
        s->fifo[i].dma_state = 0;
        s->fifo[i].subr_ret = 0;
        s->fifo[i].obj_base = 0;
        s->fifo[i].obj_limit = 0;
        s->fifo[i].obj_target = 0;
        s->fifo[i].obj_inst = 0;
        s->fifo[i].obj_valid = false;
        s->fifo[i].dma_seeded = false;
        s->ch_class[i] = 0;
    }
    s->pgraph_busy = false;
    s->pgraph_work = 0;

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
