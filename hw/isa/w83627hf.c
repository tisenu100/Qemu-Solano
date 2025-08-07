/*
 * Winbond W83627HF
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

/* NOTE: It's implementation is way too skeletal for now */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "hw/block/fdc.h"
#include "hw/char/serial.h"
#include "hw/char/parallel.h"
#include "hw/char/parallel-isa.h"
#include "hw/char/serial-isa.h"
#include "hw/isa/isa.h"
#include "hw/isa/superio.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "system/blockdev.h"

/* To save memory we negate the registers which are standard */
#define INDEX(index) (index - 0x30)
#define ENABLED s->ldn_regs[s->ldn][0]
#define ADDR ((s->ldn_regs[s->ldn][INDEX(0x60)] << 8) | s->ldn_regs[s->ldn][INDEX(0x61)])
#define IRQ (s->ldn_regs[s->ldn][INDEX(0x70)] & 0x0f)

OBJECT_DECLARE_SIMPLE_TYPE(WinbondIOState, WINBOND_W83627HF)
struct WinbondIOState {
    /*< private >*/
    ISADevice parent_obj;
    /*< public >*/

    ISADevice *fdc;
    ISADevice *lpt;
    ISADevice *uart[2];

    bool lock;
    uint8_t index;
    uint8_t ldn;
    uint8_t regs[48];
    uint8_t ldn_regs[4][208]; /* Bloat */

    MemoryRegion io;
};

static void winbond_io_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    WinbondIOState *s = opaque;
    ISAParallelState *lpt = ISA_PARALLEL(s->lpt);
    ParallelState *lpt_internal = &lpt->state;

    if(!(addr & 1)) {
        if(data == 0x87) /* Normally they got to be written twice */
            s->lock = 0;
        else if (data == 0xaa)
            s->lock = 1;

        s->index = data & 0xff;
        return;
    }

    if((s->index > 0x2f) && (s->ldn >= 4)) /* There are 11 Devices in the W83627HF. However not all are implemented */
        return;

    if(s->index > 0x2f) {
        s->ldn_regs[s->ldn][INDEX(s->index)] = data;

        switch(s->ldn) {
            case 0: /* FDC */
                isa_fdc_set_enabled(s->fdc, 0);

                if(ENABLED && (ADDR != 0) && (IRQ != 0)) {
                    isa_fdc_set_enabled(s->fdc, 1);
                    isa_fdc_set_iobase(s->fdc, ADDR);
                    qemu_printf("Winbond W83627HF: FDC set to 0x%04x with IRQ %d\n", ADDR, IRQ);
                }
            break;

            case 1: /* LPT */
                isa_parallel_set_enabled(s->lpt, 0);

                if(ENABLED && (ADDR != 0) && (IRQ != 0)) {
                    isa_parallel_set_enabled(s->lpt, 1);
                    isa_parallel_set_iobase(s->lpt, ADDR);
                    lpt_internal->irq = isa_get_irq(ISA_DEVICE(s->lpt), IRQ);
                    qemu_printf("Winbond W83627HF: LPT set to 0x%04x with IRQ %d\n", ADDR, IRQ);
                }
            break;

            case 2: /* UART 1 */
                isa_serial_set_enabled(s->uart[0], 0);

                if(ENABLED && (ADDR != 0) && (IRQ != 0)) {
                    isa_serial_set_enabled(s->uart[0], 1);
                    isa_serial_set_iobase(s->uart[0], ADDR);
                    qemu_printf("Winbond W83627HF: UART A set to 0x%04x with IRQ %d\n", ADDR, IRQ);
                }
            break;

            case 3: /* UART 2 */
                isa_serial_set_enabled(s->uart[1], 0);

                if(ENABLED && (ADDR != 0) && (IRQ != 0)) {
                    isa_serial_set_enabled(s->uart[1], 1);
                    isa_serial_set_iobase(s->uart[1], ADDR);
                    qemu_printf("Winbond W83627HF: UART B set to 0x%04x with IRQ %d\n", ADDR, IRQ);
                }
            break;
        }
    } else {
        if((s->index == 0x20) || (s->index == 0x21))
            return;

        s->regs[s->index] = data;

        if(s->index == 0x07) {
            s->ldn = (int)data;
        }
    }
}

static uint64_t winbond_io_read(void *opaque, hwaddr addr, unsigned size)
{
    WinbondIOState *s = opaque;

    if(!(addr & 1)) {
        return s->index;
    }

    if((s->index > 0x2f) && (s->ldn >= 4))  /* There are 11 Devices in the W83627HF. However not all are implemented */
        return 0xffffffffffffffffULL;

    if(s->index > 0x2f)
        return s->ldn_regs[s->ldn][s->index - 0x60];
    else
        return s->regs[s->index];
}

static const MemoryRegionOps winbond_io_ops = {
    .read = winbond_io_read,
    .write = winbond_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void w83627hf_realize(DeviceState *d, Error **errp)
{
    WinbondIOState *s = WINBOND_W83627HF(d);
    ISADevice *isa = ISA_DEVICE(d);
    DriveInfo *fd[2];

    qemu_printf("Winbond W83627HF: Starting\n");

    s->lock = 1;

    for (int i = 0; i < MAX_FD; i++) {
        fd[i] = drive_get(IF_FLOPPY, 0, i);
    }

    isa_realize_and_unref(s->fdc, isa_bus_from_device(isa), &error_fatal);
    isa_fdc_init_drives(s->fdc, fd);

    /* W83627HF can do one LPT device */
    qdev_prop_set_chr(DEVICE(s->lpt), "chardev", parallel_hds[0]);
    isa_realize_and_unref(s->lpt, isa_bus_from_device(isa), &error_fatal);

    /* W83627HF can do 2 NS16550 UART devices */
    qdev_prop_set_chr(DEVICE(s->uart[0]), "chardev", serial_hd(0));
    isa_realize_and_unref(s->uart[0], isa_bus_from_device(isa), &error_fatal);

    qdev_prop_set_chr(DEVICE(s->uart[1]), "chardev", serial_hd(1));
    isa_realize_and_unref(s->uart[1], isa_bus_from_device(isa), &error_fatal);

    isa_register_ioport(isa, &s->io, 0x2e);
}

static void w83627hf_reset(DeviceState *d)
{
    WinbondIOState *s = WINBOND_W83627HF(d);

    s->regs[0x20] = 0x52;
    s->regs[0x21] = 0x17;
    s->regs[0x22] = 0xff; /* Hardware Powerdown. It provides no function here. */
    s->regs[0x2a] = 0x7c;
    s->regs[0x2b] = 0xc0;

    /*
       LDN devices have defaults if PNPCVS(Register 24h Bit 0) is 1
       However the BIOS program the devices nonetheless so ignore
    */

    isa_fdc_set_enabled(s->fdc, 0);
    isa_parallel_set_enabled(s->lpt, 0);
    isa_serial_set_enabled(s->uart[0], 0);
    isa_serial_set_enabled(s->uart[1], 0);
}

static void w83627hf_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, w83627hf_reset);
    dc->realize = w83627hf_realize;
    dc->user_creatable = false;
}

static void w83627hf_init(Object *obj)
{
    WinbondIOState *s = WINBOND_W83627HF(obj);

    memory_region_init_io(&s->io, OBJECT(s), &winbond_io_ops, s, "winbond-w83627hf", 2);
    memory_region_set_enabled(&s->io, true);


    s->fdc = isa_new(TYPE_ISA_FDC);


    s->lpt = isa_new(TYPE_ISA_PARALLEL);


    s->uart[0] = isa_new(TYPE_ISA_SERIAL);
    s->uart[1] = isa_new(TYPE_ISA_SERIAL);
}


static const TypeInfo winbond_w83627hf_info = {
    .name          = TYPE_WINBOND_W83627HF,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(WinbondIOState),
    .class_size    = sizeof(ISASuperIOClass),
    .instance_init = w83627hf_init,
    .class_init    = w83627hf_class_init,
};

static void w83627hf_register_type(void)
{
    type_register_static(&winbond_w83627hf_info);
}

type_init(w83627hf_register_type)
