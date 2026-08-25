/*
 * IA-64 I/O SAPIC core model
 *
 * Copyright (c) 2026 Bryce Lanham
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IA64_IOSAPIC_CORE_H
#define HW_IA64_IOSAPIC_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define IA64_IOSAPIC_NUM_PINS       48
#define IA64_IOSAPIC_VERSION        0x20

#define IA64_IOSAPIC_REG_SELECT     0x00
#define IA64_IOSAPIC_WINDOW         0x10
#define IA64_IOSAPIC_EOI            0x40
#define IA64_IOSAPIC_MMIO_SIZE      0x1000

#define IA64_IOSAPIC_REG_ID         0x00
#define IA64_IOSAPIC_REG_VERSION    0x01
#define IA64_IOSAPIC_RTE_BASE       0x10

#define IA64_IOSAPIC_RTE_LOW(pin)   (IA64_IOSAPIC_RTE_BASE + (pin) * 2)
#define IA64_IOSAPIC_RTE_HIGH(pin)  (IA64_IOSAPIC_RTE_BASE + (pin) * 2 + 1)

#define IA64_IOSAPIC_VECTOR_MASK        0x000000ffU
#define IA64_IOSAPIC_DELIVERY_SHIFT     8
#define IA64_IOSAPIC_DELIVERY_MASK      (7U << IA64_IOSAPIC_DELIVERY_SHIFT)
#define IA64_IOSAPIC_DELIVERY_STATUS    (1U << 12)
#define IA64_IOSAPIC_POLARITY_LOW       (1U << 13)
#define IA64_IOSAPIC_REMOTE_IRR         (1U << 14)
#define IA64_IOSAPIC_TRIGGER_LEVEL      (1U << 15)
#define IA64_IOSAPIC_MASKED             (1U << 16)
#define IA64_IOSAPIC_DESTINATION_SHIFT  16

#define IA64_IOSAPIC_DELIVERY_FIXED     0
#define IA64_IOSAPIC_DELIVERY_LOWEST    1
#define IA64_IOSAPIC_DELIVERY_PMI       2
#define IA64_IOSAPIC_DELIVERY_NMI       4
#define IA64_IOSAPIC_DELIVERY_INIT      5
#define IA64_IOSAPIC_DELIVERY_EXTINT    7

typedef bool (*IA64IOSAPICDeliverFn)(void *opaque, uint8_t vector,
                                     uint16_t destination,
                                     uint8_t delivery_mode);

typedef struct IA64IOSAPICCore {
    uint32_t selector;
    uint64_t rte[IA64_IOSAPIC_NUM_PINS];
    uint64_t irr;
    uint64_t pin_level;

    IA64IOSAPICDeliverFn deliver;
    void *deliver_opaque;
} IA64IOSAPICCore;

void ia64_iosapic_core_init(IA64IOSAPICCore *s,
                            IA64IOSAPICDeliverFn deliver,
                            void *deliver_opaque);
void ia64_iosapic_core_reset(IA64IOSAPICCore *s);
uint32_t ia64_iosapic_core_read(IA64IOSAPICCore *s, uint32_t offset);
void ia64_iosapic_core_write(IA64IOSAPICCore *s, uint32_t offset,
                             uint32_t value);
void ia64_iosapic_core_set_irq(IA64IOSAPICCore *s, unsigned int pin,
                               int level);

#endif /* HW_IA64_IOSAPIC_CORE_H */
