/*
 * Generic ISA Super I/O
 *
 * Copyright (c) 2018 Philippe Mathieu-Daudé
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ISA_SUPERIO_H
#define HW_ISA_SUPERIO_H

#include "system/system.h"
#include "hw/isa/isa.h"
#include "qom/object.h"

#define TYPE_ISA_SUPERIO "isa-superio"
typedef struct ISASuperIOClass ISASuperIOClass;
typedef struct ISASuperIODevice ISASuperIODevice;
DECLARE_OBJ_CHECKERS(ISASuperIODevice, ISASuperIOClass,
                     ISA_SUPERIO, TYPE_ISA_SUPERIO)

#define SUPERIO_MAX_SERIAL_PORTS 4

struct ISASuperIODevice {
    /*< private >*/
    ISADevice parent_obj;
    /*< public >*/

    ISADevice *parallel[MAX_PARALLEL_PORTS];
    ISADevice *serial[SUPERIO_MAX_SERIAL_PORTS];
    ISADevice *floppy;
    ISADevice *kbc;
    ISADevice *ide;
};

typedef struct ISASuperIOFuncs {
    size_t count;
    bool (*is_enabled)(ISASuperIODevice *sio, uint8_t index);
    uint16_t (*get_iobase)(ISASuperIODevice *sio, uint8_t index);
    unsigned int (*get_irq)(ISASuperIODevice *sio, uint8_t index);
    unsigned int (*get_dma)(ISASuperIODevice *sio, uint8_t index);
} ISASuperIOFuncs;

struct ISASuperIOClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
    DeviceRealize parent_realize;

    ISASuperIOFuncs parallel;
    ISASuperIOFuncs serial;
    ISASuperIOFuncs floppy;
    ISASuperIOFuncs ide;
};

#define TYPE_FDC37M81X_SUPERIO  "fdc37m81x-superio"
#define TYPE_SMC37C669_SUPERIO  "smc37c669-superio"
#define TYPE_LPC_SIO "lpc-sio"

/* Predefined Super I/O configurations */
extern void w83627hf_create(ISABus *bus); /* Winbond W83627HF */
extern void smsc_lpc47m1xx_create(ISABus *bus); /* SMSC LPC47M1xx Series */
extern void ite8712f_create(ISABus *bus); /* ITE 8712F*/

#endif /* HW_ISA_SUPERIO_H */
