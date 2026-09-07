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
 * The GF2 MX400 variant used is the Abit Siluto. Normally it's AGP but we hack it as PCI.
 * The VBIOS is named MX4_0153.rom
 * 
 * Specs:
 * Nvidia NV11B chip (Stepping B2)
 * 64MB DDR
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/pci/pci.h"
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

    if (off >= NV11_PRAMDAC0_OFF && off < NV11_PRAMDAC0_END) {
        uint32_t reg = off - NV11_PRAMDAC0_OFF;
        uint32_t idx = reg / 4;
        if (idx < sizeof(s->pramdac[0]) / sizeof(s->pramdac[0][0])) {
            s->pramdac[0][idx] = (uint32_t)val;
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
        trace_nv11_pramdac_write(eip, 1, reg, size, (uint32_t)val);
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

    s->vga.vram_size_mb = NV11_BAR1_VRAM_SIZE_MB; /* Abit Siluto has 64MB of VRAM */
    if (!vga_common_init(&s->vga, OBJECT(dev), errp)) {
        return;
    }

    s->vga.get_params = nv11_get_params;
    s->vga.get_bpp = nv11_get_bpp;
    s->vga.get_resolution = nv11_get_resolution;

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

    pci_set_word(dev->config + PCI_COMMAND,
                 PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_FAST_BACK);
    pci_set_byte(dev->config + PCI_REVISION_ID, 0xB2);
    pci_set_byte(dev->config + PCI_INTERRUPT_PIN, 1);
}

static void nv11_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = nv11_realize;
    k->vendor_id = NV11_PCI_VENDOR_ID;
    k->device_id = NV11_PCI_DEVICE_ID;
    k->class_id  = PCI_CLASS_DISPLAY_VGA;
    dc->hotpluggable = false;
    nv11_vga_class_reset(klass);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo nv11_type_info = {
    .name          = TYPE_NV11,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NV11State),
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
