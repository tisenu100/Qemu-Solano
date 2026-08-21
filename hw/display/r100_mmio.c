/*
 * ATI Radeon DDR (R100) MMIO Handlers
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
#include "system/memory.h"
#include "r100.h"
#include "r100_regs.h"

static uint64_t size_mask(unsigned size)
{
    return MAKE_64BIT_MASK(0, size * 8);
}

static uint64_t r100_regs_read(R100State *s, hwaddr addr, unsigned size)
{
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;

        val |= (uint64_t)((s->regs[a >> 2] >> ((a & 3) * 8)) & 0xffu) << (i * 8);
    }
    return val;
}

static void r100_regs_write(R100State *s, hwaddr addr, uint64_t val,
                            unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;
        uint32_t shift = (a & 3) * 8;
        uint32_t mask = 0xffu << shift;

        s->regs[a >> 2] &= ~mask;
        s->regs[a >> 2] |= ((val >> (i * 8)) & 0xffu) << shift;
    }
}

static uint64_t r100_mm_data_read(R100State *s, hwaddr addr, unsigned size)
{
    uint64_t idx = s->mm_index & R100_MM_INDEX_ADDR_MASK;
    hwaddr off = addr - R100_MM_DATA;

    if (s->mm_index & R100_MM_INDEX_VRAM_FLAG) {
        if (idx + off + size <= s->vga.vram_size) {
            uint64_t val = 0;
            unsigned i;

            for (i = 0; i < size; i++) {
                val |= (uint64_t)s->vga.vram_ptr[idx + off + i] << (i * 8);
            }
            return val;
        }
        return 0;
    }
    if (idx + off + size > R100_MMIO_SIZE) {
        return 0;
    }
    return r100_regs_read(s, idx + off, size);
}

static void r100_mm_data_write(R100State *s, hwaddr addr, uint64_t val,
                               unsigned size)
{
    uint64_t idx = s->mm_index & R100_MM_INDEX_ADDR_MASK;
    hwaddr off = addr - R100_MM_DATA;

    if (s->mm_index & R100_MM_INDEX_VRAM_FLAG) {
        if (idx + off + size <= s->vga.vram_size) {
            unsigned i;

            for (i = 0; i < size; i++) {
                s->vga.vram_ptr[idx + off + i] = (val >> (i * 8)) & 0xffu;
            }
        }
        return;
    }
    if (idx + off + size > R100_MMIO_SIZE) {
        return;
    }
    r100_regs_write(s, idx + off, val, size);
}

static uint32_t merge_field(uint32_t old, hwaddr addr, uint64_t val,
                            unsigned size)
{
    uint32_t shift = (addr & 3) * 8;
    uint32_t mask = (uint32_t)size_mask(size) << shift;

    return (old & ~mask) | ((val << shift) & mask);
}

static uint64_t extract_field(uint64_t val, hwaddr addr, unsigned size)
{
    return (val >> ((addr & 3) * 8)) & size_mask(size);
}

static uint64_t r100_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    R100State *s = opaque;
    uint64_t val;

    switch (addr) {
    case R100_MM_INDEX:
        val = s->mm_index;
        break;
    case R100_MM_DATA:
        return r100_mm_data_read(s, addr, size);
    case R100_CLOCK_CNTL_INDEX:
        val = s->clock_cntl_index;
        break;
    case R100_CLOCK_CNTL_DATA:
        val = r100_pll_read(s);
        break;
    case R100_CONFIG_MEMSIZE:
        val = s->vga.vram_size;
        break;
    case R100_MC_STATUS:
        val = R100_MC_STATUS_IDLE;
        break;
    case R100_MEM_SDRAM_MODE_REG:
        val = s->regs[addr >> 2] | R100_MEM_SDRAM_MODE_STAT_BITS;
        break;
    case R100_CRTC_GEN_CNTL:
        val = r100_crtc_gen_cntl_read(s);
        break;
    case R100_DAC_CNTL:
        val = r100_dac_cntl_read(s);
        break;
    default:
        return r100_regs_read(s, addr, size);
    }
    return extract_field(val, addr, size);
}

static void r100_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    R100State *s = opaque;

    switch (addr) {
    case R100_MM_INDEX:
        s->mm_index = merge_field(s->mm_index, addr, val, size);
        break;
    case R100_MM_DATA:
        r100_mm_data_write(s, addr, val, size);
        break;
    case R100_CLOCK_CNTL_INDEX:
        s->clock_cntl_index = val;
        break;
    case R100_CLOCK_CNTL_DATA:
        r100_pll_write(s, val);
        break;
    case R100_CONFIG_MEMSIZE:
    case R100_MC_STATUS:
        break;
    case R100_MEM_SDRAM_MODE_REG:
        r100_regs_write(s, addr, val, size);
        break;
    case R100_CRTC_GEN_CNTL:
        r100_crtc_gen_cntl_write(s,
                                 merge_field(r100_crtc_gen_cntl_read(s),
                                             addr, val, size));
        break;
    case R100_DAC_CNTL:
        r100_dac_cntl_write(s,
                            merge_field(r100_dac_cntl_read(s),
                                        addr, val, size));
        break;
    default:
        r100_regs_write(s, addr, val, size);
        break;
    }
}

static const MemoryRegionOps r100_mmio_ops = {
    .read = r100_mmio_read,
    .write = r100_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static uint64_t r100_io_read(void *opaque, hwaddr addr, unsigned size)
{
    R100State *s = opaque;
    uint64_t val;

    switch (addr) {
    case R100_IO_CLOCK_CNTL_INDEX:
        val = s->clock_cntl_index;
        break;
    case R100_IO_CLOCK_CNTL_DATA:
        val = r100_pll_read(s);
        break;
    case R100_IO_0F:
        val = R100_IO_0F_VALUE;
        break;
    case R100_IO_GATE:
        val = r100_io_gate_read(s);
        break;
    default:
        return r100_regs_read(s, addr, size);
    }
    return extract_field(val, addr, size);
}

static void r100_io_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    R100State *s = opaque;

    switch (addr) {
    case R100_IO_CLOCK_CNTL_INDEX:
        s->clock_cntl_index = val;
        break;
    case R100_IO_CLOCK_CNTL_DATA:
        r100_pll_write(s, val);
        break;
    case R100_IO_0F:
        break;
    case R100_IO_GATE:
        r100_io_gate_write(s, val);
        break;
    default:
        r100_regs_write(s, addr, val, size);
        break;
    }
}

static const MemoryRegionOps r100_io_ops = {
    .read = r100_io_read,
    .write = r100_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

void r100_mmio_init(R100State *s, Object *owner)
{
    memory_region_init_io(&s->mmio, owner, &r100_mmio_ops, s,
                          "r100.mmio", R100_MMIO_SIZE);
    memory_region_init_io(&s->io_bar, owner, &r100_io_ops, s,
                          "r100.io", R100_IO_BAR_SIZE);
}
