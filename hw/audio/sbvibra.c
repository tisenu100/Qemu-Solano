/*
 * QEMU Sound Blaster ViBRA 16 emulation
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

/* TODO: implement ISA PnP so it's like a real vibra card, for now it works like a regular sb16.. but with cqm... 
 * also this is seriously disgusting i just copied sb16.c lazily and renamed everything with ctrl-h... i need to do it properly at some point
 * but for now maybe it's ok.. also maybe good idea to install sb vibra drivers and trace what it does and put it here..
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
#include "chardev/char-fe.h"
#include "qapi/error.h"
#include "qemu/fifo8.h"

#include "cqm.h"

#define DEBUG 0
/* #define DEBUG_vibra_MOST */

#define ldebug(fmt, ...) do { \
        if (DEBUG) { \
            error_report("vibra: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

static const char e3[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

#define TYPE_vibra "sb16vibra"
OBJECT_DECLARE_SIMPLE_TYPE(vibraState, vibra)

struct vibraState {
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
    SWVoiceIn *voice_in;
    int recording;
    int audio_avail;

    int32_t adpcm_valpred;
    int32_t adpcm_index;
    
    cqm_t cqm;
    uint16_t cqm_reg;
    int16_t *cqm_mix;
    unsigned int cqm_samps;
    uint8_t cqm_status;
    SWVoiceOut *voice_cqm;
    PortioList cqm_portio_list;
    PortioList hack_portio_list;

    /* evil */
    Fifo8 mpu_fifo;
    CharFrontend mpu_chr;
    MemoryRegion mpu_io;
    bool mpu_uart_mode;
    PortioList mpu_portio_list;
    bool mpu_has_ack;

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
static const uint8_t vibra_log_vol[32] = {
    0,   2,   5,   8,   12,  16,  20,  25,
    31,  38,  46,  54,  63,  73,  84,  96,
    108, 122, 136, 152, 168, 185, 203, 222,
    242, 255, 255, 255, 255, 255, 255, 255
};

static void vibra_update_voice_volume(vibraState *s)
{
    if (!s->voice) return;

    int ml_idx = (s->mixer_regs[0x30] >> 3) & 0x1f;
    int mr_idx = (s->mixer_regs[0x31] >> 3) & 0x1f;
    int vl_idx = (s->mixer_regs[0x32] >> 3) & 0x1f;
    int vr_idx = (s->mixer_regs[0x33] >> 3) & 0x1f;

    Volume vol;
    vol.mute = 0;
    vol.channels = 2;

    vol.vol[0] = (vibra_log_vol[ml_idx] * vibra_log_vol[vl_idx] * 192) / 65025;
    vol.vol[1] = (vibra_log_vol[mr_idx] * vibra_log_vol[vr_idx] * 192) / 65025;

    audio_be_set_volume_out(s->audio_be, s->voice, &vol);
}

static void vibra_update_cqm_volume(vibraState *s)
{
    if (!s->voice_cqm) return;

    int ml_idx = (s->mixer_regs[0x30] >> 3) & 0x1f;
    int mr_idx = (s->mixer_regs[0x31] >> 3) & 0x1f;
    int vl_idx = (s->mixer_regs[0x34] >> 3) & 0x1f;
    int vr_idx = (s->mixer_regs[0x35] >> 3) & 0x1f;

    Volume vol;
    vol.mute = 0;
    vol.channels = 2;

    vol.vol[0] = (vibra_log_vol[ml_idx] * vibra_log_vol[vl_idx] * 192) / 65025;
    vol.vol[1] = (vibra_log_vol[mr_idx] * vibra_log_vol[vr_idx] * 192) / 65025;

    audio_be_set_volume_out(s->audio_be, s->voice_cqm, &vol);
}

static void vibra_cqm_callback(void *opaque, int free)
{
    vibraState *s = opaque;
    unsigned int samples;
    
    if (!s->voice_cqm) {
        return;
    }

    samples = MIN((unsigned int)(free / 4), s->cqm_samps);

    CQM_GenerateStream(&s->cqm, s->cqm_mix, samples);
    audio_be_write(s->audio_be, s->voice_cqm, s->cqm_mix, samples * 4);    
}

static void vibra_cqm_write(void *opaque, uint32_t nport, uint32_t val)
{
    vibraState *s = opaque;
    uint32_t a = nport & 3;
    uint16_t reg;

    audio_be_set_active_out(s->audio_be, s->voice_cqm, 1);
    vibra_update_cqm_volume(s);

    switch (a) {
    case 0:  /* bank-0 address latch */
        s->cqm_reg = val & 0xff;
        break;
    case 2:  /* bank-1 address latch */
        s->cqm_reg = (val & 0xff) | 0x100;
        break;
    case 1:  /* bank-0 data commit */
    case 3:  /* bank-1 data commit; bank bit already in cqm_regs */
        /*
         * Bank-0 reg 0x04 is CQM Timer Control. Nuked-CQM ignores it,
         * so we synthesize the status byte here. Bit 7 = IRQ reset
         * (clears status); else status reflects start+mask bits per
         * AdLib/OPL3 semantics:
         *   ctrl bit 0: T1 start    ctrl bit 6: T1 mask
         *   ctrl bit 1: T2 start    ctrl bit 5: T2 mask
         */
        reg = s->cqm_reg;
        if (reg == 0x04) {
            if (val & 0x80) {
                s->cqm_status = 0;
            } else {
                uint8_t status = 0;
                if ((val & 0x01) && !(val & 0x40)) {
                    status |= 0x40;
                }
                if ((val & 0x02) && !(val & 0x20)) {
                    status |= 0x20;
                }
                if (status) {
                    status |= 0x80;
                }
                s->cqm_status = status;
            }
        }
        CQM_WriteRegBuffered(&s->cqm, reg,
                              val & 0xff);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cqm: invalid port offset %d\n", a);
    }
}

static uint32_t vibra_cqm_read(void *opaque, uint32_t nport)
{
    vibraState *s = opaque;
    return s->cqm_status;
}

static int mpu_can_receive(void *opaque)
{
    vibraState *s = opaque;
    return fifo8_num_free(&s->mpu_fifo);
}


static void mpu_receive(void *opaque, const uint8_t *buf, int size)
{
    vibraState *s = opaque;

    if (!s->mpu_uart_mode) {
        return;
    }

    for (int i = 0; i < size; i++) {
        fifo8_push(&s->mpu_fifo, buf[i]);
    }

    qemu_irq_raise(s->pic);
}


static void mpu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    vibraState *s = opaque;
    uint8_t byte = val & 0xff;

    if (addr == 1) {
        if (byte == 0x3F) {
            s->mpu_uart_mode = true;
            s->mpu_has_ack = true;
            qemu_irq_raise(s->pic);
        } else if (byte == 0xFF) {
            s->mpu_uart_mode = false;
            s->mpu_has_ack = true;
            fifo8_reset(&s->mpu_fifo);
	    for (int ch = 0; ch < 16; ch++) {
        	uint8_t all_notes_off[] = { 0xB0 | ch, 0x7B, 0x00 };
        	qemu_chr_fe_write_all(&s->mpu_chr, all_notes_off, 3);
    	}
            qemu_irq_raise(s->pic);
        }
    } else {
        if (s->mpu_uart_mode) {
            qemu_chr_fe_write_all(&s->mpu_chr, &byte, 1);
        }
    }
}

static uint64_t mpu_read(void *opaque, hwaddr addr, unsigned size)
{
    vibraState *s = opaque;

    if (addr == 1) { 
        uint8_t status = 0x3F;

        if (!s->mpu_has_ack && fifo8_is_empty(&s->mpu_fifo)) {
            status |= ~0x40;
        }
        return status;
    }

    uint8_t ret = 0xFF;
    if (s->mpu_has_ack) {
        s->mpu_has_ack = false;
        ret = 0xFE;
    } else if (!fifo8_is_empty(&s->mpu_fifo)) {
        ret = fifo8_pop(&s->mpu_fifo);
    }

    if (!s->mpu_has_ack && fifo8_is_empty(&s->mpu_fifo)) {
        qemu_irq_lower(s->pic);
    }
    return ret;
}

static void SB_audio_callback (void *opaque, int free);
static void SB_adc_callback(void *opaque, int avail);

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

static void hold_DREQ(vibraState *s, int nchan)
{
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);

    k->hold_DREQ(isa_dma, nchan);
}

static void release_DREQ(vibraState *s, int nchan)
{
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);

    k->release_DREQ(isa_dma, nchan);
}

#if 0
static void log_dsp (vibraState *dsp)
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

static void speaker (vibraState *s, int on)
{
    s->speaker = on;
    /* audio_be_enable (s->voice, on); */
}

static void control (vibraState *s, int hold)
{
    int nchan = s->use_hdma ? s->hdma : s->dma;
    s->dma_running = hold;

    //ldebug("hold %d high %d nchan %d\n", hold, s->use_hdma, nchan);

    if (hold) {
        if (s->recording) {
	    if (!s->voice_in) {
                hold_DREQ(s, nchan);
	    }
            audio_be_set_active_in(s->audio_be, s->voice_in, 1);
        } else {
	    if (!s->voice) {
                hold_DREQ(s, nchan);
	    }
            audio_be_set_active_out(s->audio_be, s->voice, 1);
        }
    }
    else {
        release_DREQ(s, nchan);
        if (s->recording) {
            audio_be_set_active_in(s->audio_be, s->voice_in, 0);
        } else {
            audio_be_set_active_out(s->audio_be, s->voice, 0);
        }
    }
}

static void aux_timer (void *opaque)
{
    vibraState *s = opaque;
    s->can_write = 1;
    qemu_irq_raise (s->pic);
}

#define DMA8_AUTO 1
#define DMA8_HIGH 2

static void continue_dma8 (vibraState *s)
{
    if (s->freq > 0) {
        struct audsettings as;

        s->audio_free = 0;
        s->audio_avail = 0;

        as.freq = s->freq;
        as.nchannels = 1 << s->fmt_stereo;
        as.fmt = s->fmt;
        as.big_endian = false;

        if (s->recording) {
            s->voice_in = audio_be_open_in(
                s->audio_be,
                s->voice_in,
                "vibra",
                s,
                SB_adc_callback,
                &as
                );
        } else {
            s->voice = audio_be_open_out(
                s->audio_be,
                s->voice,
                "vibra",
                s,
                SB_audio_callback,
                &as
                );
        }
    vibra_update_cqm_volume(s);
	vibra_update_voice_volume(s);
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

static void dma_cmd8 (vibraState *s, int mask, int dma_len)
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

static void adc_cmd8 (vibraState *s, int mask, int dma_len)
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
        s->block_size &= ~s->fmt_stereo;
    }

    s->freq >>= s->fmt_stereo;
    s->left_till_irq = s->block_size;
    s->bytes_per_second = (s->freq << s->fmt_stereo);
    s->dma_auto = (mask & DMA8_AUTO) != 0;
    s->align = (1 << s->fmt_stereo) - 1;

    if (s->block_size & s->align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", s->block_size, s->align + 1);
    }

    ldebug("adc freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d",
            s->freq, s->fmt_stereo, s->fmt_signed, s->fmt_bits,
            s->block_size, s->dma_auto, s->fifo, s->highspeed);

    if (s->freq > 0) {
        struct audsettings as;
        s->audio_avail = 0;
        as.freq = s->freq;
        as.nchannels = 1 << s->fmt_stereo;
        as.fmt = s->fmt;
        as.big_endian = false;

        s->voice_in = audio_be_open_in(
            s->audio_be, s->voice_in, "vibra", s, SB_adc_callback, &as);
    }
    vibra_update_voice_volume(s);

    s->recording = 1;
    control(s, 1);
    speaker(s, 1);
}

static void dma_cmd (vibraState *s, uint8_t cmd, uint8_t d0, int dma_len)
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
           wonders of vibra yet again */
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
        as.big_endian = false;

        s->voice = audio_be_open_out(
            s->audio_be,
            s->voice,
            "vibra",
            s,
            SB_audio_callback,
            &as
            );
	vibra_update_voice_volume(s);
    }

    control (s, 1);
    speaker (s, 1);
}

static inline void dsp_out_data (vibraState *s, uint8_t val)
{
    ldebug("outdata 0x%x", val);
    if ((size_t) s->out_data_len < sizeof (s->out_data)) {
        s->out_data[s->out_data_len++] = val;
    }
}

static inline uint8_t dsp_get_data (vibraState *s)
{
    if (s->in_index) {
        return s->in2_data[--s->in_index];
    }
    else {
        warn_report("vibra: buffer underflow");
        return 0;
    }
}

static void command (vibraState *s, uint8_t cmd)
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

        case 0x1e:              /* Auto-Initialize DMA ADC, 8-bit */
            adc_cmd8 (s, DMA8_AUTO, -1);
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

static uint16_t dsp_get_lohi (vibraState *s)
{
    uint8_t hi = dsp_get_data (s);
    uint8_t lo = dsp_get_data (s);
    return (hi << 8) | lo;
}

static uint16_t dsp_get_hilo (vibraState *s)
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

static int16_t decode_adpcm_4bit(uint8_t code, vibraState *s) {
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

static void complete (vibraState *s)
{
    int d0, d1, d2;
    ldebug("complete command 0x%x, in_index %d, needed_bytes %d",
            s->cmd, s->in_index, s->needed_bytes);

    if (s->cmd > 0xaf && s->cmd < 0xd0) {
        d2 = dsp_get_data(s);
        d1 = dsp_get_data(s);
        d0 = dsp_get_data(s);

        if (s->cmd & 8) {
            ldebug("Executing ADC cmd=0x%x mode=%d len=%d", s->cmd, d0, d1 + (d2 << 8));

            s->use_hdma = s->cmd < 0xc0;
            s->fmt_bits = (s->cmd >> 4) == 11 ? 16 : 8;
            s->fmt_signed = (d0 >> 4) & 1;
            s->fmt_stereo = (d0 >> 5) & 1;
            s->dma_auto = (s->cmd >> 2) & 1;
            s->fifo = (s->cmd >> 1) & 1;

            if (16 == s->fmt_bits) {
                s->fmt = s->fmt_signed ? AUDIO_FORMAT_S16 : AUDIO_FORMAT_U16;
            } else {
                s->fmt = s->fmt_signed ? AUDIO_FORMAT_S8 : AUDIO_FORMAT_U8;
            }

            s->block_size = (d1 + (d2 << 8) + 1) << (s->fmt_bits == 16);
            s->left_till_irq = s->block_size;
            s->bytes_per_second = (s->freq << s->fmt_stereo) << (s->fmt_bits == 16);
            s->align = (1 << (s->fmt_stereo + (s->fmt_bits == 16))) - 1;

            if (s->block_size & s->align) {
                qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                              " alignment %d\n", s->block_size, s->align + 1);
            }

            if (s->freq) {
                struct audsettings as;
                s->audio_avail = 0;

                as.freq = s->freq;
                as.nchannels = 1 << s->fmt_stereo;
                as.fmt = s->fmt;
                as.big_endian = false;

                s->voice_in = audio_be_open_in(
                    s->audio_be, s->voice_in, "vibra", s, SB_adc_callback, &as);
            }

            s->recording = 1;
            control(s, 1);
            speaker(s, 1);
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
                audio_be_set_active_out(s->audio_be, s->voice, 1);
                audio_be_write(s->audio_be, s->voice, &sample, 1);
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
             * and 0x42 the input sample rate, but in fact vibra hardware
             * seems to have only a single sample rate under the hood,
             * and FT2 sets output freq with this (go figure).  Compare:
             * http://homepages.cae.wisc.edu/~brodskye/vibradoc/vibradoc.html#SamplingRate
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
            qemu_log_mask(LOG_UNIMP, "vibra: ADPCM command 0x%x len %d not implemented\n", 
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
            d0 = dsp_get_data (s);
            s->e2_valadd += ((uint8_t) d0) ^ s->e2_valxor;
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

static void legacy_reset (vibraState *s)
{
    struct audsettings as;

    s->recording = 0;
    s->audio_avail = 0;

    s->freq = 11025;
    s->fmt_signed = 0;
    s->fmt_bits = 8;
    s->fmt_stereo = 0;

    s->audio_free = 0;

    as.freq = s->freq;
    as.nchannels = 1;
    as.fmt = AUDIO_FORMAT_U8;
    as.big_endian = false;

    s->voice = audio_be_open_out(
        s->audio_be,
        s->voice,
        "vibra",
        s,
        SB_audio_callback,
        &as
        );

    s->voice_in = audio_be_open_in(
        s->audio_be,
        s->voice_in,
        "vibra",
        s,
        SB_adc_callback,
        &as
        );

    /* Not sure about that... */
    /* audio_be_set_active_out (s->audio_be, s->voice, 1); */
}

static void reset (vibraState *s)
{
    qemu_irq_lower (s->pic);
    if (s->dma_auto) {
        qemu_irq_raise (s->pic);
        qemu_irq_lower (s->pic);
    }

    if (s->voice_in) {
        audio_be_close_in(s->audio_be, s->voice_in);
        s->voice_in = NULL;
    }
    s->recording = 0;
    s->audio_avail = 0;

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
    vibraState *s = opaque;
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
                warn_report("vibra: in data overrun");
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
    vibraState *s = opaque;
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
                warn_report("vibra: empty output buffer for command 0x%x",
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
        /* warn_report("vibra: timer interrupt clear"); */
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
    warn_report("vibra: dsp_read 0x%x error", nport);
    return 0xff;
}

static void reset_mixer (vibraState *s)
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
        s->mixer_regs[i] = 0xD8;
    }
}

static void mixer_write_indexb(void *opaque, uint32_t nport, uint32_t val)
{
    vibraState *s = opaque;
    (void) nport;
    s->mixer_nreg = val;
}

static void mixer_write_datab(void *opaque, uint32_t nport, uint32_t val)
{
    vibraState *s = opaque;

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
    vibra_update_voice_volume(s);
}

static uint32_t mixer_read(void *opaque, uint32_t nport)
{
    vibraState *s = opaque;

    (void) nport;
#ifndef DEBUG_vibra_MOST
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

static int write_audio (vibraState *s, int nchan, int dma_pos,
                        int dma_len, int len)
{
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
    IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);
    int temp, net;
    QEMU_UNINITIALIZED uint8_t tmpbuf[4096];

    temp = len;
    net = 0;

    while (temp) {
        int copied = 0;
        size_t to_copy;

        to_copy = temp;
        if (to_copy > sizeof (tmpbuf)) {
            to_copy = sizeof (tmpbuf);
        }
        if (to_copy > dma_len) {
            to_copy = dma_len;
        }
        
        to_copy &= ~s->align;
        if (!to_copy) {
            break;
        }

        int left = dma_len - dma_pos;
        if (to_copy > left) {
            copied = k->read_memory(isa_dma, nchan, tmpbuf, dma_pos, left);
            copied += k->read_memory(isa_dma, nchan, tmpbuf + left, 0, to_copy - left);
        } else {
            copied = k->read_memory(isa_dma, nchan, tmpbuf, dma_pos, to_copy);
        }

        copied = audio_be_write(s->audio_be, s->voice, tmpbuf, copied);

        temp -= copied;
        dma_pos = (dma_pos + copied) % dma_len;
        net += copied;

        if (!copied) {
            break;
        }
    }

    return net;
}


static int SB_read_DMA (void *opaque, int nchan, int dma_pos, int dma_len)
{
    vibraState *s = opaque;

    if (s->recording) {
        IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
        IsaDmaClass *k = ISADMA_GET_CLASS(isa_dma);
        uint8_t tmpbuf[4096];
        int till, avail, to_copy, acquired = 0, written = 0;

        if (s->block_size <= 0) {
            return dma_pos;
        }

        if (s->left_till_irq < 0) {
            s->left_till_irq = s->block_size;
        }

        if (s->voice_in) {
            avail = s->audio_avail & ~s->align;
            if (avail <= 0) {
                release_DREQ(s, nchan);
                return dma_pos;
            }
        } else {
            avail = dma_len;
        }

        till = s->left_till_irq;
        to_copy = MIN(avail, till);
        to_copy = MIN(to_copy, dma_len - dma_pos);
        if (to_copy > (int)sizeof(tmpbuf)) {
            to_copy = sizeof(tmpbuf);
        }

        acquired = audio_be_read(s->audio_be, s->voice_in, tmpbuf, to_copy);
        if (acquired > 0) {
            written = k->write_memory(isa_dma, nchan, tmpbuf, dma_pos, acquired);
        }

        dma_pos = (dma_pos + written) % dma_len;
        s->left_till_irq -= written;
        s->audio_avail -= written;

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

    if (s->cmd == 0x75) {
        uint8_t ref_byte;
     
        k->read_memory(isa_dma, nchan, &ref_byte, dma_pos, 1);
        s->adpcm_valpred = (int16_t)((ref_byte - 128) << 8);
        s->adpcm_index = 0;

        dma_pos = (dma_pos + 1) % dma_len;
        s->cmd = 0x74;
    }

    if (s->cmd == 0x74) {
        to_copy = MIN(to_copy, dma_len - dma_pos);
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

        int bytes_out = audio_be_write(s->audio_be, s->voice, tmpbuf, adpcm_copied * 4);
        written = bytes_out / 4;
    } else {
        written = write_audio(s, nchan, dma_pos, dma_len, to_copy);
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
    vibraState *s = opaque;
    int nchan = s->use_hdma ? s->hdma : s->dma;
    s->audio_free = free;
    /* run the DMA engine to call SB_read_DMA immediately */
    hold_DREQ(s, nchan);
}

static void SB_adc_callback(void *opaque, int avail)
{
    vibraState *s = opaque;
    int nchan = s->use_hdma ? s->hdma : s->dma;
    s->audio_avail = avail;
    hold_DREQ(s, nchan);
}

static int vibra_post_load (void *opaque, int version_id)
{
    vibraState *s = opaque;

    if (s->voice) {
        audio_be_close_out(s->audio_be, s->voice);
        s->voice = NULL;
    }

    if (s->voice_in) {
        audio_be_close_in(s->audio_be, s->voice_in);
        s->voice_in = NULL;
    }

    if (s->dma_running) {
        if (s->freq) {
            struct audsettings as;

            s->audio_free = 0;
            s->audio_avail = 0;

            as.freq = s->freq;
            as.nchannels = 1 << s->fmt_stereo;
            as.fmt = s->fmt;
            as.big_endian = false;

            if (s->recording) {
                s->voice_in = audio_be_open_in(
                    s->audio_be,
                    s->voice_in,
                    "vibra",
                    s,
                    SB_adc_callback,
                    &as
                    );
            } else {
                s->voice = audio_be_open_out(
                    s->audio_be,
                    s->voice,
                    "vibra",
                    s,
                    SB_audio_callback,
                    &as
                    );
            }
        }

        control (s, 1);
        speaker (s, s->speaker);
    }
    return 0;
}

static const VMStateDescription vmstate_vibra = {
    .name = "vibra",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = vibra_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UNUSED(  4 /* irq */
                       + 4 /* dma */
                       + 4 /* hdma */
                       + 4 /* port */
                       + 4 /* ver */),
        VMSTATE_INT32 (in_index, vibraState),
        VMSTATE_INT32 (out_data_len, vibraState),
        VMSTATE_INT32 (fmt_stereo, vibraState),
        VMSTATE_INT32 (fmt_signed, vibraState),
        VMSTATE_INT32 (fmt_bits, vibraState),
        VMSTATE_UINT32 (fmt, vibraState),
        VMSTATE_INT32 (dma_auto, vibraState),
        VMSTATE_INT32 (block_size, vibraState),
        VMSTATE_INT32 (fifo, vibraState),
        VMSTATE_INT32 (freq, vibraState),
        VMSTATE_INT32 (time_const, vibraState),
        VMSTATE_INT32 (speaker, vibraState),
        VMSTATE_INT32 (needed_bytes, vibraState),
        VMSTATE_INT32 (cmd, vibraState),
        VMSTATE_INT32 (use_hdma, vibraState),
        VMSTATE_INT32 (highspeed, vibraState),
        VMSTATE_INT32 (can_write, vibraState),
        VMSTATE_INT32 (v2x6, vibraState),

        VMSTATE_UINT8 (csp_param, vibraState),
        VMSTATE_UINT8 (csp_value, vibraState),
        VMSTATE_UINT8 (csp_mode, vibraState),
        VMSTATE_UINT8 (csp_param, vibraState),
        VMSTATE_BUFFER (csp_regs, vibraState),
        VMSTATE_UINT8 (csp_index, vibraState),
        VMSTATE_BUFFER (csp_reg83, vibraState),
        VMSTATE_INT32 (csp_reg83r, vibraState),
        VMSTATE_INT32 (csp_reg83w, vibraState),

        VMSTATE_BUFFER (in2_data, vibraState),
        VMSTATE_BUFFER (out_data, vibraState),
        VMSTATE_UINT8 (test_reg, vibraState),
        VMSTATE_UINT8 (last_read_byte, vibraState),

        VMSTATE_INT32 (nzero, vibraState),
        VMSTATE_INT32 (left_till_irq, vibraState),
        VMSTATE_INT32 (dma_running, vibraState),
        VMSTATE_INT32 (recording, vibraState),
        VMSTATE_INT32 (audio_avail, vibraState),
        VMSTATE_INT32 (bytes_per_second, vibraState),
        VMSTATE_INT32 (align, vibraState),

        VMSTATE_INT32 (mixer_nreg, vibraState),
        VMSTATE_BUFFER (mixer_regs, vibraState),

        VMSTATE_END_OF_LIST ()
    }
};

static MemoryRegionPortio vibra_ioport_list[] = {
    {  4, 1, 1, .write = mixer_write_indexb },
    {  5, 1, 1, .read = mixer_read, .write = mixer_write_datab },
    {  6, 1, 1, .read = dsp_read, .write = dsp_write },
    { 10, 1, 1, .read = dsp_read },
    { 12, 1, 1, .write = dsp_write },
    { 12, 4, 1, .read = dsp_read },
    PORTIO_END_OF_LIST (),
};

static MemoryRegionPortio cqm_portio_list[] = {
    { 0, 4, 1, .read = vibra_cqm_read, .write = vibra_cqm_write },
    PORTIO_END_OF_LIST (),
};

static const MemoryRegionOps vibra_mpu_ops = {
    .read = mpu_read,
    .write = mpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void vibra_initfn (Object *obj)
{
    vibraState *s = vibra (obj);

    s->cmd = -1;
}

static void cqm_realize (vibraState *s, ISADevice *isadev)
{
    struct audsettings as = {
        as.freq = 49716,
        as.nchannels = 2,
        as.fmt = AUDIO_FORMAT_S16,
        as.big_endian = false,
    };

    CQM_Reset (&s->cqm, 49716, 49716);

    s->voice_cqm = audio_be_open_out(s->audio_be, s->voice_cqm, "vibra-cqm", s, vibra_cqm_callback, &as);
    
    if (!s->voice_cqm) {
        warn_report("vibra: CQM audio voice open failed; FM disabled");
        return;
    }

    s->cqm_samps = audio_be_get_buffer_size_out(s->audio_be, s->voice_cqm) / 4;
    s->cqm_mix = g_malloc0(s->cqm_samps * 4);

    isa_register_portio_list(isadev, &s->cqm_portio_list, s->port, cqm_portio_list, s, "vibra-cqm");
	isa_register_portio_list(isadev, &s->hack_portio_list, 0x388, cqm_portio_list, s, "vibra-cqm");
}


static void vibra_realizefn (DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE (dev);
    ISABus *bus = isa_bus_from_device(isadev);
    vibraState *s = vibra (dev);
    IsaDmaClass *k;

    if (!audio_be_check(&s->audio_be, errp)) {
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

    s->mixer_regs[0x80] = magic_of_irq (s->irq);
    s->mixer_regs[0x81] = (1 << s->dma) | (1 << s->hdma);
    s->mixer_regs[0x82] = 2 << 5;

    s->csp_regs[5] = 1;
    s->csp_regs[9] = 0xf8;

    /* just in case */
    s->align = (s->fmt_bits == 16) ? 1 : 0;

    cqm_realize(s, isadev);

    reset_mixer (s);
    s->aux_ts = timer_new_ns(QEMU_CLOCK_VIRTUAL, aux_timer, s);
    if (!s->aux_ts) {
        error_setg(errp, "warning: Could not create auxiliary timer");
    }

    isa_register_portio_list(isadev, &s->portio_list, s->port, vibra_ioport_list, s, "vibra");

    fifo8_create(&s->mpu_fifo, 1024);

    qemu_chr_fe_set_handlers(&s->mpu_chr, mpu_can_receive, mpu_receive, NULL, NULL, s, NULL, true);

    if (s->mpu_chr.chr) {
        memory_region_init_io(&s->mpu_io, OBJECT(s), &vibra_mpu_ops, s, "vibra-mpu401", 2);
        isa_register_ioport(isadev, &s->mpu_io, 0x330);
        qemu_chr_fe_set_handlers(&s->mpu_chr, NULL, NULL, NULL, NULL, s, NULL, true);
    }

    k = ISADMA_GET_CLASS(s->isa_hdma);
    k->register_channel(s->isa_hdma, s->hdma, SB_read_DMA, s);

    k = ISADMA_GET_CLASS(s->isa_dma);
    k->register_channel(s->isa_dma, s->dma, SB_read_DMA, s);

    s->can_write = 1;
}

static const Property vibra_properties[] = {
    DEFINE_AUDIO_PROPERTIES(vibraState, audio_be),
    DEFINE_PROP_UINT32 ("version", vibraState, ver,  0x0413), /* 4.13 */
    DEFINE_PROP_UINT32 ("iobase",  vibraState, port, 0x220),
    DEFINE_PROP_UINT32 ("irq",     vibraState, irq,  5),
    DEFINE_PROP_UINT32 ("dma",     vibraState, dma,  1),
    DEFINE_PROP_UINT32 ("dma16",   vibraState, hdma, 5),
    DEFINE_PROP_CHR    ("mpu401", vibraState, mpu_chr),
};

static void vibra_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS (klass);

    dc->realize = vibra_realizefn;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Creative Sound Blaster ViBRA 16";
    dc->vmsd = &vmstate_vibra;
    device_class_set_props(dc, vibra_properties);
}

static const TypeInfo vibra_info = {
    .name          = TYPE_vibra,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof (vibraState),
    .instance_init = vibra_initfn,
    .class_init    = vibra_class_initfn,
};

static void vibra_register_types (void)
{
    type_register_static (&vibra_info);
    audio_register_model("sb16vibra", "Creative Sound Blaster ViBRA 16", TYPE_vibra);
}

type_init (vibra_register_types)
