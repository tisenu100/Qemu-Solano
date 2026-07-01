/*
 * SST LPC Flash Emulation
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
 * */

#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/block/flash.h"
#include "hw/block/sst_lpc.h"
#include "system/block-backend.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/option.h"
#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "system/blockdev.h"
#include "system/runstate.h"
#include "system/block-backend-io.h"
#include "system/address-spaces.h"

#define ADDR_MASKED (addr & 0xffff)

void sst_mount_flash(SSTState *sst, PFlashCFI01 *pfl)
{
    sst->pfl = pfl;

    if (sst->pfl) {
        fprintf(stderr, "SST: Qemu flash was mounted\n");
    }
}

static void flush_buffer_range(SSTState *s, BlockBackend *blk, int64_t offset, int64_t bytes) {
    if (blk) {
        blk_pwrite(blk, offset, bytes, s->buf + offset, 0);
    }
}

static void sst_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    SSTState *s = opaque;
    BlockBackend *blk = pflash_cfi01_get_blk(s->pfl);
    uint32_t masked_addr = addr & s->addr_mask;

    uint8_t byte_val = val & 0xff;

    switch (s->stage) {
        case 0: 
            if ((ADDR_MASKED == 0x5555) && (byte_val == 0xaa)) {
                s->stage++;
            } else if (byte_val == 0xf0) {
                memory_region_rom_device_set_romd(&s->mem, true);
            }
            break;

        case 1:  
            if ((ADDR_MASKED == 0x2aaa) && (byte_val == 0x55)) {
                s->stage++;
            } else {
                goto invalid;
            }
            break;

        case 2:
            if ((ADDR_MASKED == 0x5555) && (byte_val == 0xa0)) { /* Write Command */
                s->stage = 3;
            } else if ((ADDR_MASKED == 0x5555) && (byte_val == 0x80)) { /* Erase Command */
                s->stage = 4;
            } else if ((ADDR_MASKED == 0x5555) && (byte_val == 0x90)) { /* Software ID Entry */
                s->stage = 0;
                memory_region_rom_device_set_romd(&s->mem, false);
            } else if (byte_val == 0xf0) { /* Software ID Exit */
                s->stage = 0;
                memory_region_rom_device_set_romd(&s->mem, true);
            } else {
                goto invalid;
            }
            break;

        case 3: /* Dynamic-safe Byte Write */
            s->buf[masked_addr] = byte_val;
            flush_buffer_range(s, blk, masked_addr, 1);
            s->stage = 0;
            break;

        case 4: /* Erase Confirmation Stage 1 */
            if ((ADDR_MASKED == 0x5555) && (byte_val == 0xaa)) {
                s->stage++;
            } else {
                goto invalid;
            }
            break;

        case 5: /* Erase Confirmation Stage 2 */
            if ((ADDR_MASKED == 0x2aaa) && (byte_val == 0x55)) {
                s->stage++;
            } else {
                goto invalid;
            }
            break;

        case 6: /* Execute Erase Operation */
            switch (byte_val) {
                case 0x30: { /* 4KB Sector Erase */
                    uint32_t sector_start = masked_addr & ~(4 * KiB - 1);
                    for (int i = 0; i < (4 * KiB); i++) {
                        if ((sector_start + i) < s->flash_size) {
                            s->buf[sector_start + i] = 0xff;
                        }
                    }
                    flush_buffer_range(s, blk, sector_start, 4 * KiB);
                    break;
                }

                case 0x50: { /* 64KB Block Erase */
                    uint32_t block_start = masked_addr & ~(64 * KiB - 1);
                    for (int i = 0; i < (64 * KiB); i++) {
                        if ((block_start + i) < s->flash_size) {
                            s->buf[block_start + i] = 0xff;
                        }
                    }
                    flush_buffer_range(s, blk, block_start, 64 * KiB);
                    break;
                }

                case 0x10: /* Chip Erase */
                    fprintf(stderr, "SST: A Chip erase sequence was triggered\n");
                    memset(s->buf, 0xff, s->flash_size);
                    if (blk) {
                        blk_pwrite(blk, 0, s->flash_size, s->buf, 0);
                    }
                    break;
            }
            s->stage = 0;
            break;
    }
    return;

invalid:
    fprintf(stderr, "SST: Invalid or incompatible sequence\n");
    s->stage = 0;
}

static uint64_t sst_read(void *opaque, hwaddr addr, unsigned len)
{
    SSTState *s = opaque;
    
    if ((addr & 1) == 0) {
        return 0xbf; /* SST Vendor ID */
    }

    switch (s->flash_size) {
        case 256 * KiB: return 0x57; /* SST49LF002A */
        case 384 * KiB: return 0x1b; /* SST49LF003A */
        case 512 * KiB: return 0x60; /* SST49LF004A */
        case 1 * MiB:   return 0x5a; /* SST49LF008A */
        default:        return 0x57;
    }
}

static const MemoryRegionOps sst_ops = {
    .read = sst_read,
    .write = sst_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sst_realize(DeviceState *d, Error **errp)
{
    SSTState *s = SST_LPC(d);

    if (!s->pfl) { 
        fprintf(stderr, "SST: No flash module was detected!\n");
        return;
    }

    MemoryRegion *mem = pflash_cfi01_get_memory(s->pfl);
    BlockBackend *blk = pflash_cfi01_get_blk(s->pfl);
    
    s->flash_size = blk_getlength(blk);
    s->addr_mask = s->flash_size - 1;

    fprintf(stderr, "SST: Hijacking\n");

    memory_region_transaction_begin();
    memory_region_set_enabled(mem, false);
    memory_region_transaction_commit();

    memory_region_init_rom_device(&s->mem, OBJECT(d), &sst_ops, s, "SST", s->flash_size, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(d), &s->mem);

    fprintf(stderr, "SST: Assigned a BIOS flash image of %d KB\n", (int)(s->flash_size / 1024));

    s->buf = memory_region_get_ram_ptr(&s->mem);
    blk_check_size_and_read_all(blk, d, s->buf, s->flash_size, &error_fatal);

    memory_region_add_subregion_overlap(get_system_memory(), 0x100000000ULL - s->flash_size, &s->mem, 10);
}

static void sst_reset(DeviceState *d)
{
    SSTState *s = SST_LPC(d);

    if (!s->pfl) 
        return;

    BlockBackend *blk = pflash_cfi01_get_blk(s->pfl);

    s->stage = 0;

    if (blk) {
        blk_pwrite(blk, 0, s->flash_size, s->buf, 0);
    }
}

static void sst_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sst_realize;
    device_class_set_legacy_reset(dc, sst_reset);
    dc->user_creatable = false;
}

static const TypeInfo sst_flash_info = {
    .name          = TYPE_SST_LPC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SSTState),
    .class_init    = sst_class_init,
};

static void sst_register_type(void)
{
    type_register_static(&sst_flash_info);
}

type_init(sst_register_type)
