/*
 * QEMU MPU-401 emulation
 *
 * Copyright (c) 2025-2026 re9177
 *
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

/* NOTE: currently a bit busted, but works otherwise with most MIDI devices */

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "chardev/char-fe.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qom/object.h"
#include "hw/core/irq.h"
#include "qemu/fifo8.h"

#define TYPE_MPU401_ISA "mpu401"
OBJECT_DECLARE_SIMPLE_TYPE(MPU401State, MPU401_ISA)

struct MPU401State {
    ISADevice parent_obj;
    MemoryRegion io;
    CharFrontend chr;
    uint32_t iobase;
    uint32_t irq;
    qemu_irq qirq;
    Fifo8 fifo;
    bool uart_mode;
    bool has_ack;
};

static int mpu_can_receive(void *opaque)
{
    MPU401State *s = opaque;
    return fifo8_num_free(&s->fifo);
}

static void mpu_receive(void *opaque, const uint8_t *buf, int size)
{
    MPU401State *s = opaque;

    if (!s->uart_mode) {
        return;
    }

    for (int i = 0; i < size; i++) {
        fifo8_push(&s->fifo, buf[i]);
    }

    qemu_irq_raise(s->qirq);
}

static uint64_t mpu_read(void *opaque, hwaddr addr, unsigned size)
{
    MPU401State *s = opaque;

    if (addr == 1) {
        uint8_t status = 0x7F;
        
        if (s->has_ack || !fifo8_is_empty(&s->fifo)) {
            status &= ~0x40;
        }
        return status;
    } 

    uint8_t ret = 0xFF;
    if (s->has_ack) {
        s->has_ack = false;
        ret = 0xFE;
    } else if (!fifo8_is_empty(&s->fifo)) {
        ret = fifo8_pop(&s->fifo);
    }

    if (!s->has_ack && fifo8_is_empty(&s->fifo)) {
        qemu_irq_lower(s->qirq);
    }
    return ret;
}

static void mpu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MPU401State *s = opaque;
    uint8_t byte = val & 0xff;

    if (addr == 1) {
        if (byte == 0xFF || byte == 0x3F) {
            s->uart_mode = (byte == 0x3F);
            s->has_ack = true;
            fifo8_reset(&s->fifo);
	for (int ch = 0; ch < 16; ch++) {
        	uint8_t all_notes_off[] = { 0xB0 | ch, 0x7B, 0x00 };
        	qemu_chr_fe_write_all(&s->chr, all_notes_off, 3);
    	}
            qemu_irq_raise(s->qirq);
        }
    } else {
        if (s->uart_mode) {
            qemu_chr_fe_write_all(&s->chr, &byte, 1);
        }
    }
}

static const MemoryRegionOps mpu_ops = {
    .read = mpu_read,
    .write = mpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void mpu401_realize(DeviceState *dev, Error **errp)
{
    MPU401State *s = MPU401_ISA(dev);
    ISADevice *isadev = ISA_DEVICE(dev);

    memory_region_init_io(&s->io, OBJECT(dev), &mpu_ops, s, "mpu401", 2);
    isa_register_ioport(isadev, &s->io, s->iobase);

    fifo8_create(&s->fifo, 1024);

    s->qirq = isa_get_irq(isadev, s->irq);
    
    qemu_chr_fe_set_handlers(&s->chr, mpu_can_receive, mpu_receive, NULL, NULL, s, NULL, true);
}

static const Property mpu401_properties[] = {
    DEFINE_PROP_UINT32("iobase", MPU401State, iobase, 0x330),
    DEFINE_PROP_UINT32("irq", MPU401State, irq, 9),
    DEFINE_PROP_CHR("chardev", MPU401State, chr),
};

static void mpu401_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mpu401_realize;
    device_class_set_props(dc, mpu401_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo mpu401_info = {
    .name          = TYPE_MPU401_ISA,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(MPU401State),
    .class_init    = mpu401_class_init,
};

static void mpu401_register_types(void)
{
    type_register_static(&mpu401_info);
}

type_init(mpu401_register_types)
