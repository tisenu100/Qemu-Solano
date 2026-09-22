/*
 * Nvidia Geforce2 MX400 (NV11B)
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
 * 
 * Specs:
 * Nvidia NV11B chip (Stepping B2)
 * 64MB DDR (SDRAM variants available)
 * 
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "qom/object.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "qemu/timer.h"
#include "trace.h"
#include "nv11.h"
#include "hw/display/vga.h"

uint64_t nv11_bar0_read(NV11State *s, hwaddr addr, unsigned size)
{
    uint32_t off = (uint32_t)addr;
    uint32_t val = 0;
    uint32_t eip = nv11_get_eip();

    if (off < NV11_PMC_END) {
        uint32_t reg = off - NV11_PMC_OFF;
        if (reg == NV11_PMC_BOOT_0) {
            val = NV11_PMC_BOOT_0_NV11;
            goto return_val;
        }
        if (reg == NV11_PMC_INTR_HOST) {
            /* No PMC interrupts pending on idle HW. */
            val = 0;
            goto return_val;
        }
        goto flat_read;
    }

    if (off >= NV11_PBUS_OFF && off < NV11_PBUS_END) {
        uint32_t reg = off - NV11_PBUS_OFF;
        trace_nv11_pbus_read(eip, reg, size);
        if (reg == NV11_PBUS_PCI_BAR1) {
            /* PCI config BAR1 (VRAM base) mirror - VBIOS takes high
             * 16 bits of this value. */
            val = ldl_le_p(&s->parent_dev.config[PCI_BASE_ADDRESS_1]);
            goto return_val;
        }
        goto flat_read;
    }

    if (off >= NV11_PTMR_OFF && off < NV11_PTMR_END) {
        return nv11_ptimer_read(s, off - NV11_PTMR_OFF, size);
    }

    if (off == NV11_PFIFO_RUNOUT_STATUS ||
        off == NV11_PFIFO_CACHE0_STATUS ||
        off == NV11_PFIFO_CACHE1_STATUS) {
        /* Idle pusher: EMPTY set, RANOUT/FULL clear. */
        val = NV11_PFIFO_STATUS_EMPTY;
        if (size == 1) {
            val = (val >> (8 * (off & 0x3))) & 0xFF;
        } else if (size == 2) {
            val = (val >> (8 * (off & 0x3))) & 0xFFFF;
        }
        goto return_val;
    }

    if (off >= NV11_PFIFO_CACHE1_DMA_CTL &&
        off < NV11_PFIFO_CACHE1_DMA_CTL + 4) {
        {
            uint32_t flat = ldl_le_p((uint32_t *)(s->bar0_flat +
                                                  NV11_PFIFO_CACHE1_DMA_CTL));
            /* Card type. Some drivers might probe aperature with this:
             * TARGET=PCI (0x20000), TARGET=AGP (0x30000).
             */
            val = (flat & ~0x00030000u) | 0x00020000u |
                  NV11_PFIFO_DMA_CTL_VALID | 0x00003000u;
        }
        if (size == 1) {
            val = (val >> (8 * (off & 0x3))) & 0xFF;
        } else if (size == 2) {
            val = (val >> (8 * (off & 0x3))) & 0xFFFF;
        }
        goto return_val;
    }

    if (off >= NV11_PFB_OFF && off < NV11_PFB_END) {
        uint32_t reg = off - NV11_PFB_OFF;
        trace_nv11_pfb_read(eip, reg, size);
        if (reg == NV11_PFB_FIFO_DATA) {
            val = NV11_PFB_FIFO_DATA_64MB;
            goto return_val;
        }
        goto flat_read;
    }

    if (off >= NV11_PEXTDEV_OFF && off < NV11_PEXTDEV_END) {
        uint32_t reg = off - NV11_PEXTDEV_OFF;
        trace_nv11_pextdev_read(eip, reg, size);
        if (reg == NV11_PEXTDEV_BOOT_0) {
            val = s->pextdev_boot_0;
            goto return_val;
        }
        goto flat_read;
    }

    if (off >= NV11_PROM_OFF && off < NV11_PROM_END) {
        uint32_t roff = off - NV11_PROM_OFF;
        trace_nv11_prome_read(eip, off, size);
        if (s->rom_shadow_en && s->prom_data && roff < s->prom_size) {
            switch (size) {
            case 1: val = s->prom_data[roff]; break;
            case 2: val = lduw_le_p(s->prom_data + roff); break;
            case 4: val = ldl_le_p(s->prom_data + roff); break;
            }
        } else {
            val = 0;
        }
        goto return_val;
    }

    if (off >= NV11_PCRTC0_OFF && off < NV11_PCRTC0_END) {
        uint32_t reg = off - NV11_PCRTC0_OFF;
        uint32_t idx = reg / 4;
        trace_nv11_pcrtc_read(eip, 0, reg, size);
        if (idx < sizeof(s->pcrtc_scratch[0]) / sizeof(s->pcrtc_scratch[0][0])) {
            val = s->pcrtc_scratch[0][idx];
        }
        goto return_val;
    }

    if (off >= NV11_PRMCIO0_OFF && off < NV11_PRMCIO0_END) {
        uint32_t reg = off - NV11_PRMCIO0_OFF;
        trace_nv11_pmcio_read(eip, 0, reg, size);
        if (reg >= 0x3C0 && reg <= 0x3DF) {
            if (reg == 0x3D5) {
                val = nv11_pcrtc_read(s, s->vga.cr_index);
            } else {
                val = vga_ioport_read(&s->vga, reg);
            }
        }
        goto return_val;
    }

    if (off >= NV11_PCRTC1_OFF && off < NV11_PCRTC1_END) {
        uint32_t reg = off - NV11_PCRTC1_OFF;
        uint32_t idx = reg / 4;
        trace_nv11_pcrtc_read(eip, 1, reg, size);
        if (idx < sizeof(s->pcrtc_scratch[1]) / sizeof(s->pcrtc_scratch[1][0])) {
            val = s->pcrtc_scratch[1][idx];
        }
        goto return_val;
    }

    if (off >= NV11_PRMCIO1_OFF && off < NV11_PRMCIO1_END) {
        uint32_t reg = off - NV11_PRMCIO1_OFF;
        trace_nv11_pmcio_read(eip, 1, reg, size);
        if (reg >= 0x3C0 && reg <= 0x3DF) {
            if (reg == 0x3D5) {
                val = nv11_pcrtc_read(s, s->vga.cr_index);
            } else {
                val = vga_ioport_read(&s->vga, reg);
            }
        }
        goto return_val;
    }

    if (off >= NV11_PRMDIO_OFF && off < NV11_PRMDIO_END) {
        uint32_t port = off - NV11_PRMDIO_OFF + 0x3C0;
        val = vga_ioport_read(&s->vga, port);
        goto return_val;
    }

    if (off >= NV11_PVIO_OFF && off < NV11_PVIO_END) {
        uint32_t reg = off - NV11_PVIO_OFF;
        trace_nv11_pmcio_read(eip, 0, reg, size);
        if (reg >= 0x3B0 && reg <= 0x3DF) {
            if (reg == 0x3D5 || reg == 0x3B5) {
                val = nv11_pcrtc_read(s, s->vga.cr_index);
            } else {
                val = vga_ioport_read(&s->vga, reg);
            }
        }
        goto return_val;
    }

    if (off >= NV11_PRAMDAC0_OFF && off < NV11_PRAMDAC0_END) {
        uint32_t reg = off - NV11_PRAMDAC0_OFF;
        uint32_t idx = reg / 4;
        trace_nv11_pramdac_read(eip, 0, reg, size);
        if (idx < sizeof(s->pramdac[0]) / sizeof(s->pramdac[0][0])) {
            val = s->pramdac[0][idx];
        }
        goto return_val;
    }

    if (off >= NV11_PRAMDAC1_OFF && off < NV11_PRAMDAC1_END) {
        uint32_t reg = off - NV11_PRAMDAC1_OFF;
        uint32_t idx = reg / 4;
        trace_nv11_pramdac_read(eip, 1, reg, size);
        if (idx < sizeof(s->pramdac[1]) / sizeof(s->pramdac[1][0])) {
            val = s->pramdac[1][idx];
        }
        goto return_val;
    }

    if (off >= NV11_PGRAPH_OFF && off < NV11_PGRAPH_END) {
        return nv11_pgraph_read(s, off - NV11_PGRAPH_OFF, size);
    }

    if (off >= NV11_FIFO_OFF && off < NV11_FIFO_END) {
        return nv11_fifo_read(s, off - NV11_FIFO_OFF, size);
    }

flat_read:
    if (off + size > NV11_BAR0_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [BAR0] read beyond bounds at 0x%x\n", off);
        return 0;
    }
    switch (size) {
    case 1: val = s->bar0_flat[off]; break;
    case 2: val = lduw_le_p((uint16_t *)(s->bar0_flat + off)); break;
    case 4: val = ldl_le_p((uint32_t *)(s->bar0_flat + off)); break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [BAR0] bad read size %u at 0x%x\n", size, off);
        return 0;
    }

    trace_nv11_bar0_read(eip, off, size);
    return val;

return_val:
    trace_nv11_bar0_read(eip, off, size);
    if (off == NV11_PMC_INTR_HOST ||
        off == NV11_PFIFO_CACHE1_STATUS ||
        (off >= NV11_PFIFO_CACHE1_DMA_CTL &&
         off < NV11_PFIFO_CACHE1_DMA_CTL + 4)) {
        static uint32_t dbg_n;
        if ((dbg_n++ % 200000) < 3) {
            trace_nv11_bar0_read_val(eip, off, size, val);
        }
    }
    return val;
}

void nv11_bar0_write(NV11State *s, hwaddr addr, uint64_t val, unsigned size)
{
    uint32_t off = (uint32_t)addr;
    uint32_t eip = nv11_get_eip();

    trace_nv11_bar0_write(eip, off, size, (uint32_t)val);

    if (off < NV11_PMC_END) {
        uint32_t reg = off - NV11_PMC_OFF;
        if (reg == NV11_PMC_BOOT_0) {
            return;   /* Chip ID. Don't write */
        }
        goto flat_write;
    }

    if (off >= NV11_PBUS_OFF && off < NV11_PBUS_END) {
        uint32_t reg = off - NV11_PBUS_OFF;
        trace_nv11_pbus_write(eip, reg, size, (uint32_t)val);
        if (reg == NV11_PBUS_PCI_BAR1) {
            return;
        }
        if (reg == NV11_PBUS_PCI_NV_20) {
            s->rom_shadow_en = (val & 1) != 0;
            trace_nv11_pbus_rom_shadow(eip, s->rom_shadow_en);
        }
        goto flat_write;
    }

    if (off >= NV11_PTMR_OFF && off < NV11_PTMR_END) {
        nv11_ptimer_write(s, off - NV11_PTMR_OFF, val, size);
        return;
    }

    if (off >= NV11_PFB_OFF && off < NV11_PFB_END) {
        uint32_t reg = off - NV11_PFB_OFF;
        trace_nv11_pfb_write(eip, reg, size, (uint32_t)val);
        if (reg == NV11_PFB_FIFO_DATA) {
            return;   /* VRAM Size. Normally there are 32MB variants too. Has to be patched for this */
        }
        goto flat_write;
    }

    if (off >= NV11_PEXTDEV_OFF && off < NV11_PEXTDEV_END) {
        uint32_t reg = off - NV11_PEXTDEV_OFF;
        trace_nv11_pextdev_write(eip, reg, size, (uint32_t)val);
        if (reg == NV11_PEXTDEV_BOOT_0) {
            s->pextdev_boot_0 = (s->pextdev_boot_0 & 0xFFFFFF87) |
                                (val & 0x78);
        }
        goto flat_write;
    }

    if (off >= NV11_PROM_OFF && off < NV11_PROM_END) {
        if (!s->rom_shadow_en) {
            trace_nv11_prome_write_blocked(eip, off);
            return;
        }
        uint32_t roff = off - NV11_PROM_OFF;
        if (s->prom_data && roff < s->prom_size) {
            switch (size) {
            case 1: s->prom_data[roff] = (uint8_t)val; break;
            case 2: stw_le_p(s->prom_data + roff, (uint16_t)val); break;
            case 4: stl_le_p(s->prom_data + roff, (uint32_t)val); break;
            }
        }
        return;
    }

    if (off >= NV11_PCRTC0_OFF && off < NV11_PCRTC0_END) {
        uint32_t reg = off - NV11_PCRTC0_OFF;
        trace_nv11_pcrtc_write(eip, 0, reg, size, (uint32_t)val);
        if (reg == NV11_PCRTC_CURSOR_CFG) {
            s->cur_cfg = (uint32_t)val;
        }
        goto flat_write;
    }

    if (off >= NV11_PRMCIO0_OFF && off < NV11_PRMCIO0_END) {
        uint32_t reg = off - NV11_PRMCIO0_OFF;
        trace_nv11_pmcio_write(eip, 0, reg, size, (uint32_t)val);
        if (reg >= 0x3C0 && reg <= 0x3DF) {
            if (reg == 0x3D5) {
                nv11_pcrtc_write(s, s->vga.cr_index, (uint8_t)val);
            } else {
                vga_ioport_write(&s->vga, reg, (uint8_t)val);
            }
        }
        return;
    }

    if (off >= NV11_PCRTC1_OFF && off < NV11_PCRTC1_END) {
        uint32_t reg = off - NV11_PCRTC1_OFF;
        trace_nv11_pcrtc_write(eip, 1, reg, size, (uint32_t)val);
        if (reg == NV11_PCRTC_CURSOR_CFG) {
            s->cur_cfg = (uint32_t)val;
        }
        goto flat_write;
    }

    if (off >= NV11_PRMCIO1_OFF && off < NV11_PRMCIO1_END) {
        uint32_t reg = off - NV11_PRMCIO1_OFF;
        trace_nv11_pmcio_write(eip, 1, reg, size, (uint32_t)val);
        if (reg >= 0x3C0 && reg <= 0x3DF) {
            if (reg == 0x3D5) {
                nv11_pcrtc_write(s, s->vga.cr_index, (uint8_t)val);
            } else {
                vga_ioport_write(&s->vga, reg, (uint8_t)val);
            }
        }
        return;
    }

    if (off >= NV11_PRMDIO_OFF && off < NV11_PRMDIO_END) {
        uint32_t port = off - NV11_PRMDIO_OFF + 0x3C0;
        vga_ioport_write(&s->vga, port, (uint8_t)val);
        return;
    }

    if (off >= NV11_PVIO_OFF && off < NV11_PVIO_END) {
        uint32_t reg = off - NV11_PVIO_OFF;
        trace_nv11_pmcio_write(eip, 0, reg, size, (uint32_t)val);
        if (reg >= 0x3B0 && reg <= 0x3DF) {
            if (reg == 0x3D5 || reg == 0x3B5) {
                nv11_pcrtc_write(s, s->vga.cr_index, (uint8_t)val);
            } else {
                vga_ioport_write(&s->vga, reg, (uint8_t)val);
            }
        }
        return;
    }

    if (off >= NV11_PRAMDAC0_OFF && off < NV11_PRAMDAC0_END) {
        uint32_t reg = off - NV11_PRAMDAC0_OFF;
        uint32_t idx = reg / 4;
        if (idx < sizeof(s->pramdac[0]) / sizeof(s->pramdac[0][0])) {
            s->pramdac[0][idx] = (uint32_t)val;
        }
        if (reg == NV11_PRAMDAC_CUR_POS) {
            s->cur_pos = (uint32_t)val;
        }
        trace_nv11_pramdac_write(eip, 0, reg, size, (uint32_t)val);
        return;
    }

    if (off >= NV11_PRAMDAC1_OFF && off < NV11_PRAMDAC1_END) {
        uint32_t reg = off - NV11_PRAMDAC1_OFF;
        uint32_t idx = reg / 4;
        if (idx < sizeof(s->pramdac[1]) / sizeof(s->pramdac[1][0])) {
            s->pramdac[1][idx] = (uint32_t)val;
        }
        if (reg == NV11_PRAMDAC_CUR_POS) {
            s->cur_pos = (uint32_t)val;
        }
        trace_nv11_pramdac_write(eip, 1, reg, size, (uint32_t)val);
        return;
    }

    if (off >= NV11_PGRAPH_OFF && off < NV11_PGRAPH_END) {
        nv11_pgraph_write(s, off - NV11_PGRAPH_OFF, (uint32_t)val, size);
        return;
    }

    if (off >= NV11_FIFO_OFF && off < NV11_FIFO_END) {
        nv11_fifo_write(s, off - NV11_FIFO_OFF, (uint32_t)val, size);
        return;
    }

flat_write:
    if (off + size > NV11_BAR0_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [BAR0] write beyond bounds at 0x%x\n", off);
        return;
    }
    switch (size) {
    case 1: s->bar0_flat[off] = (uint8_t)val; break;
    case 2: stw_le_p((uint16_t *)(s->bar0_flat + off), (uint16_t)val); break;
    case 4: stl_le_p((uint32_t *)(s->bar0_flat + off), (uint32_t)val); break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NV11: [BAR0] bad write size %u at 0x%x\n", size, off);
        return;
    }
}

static uint64_t nv11_bar0_read_mr(void *opaque, hwaddr addr, unsigned size)
{
    return nv11_bar0_read(NV11(opaque), addr, size);
}

static void nv11_bar0_write_mr(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    nv11_bar0_write(NV11(opaque), addr, val, size);
}

static const MemoryRegionOps nv11_bar0_ops = {
    .read = nv11_bar0_read_mr,
    .write = nv11_bar0_write_mr,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void nv11_realize(PCIDevice *dev, Error **errp)
{
    NV11State *s = NV11(dev);

    memset(s->bar0_flat, 0, sizeof(s->bar0_flat));

    s->vga.vram_size_mb = NV11_BAR1_VRAM_SIZE_MB;
    if (!vga_common_init(&s->vga, OBJECT(dev), errp)) {
        return;
    }

    s->vram_ptr = memory_region_get_ram_ptr(&s->vga.vram);

    s->vga.get_params = nv11_get_params;
    s->vga.get_bpp = nv11_get_bpp;
    s->vga.get_resolution = nv11_get_resolution;
    s->vga.force_shadow = 1;
    s->vga.cursor_invalidate = nv11_cursor_invalidate;
    s->vga.cursor_draw_line = nv11_cursor_draw_line;

    /* BAR0: MMIO */
    memory_region_init_io(&s->bar0, OBJECT(dev), &nv11_bar0_ops, s,
                          "nv11.bar0", NV11_BAR0_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_32, &s->bar0);

    /* BAR1: Prefetched VRAM */
    memory_region_init_alias(&s->bar1, OBJECT(dev), "nv11.bar1.vram",
                             &s->vga.vram, 0, s->vga.vram_size);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_32, &s->bar1);

    vga_init(&s->vga, OBJECT(dev),
             pci_address_space(dev), pci_address_space_io(dev), false);
    s->vga.con = qemu_graphic_console_create(DEVICE(dev), 0,
                                             s->vga.hw_ops, &s->vga);


    /* Setup Programmable Functions */
    nv11_pcrtc_init(s);
    nv11_pbus_init(s);
    nv11_pramdac_init(s);
    nv11_vga_init(s, dev, errp);
    nv11_window_init(s);
    nv11_prome_init(s);
    nv11_pgraph_init(s);
    nv11_ptimer_init(s);
    nv11_fifo_init(s);
    nv11_2d_init(s);
    nv11_i2c_init(s);

    pci_set_word(dev->config + PCI_COMMAND,
                 PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_FAST_BACK);
    pci_set_byte(dev->config + PCI_REVISION_ID, 0xB2);
    pci_set_byte(dev->config + PCI_INTERRUPT_PIN, 1);
}

static const Property nv11_properties[] = {
    DEFINE_EDID_PROPERTIES(NV11State, edid_info),
};

static void nv11_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = nv11_realize;
    k->vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->device_id = PCI_DEVICE_ID_NVIDIA_NV11B;
    k->class_id  = PCI_CLASS_DISPLAY_VGA;
    dc->hotpluggable = false;
    device_class_set_props(dc, nv11_properties);
    nv11_vga_class_reset(klass);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static void nv11_instance_init(Object *o)
{
    NV11State *s = NV11(o);

    object_initialize_child(o, "ddc", &s->ddc, TYPE_I2CDDC);
}

static const TypeInfo nv11_type_info = {
    .name          = TYPE_NV11,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NV11State),
    .instance_init = nv11_instance_init,
    .class_init    = nv11_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void nv11_register_types(void)
{
    type_register_static(&nv11_type_info);
}

type_init(nv11_register_types)
