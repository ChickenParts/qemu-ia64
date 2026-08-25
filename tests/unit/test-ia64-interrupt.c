/*
 * IA-64 local SAPIC interrupt arbitration tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "target/ia64/interrupt.h"

static void test_empty_is_spurious(void)
{
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };

    g_assert_cmpint(ia64_interrupt_next(pending, in_service, 0), ==, -1);
    g_assert_cmpuint(ia64_interrupt_accept(pending, in_service, 0), ==,
                     IA64_INTERRUPT_SPURIOUS_VECTOR);
}

static void test_deposit_range(void)
{
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };

    g_assert_false(ia64_interrupt_deposit(pending, 0));
    g_assert_false(ia64_interrupt_deposit(pending, 15));
    g_assert_false(ia64_interrupt_deposit(pending, 256));
    g_assert_true(ia64_interrupt_deposit(pending, 16));
    g_assert_true(ia64_interrupt_deposit(pending, 255));
    g_assert_cmpint(ia64_interrupt_highest(pending), ==, 255);
}

static void test_highest_priority_accept(void)
{
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };

    ia64_interrupt_deposit(pending, 0x31);
    ia64_interrupt_deposit(pending, 0xd2);
    ia64_interrupt_deposit(pending, 0x80);

    g_assert_cmpuint(ia64_interrupt_accept(pending, in_service, 0), ==,
                     0xd2);
    g_assert_cmpint(ia64_interrupt_highest(pending), ==, 0x80);
    g_assert_cmpint(ia64_interrupt_highest(in_service), ==, 0xd2);
}

static void test_tpr_masks(void)
{
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };

    ia64_interrupt_deposit(pending, 0x7f);
    g_assert_cmpint(ia64_interrupt_next(pending, in_service,
                                        IA64_TPR_MMI), ==, -1);
    g_assert_cmpint(ia64_interrupt_next(pending, in_service,
                                        7ULL << IA64_TPR_MIC_SHIFT), ==, -1);
    g_assert_cmpint(ia64_interrupt_next(pending, in_service,
                                        6ULL << IA64_TPR_MIC_SHIFT), ==,
                    0x7f);
}

static void test_tpr_reserved_bits_are_zero(void)
{
    uint64_t value = UINT64_MAX;

    g_assert_cmphex(ia64_interrupt_sanitize_tpr(value), ==,
                    IA64_TPR_WRITABLE_MASK);
    g_assert_cmphex(ia64_interrupt_sanitize_tpr(IA64_TPR_RESET), ==,
                    IA64_TPR_RESET);
}

static void test_in_service_priority(void)
{
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };

    ia64_interrupt_deposit(in_service, 0x70);
    ia64_interrupt_deposit(pending, 0x6f);
    g_assert_cmpint(ia64_interrupt_next(pending, in_service, 0), ==, -1);

    ia64_interrupt_deposit(pending, 0x71);
    g_assert_cmpint(ia64_interrupt_next(pending, in_service, 0), ==, 0x71);
}

static void test_nested_accept_and_eoi(void)
{
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };

    ia64_interrupt_deposit(pending, 0x40);
    g_assert_cmpuint(ia64_interrupt_accept(pending, in_service, 0), ==,
                     0x40);

    ia64_interrupt_deposit(pending, 0x90);
    g_assert_cmpuint(ia64_interrupt_accept(pending, in_service, 0), ==,
                     0x90);
    g_assert_cmpint(ia64_interrupt_eoi(in_service), ==, 0x90);
    g_assert_cmpint(ia64_interrupt_highest(in_service), ==, 0x40);
    g_assert_cmpint(ia64_interrupt_eoi(in_service), ==, 0x40);
    g_assert_cmpint(ia64_interrupt_eoi(in_service), ==, -1);
}

static void test_reserved_bitmap_bits_ignored(void)
{
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS] = { 0 };

    pending[0] = (1ULL << 2) | (1ULL << IA64_INTERRUPT_SPURIOUS_VECTOR);
    g_assert_cmpint(ia64_interrupt_highest(pending), ==, -1);
    g_assert_cmpuint(ia64_interrupt_accept(pending, in_service, 0), ==,
                     IA64_INTERRUPT_SPURIOUS_VECTOR);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ia64/interrupt/empty-is-spurious",
                    test_empty_is_spurious);
    g_test_add_func("/ia64/interrupt/deposit-range", test_deposit_range);
    g_test_add_func("/ia64/interrupt/highest-priority-accept",
                    test_highest_priority_accept);
    g_test_add_func("/ia64/interrupt/tpr-masks", test_tpr_masks);
    g_test_add_func("/ia64/interrupt/tpr-reserved-bits",
                    test_tpr_reserved_bits_are_zero);
    g_test_add_func("/ia64/interrupt/in-service-priority",
                    test_in_service_priority);
    g_test_add_func("/ia64/interrupt/nested-accept-and-eoi",
                    test_nested_accept_and_eoi);
    g_test_add_func("/ia64/interrupt/reserved-bitmap-bits",
                    test_reserved_bitmap_bits_ignored);

    return g_test_run();
}
