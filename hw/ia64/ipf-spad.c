/*
 * IA-64 IPF firmware scratchpad device
 *
 * Copyright (c) 2026 Bryce Lanham
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ia64/ipf-spad.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/module.h"

struct IA64IPFSPADState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint8_t data[IA64_IPF_SPAD_SIZE];
};

static uint64_t ipf_spad_read(void *opaque, hwaddr offset,
                              unsigned int size)
{
    IA64IPFSPADState *s = opaque;
    const uint8_t *p;

    if (offset + size > sizeof(s->data)) {
        return 0;
    }
    p = &s->data[offset];

    switch (size) {
    case 1:
        return ldub_p(p);
    case 2:
        return lduw_le_p(p);
    case 4:
        return ldl_le_p(p);
    case 8:
        return ldq_le_p(p);
    default:
        return 0;
    }
}

static void ipf_spad_write(void *opaque, hwaddr offset,
                           uint64_t value, unsigned int size)
{
    IA64IPFSPADState *s = opaque;
    uint8_t *p;

    if (offset + size > sizeof(s->data)) {
        return;
    }
    p = &s->data[offset];

    switch (size) {
    case 1:
        stb_p(p, value);
        break;
    case 2:
        stw_le_p(p, value);
        break;
    case 4:
        stl_le_p(p, value);
        break;
    case 8:
        stq_le_p(p, value);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ipf_spad_ops = {
    .read = ipf_spad_read,
    .write = ipf_spad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void ipf_spad_init_state(IA64IPFSPADState *s)
{
    static const uint8_t bsp_signature[8] = {
        0x20, 0x5f, 0x5f, 0x42, 0x53, 0x50, 0x5f, 0x5f,
    };
    uint8_t *record =
        &s->data[IA64_IPF_SPAD_MP_RECORD_OFFSET];

    memset(s->data, 0xff, sizeof(s->data));
    stq_le_p(&s->data[IA64_IPF_SPAD_LOCK_PTR_OFFSET],
             IA64_IPF_SPAD_BASE);
    memset(record, 0, IA64_IPF_SPAD_MP_RECORD_SIZE);
    memcpy(record + IA64_IPF_SPAD_MP_SIGNATURE_OFFSET,
           bsp_signature, sizeof(bsp_signature));
}

static void ipf_spad_init(Object *obj)
{
    IA64IPFSPADState *s = IA64_IPF_SPAD(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    ipf_spad_init_state(s);
    memory_region_init_io(&s->mmio, obj, &ipf_spad_ops, s,
                          TYPE_IA64_IPF_SPAD,
                          IA64_IPF_SPAD_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
}

static const VMStateDescription vmstate_ipf_spad = {
    .name = TYPE_IA64_IPF_SPAD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(data, IA64IPFSPADState,
                            IA64_IPF_SPAD_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void ipf_spad_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipf_spad;
    dc->desc = "IA-64 IPF firmware scratchpad";
    dc->user_creatable = false;
}

static const TypeInfo ipf_spad_info = {
    .name = TYPE_IA64_IPF_SPAD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IA64IPFSPADState),
    .instance_init = ipf_spad_init,
    .class_init = ipf_spad_class_init,
};

static void ipf_spad_register_types(void)
{
    type_register_static(&ipf_spad_info);
}

type_init(ipf_spad_register_types)
