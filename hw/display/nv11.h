/*
 * Nvidia Geforce2 MX400 (NV11B)
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

#ifndef HW_NV11_H
#define HW_NV11_H

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/display/vga_int.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "hw/display/edid.h"
#include "system/memory.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"

/* BAR sizes */
#define NV11_BAR0_SIZE           0x1000000   /* 16 MB */
#define NV11_BAR1_VRAM_SIZE      0x4000000   /* 64 MB (Needs work for 32MB variants some day) */
#define NV11_BAR1_VRAM_SIZE_MB   64
#define NV11_ROM_SIZE            0x010000    /* 64 KB */

/* BAR0 region offsets */
#define NV11_PMC_OFF             0x000000
#define NV11_PMC_END             0x001000
#define NV11_PBUS_OFF            0x001000
#define NV11_PBUS_END            0x002000
#define NV11_PFB_OFF             0x100000
#define NV11_PFB_END             0x101000
#define NV11_PEXTDEV_OFF         0x101000
#define NV11_PEXTDEV_END         0x102000
#define NV11_PROM_OFF            0x300000
#define NV11_PROM_END            0x310000
#define NV11_PGRAPH_OFF          0x400000
#define NV11_PGRAPH_END          0x401000
#define NV11_PFIFO_OFF           0x002000
#define NV11_PTMR_OFF            0x009000
#define NV11_PTMR_END            0x00A000
#define NV11_PRAMIN_OFF          0x700000
#define NV11_FIFO_OFF            0x800000
#define NV11_FIFO_END            0x810000
#define NV11_PCRTC0_OFF          0x600000
#define NV11_PCRTC0_END          0x601000
#define NV11_PRMCIO0_OFF         0x601000
#define NV11_PRMCIO0_END         0x603000
#define NV11_PCRTC1_OFF          0x602000
#define NV11_PCRTC1_END          0x603000
#define NV11_PRMCIO1_OFF         0x603000
#define NV11_PRMCIO1_END         0x605000
#define NV11_PRAMDAC0_OFF        0x680000
#define NV11_PRAMDAC0_END        0x682000
#define NV11_PRAMDAC1_OFF        0x682000
#define NV11_PRAMDAC1_END        0x684000
#define NV11_PRMDIO_OFF          0x6813C0
#define NV11_PRMDIO_END          0x6813D0
#define NV11_PVIO_OFF            0x0C0000
#define NV11_PVIO_END            0x0C1000

/* PBUS */
#define NV11_PBUS_DEBUG_1        0x000084
#define NV11_PBUS_PCI_NV_19      0x00084C
#define NV11_PBUS_PCI_NV_20      0x000850
#define NV11_PBUS_PCI_BAR1       0x000814   /* PCI config BAR1 (VRAM base) mirror */

/* PMC */
#define NV11_PMC_BOOT_0          0x000000   /* chipset ID readback */
#define NV11_PMC_BOOT_0_NV11     0x011000B2 /* NV11: chipset 0x11, stepping 0xB2 */

/* PFB registers */
#define NV11_PFB_FIFO_DATA       0x00020C   /* RAM amount MB [31:20] */
#define NV11_PFB_FIFO_DATA_64MB  0x04000000 /* 64 MB RAM. Read BAR1 headers for more info. */

/* PCRTC registers (within 0x1000 block) */
#define NV11_PCRTC_INTR          0x000100
#define NV11_PCRTC_INTR_EN       0x000140

/* PRAMDAC registers (within 0x2000 block) */
#define NV11_PRAMDAC_NVPLL       0x000500
#define NV11_PRAMDAC_MPLL        0x000504
#define NV11_PRAMDAC_VPLL        0x000508
#define NV11_PRAMDAC_PLL_SEL     0x00050C
#define NV11_PRAMDAC_VPLL2       0x000520
#define NV11_PRAMDAC_GEN_CTL     0x000600
#define NV11_PRAMDAC_TEST_CTL    0x000608
#define NV11_PRAMDAC_TESTPOINT   0x000610
#define NV11_PRAMDAC_TV_SETUP    0x000700
#define NV11_PRAMDAC_FP_VDISP    0x000800
#define NV11_PRAMDAC_FP_VT       0x000804
#define NV11_PRAMDAC_FP_HDISP    0x000820
#define NV11_PRAMDAC_FP_HT       0x000824
#define NV11_PRAMDAC_FP_TG_CTL   0x000848
#define NV11_PRAMDAC_FP_DBG0     0x000880
#define NV11_PRAMDAC_FP_DBG1     0x000884
#define NV11_PRAMDAC_FP_DBG2     0x000888

/* PEXTDEV registers (within 0x1000 block) */
#define NV11_PEXTDEV_BOOT_0      0x000000

/* PTIMER registers (within 0x1000 block, BAR0 0x9000) */
#define NV11_PTIMER_INTR         0x000100
#define NV11_PTIMER_INTR_EN      0x000140
#define NV11_PTIMER_NUMERATOR    0x000200
#define NV11_PTIMER_DENOMINATOR  0x000210
#define NV11_PTIMER_TIME_LOW     0x000400
#define NV11_PTIMER_TIME_HIGH    0x000410
#define NV11_PTIMER_ALARM        0x000420

/* PGRAPH registers (within 0x1000 block, BAR0 0x400000) */
#define NV11_PGRAPH_INTR         0x000100
#define NV11_PGRAPH_INTR_EN      0x000140
#define NV11_PGRAPH_STATUS       0x000700   /* bit0 = busy */
#define NV11_PGRAPH_CTX_CTRL     0x000710
#define NV11_PGRAPH_FIFO         0x000720   /* bit0 = fifo enable */

/* PMC */
#define NV11_PMC_INTR_HOST                   0x000100   /* no pending IRQs = 0 */

/* PFIFO */
#define NV11_PFIFO_RUNOUT_STATUS             0x002400
#define NV11_PFIFO_CACHES                    0x002500
#define NV11_PFIFO_CACHE0_STATUS             0x003014
#define NV11_PFIFO_CACHE1_STATUS             0x003214
#define NV11_PFIFO_STATUS_EMPTY              0x00000010u /* EMPTY bit, RANOUT=0 */

/* PFIFO CACHE1 DMA context */
#define NV11_PFIFO_CACHE1_DMA_FETCH          0x003224
#define NV11_PFIFO_CACHE1_DMA_CTL            0x003230
#define NV11_PFIFO_DMA_CTL_VALID             0x80000000u

/* FIFO window
 * NV11 has 8 subchannels of 0x2000 bytes each. Every channel:
 *   +0x00   U032   context / RAMIN-instance binding
 *   +0x10   U016   FIFOFree (free bytes in this subchannel)
 *   +0x12   U016   NOP
 *   +0x40   U032   DMA_PUT  (write-only, pushbuffer end, sync FIFO)
 *   +0x44   U032   DMA_GET  (read-only)
 *   +0x48   U032   REF      (read-only, fence counter, NV10+)
 *   +0x300+        32-bit method register bank
 *
 * FIFO subchannels
 *
 * The 8 subchannels carry no fixed Nvidia-defined role: each is bound to an
 * object by a context write (0x8000000X, decoded through the RAMIN
 * instance/object table), and method traffic is dispatched purely on the
 * bound class. A subchannel with no binding (ch_class = 0) ignores methods.
 */
#define NV11_FIFO_CHANNELS       8
#define NV11_FIFO_CHAN_SIZE      0x2000
#define NV11_FIFO_CONTEXT_OFF    0x0000
#define NV11_FIFO_FREE_OFF       0x0010
#define NV11_FIFO_METHOD_OFF     0x0300
#define NV11_FIFO_DMA_PUT_OFF    0x0040
#define NV11_FIFO_DMA_GET_OFF    0x0044
#define NV11_FIFO_FULL           0x0800   /* FIFOFree start/idle watermark (bytes) */
#define NV11_FIFO_DRAIN_NS       (NANOSECONDS_PER_SECOND / 1000)

/* DMA pusher: ring at VRAM FbUsableSize, 32 KB, word-addressed */
#define NV11_DMA_RING_SIZE       0x8000
#define NV11_DMA_RING_MASK       0x7FFF

/* 2D Object Class IDs (Defined by envytools method Nvidia Generation_Command ) */
#define NV11_CLASS_NONE          0x00
#define NV11_CLASS_PATT          0x18   /* NV1_PATTERN */
#define NV11_CLASS_CLIP          0x19   /* NV1_CLIP */
#define NV11_CLASS_IFC           0x21   /* NV1_IFC (pixmap) */
#define NV11_CLASS_ROP           0x43   /* NV3_ROP */
#define NV11_CLASS_PATT_NV4      0x44   /* NV4_PATTERN */
#define NV11_CLASS_LINE          0x48   /* NV1_LIN */
#define NV11_CLASS_GDI           0x4B   /* NV3_GDI */
#define NV11_CLASS_RECT_NV4      0x4A   /* NV4_GDI (fill rect) */
#define NV11_CLASS_LINE_NV4      0x5C   /* NV4_LIN */
#define NV11_CLASS_BLT           0x5F   /* NV4_BLIT */
#define NV11_CLASS_BLT_NV15      0x9F   /* NV15_BLIT */
#define NV11_CLASS_LIN           0x1C   /* NV1_LIN */
#define NV11_CLASS_SURF          0x62   /* NV4_SURFACE */
#define NV11_CLASS_DMA           0x30   /* NV_DMA_IN_MEMORY (legacy) */
#define NV11_CLASS_M2MF          0x39   /* NV3_M2MF */
#define NV11_CLASS_SIFM          0x77   /* NV4_SIFM (stretch blit) */
#define NV11_CLASS_NOP           0x100  /* NV_NOP */
#define NV11_CLASS_NOTIFY        0x104  /* NV_NOTIFY */

/* RAMIN instance table: context value 0x8000000X selects instance X, whose
 * entry lives at (X ^ 0x10) * 8 within the table; dword1's low 15 bits are
 * the (word) pointer to the class object at NV11_RAMIN_OBJ_BASE + p << 4. */
#define NV11_RAMIN_OBJ_BASE      0x700000
#define NV11_RAMIN_INST_TABLE    0x710000

/* Surface geometry */
#define NV11_2D_SURF_OFF_0       0x000640
#define NV11_2D_SURF_PITCH_0     0x000670
#define NV11_2D_SURF_FMT         0x000724   /* nibble0: 1=8, 2=15, 5=16, 7=32 */

/* 2D Object Offsets */
#define NV11_2D_ROP3             0x300
#define NV11_2D_CLIP_TL          0x300
#define NV11_2D_CLIP_WH          0x304
#define NV11_2D_PATT_SHAPE       0x308
#define NV11_2D_PATT_COLOR0      0x310
#define NV11_2D_PATT_COLOR1      0x314
#define NV11_2D_PATT_MONO0       0x318
#define NV11_2D_PATT_MONO1       0x31C
#define NV11_2D_SURF_FMT_M       0x300   /* class 0x62: 1=8, 2=15, 4=16, 6=24 */
#define NV11_2D_SURF_PITCH_M     0x304   /* (dst<<16)|src */
#define NV11_2D_SURF_OFF_M       0x308
#define NV11_2D_SURF_OFFDST_M    0x30C
#define NV11_2D_BLT_TL_SRC       0x300
#define NV11_2D_BLT_TL_DST       0x304
#define NV11_2D_BLT_WH           0x308
#define NV11_2D_BITMAP_COLOR1A   0x3FC
#define NV11_2D_BITMAP_RECT_TL   0x400
#define NV11_2D_BITMAP_RECT_WH   0x404
#define NV11_2D_BITMAP_CLIPC_TL  0xBEC   /* transparent color-expand */
#define NV11_2D_BITMAP_CLIPC_BR  0xBF0
#define NV11_2D_BITMAP_COLOR1C   0xBF4
#define NV11_2D_BITMAP_WHC       0xBF8
#define NV11_2D_BITMAP_POINTC    0xBFC
#define NV11_2D_BITMAP_MONOC     0xC00
#define NV11_2D_BITMAP_CLIPE_TL  0x13E4  /* opaque color-expand */
#define NV11_2D_BITMAP_CLIPE_BR  0x13E8
#define NV11_2D_BITMAP_COLOR0E   0x13EC
#define NV11_2D_BITMAP_COLOR1E   0x13F0
#define NV11_2D_BITMAP_WHINE     0x13F4
#define NV11_2D_BITMAP_WHOUTE    0x13F8
#define NV11_2D_BITMAP_POINTE    0x13FC
#define NV11_2D_BITMAP_MONOE     0x1400
#define NV11_2D_LINE_COLOR       0x304
#define NV11_2D_LINE_P0          0x400
#define NV11_2D_LINE_P1          0x404
#define NV11_2D_LINE_P0B         0x408
#define NV11_2D_LINE_P1B         0x40C

/* NV4_GDI (class 0x4A) offsets within the 0x2000 subchannel window.  */
#define NV11_2D_RECT_FMT         0x300   /* ignored (like other *_FORMAT) */
#define NV11_2D_RECT_SOLID_COLOR 0x3FC   /* == BITMAP_COLOR1A */
#define NV11_2D_RECT_SOLID_TL    0x400   /* == BITMAP_RECT_TL (y<<16)|x */
#define NV11_2D_RECT_SOLID_WH    0x404   /* == BITMAP_RECT_WH (h<<16)|w */
#define NV11_2D_RECT_X0          0x7EC   /* one-color CLIP_POINT0 */
#define NV11_2D_RECT_X1          0x7F0   /* one-color CLIP_POINT1 */
#define NV11_2D_RECT_XCOLOR      0x7F4   /* one-color foreground */
#define NV11_2D_RECT_XSIZE       0x7F8   /* one-color (h<<16)|w */
#define NV11_2D_RECT_XPOINT      0x7FC   /* one-color (y<<16)|x */
#define NV11_2D_RECT_XDATA       0x800
#define NV11_2D_RECT_XDATA_END   0x900
#define NV11_2D_RECT_Y0          0xBE4   /* two-color CLIP_POINT0 */
#define NV11_2D_RECT_Y1          0xBE8   /* two-color CLIP_POINT1 */
#define NV11_2D_RECT_YBG         0xBEC   /* two-color COLOR_0 */
#define NV11_2D_RECT_YFG         0xBF0   /* two-color COLOR_1 */
#define NV11_2D_RECT_YSIZE_IN    0xBF4
#define NV11_2D_RECT_YSIZE_OUT   0xBF8
#define NV11_2D_RECT_YPOINT      0xBFC
#define NV11_2D_RECT_YDATA       0xC00
#define NV11_2D_RECT_YDATA_END   0xD00

/* NV4_SIFM / stretch blit (class 0x77). */
#define NV11_2D_SIFM_FMT         0x300
#define NV11_2D_SIFM_OPER        0x304
#define NV11_2D_SIFM_CLIP_TL     0x308
#define NV11_2D_SIFM_CLIP_WH     0x30C
#define NV11_2D_SIFM_DST_TL      0x310
#define NV11_2D_SIFM_DST_WH      0x314
#define NV11_2D_SIFM_DUDX        0x318
#define NV11_2D_SIFM_DVDY        0x31C
#define NV11_2D_SIFM_SRC_WH      0x400
#define NV11_2D_SIFM_SRC_FMT     0x404
#define NV11_2D_SIFM_SRC_OFF     0x408
#define NV11_2D_SIFM_SRC_POINT   0x40C   /* write triggers the blit */
#define NV11_2D_SIFM_FMT_X8R8G8B8 0x04
#define NV11_2D_SIFM_FMT_UYVY     0x06
#define NV11_2D_SIFM_FMT_YUYV     0x05
#define NV11_2D_SIFM_FMT_MASK     0xFF

/* NV3_M2MF (class 0x39). */
#define NV11_2D_M2MF_DMA_NOTIFY  0x180
#define NV11_2D_M2MF_DMA_IN      0x184
#define NV11_2D_M2MF_DMA_OUT     0x188
#define NV11_2D_M2MF_OFF_IN      0x30C
#define NV11_2D_M2MF_OFF_OUT     0x310
#define NV11_2D_M2MF_PITCH_IN    0x314
#define NV11_2D_M2MF_PITCH_OUT   0x318
#define NV11_2D_M2MF_LINE_LEN    0x31C
#define NV11_2D_M2MF_LINE_COUNT  0x320   /* write triggers the copy */
#define NV11_2D_M2MF_FORMAT      0x324
#define NV11_2D_M2MF_BUF_NOTIFY  0x328

/* Notify (classes 0x100/0x104). */
#define NV11_2D_NOTIFY_METHOD    0x104
#define NV11_NOTIFY_STATUS_OFF   0x0C
#define NV11_NOTIFY_STATUS_DONE  0x00000000

#define NV11_2D_EXP_BUF_DWORDS   128

/* Window opcodes */
#define NV11_WINDOW_OP_INDEX     3
#define NV11_WINDOW_OP_READ      5
#define NV11_WINDOW_OP_WRITE     7

/* CRTC extended index */
#define NV11_CRTC_WIN_OP         0x38

/* Hardware cursor register locations (head 0) */
#define NV11_CRTC_HCUR_ADDR0     0x30   /* image addr bits 17-11, bit7 = ASI */
#define NV11_CRTC_HCUR_ADDR1     0x31   /* image addr bits 10-2, bit0 = ENABLE */
#define NV11_CRTC_HCUR_ADDR2     0x2F   /* image addr bits 31-24 */
#define NV11_PRAMDAC_CUR_POS     0x300  /* (Y<<16)|X cursor position */
#define NV11_PCRTC_CURSOR_CFG    0x810  /* 64x64 ARGB cursor configuration */

/* DDC / I2C. NV11 exposes one bit-banged DDC port through extended CRTC
 * levels, the write (drive) register drives them. */
#define NV11_DDC_BUS_A           0      /* CRTC 0x3e/0x3f, VGA / head 0 */
#define NV11_DDC_BUS_B           1      /* CRTC 0x36/0x37, DFP / head 1 */
#define NV11_DDC_BUSES           2
#define NV11_DDC_SLAVE_ADDR      0x50   /* monitor EDID address */

#define NV11_CRTC_DDC0_STATUS    0x36   /* bus B: SCL/SDA sense */
#define NV11_CRTC_DDC0_WR        0x37   /* bus B: SCL/SDA drive */
#define NV11_CRTC_DDC_STATUS     0x3E   /* bus A: SCL/SDA sense */
#define NV11_CRTC_DDC_WR         0x3F   /* bus A: SCL/SDA drive */

#define NV11_DDC_SCL_READ        (1 << 2)
#define NV11_DDC_SDA_READ        (1 << 3)
#define NV11_DDC_SDA_WRITE       (1 << 4)
#define NV11_DDC_SCL_WRITE       (1 << 5)

/* EIP handler for debugging */
static inline uint32_t nv11_get_eip(void)
{
    CPUState *cs = current_cpu ? current_cpu : first_cpu;

    if (cs && cs->cc && cs->cc->get_pc) {
        return (uint32_t)cs->cc->get_pc(cs);
    }
    return 0;
}

typedef struct NV11State {
    PCIDevice parent_dev;
    VGACommonState vga;

    MemoryRegion bar0;
    uint8_t     bar0_flat[NV11_BAR0_SIZE];
    MemoryRegion bar1;
    MemoryRegion window_io;
    uint8_t     *vram_ptr;      /* host pointer to vga.vram, for DMA ring */

    /* Serial I/O */
    uint32_t win_addr;      /* latched BAR0 offset (set by INDEX op) */
    uint8_t  win_op;        /* CRTC[0x38] & 7 */
    uint32_t win_write_latch;

    /* PBUS / PEXTDEV */
    uint32_t pextdev_boot_0;     /* 0x101000 */

    /* PTIMER (BAR0 0x9000) */
    uint32_t ptimer_intr;        /* 0x9100, bit0 = ALARM pending */
    uint32_t ptimer_intr_en;     /* 0x9140 */
    uint32_t ptimer_num;         /* 0x9200 numerator */
    uint32_t ptimer_denom;       /* 0x9210 denominator */
    uint32_t ptimer_alarm;       /* 0x9420 */
    uint64_t ptimer_base;        /* counter value at ptimer_base_ns */
    int64_t  ptimer_base_ns;     /* QEMU_CLOCK_VIRTUAL ns of base */

    /* PROM shadow gate */
    bool     rom_shadow_en;      /* 0x101850 bit0 */
    uint8_t  *prom_data;         /* VBIOS bytes */
    uint32_t prom_size;

    /* PRAMDAC */
    uint32_t pramdac[2][0x2000 / 4]; /* [head0/head1][offset/4] */

    /* PCRTC */
    uint32_t pcrtc_scratch[2][0x1000 / 4];

    /* Extended CRTC */
    uint8_t  nv_crtc_reg[256];

    /* FIFO window subchannel. One per 0x2000-byte channel. */
    struct {
        uint32_t methods[NV11_FIFO_CHAN_SIZE / 4]; /* Shadow of whole channel */
        uint16_t fifo_free;                        /* FIFOFree (bytes) */
        uint32_t pending;                          /* dwords queued, not drained */
        uint32_t dma_get;                          /* DMA pusher GET pointer (bytes) */
    } fifo[NV11_FIFO_CHANNELS];

    /* Per-subchannel object class, decoded from RAMIN on context bind.
     * 0 = unbound: nv11_2d_method ignores the method. */
    uint16_t ch_class[NV11_FIFO_CHANNELS];

    /* PGRAPH */
    uint32_t pgraph_scratch[(NV11_PGRAPH_END - NV11_PGRAPH_OFF) / 4];
    bool     pgraph_busy;    /* PGRAPH[0x700] bit0, set on FIFO method write */

    /* FIFO window drain timer */
    QEMUTimer  *fifo_timer;

    /* DDC / I2C. One bit-banged bus + monitor EDID slave. */
    bitbang_i2c_interface bbi2c[NV11_DDC_BUSES];
    I2CDDCState ddc;
    qemu_edid_info edid_info;   /* monitor EDID */
    bool     ddc_scl[NV11_DDC_BUSES];  /* last sensed SCL level */
    bool     ddc_sda[NV11_DDC_BUSES];  /* last sensed SDA level */

    /* 2D/D2D engine */
    uint32_t d2d_rop3;
    uint32_t d2d_clip_tl, d2d_clip_wh;         /* (y<<16)|x, (h<<16)|w */
    uint32_t d2d_pat_shape, d2d_pat_col0, d2d_pat_col1;
    uint32_t d2d_pat_mono[2];
    uint32_t d2d_col1a;                        /* Bitmap solid color */
    uint32_t d2d_rect_tl;
    uint32_t d2d_blt_src, d2d_blt_dst;
    uint32_t d2d_line_color, d2d_line_p0, d2d_line_p0b;
    bool     d2d_exp_active, d2d_exp_opaque;
    uint32_t d2d_clip_c_tl, d2d_clip_c_br;
    uint32_t d2d_clip_e_tl, d2d_clip_e_br;
    uint32_t d2d_exp_wh;
    uint32_t d2d_exp_fg, d2d_exp_bg;
    uint32_t d2d_exp_x, d2d_exp_y;
    uint32_t d2d_exp_h, d2d_exp_bw;
    int      d2d_exp_bw32, d2d_exp_row, d2d_exp_dw;
    uint32_t d2d_exp_buf[NV11_2D_EXP_BUF_DWORDS];

    /* SIFM (stretch blit) */
    uint32_t d2d_sifm_fmt, d2d_sifm_clip_tl, d2d_sifm_clip_wh;
    uint32_t d2d_sifm_dst, d2d_sifm_dst_wh;
    uint32_t d2d_sifm_dudx, d2d_sifm_dvdy;
    uint32_t d2d_sifm_src_wh, d2d_sifm_src_fmt, d2d_sifm_src_off;
    uint32_t d2d_sifm_src_point;

    /* M2MF */
    uint32_t d2d_m2mf_in, d2d_m2mf_out;
    uint32_t d2d_m2mf_off_in, d2d_m2mf_off_out;
    uint32_t d2d_m2mf_pitch_in, d2d_m2mf_pitch_out;
    uint32_t d2d_m2mf_len, d2d_m2mf_lines, d2d_m2mf_fmt;
    uint32_t d2d_m2mf_notify;

    /* Hardware cursor (head 0) */
    uint32_t cur_pos;           /* NV_PRAMDAC_CU_START_POS: (Y<<16)|X */
    uint32_t cur_cfg;           /* NV_PCRTC_CURSOR_CONFIG */
    uint32_t cur_img;           /* cursor image byte offset in VRAM */
    bool     cur_enabled;       /* HCUR_ADDR1 bit0 */
    uint32_t last_cur_pos;      /* last-drawn position, for invalidation */
    uint32_t last_cur_img;
    bool     last_cur_enabled;
} NV11State;

#define TYPE_NV11 "nv11"
OBJECT_DECLARE_SIMPLE_TYPE(NV11State, NV11)

void nv11_pcrtc_init(NV11State *s);
uint8_t nv11_pcrtc_read(NV11State *s, uint8_t index);
void nv11_pcrtc_write(NV11State *s, uint8_t index, uint8_t value);
void nv11_pbus_init(NV11State *s);
void nv11_pramdac_init(NV11State *s);
void nv11_vga_init(NV11State *s, PCIDevice *dev, Error **errp);
void nv11_vga_class_reset(ObjectClass *klass);
int nv11_get_bpp(VGACommonState *s);
void nv11_get_params(VGACommonState *s, VGADisplayParams *params);
void nv11_get_resolution(VGACommonState *s, int *pwidth, int *pheight);
void nv11_cursor_invalidate(VGACommonState *s);
void nv11_cursor_draw_line(VGACommonState *s, uint8_t *d, int y);
void nv11_window_init(NV11State *s);
void nv11_prome_init(NV11State *s);
void nv11_fifo_init(NV11State *s);
void nv11_fifo_reset(NV11State *s);
uint64_t nv11_fifo_read(NV11State *s, hwaddr offset, unsigned size);
void nv11_fifo_write(NV11State *s, hwaddr offset, uint64_t val,
                     unsigned size);
void nv11_pgraph_init(NV11State *s);
void nv11_pgraph_reset(NV11State *s);
uint64_t nv11_pgraph_read(NV11State *s, hwaddr offset, unsigned size);
void nv11_pgraph_write(NV11State *s, hwaddr offset, uint64_t val,
                       unsigned size);
void nv11_ptimer_init(NV11State *s);
void nv11_ptimer_reset(NV11State *s);
uint64_t nv11_ptimer_read(NV11State *s, hwaddr offset, unsigned size);
void nv11_ptimer_write(NV11State *s, hwaddr offset, uint64_t val,
                       unsigned size);
void nv11_2d_init(NV11State *s);
void nv11_2d_reset(NV11State *s);
void nv11_2d_method(NV11State *s, uint32_t chan, uint32_t reg, uint32_t val);
uint32_t nv11_fifo_dma_frame(NV11State *s, uint32_t handle);

void nv11_i2c_init(NV11State *s);
void nv11_i2c_reset(NV11State *s);
void nv11_ddc_drive(NV11State *s, int bus, uint8_t value);
uint8_t nv11_ddc_sense(NV11State *s, int bus);

uint64_t nv11_bar0_read(NV11State *s, hwaddr offset, unsigned size);
void nv11_bar0_write(NV11State *s, hwaddr offset, uint64_t val,
                     unsigned size);

#endif /* HW_NV11_H */
