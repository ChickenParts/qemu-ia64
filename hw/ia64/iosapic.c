/*
 * IA-64 I/O SAPIC device
 *
 * Copyright (c) 2026 Bryce Lanham
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ia64/iosapic.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "target/ia64/cpu.h"

struct IA64IOSAPICState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IA64CPU *cpu;
    IA64IOSAPICCore core;
};

static bool ia64_iosapic_deliver(void *opaque, uint8_t vector,
                                 uint16_t destination,
                                 uint8_t delivery_mode)
{
    IA64IOSAPICState *s = opaque;

    if (destination != 0) {
        qemu_log_mask(LOG_UNIMP,
                      "IA-64 IOSAPIC: destination 0x%x is not available "
                      "on the current uniprocessor machine\n",
                      destination);
        return false;
    }

    switch (delivery_mode) {
    case IA64_IOSAPIC_DELIVERY_FIXED:
    case IA64_IOSAPIC_DELIVERY_LOWEST:
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "IA-64 IOSAPIC: unsupported delivery mode %u "
                      "for vector 0x%x\n",
                      delivery_mode, vector);
        return false;
    }

    if (!ia64_cpu_deposit_interrupt(s->cpu, vector)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA-64 IOSAPIC: rejected reserved vector 0x%x\n",
                      vector);
        return false;
    }

    return true;
}

static uint64_t ia64_iosapic_mmio_read(void *opaque, hwaddr offset,
                                       unsigned int size)
{
    IA64IOSAPICState *s = opaque;

    (void)size;
    return ia64_iosapic_core_read(&s->core, offset);
}

static void ia64_iosapic_mmio_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned int size)
{
    IA64IOSAPICState *s = opaque;

    (void)size;
    ia64_iosapic_core_write(&s->core, offset, value);
}

static const MemoryRegionOps ia64_iosapic_mmio_ops = {
    .read = ia64_iosapic_mmio_read,
    .write = ia64_iosapic_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

void ia64_iosapic_set_irq(void *opaque, int pin, int level)
{
    IA64IOSAPICState *s = opaque;

    if (pin < 0) {
        return;
    }
    ia64_iosapic_core_set_irq(&s->core, pin, level);
}

qemu_irq ia64_iosapic_get_irq(IA64IOSAPICState *s, unsigned int pin)
{
    assert(pin < IA64_IOSAPIC_NUM_PINS);
    return qdev_get_gpio_in(DEVICE(s), pin);
}

static void ia64_iosapic_reset(DeviceState *dev)
{
    IA64IOSAPICState *s = IA64_IOSAPIC(dev);

    ia64_iosapic_core_reset(&s->core);
}

static void ia64_iosapic_realize(DeviceState *dev, Error **errp)
{
    IA64IOSAPICState *s = IA64_IOSAPIC(dev);

    if (!s->cpu) {
        error_setg(errp, "IA-64 IOSAPIC requires a CPU link");
        return;
    }
}

static void ia64_iosapic_init(Object *obj)
{
    IA64IOSAPICState *s = IA64_IOSAPIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    ia64_iosapic_core_init(&s->core, ia64_iosapic_deliver, s);
    memory_region_init_io(&s->mmio, obj, &ia64_iosapic_mmio_ops, s,
                          TYPE_IA64_IOSAPIC, IA64_IOSAPIC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    qdev_init_gpio_in(DEVICE(obj), ia64_iosapic_set_irq,
                      IA64_IOSAPIC_NUM_PINS);
}

static const VMStateDescription vmstate_ia64_iosapic = {
    .name = TYPE_IA64_IOSAPIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(core.selector, IA64IOSAPICState),
        VMSTATE_UINT64_ARRAY(core.rte, IA64IOSAPICState,
                             IA64_IOSAPIC_NUM_PINS),
        VMSTATE_UINT64(core.irr, IA64IOSAPICState),
        VMSTATE_UINT64(core.pin_level, IA64IOSAPICState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property ia64_iosapic_properties[] = {
    DEFINE_PROP_LINK("cpu", IA64IOSAPICState, cpu, TYPE_IA64_CPU, IA64CPU *),
};

static void ia64_iosapic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ia64_iosapic_realize;
    device_class_set_legacy_reset(dc, ia64_iosapic_reset);
    dc->vmsd = &vmstate_ia64_iosapic;
    dc->desc = "IA-64 I/O SAPIC";
    dc->user_creatable = false;
    device_class_set_props(dc, ia64_iosapic_properties);
}

static const TypeInfo ia64_iosapic_info = {
    .name = TYPE_IA64_IOSAPIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IA64IOSAPICState),
    .instance_init = ia64_iosapic_init,
    .class_init = ia64_iosapic_class_init,
};

static void ia64_iosapic_register_types(void)
{
    type_register_static(&ia64_iosapic_info);
}

type_init(ia64_iosapic_register_types)
