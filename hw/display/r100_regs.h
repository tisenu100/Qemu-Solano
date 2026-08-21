/*
 * ATI Radeon DDR (R100) Configuration Registers
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

#ifndef HW_DISPLAY_R100_REGS_H
#define HW_DISPLAY_R100_REGS_H

#include "qemu/osdep.h"

/*
 * Indexed VRAM/register access
 * MM_INDEX bit 31 selects VRAM vs register-indexed MM_DATA access
 * this distinction is what makes the VBIOS's own RAM-size autodetect
 * converge on the configured VRAM size.
 */
#define R100_MM_INDEX            0x0000
#define R100_MM_DATA             0x0004
#define R100_MM_INDEX_VRAM_FLAG  0x80000000u
#define R100_MM_INDEX_ADDR_MASK  0x7fffffffu

/*
 * PLL index/data dispatched from both BAR2 here and the BAR1 mirror below. 
 * One single PLL register file serves both apertures.
 */
#define R100_CLOCK_CNTL_INDEX    0x0008
#define R100_CLOCK_CNTL_DATA     0x000C

/* Fixed-behavior registers */
#define R100_CONFIG_MEMSIZE      0x00F8 /* Actual VRAM size in bytes */
#define R100_MC_STATUS           0x0150 /* Always report idle MC */

/* Memory modes and training */
#define R100_MEM_SDRAM_MODE_REG  0x0158
#define R100_MEM_SDRAM_MODE_STAT_BITS ((1u << 28) | (1u << 20))
#define R100_MC_STATUS_IDLE      0x00000005u

/* PLL index/data mirror */
#define R100_IO_CLOCK_CNTL_INDEX 0x08
#define R100_IO_CLOCK_CNTL_DATA  0x0C

/* I/O Gate */
#define R100_IO_0F               0x0F
#define R100_IO_0F_VALUE         0x1Bu
#define R100_IO_GATE             0x10
#define R100_IO_GATE_BIT         0x80u

/* Unordered registers */
#define R100_BIOS_0_SCRATCH0     0x0010
#define R100_BIOS_0_SCRATCH1     0x0014
#define R100_BIOS_0_SCRATCH2     0x0018
#define R100_BIOS_0_SCRATCH3     0x001C
#define R100_BUS_CNTL            0x0030
#define R100_UNNAMED_00EC        0x00EC
#define R100_MEM_CNTL            0x0140
#define R100_UNNAMED_0144        0x0144
#define R100_MC_FB_LOCATION      0x0148
#define R100_MC_AGP_LOCATION     0x014C
#define R100_UNNAMED_0154        0x0154
#define R100_UNNAMED_0168        0x0168
#define R100_AGP_APER_OFFSET     0x0178
#define R100_PCI_GART_PAGE       0x017C
#define R100_PC_NGUI_MODE        0x0180
#define R100_PC_NGUI_CTLSTAT     0x0184
#define R100_UNNAMED_0188        0x0188
#define R100_UNNAMED_018C        0x018C
#define R100_UNNAMED_0D00        0x0D00
#define R100_UNNAMED_0D10        0x0D10
#define R100_UNNAMED_04DC        0x04DC
#define R100_UNNAMED_0910        0x0910
#define R100_CRTC_GEN_CNTL       0x0050
#define R100_DAC_CNTL            0x0058

#endif
