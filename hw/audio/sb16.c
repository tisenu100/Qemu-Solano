/*
 * QEMU Soundblaster 16 emulation
 *
 * Copyright (c) 2003-2005 Vassili Karpov (malc)
 *
 * Modified by re9177 2025-2026.
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
#include "hw/audio/model.h"
#include "qemu/audio.h"
#include "hw/core/irq.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "ymf262.h"

#define DEBUG 0
/* #define DEBUG_SB16_MOST */

#define ldebug(fmt, ...) do { \
        if (DEBUG) { \
            error_report("sb16: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

static const char e3[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

#define TYPE_SB16 "sb16"
OBJECT_DECLARE_SIMPLE_TYPE(SB16State, SB16)

struct SB16State {
    ISADevice parent_obj;

    AudioBackend *audio_be;
    qemu_irq pic;
    uint32_t irq;
    uint32_t dma;
    uint32_t hdma;
    uint32_t port;
    uint32_t ver;
    IsaDma *isa_dma;
    IsaDma *isa_hdma;

    int in_index;
    int out_data_len;
    int fmt_stereo;
    int fmt_signed;
    int fmt_bits;
    AudioFormat fmt;
    int dma_auto;
    int block_size;
    int fifo;
    int freq;
    int time_const;
    int speaker;
    int needed_bytes;
    int cmd;
    int use_hdma;
    int highspeed;
    int can_write;

    int v2x6;

    uint8_t csp_param;
    uint8_t csp_value;
    uint8_t csp_mode;
    uint8_t csp_regs[256];
    uint8_t csp_index;
    uint8_t csp_reg83[4];
    int csp_reg83r;
    int csp_reg83w;

    uint8_t in2_data[10];
    uint8_t out_data[50];
    uint8_t test_reg;
    uint8_t last_read_byte;
    int nzero;

    int left_till_irq;

    int dma_running;
    int bytes_per_second;
    int align;
    int audio_free;
    SWVoiceOut *voice;
    
    int32_t adpcm_valpred;
    int32_t adpcm_index;

    void *ymf262;
    SWVoiceOut *voice_opl;
    int opl_ticking[2];
    uint64_t opl_dexp[2];
    QEMUAudioTimeStamp opl_ats;
    PortioList opl_portio_list;
    PortioList hack_portio_list;

    /* evil */
    PortioList mpu_portio_list;

    QEMUTimer *aux_ts;
    /* mixer state */
    int mixer_nreg;
    uint8_t mixer_regs[256];
    uint8_t e2_valadd;
    uint8_t e2_valxor;
    PortioList portio_list;
};

#define SAMPLE_RATE_MIN 5000
#define SAMPLE_RATE_MAX 49716

/* Get your FREE tables! */
static const uint8_t sb16_log_vol[32] = {
    0,   2,   5,   8,   12,  16,  20,  25,
    31,  38,  46,  54,  63,  73,  84,  96,
    108, 122, 136, 152, 168, 185, 203, 222,
    242, 255, 255, 255, 255, 255, 255, 255
};

static void sb16_update_voice_volume(SB16State *s)
{
    if (!s->voice) return;

    int ml_idx = (s->mixer_regs[0x30] >> 3) & 0x1f;
    int mr_idx = (s->mixer_regs[0x31] >> 3) & 0x1f;
    int vl_idx = (s->mixer_regs[0x32] >> 3) & 0x1f;
    int vr_idx = (s->mixer_regs[0x33] >> 3) & 0x1f;

    Volume vol;
    vol.mute = 0;
    vol.channels = 2;

    vol.vol[0] = (sb16_log_vol[ml_idx] * sb16_log_vol[vl_idx] * 192) / 65025;
    vol.vol[1] = (sb16_log_vol[mr_idx] * sb16_log_vol[vr_idx] * 192) / 65025;

    AUD_set_volume_out(s->voice, &vol);
}

static void sb16_update_opl_volume(SB16State *s)
{
    int ml_idx = (s->mixer_regs[0x30] >> 3) & 0x1f;
    int mr_idx = (s->mixer_regs[0x31] >> 3) & 0x1f;
    int fl_idx = (s->mixer_regs[0x34] >> 3) & 0x1f;
    int fr_idx = (s->mixer_regs[0x35] >> 3) & 0x1f;

    int32_t vol_l = (sb16_log_vol[ml_idx] * sb16_log_vol[fl_idx] * 0x8000) / 65025;
    int32_t vol_r = (sb16_log_vol[mr_idx] * sb16_log_vol[fr_idx] * 0x8000) / 65025;

    if (s->ymf262) {
        ymf262_set_vol_lr(s->ymf262, vol_l, vol_r);
    }
}

static void sb16_opl_callback(void *opaque, int free)
{
    SB16State *s = opaque;
    int samples = free >> 2;
    
    if (!s->ymf262 || !s->voice_opl) {
        return;
    }

    if (samples > 0 && s->ymf262) {
        int bytes = samples * 4;
        DEV_SMPL *buf_l = g_new(DEV_SMPL, samples);
        DEV_SMPL *buf_r = g_new(DEV_SMPL, samples);
        DEV_SMPL *bufs[2] = { buf_l, buf_r };
        int16_t *interleaved = g_malloc(bytes);

        ymf262_update_one(s->ymf262, samples, bufs);

        for (int i = 0; i < samples; i++) {
            interleaved[i * 2 + 0] = (int16_t)buf_l[i];
            interleaved[i * 2 + 1] = (int16_t)buf_r[i];
        }

        AUD_write(s->voice_opl, interleaved, bytes);
        g_free(buf_l);
        g_free(buf_r);
        g_free(interleaved);
    }
}

static void sb16_opl_timer_handler(void *opaque, UINT8 c, UINT32 period)
{
    SB16State *s = opaque;
    int64_t fuck = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    uint64_t interval_ns = (uint64_t)((double)period * NANOSECONDS_PER_SECOND / 49716.0);
    unsigned n = c & 1;

    if (interval_ns == 0) {
        s->opl_ticking[n] = 0;
        return;
    }

    s->opl_ticking[n] = 1;
    s->opl_dexp[n] = fuck + interval_ns;
}

static void sb16_opl_write(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;
    uint32_t a = nport & 3;
    
    if ((nport & 0xf00) != 0x300) {
        if ((nport & 0xF) == 8) a = 0;
	if ((nport & 0xF) == 9) a = 1;
    }
    if (s->voice_opl) {
        AUD_set_active_out(s->voice_opl, 1);
    }
    ymf262_timer_over(s->ymf262, a); 
    ymf262_write(s->ymf262, a, val);
}

static uint32_t sb16_opl_read(void *opaque, uint32_t nport)
{
    SB16State *s = opaque;
    uint32_t a = nport & 3;
    if ((nport & 0xf00) != 0x300) {
        if ((nport & 0xF) == 8) a = 0;
        if ((nport & 0xF) == 9) a = 1;
    }
    ymf262_timer_over(s->ymf262, a); 
    return ymf262_read(s->ymf262, a);
}

static void mpu_write(void *opaque, uint32_t addr, uint32_t val)
{
    /* stub!! for now :) */
    ldebug("MPU-401 write addr 0x%x <- 0x%x", addr, val);
}

static uint32_t mpu_read(void *opaque, uint32_t addr)
{
    if ((addr & 1) == 1) {
        return 0x3F; /* FIXME: this is not good, what's being done is that the sound card is told that there's a device ready even if there isn't one, although this seems to satisfy the 1992 Win3.1 drivers so.. i guess it's fine then? */
    }

    return 0xff;
}

static void SB_audio_callback (void *opaque, int free);

static int magic_of_irq (int irq)
{
    switch (irq) {
    case 5:
        return 2;
    case 7:
        return 4;
    case 9:
        return 1;
    case 10:
        return 8;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bad irq %d\n", irq);
        return 2;
    }
}

static int irq_of_magic (int magic)
{
    switch (magic) {
    case 1:
        return 9;
    case 2:
        return 5;
    case 4:
        return 7;
    case 8:
        return 10;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bad irq magic %d\n", magic);
        return -1;
    }
}

static void hold_DREQ(SB16State *s, int nchan)
{
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);

    k->hold_DREQ(isa_dma, nchan);
}

static void release_DREQ(SB16State *s, int nchan)
{
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);

    k->release_DREQ(isa_dma, nchan);
}

#if 0
static void log_dsp (SB16State *dsp)
{
    ldebug("%s:%s:%d:%s:dmasize=%d:freq=%d:const=%d:speaker=%d",
            dsp->fmt_stereo ? "Stereo" : "Mono",
            dsp->fmt_signed ? "Signed" : "Unsigned",
            dsp->fmt_bits,
            dsp->dma_auto ? "Auto" : "Single",
            dsp->block_size,
            dsp->freq,
            dsp->time_const,
            dsp->speaker);
}
#endif

static void speaker (SB16State *s, int on)
{
    s->speaker = on;
    /*AUD_enable (s->voice, on);*/
}

static void control (SB16State *s, int hold)
{
    int nchan = s->use_hdma ? s->hdma : s->dma;
    s->dma_running = hold;

    //ldebug("hold %d high %d nchan %d\n", hold, s->use_hdma, nchan);

    if (hold) {
	if (!s->voice) {
        hold_DREQ(s, nchan);
	}
        AUD_set_active_out (s->voice, 1);
    }
    else {
        release_DREQ(s, nchan);
        AUD_set_active_out (s->voice, 0);
    }
}

static void aux_timer (void *opaque)
{
    SB16State *s = opaque;
    s->can_write = 1;
    qemu_irq_raise (s->pic);
}

#define DMA8_AUTO 1
#define DMA8_HIGH 2

static void continue_dma8 (SB16State *s)
{
    if (s->freq > 0) {
        struct audsettings as;

        s->audio_free = 0;

        as.freq = s->freq;
        as.nchannels = 1 << s->fmt_stereo;
        as.fmt = s->fmt;
        as.endianness = 0;

        s->voice = AUD_open_out (
            s->audio_be,
            s->voice,
            "sb16",
            s,
            SB_audio_callback,
            &as
            );
	sb16_update_voice_volume(s);
    }

    control (s, 1);
}

static inline int restrict_sampling_rate(int freq)
{
    if (freq < SAMPLE_RATE_MIN) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sampling range too low: %d, increasing to %u\n",
                      freq, SAMPLE_RATE_MIN);
        return SAMPLE_RATE_MIN;
    } else if (freq > SAMPLE_RATE_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sampling range too high: %d, decreasing to %u\n",
                      freq, SAMPLE_RATE_MAX);
        return SAMPLE_RATE_MAX;
    } else {
        return freq;
    }
}

static void dma_cmd8 (SB16State *s, int mask, int dma_len)
{
    s->fmt = AUDIO_FORMAT_U8;
    s->use_hdma = 0;
    s->fmt_bits = 8;
    s->fmt_signed = 0;
    s->fmt_stereo = (s->mixer_regs[0x0e] & 2) != 0;
    if (-1 == s->time_const) {
        if (s->freq <= 0)
            s->freq = 11025;
    }
    else {
        int tmp = (256 - s->time_const);
        s->freq = (1000000 + (tmp / 2)) / tmp;
    }
    s->freq = restrict_sampling_rate(s->freq);

    if (dma_len != -1) {
        s->block_size = dma_len << s->fmt_stereo;
    }
    else {
        /* This is apparently the only way to make both Act1/PL
           and SecondReality/FC work

           Act1 sets block size via command 0x48 and it's an odd number
           SR does the same with even number
           Both use stereo, and Creatives own documentation states that
           0x48 sets block size in bytes less one.. go figure */
        s->block_size &= ~s->fmt_stereo;
    }

    s->freq >>= s->fmt_stereo;
    s->left_till_irq = s->block_size;
    s->bytes_per_second = (s->freq << s->fmt_stereo);
    /* s->highspeed = (mask & DMA8_HIGH) != 0; */
    s->dma_auto = (mask & DMA8_AUTO) != 0;
    s->align = (1 << s->fmt_stereo) - 1;

    if (s->block_size & s->align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", s->block_size, s->align + 1);
    }

    ldebug("freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d",
            s->freq, s->fmt_stereo, s->fmt_signed, s->fmt_bits,
            s->block_size, s->dma_auto, s->fifo, s->highspeed);

    continue_dma8 (s);
    speaker (s, 1);
}

static void dma_cmd (SB16State *s, uint8_t cmd, uint8_t d0, int dma_len)
{
    s->use_hdma = cmd < 0xc0;
    s->fifo = (cmd >> 1) & 1;
    s->dma_auto = (cmd >> 2) & 1;
    s->fmt_signed = (d0 >> 4) & 1;
    s->fmt_stereo = (d0 >> 5) & 1;

    switch (cmd >> 4) {
    case 11:
        s->fmt_bits = 16;
        break;

    case 12:
        s->fmt_bits = 8;
        break;
    }

    if (-1 != s->time_const) {
#if 1
        int tmp = 256 - s->time_const;
        s->freq = (1000000 + (tmp / 2)) / tmp;
#else
        /* s->freq = 1000000 / ((255 - s->time_const) << s->fmt_stereo); */
        s->freq = 1000000 / ((255 - s->time_const));
#endif
        s->time_const = -1;
    }

    s->block_size = dma_len + 1;
    s->block_size <<= (s->fmt_bits == 16);
    if (!s->dma_auto) {
        /* It is clear that for DOOM and auto-init this value
           shouldn't take stereo into account, while Miles Sound Systems
           setsound.exe with single transfer mode wouldn't work without it
           wonders of SB16 yet again */
        s->block_size <<= s->fmt_stereo;
    }

    ldebug("freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d",
            s->freq, s->fmt_stereo, s->fmt_signed, s->fmt_bits,
            s->block_size, s->dma_auto, s->fifo, s->highspeed);

    if (16 == s->fmt_bits) {
        if (s->fmt_signed) {
            s->fmt = AUDIO_FORMAT_S16;
        }
        else {
            s->fmt = AUDIO_FORMAT_U16;
        }
    }
    else {
        if (s->fmt_signed) {
            s->fmt = AUDIO_FORMAT_S8;
        }
        else {
            s->fmt = AUDIO_FORMAT_U8;
        }
    }

    s->left_till_irq = s->block_size;

    s->bytes_per_second = (s->freq << s->fmt_stereo) << (s->fmt_bits == 16);
    s->highspeed = 0;
    s->align = (1 << (s->fmt_stereo + (s->fmt_bits == 16))) - 1;
    if (s->block_size & s->align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", s->block_size, s->align + 1);
    }

    if (s->freq) {
        struct audsettings as;

        s->audio_free = 0;

        as.freq = s->freq;
        as.nchannels = 1 << s->fmt_stereo;
        as.fmt = s->fmt;
        as.endianness = 0;

        s->voice = AUD_open_out (
            s->audio_be,
            s->voice,
            "sb16",
            s,
            SB_audio_callback,
            &as
            );
	sb16_update_voice_volume(s);
    }

    control (s, 1);
    speaker (s, 1);
}

static inline void dsp_out_data (SB16State *s, uint8_t val)
{
    ldebug("outdata 0x%x", val);
    if ((size_t) s->out_data_len < sizeof (s->out_data)) {
        s->out_data[s->out_data_len++] = val;
    }
}

static inline uint8_t dsp_get_data (SB16State *s)
{
    if (s->in_index) {
        return s->in2_data[--s->in_index];
    }
    else {
        warn_report("sb16: buffer underflow");
        return 0;
    }
}

static void command (SB16State *s, uint8_t cmd)
{
    ldebug("command 0x%x", cmd);

    if (cmd > 0xaf && cmd < 0xd0) {
        if (cmd & 8) {
            ldebug("ADC command 0x%x is being used!!", cmd);
        }
        s->needed_bytes = 3;

        switch (cmd >> 4) {
        case 11:
        case 12:
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "0x%x wrong bits\n", cmd);
        }
        s->needed_bytes = 3;
    }
    else {
        s->needed_bytes = 0;

        switch (cmd) {
        case 0x03:
            dsp_out_data (s, 0x10); /* s->csp_param); */
            goto warn;

        case 0x04:
            s->needed_bytes = 1;
            goto warn;

        case 0x05:
            s->needed_bytes = 2;
            goto warn;

        case 0x08:
            /* __asm__ ("int3"); */
            goto warn;

        case 0x0e:
            s->needed_bytes = 2;
            goto warn;

        case 0x09:
            dsp_out_data (s, 0xf8);
            goto warn;

        case 0x0f:
            s->needed_bytes = 1;
            goto warn;

        case 0x10:
            s->needed_bytes = 1;
            break;

        case 0x14:
            s->needed_bytes = 2;
            s->block_size = 0;
            break;

        case 0x1c:              /* Auto-Initialize DMA DAC, 8-bit */
            dma_cmd8 (s, DMA8_AUTO, -1);
            break;

        case 0x20:              /* Direct ADC, Juice/PL */
            dsp_out_data (s, 0xff);
            goto warn;

        case 0x35:
            qemu_log_mask(LOG_UNIMP, "0x35 - MIDI command not implemented\n");
            break;

        case 0x40:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 1;
            break;

        case 0x41:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 2;
            break;

        case 0x42:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 2;
            goto warn;

        case 0x45:
            dsp_out_data (s, 0xaa);
            goto warn;

        case 0x47:                /* Continue Auto-Initialize DMA 16bit */
            break;

        case 0x48:
            s->needed_bytes = 2;
            break;

        case 0x74:
        case 0x75:              /* DMA DAC, 4-bit ADPCM Reference */
        case 0x76:              /* DMA DAC, 2.6-bit ADPCM */
        case 0x77:              /* DMA DAC, 2.6-bit ADPCM Reference */
            s->needed_bytes = 2;
            break;

        case 0x7d:
            qemu_log_mask(LOG_UNIMP, "0x7d - Auto-Initialize DMA DAC, 4-bit"
                          " ADPCM Reference\n");
            qemu_log_mask(LOG_UNIMP, "not implemented\n");
            break;

        case 0x7f:
            qemu_log_mask(LOG_UNIMP, "0x7d - Auto-Initialize DMA DAC, 2.6-bit"
                          " ADPCM Reference\n");
            qemu_log_mask(LOG_UNIMP, "not implemented\n");
            break;

        case 0x80:
            s->needed_bytes = 2;
            break;

        case 0x90:
        case 0x91:
            dma_cmd8 (s, ((cmd & 1) == 0) | DMA8_HIGH, -1);
            break;

        case 0xd0:              /* halt DMA operation. 8bit */
            control (s, 0);
            break;

        case 0xd1:              /* speaker on */
            speaker (s, 1);
            break;

        case 0xd3:              /* speaker off */
            speaker (s, 0);
            break;

        case 0xd4:              /* continue DMA operation. 8bit */
            /* KQ6 (or maybe Sierras audblst.drv in general) resets
               the frequency between halt/continue */
            continue_dma8 (s);
            break;

        case 0xd5:              /* halt DMA operation. 16bit */
            control (s, 0);
            break;

        case 0xd6:              /* continue DMA operation. 16bit */
            control (s, 1);
            break;

	case 0xd8:              /* Get speaker status */
            dsp_out_data(s, s->speaker ? 0xff : 0x00);
            break;

        case 0xd9:              /* exit auto-init DMA after this block. 16bit */
            s->dma_auto = 0;
            break;

        case 0xda:              /* exit auto-init DMA after this block. 8bit */
            s->dma_auto = 0;
            break;

        case 0xe0:              /* DSP identification */
            s->needed_bytes = 1;
	    s->out_data_len = 0;
            break;

        case 0xe1:
            dsp_out_data (s, s->ver & 0xff);
            dsp_out_data (s, s->ver >> 8);
            break;

        case 0xe2:
            s->needed_bytes = 1;
            goto warn;

        case 0xe3:
            {
                int i;
                for (i = sizeof (e3) - 1; i >= 0; --i)
                    dsp_out_data (s, e3[i]);
            }
            break;

        case 0xe4:              /* write test reg */
            s->needed_bytes = 1;
            break;

        case 0xe7:
            qemu_log_mask(LOG_UNIMP, "Attempt to probe for ESS (0xe7)?\n");
            break;

        case 0xe8:              /* read test reg */
            dsp_out_data (s, s->test_reg);
            break;

        case 0xf2:
        case 0xf3:
            dsp_out_data (s, 0xaa);
            s->mixer_regs[0x82] |= (cmd == 0xf2) ? 1 : 2;
            qemu_irq_raise (s->pic);
            break;

        case 0xf9:
            s->needed_bytes = 1;
            goto warn;

        case 0xfa:
            dsp_out_data (s, 0);
            goto warn;

        case 0xfc:              /* FIXME */
            dsp_out_data (s, 0);
            goto warn;

        default:
            qemu_log_mask(LOG_UNIMP, "Unrecognized command 0x%x\n", cmd);
            break;
        }
    }

    if (!s->needed_bytes) {
        ldebug("!needed_bytes");
    }

 exit:
    if (!s->needed_bytes) {
        s->cmd = -1;
    }
    else {
        s->cmd = cmd;
    }
    return;

 warn:
    qemu_log_mask(LOG_UNIMP, "warning: command 0x%x,%d is not truly understood"
                  " yet\n", cmd, s->needed_bytes);
    goto exit;

}

static uint16_t dsp_get_lohi (SB16State *s)
{
    uint8_t hi = dsp_get_data (s);
    uint8_t lo = dsp_get_data (s);
    return (hi << 8) | lo;
}

static uint16_t dsp_get_hilo (SB16State *s)
{
    uint8_t lo = dsp_get_data (s);
    uint8_t hi = dsp_get_data (s);
    return (hi << 8) | lo;
}


/* ADPCM PAIN */

static const int index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static int16_t decode_adpcm_4bit(uint8_t code, SB16State *s) {
    int step = step_table[s->adpcm_index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;

    if (code & 8) s->adpcm_valpred -= diff;
    else s->adpcm_valpred += diff;

    if (s->adpcm_valpred > 32767) s->adpcm_valpred = 32767;
    else if (s->adpcm_valpred < -32768) s->adpcm_valpred = -32768;

    s->adpcm_index += index_table[code];
    if (s->adpcm_index < 0) s->adpcm_index = 0;
    else if (s->adpcm_index > 88) s->adpcm_index = 88;

    return (int16_t)s->adpcm_valpred;
}

/* THE END OF ADPCM PAIN */

static void complete (SB16State *s)
{
    int d0, d1, d2;
    ldebug("complete command 0x%x, in_index %d, needed_bytes %d",
            s->cmd, s->in_index, s->needed_bytes);

    if (s->cmd > 0xaf && s->cmd < 0xd0) {
        d2 = dsp_get_data(s);
        d1 = dsp_get_data(s);
        d0 = dsp_get_data(s);

        if (s->cmd & 8) {
		/* this is yet another todo for another time */
            ldebug("Executing ADC cmd=0x%x mode=%d len=%d", s->cmd, d0, d1 + (d2 << 8));
            
            s->use_hdma = s->cmd < 0xc0;
            s->fmt_bits = (s->cmd >> 4) == 11 ? 16 : 8;
            s->fmt_signed = (d0 >> 4) & 1;
            s->fmt_stereo = (d0 >> 5) & 1;
            s->block_size = (d1 + (d2 << 8) + 1) << (s->fmt_bits == 16);

            control(s, 1);
        }
        else {
            dma_cmd(s, s->cmd, d0, d1 + (d2 << 8));
        }
    }
    else {
        switch (s->cmd) {
        case 0x04:
            s->csp_mode = dsp_get_data (s);
            s->csp_reg83r = 0;
            s->csp_reg83w = 0;
            ldebug("CSP command 0x04: mode=0x%x", s->csp_mode);
            break;

        case 0x05:
            s->csp_param = dsp_get_data (s);
            s->csp_value = dsp_get_data (s);
            ldebug("CSP command 0x05: param=0x%x value=0x%x",
                    s->csp_param,
                    s->csp_value);
            break;

        case 0x0e:
            d0 = dsp_get_data (s);
            d1 = dsp_get_data (s);
            ldebug("write CSP register %d <- 0x%x", d1, d0);
            if (d1 == 0x83) {
                ldebug("0x83[%d] <- 0x%x", s->csp_reg83r, d0);
                s->csp_reg83[s->csp_reg83r % 4] = d0;
                s->csp_reg83r += 1;
            }
            else {
                s->csp_regs[d1] = d0;
            }
            break;

        case 0x0f:
            d0 = dsp_get_data (s);
            ldebug("read CSP register 0x%x -> 0x%x, mode=0x%x",
                    d0, s->csp_regs[d0], s->csp_mode);
            if (d0 == 0x83) {
                ldebug("0x83[%d] -> 0x%x",
                        s->csp_reg83w,
                        s->csp_reg83[s->csp_reg83w % 4]);
                dsp_out_data (s, s->csp_reg83[s->csp_reg83w % 4]);
                s->csp_reg83w += 1;
            }
            else {
                dsp_out_data (s, s->csp_regs[d0]);
            }
            break;

	case 0x10:
            d0 = dsp_get_data(s);
            if (s->speaker) {
                uint8_t sample = d0;
                /* i cannot be bothered right now, this should be done properly later */
                AUD_set_active_out(s->voice, 1);
                AUD_write(s->voice, &sample, 1);
            }
            break;

        case 0x14:
            dma_cmd8 (s, 0, dsp_get_lohi (s) + 1);
            break;
	
        case 0x40:
            s->time_const = dsp_get_data (s);
            ldebug("set time const %d", s->time_const);
            break;

        case 0x41:
        case 0x42:
            /*
             * 0x41 is documented as setting the output sample rate,
             * and 0x42 the input sample rate, but in fact SB16 hardware
             * seems to have only a single sample rate under the hood,
             * and FT2 sets output freq with this (go figure).  Compare:
             * http://homepages.cae.wisc.edu/~brodskye/sb16doc/sb16doc.html#SamplingRate
             */
            s->freq = restrict_sampling_rate(dsp_get_hilo(s));
	    s->highspeed = 1;
            ldebug("set freq %d", s->freq);
            break;

        case 0x48:
            s->block_size = dsp_get_lohi (s) + 1;
	    s->left_till_irq = s->block_size;
            ldebug("set dma block len %d", s->block_size);
            break;

        case 0x74:
		d0 = dsp_get_lohi(s);
	        s->block_size = d0 + 1;
	        s->adpcm_valpred = (int8_t)dsp_get_data(s) << 8;
	        s->adpcm_index = 0;
	        control(s, 1);
	        break;
        case 0x75:
        case 0x76:
        case 0x77:
            d0 = dsp_get_lohi(s);
            qemu_log_mask(LOG_UNIMP, "sb16: ADPCM command 0x%x len %d not implemented\n", 
                          s->cmd, d0);
            break;

        case 0x80:
            {
                int freq, samples, bytes;
                int64_t ticks;

                freq = s->freq > 0 ? s->freq : 11025;
                samples = dsp_get_lohi (s) + 1;
                bytes = samples << s->fmt_stereo << (s->fmt_bits == 16);
                ticks = muldiv64(bytes, NANOSECONDS_PER_SECOND, freq);
                if (ticks < NANOSECONDS_PER_SECOND / 1024) {
                    qemu_irq_raise (s->pic);
                }
                else {
                    if (s->aux_ts) {
                        timer_mod (
                            s->aux_ts,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ticks
                            );
                    }
                }
                ldebug("mix silence %d %d %" PRId64, samples, bytes, ticks);
            }
            break;

	case 0xd8:
            dsp_out_data(s, s->speaker ? 0xff : 0x00);
            goto exit;

        case 0xe0:
            d0 = dsp_get_data (s);
            s->out_data_len = 0;
            ldebug("E0 data = 0x%x", d0);
            dsp_out_data (s, ~d0);
            break;

        case 0xe2:
            d0 = dsp_get_data (s);s->e2_valadd += ((uint8_t) d0) ^ s->e2_valxor;
            s->e2_valxor = (s->e2_valxor >> 2) | (s->e2_valxor << 6);
            break;

        case 0xe4:
            s->test_reg = dsp_get_data (s);
            break;

        case 0xf9:
            d0 = dsp_get_data (s);
            ldebug("command 0xf9 with 0x%x", d0);
            switch (d0) {
            case 0x0e:
                dsp_out_data (s, 0xff);
                break;

            case 0x0f:
                dsp_out_data (s, 0x07);
                break;

            case 0x37:
                dsp_out_data (s, 0x38);
                break;

            default:
                dsp_out_data (s, 0x00);
                break;
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "complete: unrecognized command 0x%x\n",
                          s->cmd);
            return;
        }
    }

    ldebug("");
    s->cmd = -1;
    exit:
	return;
}

static void legacy_reset (SB16State *s)
{
    struct audsettings as;

    s->freq = 11025;
    s->fmt_signed = 0;
    s->fmt_bits = 8;
    s->fmt_stereo = 0;

    s->audio_free = 0;

    as.freq = s->freq;
    as.nchannels = 1;
    as.fmt = AUDIO_FORMAT_U8;
    as.endianness = 0;

    s->voice = AUD_open_out (
        s->audio_be,
        s->voice,
        "sb16",
        s,
        SB_audio_callback,
        &as
        );

    /* Not sure about that... */
    /* AUD_set_active_out (s->voice, 1); */
}

static void reset (SB16State *s)
{
    qemu_irq_lower (s->pic);
    if (s->dma_auto) {
        qemu_irq_raise (s->pic);
        qemu_irq_lower (s->pic);
    }

    s->mixer_regs[0x82] = 0;
    s->dma_auto = 0;
    s->in_index = 0;
    s->out_data_len = 0;
    s->left_till_irq = 0;
    s->needed_bytes = 0;
    s->block_size = -1;
    s->nzero = 0;
    s->highspeed = 0;
    s->v2x6 = 0;
    s->cmd = -1;

    s->e2_valadd = 0xaa;
    s->e2_valxor = 0x96;
    dsp_out_data (s, 0xaa);
    speaker (s, 0);
    control (s, 0);
    legacy_reset (s);
}

static void dsp_write(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;
    int iport;

    iport = nport - s->port;

    ldebug("write 0x%x <- 0x%x", nport, val);
    switch (iport) {
    case 0x06:
        switch (val) {
        case 0x00:
            if (s->v2x6 == 1) {
                reset (s);
            }
            s->v2x6 = 0;
            break;

        case 0x01:
        case 0x03:              /* FreeBSD kludge */
            s->v2x6 = 1;
            break;

        case 0xc6:
            s->v2x6 = 0;        /* Prince of Persia, csp.sys, diagnose.exe */
            break;

        case 0xb8:              /* Panic */
            reset (s);
            break;

        case 0x39:
            dsp_out_data (s, 0x38);
            reset (s);
            s->v2x6 = 0x39;
            break;

        default:
            s->v2x6 = val;
            break;
        }
        break;

    case 0x0c:                  /* write data or command | write status */
/*         if (s->highspeed) */
/*             break; */

        if (s->needed_bytes == 0) {
            command (s, val);
#if 0
            if (0 == s->needed_bytes) {
                log_dsp (s);
            }
#endif
        }
        else {
            if (s->in_index == sizeof (s->in2_data)) {
                warn_report("sb16: in data overrun");
            }
            else {
                s->in2_data[s->in_index++] = val;
                if (s->in_index == s->needed_bytes) {
                    s->needed_bytes = 0;
                    complete (s);
#if 0
                    log_dsp (s);
#endif
                }
            }
        }
        break;

    default:
        ldebug("(nport=0x%x, val=0x%x)", nport, val);
        break;
    }
}

static uint32_t dsp_read(void *opaque, uint32_t nport)
{
    SB16State *s = opaque;
    int iport, retval, ack = 0;

    iport = nport - s->port;

    switch (iport) {
    case 0x06:                  /* reset */
        retval = 0xff;
        break;

    case 0x0a:                  /* read data */
        if (s->out_data_len) {
            retval = s->out_data[--s->out_data_len];
            s->last_read_byte = retval;
        }
        else {
            if (s->cmd != -1) {
                warn_report("sb16: empty output buffer for command 0x%x",
                       s->cmd);
            }
            retval = s->last_read_byte;
            /* goto error; */
        }
        break;

    case 0x0c:                  /* 0 can write */
        retval = s->can_write ? 0 : 0x80;
        break;

    case 0x0d:                  /* timer interrupt clear */
        /* warn_report("sb16: timer interrupt clear"); */
        retval = 0;
        break;

    case 0x0e:                  /* data available status | irq 8 ack */
        retval = (!s->out_data_len || s->highspeed) ? 0 : 0x80;
        if (s->mixer_regs[0x82] & 1) {
            ack = 1;
            s->mixer_regs[0x82] &= ~1;
            qemu_irq_lower (s->pic);
        }
        break;

    case 0x0f:                  /* irq 16 ack */
        retval = 0xff;
        if (s->mixer_regs[0x82] & 2) {
            ack = 1;
            s->mixer_regs[0x82] &= ~2;
            qemu_irq_lower (s->pic);
        }
        break;

    default:
        goto error;
    }

    if (!ack) {
        ldebug("read 0x%x -> 0x%x", nport, retval);
    }

    return retval;

 error:
    warn_report("sb16: dsp_read 0x%x error", nport);
    return 0xff;
}

static void reset_mixer (SB16State *s)
{
    int i;

    memset (s->mixer_regs, 0xff, 0x7f);
    memset (s->mixer_regs + 0x83, 0xff, sizeof (s->mixer_regs) - 0x83);

    s->mixer_regs[0x02] = 4;    /* master volume 3bits */
    s->mixer_regs[0x06] = 4;    /* MIDI volume 3bits */
    s->mixer_regs[0x08] = 0;    /* CD volume 3bits */
    s->mixer_regs[0x0a] = 0;    /* voice volume 2bits */

    /* d5=input filt, d3=lowpass filt, d1,d2=input source */
    s->mixer_regs[0x0c] = 0;

    /* d5=output filt, d1=stereo switch */
    s->mixer_regs[0x0e] = 0;

    /* voice volume L d5,d7, R d1,d3 */
    s->mixer_regs[0x04] = (4 << 5) | (4 << 1);
    /* master ... */
    s->mixer_regs[0x22] = (4 << 5) | (4 << 1);
    /* MIDI ... */
    s->mixer_regs[0x26] = (4 << 5) | (4 << 1);

    for (i = 0x30; i < 0x48; i++) {
        s->mixer_regs[i] = 0x20;
    }

    sb16_update_opl_volume(s);
}

static void mixer_write_indexb(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;
    (void) nport;
    s->mixer_nreg = val;
}

static void mixer_write_datab(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;

    (void) nport;
    ldebug("mixer_write [0x%x] <- 0x%x", s->mixer_nreg, val);

    switch (s->mixer_nreg) {
    case 0x00:
        reset_mixer(s);
        break;

    case 0x04:
        s->mixer_regs[0x04] = val;
        s->mixer_regs[0x32] = (val & 0xf0);
        s->mixer_regs[0x33] = (val & 0x0f) << 4;
        break;

    case 0x22:
        s->mixer_regs[0x22] = val;
        s->mixer_regs[0x30] = (val & 0xf0);
        s->mixer_regs[0x31] = (val & 0x0f) << 4;
        break;

    case 0x26:
        s->mixer_regs[0x26] = val;
        s->mixer_regs[0x34] = (val & 0xf0);
        s->mixer_regs[0x35] = (val & 0x0f) << 4;
        break;

    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
        s->mixer_regs[s->mixer_nreg] = val;
        s->mixer_regs[0x22] = (s->mixer_regs[0x30] & 0xf0) | (s->mixer_regs[0x31] >> 4);
        s->mixer_regs[0x04] = (s->mixer_regs[0x32] & 0xf0) | (s->mixer_regs[0x33] >> 4);
        s->mixer_regs[0x26] = (s->mixer_regs[0x34] & 0xf0) | (s->mixer_regs[0x35] >> 4);
        break;

    case 0x80:
        {
            int irq = irq_of_magic (val);
            ldebug("setting irq to %d (val=0x%x)", irq, val);
            if (irq > 0) {
                s->irq = irq;
            }
        }
        break;

    case 0x81:
        {
            int dma = ctz32(val & 0xf);
            int hdma = ctz32(val & 0xf0);
            
            if (dma != s->dma || hdma != s->hdma) {
                ldebug("jumping DMA 8bit %d -> %d, 16bit %d -> %d", 
                       s->dma, dma, s->hdma, hdma);
                
                s->dma = dma;
                s->hdma = hdma;
                
                ISABus *bus = isa_bus_from_device(ISA_DEVICE(s));
                s->isa_dma = isa_bus_get_dma(bus, s->dma);
                s->isa_hdma = isa_bus_get_dma(bus, s->hdma);
            }
        }
        break;

#if 0
            s->dma = dma;
            s->hdma = hdma;
#endif

    case 0x82:
        qemu_log_mask(LOG_GUEST_ERROR, "attempt to write into IRQ status"
                      " register (val=0x%x)\n", val);
        return;

    default:
        if (s->mixer_nreg >= 0x80) {
            ldebug("attempt to write mixer[0x%x] <- 0x%x", s->mixer_nreg, val);
        }
        s->mixer_regs[s->mixer_nreg] = val;
        break;
    }
    sb16_update_opl_volume(s);
    sb16_update_voice_volume(s);
}

static uint32_t mixer_read(void *opaque, uint32_t nport)
{
    SB16State *s = opaque;

    (void) nport;
#ifndef DEBUG_SB16_MOST
    if (s->mixer_nreg != 0x82) {
        ldebug("mixer_read[0x%x] -> 0x%x",
                s->mixer_nreg, s->mixer_regs[s->mixer_nreg]);
    }
#else
    ldebug("mixer_read[0x%x] -> 0x%x",
            s->mixer_nreg, s->mixer_regs[s->mixer_nreg]);
#endif
    return s->mixer_regs[s->mixer_nreg];
}

static int write_audio (SB16State *s, int nchan, int dma_pos,
                        int dma_len, int len)
{
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);
    int temp, net;
    QEMU_UNINITIALIZED uint8_t tmpbuf[4096];

    temp = len;
    net = 0;

    while (temp) {
        int left = dma_len - dma_pos;
        int copied;
        size_t to_copy;

        to_copy = MIN (temp, left);
        if (to_copy > sizeof (tmpbuf)) {
            to_copy = sizeof (tmpbuf);
        }

        copied = k->read_memory(isa_dma, nchan, tmpbuf, dma_pos, to_copy);
        copied = AUD_write (s->voice, tmpbuf, copied);

        temp -= copied;
        dma_pos = (dma_pos + copied) % dma_len;
        net += copied;

        if (!copied) {
            break;
        }
    }

    return net;
}

static int SB_write_DMA (void *opaque, int nchan, int dma_pos, int dma_len)
{
    SB16State *s = opaque;
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);
    uint8_t tmpbuf[4096];
    int to_copy, copied;

    to_copy = MIN(s->left_till_irq, dma_len - dma_pos);
    if (to_copy > (int)sizeof(tmpbuf)) {
        to_copy = sizeof(tmpbuf);
    }
    /* silence 4 now */
    memset(tmpbuf, (s->fmt_bits == 8 && !s->fmt_signed) ? 0x80 : 0x00, to_copy);

    copied = k->write_memory(isa_dma, nchan, tmpbuf, dma_pos, to_copy);

    dma_pos = (dma_pos + copied) % dma_len;
    s->left_till_irq -= copied;

    if (s->left_till_irq <= 0) {
        s->mixer_regs[0x82] |= (nchan & 4) ? 2 : 1;
        qemu_irq_raise (s->pic);
        s->left_till_irq = s->block_size;
    }

    return dma_pos;
}

static int SB_read_DMA (void *opaque, int nchan, int dma_pos, int dma_len)
{
    SB16State *s = opaque;
    int till, copy, written = 0, free;
    
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);
    uint8_t tmpbuf[4096];
    int to_copy;

    if (s->block_size <= 0) {
        return dma_pos;
    }

    if (s->left_till_irq < 0) {
        s->left_till_irq = s->block_size;
    }

    if (s->voice) {
        free = s->audio_free & ~s->align;
        if (free <= 0) {
            release_DREQ(s, nchan);
            return dma_pos;
        }
    } else {
        free = dma_len;
    }

    copy = free;
    till = s->left_till_irq;

    to_copy = MIN(copy, till);
    to_copy = MIN(to_copy, dma_len - dma_pos);

    if (s->cmd == 0x74) {
        if (to_copy > (int)sizeof(tmpbuf) / 4) {
            to_copy = sizeof(tmpbuf) / 4;
        }

        uint8_t adpcm_data[1024]; 
        int adpcm_copied = k->read_memory(isa_dma, nchan, adpcm_data, dma_pos, to_copy);
        
        int16_t *out_samples = (int16_t *)tmpbuf;
        for (int i = 0; i < adpcm_copied; i++) {
            out_samples[i * 2] = decode_adpcm_4bit(adpcm_data[i] >> 4, s);
            out_samples[i * 2 + 1] = decode_adpcm_4bit(adpcm_data[i] & 0x0f, s);
        }

        int bytes_out = AUD_write(s->voice, tmpbuf, adpcm_copied * 4);
        written = bytes_out / 4;
    } else {
        written = write_audio(s, nchan, dma_pos, dma_len, to_copy);
    }

    if (s->cmd == 0x75) {
    	uint8_t ref_byte;
    
    	k->read_memory(isa_dma, nchan, &ref_byte, dma_pos, 1);
    	s->adpcm_valpred = (int16_t)((ref_byte - 128) << 8);
    	s->adpcm_index = 0;

    	dma_pos = (dma_pos + 1) % dma_len;
    	s->cmd = 0x74; 
    }

    dma_pos = (dma_pos + written) % dma_len;
    s->left_till_irq -= written;
    s->audio_free -= written;

    if (s->left_till_irq <= 0) {
        s->mixer_regs[0x82] |= (nchan & 4) ? 2 : 1;
        qemu_irq_raise(s->pic);

        if (s->block_size > 0) {
            s->left_till_irq = s->block_size + (s->left_till_irq % s->block_size);
        } else {
            s->left_till_irq = s->block_size = 1024;
        }

        if (s->dma_auto == 0) {
            control(s, 0);
            speaker(s, 0);
        }
    }

    return dma_pos;
}

static void SB_audio_callback (void *opaque, int free)
{
    SB16State *s = opaque;
    int nchan = s->use_hdma ? s->hdma : s->dma;
    s->audio_free = free;
    /* run the DMA engine to call SB_read_DMA immediately */
    hold_DREQ(s, nchan);
}

static int sb16_post_load (void *opaque, int version_id)
{
    SB16State *s = opaque;

    if (s->voice) {
        AUD_close_out (s->audio_be, s->voice);
        s->voice = NULL;
    }

    if (s->dma_running) {
        if (s->freq) {
            struct audsettings as;

            s->audio_free = 0;

            as.freq = s->freq;
            as.nchannels = 1 << s->fmt_stereo;
            as.fmt = s->fmt;
            as.endianness = 0;

            s->voice = AUD_open_out (
                s->audio_be,
                s->voice,
                "sb16",
                s,
                SB_audio_callback,
                &as
                );
        }

        control (s, 1);
        speaker (s, s->speaker);
    }
    if (s->ymf262) ymf262_reset_chip(s->ymf262);
    return 0;
}

static const VMStateDescription vmstate_sb16 = {
    .name = "sb16",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sb16_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UNUSED(  4 /* irq */
                       + 4 /* dma */
                       + 4 /* hdma */
                       + 4 /* port */
                       + 4 /* ver */),
        VMSTATE_INT32 (in_index, SB16State),
        VMSTATE_INT32 (out_data_len, SB16State),
        VMSTATE_INT32 (fmt_stereo, SB16State),
        VMSTATE_INT32 (fmt_signed, SB16State),
        VMSTATE_INT32 (fmt_bits, SB16State),
        VMSTATE_UINT32 (fmt, SB16State),
        VMSTATE_INT32 (dma_auto, SB16State),
        VMSTATE_INT32 (block_size, SB16State),
        VMSTATE_INT32 (fifo, SB16State),
        VMSTATE_INT32 (freq, SB16State),
        VMSTATE_INT32 (time_const, SB16State),
        VMSTATE_INT32 (speaker, SB16State),
        VMSTATE_INT32 (needed_bytes, SB16State),
        VMSTATE_INT32 (cmd, SB16State),
        VMSTATE_INT32 (use_hdma, SB16State),
        VMSTATE_INT32 (highspeed, SB16State),
        VMSTATE_INT32 (can_write, SB16State),
        VMSTATE_INT32 (v2x6, SB16State),

        VMSTATE_UINT8 (csp_param, SB16State),
        VMSTATE_UINT8 (csp_value, SB16State),
        VMSTATE_UINT8 (csp_mode, SB16State),
        VMSTATE_UINT8 (csp_param, SB16State),
        VMSTATE_BUFFER (csp_regs, SB16State),
        VMSTATE_UINT8 (csp_index, SB16State),
        VMSTATE_BUFFER (csp_reg83, SB16State),
        VMSTATE_INT32 (csp_reg83r, SB16State),
        VMSTATE_INT32 (csp_reg83w, SB16State),

        VMSTATE_BUFFER (in2_data, SB16State),
        VMSTATE_BUFFER (out_data, SB16State),
        VMSTATE_UINT8 (test_reg, SB16State),
        VMSTATE_UINT8 (last_read_byte, SB16State),

        VMSTATE_INT32 (nzero, SB16State),
        VMSTATE_INT32 (left_till_irq, SB16State),
        VMSTATE_INT32 (dma_running, SB16State),
        VMSTATE_INT32 (bytes_per_second, SB16State),
        VMSTATE_INT32 (align, SB16State),

        VMSTATE_INT32 (mixer_nreg, SB16State),
        VMSTATE_BUFFER (mixer_regs, SB16State),

        VMSTATE_END_OF_LIST ()
    }
};

static MemoryRegionPortio sb16_ioport_list[] = {
    {  4, 1, 1, .write = mixer_write_indexb },
    {  5, 1, 1, .read = mixer_read, .write = mixer_write_datab },
    {  6, 1, 1, .read = dsp_read, .write = dsp_write },
    { 10, 1, 1, .read = dsp_read },
    { 12, 1, 1, .write = dsp_write },
    { 12, 4, 1, .read = dsp_read },
    PORTIO_END_OF_LIST (),
};

static MemoryRegionPortio opl_portio_list[] = {
    { 0, 4, 1, .read = sb16_opl_read, .write = sb16_opl_write },
    PORTIO_END_OF_LIST (),
};

static MemoryRegionPortio mpu_ioport_list[] = {
    { 0, 2, 1, .read = mpu_read, .write = mpu_write },
    PORTIO_END_OF_LIST (),
};

static void sb16_initfn (Object *obj)
{
    SB16State *s = SB16 (obj);

    s->cmd = -1;
}

static void sb16_realizefn (DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE (dev);
    ISABus *bus = isa_bus_from_device(isadev);
    SB16State *s = SB16 (dev);
    IsaDmaClass *k;
    struct audsettings as;

    if (!AUD_backend_check(&s->audio_be, errp)) {
        return;
    }

    s->isa_hdma = isa_bus_get_dma(bus, s->hdma);
    s->isa_dma = isa_bus_get_dma(bus, s->dma);
    if (!s->isa_dma || !s->isa_hdma) {
        error_setg(errp, "ISA controller does not support DMA");
        return;
    }

    s->pic = isa_bus_get_irq(bus, s->irq);

    k = ISADMA_GET_CLASS(s->isa_hdma);
    k->register_channel(s->isa_hdma, s->hdma, SB_read_DMA, s);
    
    k->register_channel(s->isa_hdma, s->hdma, SB_write_DMA, s);
    k->register_channel(s->isa_dma, s->dma, SB_write_DMA, s);

    s->mixer_regs[0x80] = magic_of_irq (s->irq);
    s->mixer_regs[0x81] = (1 << s->dma) | (1 << s->hdma);
    s->mixer_regs[0x82] = 0x00;

    s->csp_regs[5] = 1;
    s->csp_regs[9] = 0xf8;

    /* just in case */
    s->align = (s->fmt_bits == 16) ? 1 : 0;

    OPL3_LockTable();
    s->ymf262 = ymf262_init(14318180, 44100);
    if (s->ymf262) {
        ymf262_reset_chip(s->ymf262);
        ymf262_set_timer_handler(s->ymf262, sb16_opl_timer_handler, s);
        as.freq = 44100;
        as.nchannels = 2;
        as.fmt = AUDIO_FORMAT_S16;
        as.endianness = 0;
        s->voice_opl = AUD_open_out(s->audio_be, s->voice_opl, "sb16-opl", s, sb16_opl_callback, &as);
        AUD_set_active_out(s->voice_opl, 1);
	isa_register_portio_list(isadev, &s->opl_portio_list, s->port, opl_portio_list, s, "sb16-opl");
	isa_register_portio_list(isadev, &s->hack_portio_list, 0x388, opl_portio_list, s, "sb16-opl");
    }

    reset_mixer (s);
    s->aux_ts = timer_new_ns(QEMU_CLOCK_VIRTUAL, aux_timer, s);
    if (!s->aux_ts) {
        error_setg(errp, "warning: Could not create auxiliary timer");
    }

    isa_register_portio_list(isadev, &s->portio_list, s->port, sb16_ioport_list, s, "sb16");

    isa_register_portio_list(isadev, &s->mpu_portio_list, 0x330, mpu_ioport_list, s, "sb16-mpu401");

    k = ISADMA_GET_CLASS(s->isa_hdma);
    k->register_channel(s->isa_hdma, s->hdma, SB_read_DMA, s);

    k = ISADMA_GET_CLASS(s->isa_dma);
    k->register_channel(s->isa_dma, s->dma, SB_read_DMA, s);

    s->can_write = 1;
}

static const Property sb16_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SB16State, audio_be),
    DEFINE_PROP_UINT32 ("version", SB16State, ver,  0x0405), /* 4.5 */
    DEFINE_PROP_UINT32 ("iobase",  SB16State, port, 0x220),
    DEFINE_PROP_UINT32 ("irq",     SB16State, irq,  5),
    DEFINE_PROP_UINT32 ("dma",     SB16State, dma,  1),
    DEFINE_PROP_UINT32 ("dma16",   SB16State, hdma, 5),
};

static void sb16_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS (klass);

    dc->realize = sb16_realizefn;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Creative Sound Blaster 16";
    dc->vmsd = &vmstate_sb16;
    device_class_set_props(dc, sb16_properties);
}

static const TypeInfo sb16_info = {
    .name          = TYPE_SB16,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof (SB16State),
    .instance_init = sb16_initfn,
    .class_init    = sb16_class_initfn,
};

static void sb16_register_types (void)
{
    type_register_static (&sb16_info);
    audio_register_model("sb16", "Creative Sound Blaster 16", TYPE_SB16);
}

type_init (sb16_register_types)
