/*
 * Intel 82845 845 Chipset Host Bridge and Memory Controller Hub (Brookdale)
 *
 * Copyright (c) 2006 Fabrice Bellard
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
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/range.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/brookdale.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(I845State, I845_PCI_HOST_BRIDGE)

struct I845State {
    PCIHostState parent_obj;

    MemoryRegion *system_memory;
    MemoryRegion *io_memory;
    MemoryRegion *pci_address_space;
    MemoryRegion *ram_memory;
    Range pci_hole;
    uint64_t below_4g_mem_size;
    uint64_t above_4g_mem_size;
    uint64_t pci_hole64_size;
    bool pci_hole64_fix;

    char *pci_type;
};

static void i845_realize(PCIDevice *dev, Error **errp)
{
    if (object_property_get_bool(qdev_get_machine(), "iommu", NULL)) {
        warn_report("i845E doesn't support emulated iommu");
    }
}

static void i845_update_pam(int segment, PCII845State *d)
{
    PCIDevice *pci_dev = PCI_DEVICE(d);
    uint8_t val = pci_get_byte(pci_dev->config + 0x90 + segment);

    memory_region_transaction_begin();

    if(segment == 0) {
        pam_update(&d->pam_regions[0], 0, val);
    } else {
        pam_update(&d->pam_regions[segment * 2], segment * 2, val);
        pam_update(&d->pam_regions[(segment * 2) - 1], (segment * 2) - 1, val);
    }

    memory_region_transaction_commit();
}

static void i845_update_smram(PCII845State *d)
{
    PCIDevice *pci_dev = PCI_DEVICE(d);
    uint8_t val = pci_get_byte(pci_dev->config + 0x9d);

    memory_region_transaction_begin();

    memory_region_set_enabled(&d->low_smram, false);
    memory_region_set_enabled(&d->smram_region, false);

    if(val & 0x08) { /* G_SMRAME */
        if(val & 0x40) { /* D_OPEN */
            memory_region_set_enabled(&d->low_smram, true);
        }
        else {
            memory_region_set_enabled(&d->low_smram, true);
            memory_region_set_enabled(&d->smram_region, true);
        }
    }
    else {
        memory_region_set_enabled(&d->smram_region, true);
    }

    memory_region_transaction_commit();
}

static void i845_write_config(PCIDevice *dev, uint32_t address, uint32_t val, int len)
{
    PCII845State *d = I845_PCI_DEVICE(dev);
    
    switch(address) {
        case 0x86: case 0x8c: case 0x8d: case 0x8e:
        case 0x8f: case 0xa0: case 0xa1: case 0xa2:
        case 0xa3: case 0xa4: case 0xa5: case 0xa6:
        case 0xa7:
        return;
    }

    pci_default_write_config(dev, address, val, len);

    switch(address) {
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96:
            i845_update_pam(address - 0x90, d);
        break;

        case 0x9d:
            i845_update_smram(d);
        break;
    }
}

static void i845_pcihost_get_pci_hole_start(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    I845State *s = I845_PCI_HOST_BRIDGE(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_lob(&s->pci_hole);
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static void i845_pcihost_get_pci_hole_end(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    I845State *s = I845_PCI_HOST_BRIDGE(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_upb(&s->pci_hole) + 1;
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static uint64_t i845_pcihost_get_pci_hole64_start_value(Object *obj)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    I845State *s = I845_PCI_HOST_BRIDGE(obj);
    Range w64;
    uint64_t value;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_lob(&w64);
    if (!value && s->pci_hole64_fix) {
        value = pc_pci_hole64_start();
    }
    return value;
}

static void i845_pcihost_get_pci_hole64_start(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t hole64_start = i845_pcihost_get_pci_hole64_start_value(obj);

    visit_type_uint64(v, name, &hole64_start, errp);
}

static void i845_pcihost_get_pci_hole64_end(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    I845State *s = I845_PCI_HOST_BRIDGE(obj);
    uint64_t hole64_start = i845_pcihost_get_pci_hole64_start_value(obj);
    Range w64;
    uint64_t value, hole64_end;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_upb(&w64) + 1;
    hole64_end = ROUND_UP(hole64_start + s->pci_hole64_size, 1ULL << 30);
    if (s->pci_hole64_fix && value < hole64_end) {
        value = hole64_end;
    }
    visit_type_uint64(v, name, &value, errp);
}

static void i845_pcihost_initfn(Object *obj)
{
    I845State *s = I845_PCI_HOST_BRIDGE(obj);
    PCIHostState *phb = PCI_HOST_BRIDGE(obj);

    memory_region_init_io(&phb->conf_mem, obj, &pci_host_conf_le_ops, phb, "pci-conf-idx", 4);
    memory_region_init_io(&phb->data_mem, obj, &pci_host_data_le_ops, phb, "pci-conf-data", 4);

    object_property_add_link(obj, PCI_HOST_PROP_RAM_MEM, TYPE_MEMORY_REGION, (Object **) &s->ram_memory, qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_PCI_MEM, TYPE_MEMORY_REGION, (Object **) &s->pci_address_space, qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_SYSTEM_MEM, TYPE_MEMORY_REGION, (Object **) &s->system_memory, qdev_prop_allow_set_link_before_realize, 0);

    object_property_add_link(obj, PCI_HOST_PROP_IO_MEM, TYPE_MEMORY_REGION, (Object **) &s->io_memory, qdev_prop_allow_set_link_before_realize, 0);
}

static void i845_reset(DeviceState *dev)
{
    PCII845State *d = I845_PCI_DEVICE(dev);
    PCIDevice *pci_dev = PCI_DEVICE(d);

    pci_set_word(pci_dev->config + PCI_COMMAND, PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY);
    pci_set_word(pci_dev->config + PCI_STATUS, PCI_STATUS_FAST_BACK | PCI_STATUS_CAP_LIST);
    pci_set_long(pci_dev->config + 0x10, 0x00000008);
    pci_set_byte(pci_dev->config + 0x34, 0xa0);
    pci_set_long(pci_dev->config + 0x78, 0x00000010);
    pci_set_long(pci_dev->config + 0xa0, 0x00200002);
    pci_set_long(pci_dev->config + 0xa4, 0x1f000216);
    pci_set_long(pci_dev->config + 0xe4, 0xf104a009);

    pci_set_byte(pci_dev->config + 0x90, 0x00);
    pci_set_byte(pci_dev->config + 0x91, 0x00);
    pci_set_byte(pci_dev->config + 0x92, 0x00);
    pci_set_byte(pci_dev->config + 0x93, 0x00);
    pci_set_byte(pci_dev->config + 0x94, 0x00);
    pci_set_byte(pci_dev->config + 0x95, 0x00);
    pci_set_byte(pci_dev->config + 0x96, 0x00);
    pci_set_byte(pci_dev->config + 0x9d, 0x02);
    pci_set_byte(pci_dev->config + 0x9e, 0x38);

    i845_update_pam(0, d);
    i845_update_pam(1, d);
    i845_update_pam(2, d);
    i845_update_pam(3, d);
    i845_update_pam(4, d);
    i845_update_pam(5, d);
    i845_update_pam(6, d);
    i845_update_smram(d);
}

static void i845_pcihost_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    I845State *s = I845_PCI_HOST_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    PCIBus *b;
    PCIDevice *d;
    PCII845State *f;

    fprintf(stderr, "Intel 845: Setting up the Bus\n");

    memory_region_add_subregion(s->io_memory, 0xcf8, &phb->conf_mem);
    sysbus_init_ioports(sbd, 0xcf8, 4);

    memory_region_add_subregion(s->io_memory, 0xcfc, &phb->data_mem);
    sysbus_init_ioports(sbd, 0xcfc, 4);

    memory_region_set_flush_coalesced(&phb->data_mem);
    memory_region_add_coalescing(&phb->conf_mem, 0, 4);

    b = pci_root_bus_new(dev, NULL, s->pci_address_space, s->io_memory, 0, TYPE_PCI_BUS);
    phb->bus = b;

    d = pci_create_simple(b, 0, s->pci_type);
    f = I845_PCI_DEVICE(d);

    range_set_bounds(&s->pci_hole, s->below_4g_mem_size, IO_APIC_DEFAULT_ADDRESS - 1);

    fprintf(stderr, "Intel 845: Setting up Memory\n");
    pc_pci_as_mapping_init(s->system_memory, s->pci_address_space);

    /* AB segment for PCI */
    memory_region_init_alias(&f->smram_region, OBJECT(d), "smram-region", s->pci_address_space, 0xa0000, 0x20000);
    memory_region_add_subregion_overlap(s->system_memory, 0xa0000, &f->smram_region, 1);

    /* AB segment for DRAM/SMM */
    memory_region_init(&f->smram, OBJECT(d), "smram", 4 * GiB);
    memory_region_set_enabled(&f->smram, true);

    memory_region_init_alias(&f->low_smram, OBJECT(d), "smram-low", s->ram_memory, 0xa0000, 0x20000);
    memory_region_add_subregion(&f->smram, 0xa0000, &f->low_smram);

    memory_region_init_alias(&f->smbase, OBJECT(d), "smbase", s->ram_memory, 0x30000, 0x20000);
    memory_region_set_enabled(&f->smbase, true);
    memory_region_add_subregion(&f->smram, 0x30000, &f->smbase);

    object_property_add_const_link(qdev_get_machine(), "smram", OBJECT(&f->smram));

    init_pam(&f->pam_regions[0], OBJECT(d), s->ram_memory, s->system_memory, s->pci_address_space, 0xf0000, 0x10000);
    for (int i = 0; i < ARRAY_SIZE(f->pam_regions) - 1; i++) {
        init_pam(&f->pam_regions[i + 1], OBJECT(d), s->ram_memory, s->system_memory, s->pci_address_space, 0xc0000 + i * 0x4000, 0x4000);
    }

    fprintf(stderr, "Intel 845: Initialization complete\n");
}

static void i845_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = i845_realize;
    k->config_write = i845_write_config;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_I845;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "Host bridge";
    device_class_set_legacy_reset(dc, i845_reset);
    dc->user_creatable = false;
    dc->hotpluggable   = false;
}

static const TypeInfo i845_info = {
    .name          = TYPE_I845_PCI_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCII845State),
    .class_init    = i845_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const char *i845_pcihost_root_bus_path(PCIHostState *host_bridge, PCIBus *rootbus)
{
    return "0000:00";
}

static const Property i845_props[] = {
    DEFINE_PROP_SIZE(PCI_HOST_PROP_PCI_HOLE64_SIZE, I845State, pci_hole64_size, 1ULL << 31),
    DEFINE_PROP_SIZE(PCI_HOST_BELOW_4G_MEM_SIZE, I845State, below_4g_mem_size, 0),
    DEFINE_PROP_SIZE(PCI_HOST_ABOVE_4G_MEM_SIZE, I845State, above_4g_mem_size, 0),
    DEFINE_PROP_BOOL("x-pci-hole64-fix", I845State, pci_hole64_fix, true),
    DEFINE_PROP_STRING(I845_HOST_PROP_PCI_TYPE, I845State, pci_type),
};

static void i845_pcihost_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    hc->root_bus_path = i845_pcihost_root_bus_path;
    dc->realize = i845_pcihost_realize;
    dc->fw_name = "pci";
    device_class_set_props(dc, i845_props);
    dc->user_creatable = false;

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE_START, "uint32", i845_pcihost_get_pci_hole_start, NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE_END, "uint32", i845_pcihost_get_pci_hole_end, NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE64_START, "uint64", i845_pcihost_get_pci_hole64_start, NULL, NULL, NULL);

    object_class_property_add(klass, PCI_HOST_PROP_PCI_HOLE64_END, "uint64", i845_pcihost_get_pci_hole64_end, NULL, NULL, NULL);
}

static const TypeInfo i845_pcihost_info = {
    .name          = TYPE_I845_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(I845State),
    .instance_init = i845_pcihost_initfn,
    .class_init    = i845_pcihost_class_init,
};

static void i845_register_types(void)
{
    type_register_static(&i845_info);
    type_register_static(&i845_pcihost_info);
}

type_init(i845_register_types)
