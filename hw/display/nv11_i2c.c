/*
 * Nvidia Geforce2 MX400 (NV11B) DDC / I2C
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

/*
 * The NV11 drives two DDC ports through extended CRTC index registers:
 *
 *   bus A (VGA / head 0):  sense CRTC[0x3e], drive CRTC[0x3f]
 *   bus B (DFP / head 1):  sense CRTC[0x36], drive CRTC[0x37]
 *
 * The sense register returns the SCL/SDA line levels (bit2 = SCL, bit3 =
 * SDA), the drive register drives them (bit5 = SCL, bit4 = SDA, bit0 must
 * stay set). This matches nouveau's nv04_i2c bit-banging bus and the X
 * driver's NV_I2CGetBits/NV_I2CPutBits.
 *
 * Qemu's bitbang_i2c helper turns the SCL/SDA edges into an I2C bus, and
 * the i2c-ddc slave answers address 0x50 with the generated monitor EDID.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "nv11.h"
#include "trace.h"

static const char *const nv11_ddc_bus_name[NV11_DDC_BUSES] = {
    "nv11.ddc-a",
    "nv11.ddc-b",
};

static void nv11_i2c_bus_reset(NV11State *s, int bus)
{
    bitbang_i2c_interface *i2c = &s->bbi2c[bus];

    if (i2c->current_addr >= 0) {
        i2c_end_transfer(i2c->bus);
    }
    bitbang_i2c_init(i2c, i2c->bus);
    i2c->state = STOPPED;
    i2c->current_addr = -1;
    i2c->buffer = 0;

    /* Both lines idle high (pull-ups). */
    s->ddc_scl[bus] = true;
    s->ddc_sda[bus] = true;
}

void nv11_i2c_init(NV11State *s)
{
    int i;

    for (i = 0; i < NV11_DDC_BUSES; i++) {
        I2CBus *bus = i2c_init_bus(DEVICE(s), nv11_ddc_bus_name[i]);

        bitbang_i2c_init(&s->bbi2c[i], bus);
        nv11_i2c_bus_reset(s, i);

        /* Both connectors are wired to the same monitor. Regenerate the
         * EDID here so the device properties (set before realize) apply. */
        qemu_edid_generate(s->ddc[i].edid_blob,
                           sizeof(s->ddc[i].edid_blob), &s->edid_info);

        i2c_slave_set_address(I2C_SLAVE(&s->ddc[i]), NV11_DDC_SLAVE_ADDR);
        qdev_realize(DEVICE(&s->ddc[i]), BUS(bus), &error_abort);
    }

    trace_nv11_i2c_init();
}

void nv11_i2c_reset(NV11State *s)
{
    int i;

    for (i = 0; i < NV11_DDC_BUSES; i++) {
        nv11_i2c_bus_reset(s, i);
    }
}

/* CRTC drive register write: push SCL/SDA onto the bit-banged bus. */
void nv11_ddc_drive(NV11State *s, int bus, uint8_t value)
{
    bitbang_i2c_interface *i2c = &s->bbi2c[bus];
    bool scl = (value & NV11_DDC_SCL_WRITE) != 0;
    bool sda = (value & NV11_DDC_SDA_WRITE) != 0;

    /* Clock first, then data, so START/STOP edges are detected. */
    bitbang_i2c_set(i2c, BITBANG_I2C_SCL, scl);
    sda = bitbang_i2c_set(i2c, BITBANG_I2C_SDA, sda);

    s->ddc_scl[bus] = scl;
    s->ddc_sda[bus] = sda;

    trace_nv11_i2c_drive(nv11_get_eip(), bus, value, scl, sda);
}

/* CRTC sense register read: report the current SCL/SDA line levels. */
uint8_t nv11_ddc_sense(NV11State *s, int bus)
{
    uint8_t val = 0;

    if (s->ddc_scl[bus]) {
        val |= NV11_DDC_SCL_READ;
    }
    if (s->ddc_sda[bus]) {
        val |= NV11_DDC_SDA_READ;
    }

    trace_nv11_i2c_sense(nv11_get_eip(), bus, val);
    return val;
}
