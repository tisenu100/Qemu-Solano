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
#include "system/memory.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"

/* PCI IDs */
#define NV11_PCI_VENDOR_ID       0x10DE
#define NV11_PCI_DEVICE_ID       0x0110
#define NV11_PCI_CLASS_VGA       0x000300

/* BAR sizes */
#define NV11_BAR0_SIZE           0x1000000   /* 16 MB */
#define NV11_BAR1_VRAM_SIZE      0x0400000   /* 64 MB (Needs work for 32MB variants some day) */
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

/* Window opcodes */
#define NV11_WINDOW_OP_INDEX     3
#define NV11_WINDOW_OP_READ      5
#define NV11_WINDOW_OP_WRITE     7

/* CRTC extended index */
#define NV11_CRTC_WIN_OP         0x38

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

    /* Serial I/O */
    uint32_t win_addr;      /* latched BAR0 offset (set by INDEX op) */
    uint8_t  win_op;        /* CRTC[0x38] & 7 */
    uint32_t win_write_latch;

    /* PBUS / PEXTDEV */
    uint32_t pextdev_boot_0;     /* 0x101000 */

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
void nv11_window_init(NV11State *s);
void nv11_prome_init(NV11State *s);

uint64_t nv11_bar0_read(NV11State *s, hwaddr offset, unsigned size);
void nv11_bar0_write(NV11State *s, hwaddr offset, uint64_t val,
                     unsigned size);

#endif /* HW_NV11_H */
