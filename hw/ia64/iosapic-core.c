/*
 * IA-64 I/O SAPIC core model
 *
 * Copyright (c) 2026 Bryce Lanham
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ia64/iosapic-core.h"

#define IA64_IOSAPIC_RTE_READ_ONLY \
    (IA64_IOSAPIC_DELIVERY_STATUS | IA64_IOSAPIC_REMOTE_IRR)

static unsigned int ia64_iosapic_selected_pin(uint32_t selector)
{
    return (selector - IA64_IOSAPIC_RTE_BASE) >> 1;
}

static bool ia64_iosapic_selector_is_rte(uint32_t selector)
{
    return selector >= IA64_IOSAPIC_RTE_BASE &&
           selector <= IA64_IOSAPIC_RTE_HIGH(IA64_IOSAPIC_NUM_PINS - 1);
}

static uint32_t ia64_iosapic_rte_low(const IA64IOSAPICCore *s,
                                     unsigned int pin)
{
    return (uint32_t)s->rte[pin];
}

static bool ia64_iosapic_service_pin(IA64IOSAPICCore *s, unsigned int pin)
{
    uint64_t bit = UINT64_C(1) << pin;
    uint32_t low = ia64_iosapic_rte_low(s, pin);
    uint8_t vector;
    uint8_t delivery_mode;
    uint16_t destination;
    bool level_triggered;

    if (!(s->irr & bit) || (low & IA64_IOSAPIC_MASKED)) {
        return false;
    }

    level_triggered = low & IA64_IOSAPIC_TRIGGER_LEVEL;
    if (level_triggered && (low & IA64_IOSAPIC_REMOTE_IRR)) {
        return false;
    }

    vector = low & IA64_IOSAPIC_VECTOR_MASK;
    delivery_mode = (low & IA64_IOSAPIC_DELIVERY_MASK) >>
                    IA64_IOSAPIC_DELIVERY_SHIFT;
    destination = (uint32_t)(s->rte[pin] >> 32) >>
                  IA64_IOSAPIC_DESTINATION_SHIFT;

    if (!s->deliver || !s->deliver(s->deliver_opaque, vector, destination,
                                   delivery_mode)) {
        return false;
    }

    if (level_triggered) {
        s->rte[pin] |= IA64_IOSAPIC_REMOTE_IRR;
    } else {
        s->irr &= ~bit;
    }

    return true;
}

static uint32_t ia64_iosapic_read_window(IA64IOSAPICCore *s)
{
    uint32_t selector = s->selector & 0xff;
    unsigned int pin;

    switch (selector) {
    case IA64_IOSAPIC_REG_ID:
        return 0;
    case IA64_IOSAPIC_REG_VERSION:
        return IA64_IOSAPIC_VERSION |
               ((IA64_IOSAPIC_NUM_PINS - 1) << 16);
    default:
        break;
    }

    if (!ia64_iosapic_selector_is_rte(selector)) {
        return 0;
    }

    pin = ia64_iosapic_selected_pin(selector);
    if (selector & 1) {
        return s->rte[pin] >> 32;
    }
    return s->rte[pin];
}

static void ia64_iosapic_write_rte(IA64IOSAPICCore *s, uint32_t selector,
                                    uint32_t value)
{
    unsigned int pin = ia64_iosapic_selected_pin(selector);
    uint64_t old = s->rte[pin];
    uint64_t updated;

    if (selector & 1) {
        updated = (old & UINT64_C(0xffffffff)) | ((uint64_t)value << 32);
    } else {
        uint32_t old_low = old;
        uint32_t new_low = (value & ~IA64_IOSAPIC_RTE_READ_ONLY) |
                           (old_low & IA64_IOSAPIC_RTE_READ_ONLY);

        /*
         * Remote IRR is meaningful only for level-triggered entries.  It is
         * distinct from the pending-request bit in irr: rewriting an edge RTE
         * must not discard a request whose previous delivery was rejected.
         */
        if (!(new_low & IA64_IOSAPIC_TRIGGER_LEVEL)) {
            new_low &= ~IA64_IOSAPIC_REMOTE_IRR;
        }

        updated = (old & UINT64_C(0xffffffff00000000)) | new_low;
    }

    s->rte[pin] = updated;
    ia64_iosapic_service_pin(s, pin);
}

static void ia64_iosapic_eoi(IA64IOSAPICCore *s, uint8_t vector)
{
    unsigned int pin;

    /*
     * Multiple level-triggered inputs may share one vector.  The explicit
     * EOI applies to every matching RTE, not merely the first one found.
     */
    for (pin = 0; pin < IA64_IOSAPIC_NUM_PINS; pin++) {
        uint32_t low = ia64_iosapic_rte_low(s, pin);

        if (!(low & IA64_IOSAPIC_TRIGGER_LEVEL) ||
            !(low & IA64_IOSAPIC_REMOTE_IRR) ||
            (low & IA64_IOSAPIC_VECTOR_MASK) != vector) {
            continue;
        }

        s->rte[pin] &= ~((uint64_t)IA64_IOSAPIC_REMOTE_IRR);
        ia64_iosapic_service_pin(s, pin);
    }
}

void ia64_iosapic_core_init(IA64IOSAPICCore *s,
                            IA64IOSAPICDeliverFn deliver,
                            void *deliver_opaque)
{
    memset(s, 0, sizeof(*s));
    s->deliver = deliver;
    s->deliver_opaque = deliver_opaque;
    ia64_iosapic_core_reset(s);
}

void ia64_iosapic_core_reset(IA64IOSAPICCore *s)
{
    unsigned int pin;

    s->selector = 0;
    s->irr = 0;
    s->pin_level = 0;
    for (pin = 0; pin < IA64_IOSAPIC_NUM_PINS; pin++) {
        s->rte[pin] = IA64_IOSAPIC_MASKED;
    }
}

uint32_t ia64_iosapic_core_read(IA64IOSAPICCore *s, uint32_t offset)
{
    switch (offset) {
    case IA64_IOSAPIC_REG_SELECT:
        return s->selector;
    case IA64_IOSAPIC_WINDOW:
        return ia64_iosapic_read_window(s);
    case IA64_IOSAPIC_EOI:
    default:
        return 0;
    }
}

void ia64_iosapic_core_write(IA64IOSAPICCore *s, uint32_t offset,
                             uint32_t value)
{
    uint32_t selector;

    switch (offset) {
    case IA64_IOSAPIC_REG_SELECT:
        s->selector = value;
        break;
    case IA64_IOSAPIC_WINDOW:
        selector = s->selector & 0xff;
        if (ia64_iosapic_selector_is_rte(selector)) {
            ia64_iosapic_write_rte(s, selector, value);
        }
        break;
    case IA64_IOSAPIC_EOI:
        ia64_iosapic_eoi(s, value & IA64_IOSAPIC_VECTOR_MASK);
        break;
    default:
        break;
    }
}

void ia64_iosapic_core_set_irq(IA64IOSAPICCore *s, unsigned int pin,
                               int level)
{
    uint64_t bit;
    uint32_t low;
    bool was_high;

    if (pin >= IA64_IOSAPIC_NUM_PINS) {
        return;
    }

    bit = UINT64_C(1) << pin;
    low = ia64_iosapic_rte_low(s, pin);
    was_high = s->pin_level & bit;

    if (level) {
        s->pin_level |= bit;
    } else {
        s->pin_level &= ~bit;
    }

    if (low & IA64_IOSAPIC_TRIGGER_LEVEL) {
        if (level) {
            s->irr |= bit;
            ia64_iosapic_service_pin(s, pin);
        } else {
            s->irr &= ~bit;
        }
        return;
    }

    /* The 82093AA/IOSAPIC rule also applies here: masked edges are lost. */
    if (level && !was_high && !(low & IA64_IOSAPIC_MASKED)) {
        s->irr |= bit;
        ia64_iosapic_service_pin(s, pin);
    }
}
