/*
 * SST LPC Flash Emulation
 *
 * Copyright (c) 2025 Tisenu100
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
 * 
 */

#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/block/flash.h"
#include "hw/block/sst_lpc.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/option.h"
#include "qemu/units.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "system/blockdev.h"
#include "system/runstate.h"
#include "system/block-backend-io.h"

void sst_mount_flash(SSTState *sst, PFlashCFI01 *pfl)
{
    sst->pfl = pfl;

    if(sst->pfl)
        fprintf(stderr, "SST: Qemu flash was mounted\n");
}

static void sst_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    SSTState *s = opaque;
    BlockBackend *blk = pflash_cfi01_get_blk(s->pfl);

    fprintf(stderr, "Writing\n");

    switch(s->stage) {
        case 0: /* Very Standard Procedure(Unless it is about exiting Software ID) */
            if((addr == 0x5555) && (val == 0xaa))
                s->stage++;
            else if((val == 0xf0))
                memory_region_rom_device_set_romd(&s->mem, true);
        break;

        case 1:  /* Very Standard Procedure */
            if((addr == 0x2aaa) && (val == 0x55))
                s->stage++;
            else
                goto invalid;
        break;

        case 2:
            if((addr == 0x5555) && (val == 0xa0)) /* Write */
                s->stage = 3;
            else if((addr == 0x5555) && (val == 0x80)) /* Erase */
                s->stage = 4;
            else if((addr == 0x5555) && (val == 0x90)) { /* Software ID Entry*/
                s->stage = 0;
                memory_region_rom_device_set_romd(&s->mem, false);
            }
            else if((val == 0xf0))
                memory_region_rom_device_set_romd(&s->mem, true);
            else
                goto invalid;
        break;

        case 3: /* Byte Write */
            if(blk)
                blk_pwrite(blk, addr & 0x001fffff, 1, (uint8_t *)(val & 0xff), 0);

            s->stage = 0;
        break;

        case 4: /* Erase Confirmation */
            if((addr == 0x5555) && (val == 0xaa))
                s->stage++;
            else
                goto invalid;
        break;

        case 5: /* Erase Confirmation */
            if((addr == 0x2aaa) && (val == 0x55))
                s->stage++;
            else
                goto invalid;
        break;

        case 6: /* Erase */
            if(blk) {
                switch(val & 0xff) {
                    case 0x30: /* 4KB Sector Erase */
                        blk_pwrite(blk, addr & 0x001fffff, 4 * KiB, 0, 0);
                    break;

                    case 0x50: /* 64KB Block Erase */
                        blk_pwrite(blk, addr & 0x001fffff, 64 * KiB, 0, 0);
                    break;

                    case 0x10: /* Chip Erase */
                        fprintf(stderr, "SST: A Chip erase sequence was triggered\n");
                        blk_pwrite(blk, addr & 0x001fffff, blk_getlength(blk), 0, 0);
                    break;
                }
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
    /* When in Software ID mode we return the SST Chip ID up until we ask for exit */
    SSTState *s = opaque;
    BlockBackend *blk = pflash_cfi01_get_blk(s->pfl);
    int size = blk_getlength(blk);

    memory_region_rom_device_set_romd(&s->mem, true);

    switch(size) {
        default:
            fprintf(stderr, "SST: SST49LF002A\n");
            return 0x57;
        break;

        case 384 * KiB: /* Very absurd size for a PC BIOS */
            fprintf(stderr, "SST: SST49LF003A\n");
            return 0x1b;
        break;

        case 512 * KiB:
            fprintf(stderr, "SST: SST49LF004A\n");
            return 0x60;
        break;

        case 1 * MiB:
            fprintf(stderr, "SST: SST49LF008A\n");
            return 0x5a;
        break;
    }
}

static const MemoryRegionOps sst_ops = {
    .read = sst_read,
    .write = sst_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void sst_realize(DeviceState *d, Error **errp)
{
    SSTState *s = SST_LPC(d);

    if(!s->pfl) { /* Don't do anything if there's no flash chip */
        fprintf(stderr, "SST: No flash module was detected!\n");
        return;
    }

    MemoryRegion *mem = pflash_cfi01_get_memory(s->pfl);
    BlockBackend *blk = pflash_cfi01_get_blk(s->pfl);
    fprintf(stderr, "SST: Hijacking\n");

    /* First disable prebuilt flash's memory */
    memory_region_transaction_begin();
    memory_region_set_enabled(mem, false);
    memory_region_transaction_commit();

    /* Initiate the SST Flash and "steal" Qemu's block */
    memory_region_init_rom_device(&s->mem, OBJECT(d), &sst_ops, s, "SST", blk_getlength(blk), &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(d), &s->mem);

    fprintf(stderr, "SST: Assigned a BIOS flash image of %d KB\n", (int)(blk_getlength(blk) / 1024));
    sst_read(s, 0, 0); /* Return the Chip variant on console */

    sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, 0x100000000 - blk_getlength(blk));
}

static void sst_reset(DeviceState *d)
{
    SSTState *s = SST_LPC(d);

    if(!s->pfl) /* Don't do anything if there's no flash chip */
        return;

    BlockBackend *blk = pflash_cfi01_get_blk(s->pfl);
    s->stage = 0;
    s->buf = memory_region_get_ram_ptr(&s->mem);

    /* Get all updated changes */
    if(blk)
        blk_check_size_and_read_all(blk, d, s->buf, blk_getlength(blk), &error_fatal);
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
