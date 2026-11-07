/*
 * Nvidia Geforce2 MX400 (NV11B) Serial I/O
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
 * Serial I/O is used so we can access MMIO in Real Mode
 */

#include "qemu/osdep.h"
#include "hw/display/vga.h"
#include "nv11.h"
#include "trace.h"


static uint64_t nv11_window_port_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    NV11State *s = NV11(opaque);
    uint64_t val = 0;
    uint32_t flat_val;
    uint32_t eip = nv11_get_eip();

    addr += 0x3D0;
    trace_nv11_window_read(eip, addr, size, s->win_op, s->win_addr);

    if (s->win_op == NV11_WINDOW_OP_READ) {
        flat_val = nv11_bar0_read(s, s->win_addr, 4);

        if (size == 4 && addr == 0x3D0) {
            val = flat_val;
        } else if (addr == 0x3D0) {
            val = flat_val & 0xFFFF;
        } else if (addr == 0x3D1) {
            val = (flat_val >> 8) & 0xFF;
        } else if (addr == 0x3D2) {
            val = (flat_val >> 16) & 0xFFFF;
        } else if (addr == 0x3D3) {
            val = (flat_val >> 24) & 0xFF;
        }

        trace_nv11_window_read_data(eip, s->win_addr, flat_val);
    } else if (s->win_op == NV11_WINDOW_OP_INDEX) {
        if (addr == 0x3D0) {
            val = s->win_addr & 0xFFFF;
        } else if (addr == 0x3D2) {
            val = (s->win_addr >> 16) & 0xFFFF;
        }
    }

    return val;
}

static void nv11_window_port_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    NV11State *s = NV11(opaque);
    uint32_t eip = nv11_get_eip();

    addr += 0x3D0;
    trace_nv11_window_write(eip, addr, size, (uint32_t)val,
                            s->win_op, s->win_addr);

    switch (s->win_op) {
    case NV11_WINDOW_OP_INDEX:
        if (size == 4 && addr == 0x3D0) {
            s->win_addr = (uint32_t)val;
        } else {
            switch ((uint32_t)addr) {
            case 0x3D0:
                if (size == 2) {
                    s->win_addr = (s->win_addr & 0xFFFF0000) |
                                  ((uint32_t)val & 0xFFFF);
                } else {
                    s->win_addr = (s->win_addr & 0xFFFFFF00) |
                                  ((uint32_t)val & 0xFF);
                }
                break;
            case 0x3D1:
                s->win_addr = (s->win_addr & 0xFFFF00FF) |
                              (((uint32_t)val & 0xFF) << 8);
                break;
            case 0x3D2:
                if (size == 2) {
                    s->win_addr = (s->win_addr & 0x0000FFFF) |
                                  (((uint32_t)val & 0xFFFF) << 16);
                } else {
                    s->win_addr = (s->win_addr & 0xFF00FFFF) |
                                  (((uint32_t)val & 0xFF) << 16);
                }
                break;
            case 0x3D3:
                s->win_addr = (s->win_addr & 0x00FFFFFF) |
                              (((uint32_t)val & 0xFF) << 24);
                break;
            }
        }
        trace_nv11_window_index(eip, s->win_addr);
        break;

    case NV11_WINDOW_OP_WRITE:
        if (size == 4 && addr == 0x3D0) {
            nv11_bar0_write(s, s->win_addr, val, 4);
            trace_nv11_window_write_data(eip, s->win_addr, (uint32_t)val);
        } else if (size == 2 && addr == 0x3D0) {
            s->win_write_latch =
                (s->win_write_latch & 0xFFFF0000) | ((uint32_t)val & 0xFFFF);
            nv11_bar0_write(s, s->win_addr, (uint16_t)val, 2);
            trace_nv11_window_write_data(eip, s->win_addr, (uint16_t)val);
        } else {
            if (addr == 0x3D1) {
                s->win_write_latch = (s->win_write_latch & 0xFFFF00FF) |
                                     (((uint32_t)val & 0xFF) << 8);
            }
            if (addr == 0x3D3) {
                s->win_write_latch = (s->win_write_latch & 0x00FFFFFF) |
                                     (((uint32_t)val & 0xFF) << 24);
            }
            if (addr == 0x3D0) {
                s->win_write_latch = (s->win_write_latch & 0xFFFF0000) |
                                     ((uint32_t)val & 0xFFFF);
            }
            if (addr == 0x3D2) {
                s->win_write_latch = (s->win_write_latch & 0x0000FFFF) |
                                     (((uint32_t)val & 0xFFFF) << 16);
                nv11_bar0_write(s, s->win_addr, s->win_write_latch, 4);
                trace_nv11_window_write_data(eip, s->win_addr,
                                             s->win_write_latch);
            }
        }
        break;

    case NV11_WINDOW_OP_READ:
    default:
        break;
    }
}

static const MemoryRegionOps nv11_window_ops = {
    .read = nv11_window_port_read,
    .write = nv11_window_port_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void nv11_window_init(NV11State *s)
{
    s->win_addr = 0;
    s->win_op = 0;
    s->win_write_latch = 0;

    memory_region_init_io(&s->window_io, OBJECT(s), &nv11_window_ops, s,
                          "nv11.window", 4);
    memory_region_add_subregion(pci_address_space_io(&s->parent_dev), 0x3D0,
                                &s->window_io);

    trace_nv11_window_init();
}