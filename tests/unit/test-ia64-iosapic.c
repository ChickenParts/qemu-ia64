/*
 * IA-64 I/O SAPIC core tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ia64/iosapic-core.h"

#define BIT64(n) (UINT64_C(1) << (n))

typedef struct DeliveryLog {
    unsigned int count;
    uint8_t vector;
    uint16_t destination;
    uint8_t mode;
    bool accept;
} DeliveryLog;

static bool record_delivery(void *opaque, uint8_t vector,
                            uint16_t destination, uint8_t mode)
{
    DeliveryLog *log = opaque;

    log->count++;
    log->vector = vector;
    log->destination = destination;
    log->mode = mode;
    return log->accept;
}

static void select_register(IA64IOSAPICCore *s, uint32_t reg)
{
    ia64_iosapic_core_write(s, IA64_IOSAPIC_REG_SELECT, reg);
}

static uint32_t read_register(IA64IOSAPICCore *s, uint32_t reg)
{
    select_register(s, reg);
    return ia64_iosapic_core_read(s, IA64_IOSAPIC_WINDOW);
}

static void write_register(IA64IOSAPICCore *s, uint32_t reg, uint32_t value)
{
    select_register(s, reg);
    ia64_iosapic_core_write(s, IA64_IOSAPIC_WINDOW, value);
}

static void program_rte(IA64IOSAPICCore *s, unsigned int pin,
                        uint8_t vector, uint8_t mode, bool level,
                        bool masked, uint16_t destination)
{
    uint32_t low = vector | ((uint32_t)mode << IA64_IOSAPIC_DELIVERY_SHIFT);

    if (level) {
        low |= IA64_IOSAPIC_TRIGGER_LEVEL;
    }
    if (masked) {
        low |= IA64_IOSAPIC_MASKED;
    }

    write_register(s, IA64_IOSAPIC_RTE_HIGH(pin),
                   (uint32_t)destination << IA64_IOSAPIC_DESTINATION_SHIFT);
    write_register(s, IA64_IOSAPIC_RTE_LOW(pin), low);
}

static void test_reset_and_registers(void)
{
    IA64IOSAPICCore s;
    DeliveryLog log = { .accept = true };
    unsigned int pin;

    ia64_iosapic_core_init(&s, record_delivery, &log);

    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_REG_VERSION), ==,
                    IA64_IOSAPIC_VERSION |
                    ((IA64_IOSAPIC_NUM_PINS - 1) << 16));
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_REG_ID), ==, 0);

    for (pin = 0; pin < IA64_IOSAPIC_NUM_PINS; pin++) {
        g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin)), ==,
                        IA64_IOSAPIC_MASKED);
        g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_HIGH(pin)), ==, 0);
    }

    s.irr = UINT64_MAX;
    s.pin_level = UINT64_MAX;
    ia64_iosapic_core_reset(&s);
    g_assert_cmphex(s.irr, ==, 0);
    g_assert_cmphex(s.pin_level, ==, 0);
    g_assert_true(s.deliver == record_delivery);
    g_assert_true(s.deliver_opaque == &log);
}

static void test_edge_rising_and_masking(void)
{
    IA64IOSAPICCore s;
    DeliveryLog log = { .accept = true };
    const unsigned int pin = 16;

    ia64_iosapic_core_init(&s, record_delivery, &log);
    program_rte(&s, pin, 0x40, IA64_IOSAPIC_DELIVERY_FIXED,
                false, false, 0);

    ia64_iosapic_core_set_irq(&s, pin, 1);
    g_assert_cmpuint(log.count, ==, 1);
    g_assert_cmphex(log.vector, ==, 0x40);
    g_assert_cmphex(s.irr & BIT64(pin), ==, 0);

    /* A level that remains asserted is not another edge. */
    ia64_iosapic_core_set_irq(&s, pin, 1);
    g_assert_cmpuint(log.count, ==, 1);

    ia64_iosapic_core_set_irq(&s, pin, 0);
    ia64_iosapic_core_set_irq(&s, pin, 1);
    g_assert_cmpuint(log.count, ==, 2);

    /* Masked edge requests are lost and are not replayed on unmask. */
    ia64_iosapic_core_set_irq(&s, pin, 0);
    program_rte(&s, pin, 0x40, IA64_IOSAPIC_DELIVERY_FIXED,
                false, true, 0);
    ia64_iosapic_core_set_irq(&s, pin, 1);
    g_assert_cmpuint(log.count, ==, 2);
    program_rte(&s, pin, 0x40, IA64_IOSAPIC_DELIVERY_FIXED,
                false, false, 0);
    g_assert_cmpuint(log.count, ==, 2);

    ia64_iosapic_core_set_irq(&s, pin, 0);
    ia64_iosapic_core_set_irq(&s, pin, 1);
    g_assert_cmpuint(log.count, ==, 3);
}

static void test_level_eoi_and_deassert(void)
{
    IA64IOSAPICCore s;
    DeliveryLog log = { .accept = true };
    const unsigned int pin = 5;

    ia64_iosapic_core_init(&s, record_delivery, &log);
    program_rte(&s, pin, 0x50, IA64_IOSAPIC_DELIVERY_FIXED,
                true, false, 0);

    ia64_iosapic_core_set_irq(&s, pin, 1);
    g_assert_cmpuint(log.count, ==, 1);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin)) &
                    IA64_IOSAPIC_REMOTE_IRR, !=, 0);

    ia64_iosapic_core_set_irq(&s, pin, 1);
    g_assert_cmpuint(log.count, ==, 1);

    ia64_iosapic_core_write(&s, IA64_IOSAPIC_EOI, 0x50);
    g_assert_cmpuint(log.count, ==, 2);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin)) &
                    IA64_IOSAPIC_REMOTE_IRR, !=, 0);

    ia64_iosapic_core_set_irq(&s, pin, 0);
    ia64_iosapic_core_write(&s, IA64_IOSAPIC_EOI, 0x50);
    g_assert_cmpuint(log.count, ==, 2);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin)) &
                    IA64_IOSAPIC_REMOTE_IRR, ==, 0);
    g_assert_cmphex(s.irr & BIT64(pin), ==, 0);
}

static void test_shared_vector_eoi(void)
{
    IA64IOSAPICCore s;
    DeliveryLog log = { .accept = true };
    const unsigned int pin_a = 1;
    const unsigned int pin_b = 2;

    ia64_iosapic_core_init(&s, record_delivery, &log);
    program_rte(&s, pin_a, 0x60, IA64_IOSAPIC_DELIVERY_LOWEST,
                true, false, 0);
    program_rte(&s, pin_b, 0x60, IA64_IOSAPIC_DELIVERY_LOWEST,
                true, false, 0);

    ia64_iosapic_core_set_irq(&s, pin_a, 1);
    ia64_iosapic_core_set_irq(&s, pin_b, 1);
    g_assert_cmpuint(log.count, ==, 2);

    ia64_iosapic_core_write(&s, IA64_IOSAPIC_EOI, 0x60);
    g_assert_cmpuint(log.count, ==, 4);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin_a)) &
                    IA64_IOSAPIC_REMOTE_IRR, !=, 0);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin_b)) &
                    IA64_IOSAPIC_REMOTE_IRR, !=, 0);

    ia64_iosapic_core_set_irq(&s, pin_a, 0);
    ia64_iosapic_core_set_irq(&s, pin_b, 0);
    ia64_iosapic_core_write(&s, IA64_IOSAPIC_EOI, 0x60);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin_a)) &
                    IA64_IOSAPIC_REMOTE_IRR, ==, 0);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin_b)) &
                    IA64_IOSAPIC_REMOTE_IRR, ==, 0);
}

static void test_rte_read_only_bits_and_reprogram(void)
{
    IA64IOSAPICCore s;
    DeliveryLog log = { .accept = true };
    const unsigned int pin = 7;
    uint32_t low;

    ia64_iosapic_core_init(&s, record_delivery, &log);
    program_rte(&s, pin, 0x70, IA64_IOSAPIC_DELIVERY_FIXED,
                true, false, 0);
    ia64_iosapic_core_set_irq(&s, pin, 1);

    /* A guest write cannot clear the hardware-owned Remote IRR bit. */
    low = read_register(&s, IA64_IOSAPIC_RTE_LOW(pin));
    write_register(&s, IA64_IOSAPIC_RTE_LOW(pin),
                   low & ~IA64_IOSAPIC_REMOTE_IRR);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin)) &
                    IA64_IOSAPIC_REMOTE_IRR, !=, 0);

    /* Switching the entry to edge mode architecturally retires Remote IRR. */
    write_register(&s, IA64_IOSAPIC_RTE_LOW(pin),
                   (low & ~IA64_IOSAPIC_TRIGGER_LEVEL) |
                   IA64_IOSAPIC_MASKED);
    g_assert_cmphex(read_register(&s, IA64_IOSAPIC_RTE_LOW(pin)) &
                    IA64_IOSAPIC_REMOTE_IRR, ==, 0);
}

static void test_rejected_delivery_remains_pending(void)
{
    IA64IOSAPICCore s;
    DeliveryLog log = { .accept = false };
    const unsigned int pin = 31;

    ia64_iosapic_core_init(&s, record_delivery, &log);
    program_rte(&s, pin, 0x80, IA64_IOSAPIC_DELIVERY_NMI,
                false, false, 0x1234);

    ia64_iosapic_core_set_irq(&s, pin, 1);
    g_assert_cmpuint(log.count, ==, 1);
    g_assert_cmphex(log.vector, ==, 0x80);
    g_assert_cmphex(log.destination, ==, 0x1234);
    g_assert_cmpuint(log.mode, ==, IA64_IOSAPIC_DELIVERY_NMI);
    g_assert_cmphex(s.irr & BIT64(pin), !=, 0);

    log.accept = true;
    write_register(&s, IA64_IOSAPIC_RTE_LOW(pin),
                   0x80 | (IA64_IOSAPIC_DELIVERY_FIXED <<
                           IA64_IOSAPIC_DELIVERY_SHIFT));
    g_assert_cmpuint(log.count, ==, 2);
    g_assert_cmphex(s.irr & BIT64(pin), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ia64/iosapic/reset-registers",
                    test_reset_and_registers);
    g_test_add_func("/ia64/iosapic/edge-rising-masking",
                    test_edge_rising_and_masking);
    g_test_add_func("/ia64/iosapic/level-eoi",
                    test_level_eoi_and_deassert);
    g_test_add_func("/ia64/iosapic/shared-vector-eoi",
                    test_shared_vector_eoi);
    g_test_add_func("/ia64/iosapic/rte-read-only",
                    test_rte_read_only_bits_and_reprogram);
    g_test_add_func("/ia64/iosapic/rejected-pending",
                    test_rejected_delivery_remains_pending);

    return g_test_run();
}
