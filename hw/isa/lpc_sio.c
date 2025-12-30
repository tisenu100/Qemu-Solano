/*
 * Standard LPC Super I/O Implementation
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
#include "hw/block/fdc.h"
#include "hw/char/serial.h"
#include "hw/char/parallel.h"
#include "hw/char/parallel-isa.h"
#include "hw/char/serial-isa.h"
#include "hw/isa/isa.h"
#include "hw/isa/superio.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "system/blockdev.h"

/* To save memory we negate the registers which are standard */
#define INDEX(index) (index - 0x30)
#define ENABLED s->ldn_regs[s->ldn][0]
#define ADDR ((s->ldn_regs[s->ldn][INDEX(0x60)] << 8) | s->ldn_regs[s->ldn][INDEX(0x61)])
#define IRQ (s->ldn_regs[s->ldn][INDEX(0x70)] & 0x0f)

OBJECT_DECLARE_SIMPLE_TYPE(LPCSIOState, LPC_SIO)
struct LPCSIOState {
    /*< private >*/
    ISADevice parent_obj;
    /*< public >*/

    ISADevice *fdc;
    ISADevice *lpt;
    ISADevice *uart[2];

    uint8_t id1;
    uint8_t id2;
    uint8_t lock_code;
    uint8_t unlock_code;

    bool lock;
    uint8_t index;
    uint8_t ldn;
    uint8_t regs[48];
    uint8_t ldn_regs[4][208]; /* Bloat */

    MemoryRegion io;
};

static void lpc_sio_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    LPCSIOState *s = opaque;
    ISAParallelState *lpt = ISA_PARALLEL(s->lpt);
    ParallelState *lpt_internal = &lpt->state;

    if(!(addr & 1)) {
        if(data == s->unlock_code) /* Normally they got to be written twice */
            s->lock = 0;
        else if (data == s->lock_code)
            s->lock = 1;

        s->index = data & 0xff;
        return;
    }

    if(s->lock) /* Don't write if the chip is locked */
        return;

    if((s->index > 0x2f) && (s->ldn >= 4)) /* There are 11 Devices in the W83627HF. However not all are implemented */
        return;

    if(s->index > 0x2f) {
        s->ldn_regs[s->ldn][INDEX(s->index)] = data;

        switch(s->ldn) {
            case 0: /* FDC */
                isa_fdc_set_enabled(s->fdc, 0);

                if(ENABLED && (ADDR != 0)) {
                    isa_fdc_set_enabled(s->fdc, 1);
                    isa_fdc_set_iobase(s->fdc, ADDR);
                    isa_fdc_set_irq(s->fdc, IRQ);

                    if((ADDR != 0) && (IRQ != 0))
                        fprintf(stderr, "LPC Super I/O: FDC set to 0x%04x with IRQ %d\n", ADDR, IRQ);
                }
            break;

            case 1: /* LPT */
                isa_parallel_set_enabled(s->lpt, 0);

                if(ENABLED && (ADDR != 0)) {
                    isa_parallel_set_enabled(s->lpt, 1);
                    isa_parallel_set_iobase(s->lpt, ADDR);
                    lpt_internal->irq = isa_get_irq(ISA_DEVICE(s->lpt), IRQ);

                    if((ADDR != 0) && (IRQ != 0))
                        fprintf(stderr, "LPC Super I/O: LPT set to 0x%04x with IRQ %d\n", ADDR, IRQ);
                }
            break;

            case 2: /* UART 1 */
                isa_serial_set_enabled(s->uart[0], 0);

                if(ENABLED && (ADDR != 0)) {
                    isa_serial_set_enabled(s->uart[0], 1);
                    isa_serial_set_iobase(s->uart[0], ADDR);
                    isa_serial_set_irq(s->uart[0], IRQ);

                    if((ADDR != 0) && (IRQ != 0))
                        fprintf(stderr, "LPC Super I/O: UART A set to 0x%04x with IRQ %d\n", ADDR, IRQ);
                }
            break;

            case 3: /* UART 2 */
                isa_serial_set_enabled(s->uart[1], 0);

                if(ENABLED && (ADDR != 0)) {
                    isa_serial_set_enabled(s->uart[1], 1);
                    isa_serial_set_iobase(s->uart[1], ADDR);
                    isa_serial_set_irq(s->uart[1], IRQ);

                    if((ADDR != 0) && (IRQ != 0))
                        fprintf(stderr, "LPC Super I/O: UART B set to 0x%04x with IRQ %d\n", ADDR, IRQ);
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

static uint64_t lpc_sio_read(void *opaque, hwaddr addr, unsigned size)
{
    LPCSIOState *s = opaque;

    if(!(addr & 1)) {
        return s->index;
    }

    if((s->index > 0x2f) && (s->ldn >= 4))  /* There are 11 Devices in the W83627HF. However not all are implemented */
        return 0;

    if(s->index > 0x2f)
        return s->ldn_regs[s->ldn][s->index - 0x60];
    else
        return s->regs[s->index];
}

static const MemoryRegionOps lpc_sio_ops = {
    .read = lpc_sio_read,
    .write = lpc_sio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void lpc_sio_realize(DeviceState *d, Error **errp)
{
    LPCSIOState *s = LPC_SIO(d);
    ISADevice *isa = ISA_DEVICE(d);
    DriveInfo *fd[2];

    fprintf(stderr, "LPC Super I/O: Starting\n");

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

static void lpc_sio_reset(DeviceState *d)
{
    LPCSIOState *s = LPC_SIO(d);

    s->regs[0x20] = s->id1;
    s->regs[0x21] = s->id2;
    s->regs[0x22] = 0xff;

    /*
       LDN devices have defaults if PNPCVS(Register 24h Bit 0) is 1
       However the BIOS program the devices nonetheless so ignore
    */

    isa_fdc_set_enabled(s->fdc, 0);
    isa_parallel_set_enabled(s->lpt, 0);
    isa_serial_set_enabled(s->uart[0], 0);
    isa_serial_set_enabled(s->uart[1], 0);
}

static const Property lpc_sio_properties[] = {
    DEFINE_PROP_UINT8("id1", LPCSIOState, id1, 0x00), /* Vendor */
    DEFINE_PROP_UINT8("id2", LPCSIOState, id2, 0x00), /* Revision */
    DEFINE_PROP_UINT8("lock_code", LPCSIOState, lock_code, 0xaa), /* Lock Key(usually AAh) */
    DEFINE_PROP_UINT8("unlock_code", LPCSIOState, unlock_code, 0x00), /* Unlock Key */
};

static void lpc_sio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc_sio_reset);
    dc->realize = lpc_sio_realize;
    dc->user_creatable = false;
    device_class_set_props(dc, lpc_sio_properties);
}

static void lpc_sio_init(Object *obj)
{
    LPCSIOState *s = LPC_SIO(obj);

    memory_region_init_io(&s->io, OBJECT(s), &lpc_sio_ops, s, "lpc-sio", 2);
    memory_region_set_enabled(&s->io, true);


    s->fdc = isa_new(TYPE_ISA_FDC);
    s->lpt = isa_new(TYPE_ISA_PARALLEL);
    s->uart[0] = isa_new(TYPE_ISA_SERIAL);
    s->uart[1] = isa_new(TYPE_ISA_SERIAL);
}

static const TypeInfo lpc_sio_info = {
    .name          = TYPE_LPC_SIO,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(LPCSIOState),
    .class_size    = sizeof(ISASuperIOClass),
    .instance_init = lpc_sio_init,
    .class_init    = lpc_sio_class_init,
};

static void lpc_sio_register_type(void)
{
    type_register_static(&lpc_sio_info);
}

/* Winbond W83627HF Configuration */
void w83627hf_create(ISABus *bus)
{
    ISADevice *isadev;
    DeviceState *sio;

    isadev = isa_new(TYPE_LPC_SIO);
    sio = DEVICE(isadev);

    fprintf(stderr, "LPC Super I/O: Assuming Winbond W83627HF\n");

    qdev_prop_set_uint8(sio, "id1", 0x52);
    qdev_prop_set_uint8(sio, "id2", 0x17);

    /* Normally the Unlock key has to be sent 2 times. Just unlock it at once */
    qdev_prop_set_uint8(sio, "unlock_code", 0x87);

    isa_realize_and_unref(isadev, bus, &error_fatal);
}

/* SMSC LPC47M1xx Configuration */
void smsc_lpc47m1xx_create(ISABus *bus)
{
    ISADevice *isadev;
    DeviceState *sio;

    isadev = isa_new(TYPE_LPC_SIO);
    sio = DEVICE(isadev);

    fprintf(stderr, "LPC Super I/O: Assuming SMSC LPC47M1xx\n");

    qdev_prop_set_uint8(sio, "id1", 0x60);
    qdev_prop_set_uint8(sio, "id2", 0x10);
    qdev_prop_set_uint8(sio, "unlock_code", 0x55);

    isa_realize_and_unref(isadev, bus, &error_fatal);
}

/* ITE 8712F Configuration */
void ite8712f_create(ISABus *bus)
{
    ISADevice *isadev;
    DeviceState *sio;

    isadev = isa_new(TYPE_LPC_SIO);
    sio = DEVICE(isadev);

    fprintf(stderr, "LPC Super I/O: Assuming ITE 8712F\n");

    qdev_prop_set_uint8(sio, "id1", 0x87);
    qdev_prop_set_uint8(sio, "id2", 0x12);

    /*
        Unlock: 87h, 01h, 55h, (55h)
        Lock:   87h, 01h, 55h, (AAh)

        Just use the final value as the lock/unlock trigger
    */
    qdev_prop_set_uint8(sio, "unlock_code", 0x55);

    isa_realize_and_unref(isadev, bus, &error_fatal);
}

type_init(lpc_sio_register_type)
