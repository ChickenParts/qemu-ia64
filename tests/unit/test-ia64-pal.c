/*
 * IA-64 Processor Abstraction Layer contract tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "target/ia64/pal.h"

static uint64_t field(uint64_t value, unsigned int shift,
                      uint64_t mask)
{
    return (value >> shift) & mask;
}

static void assert_return_action(IA64PALResult result)
{
    g_assert_cmpint(result.action, ==, IA64_PAL_ACTION_RETURN);
}

static void assert_error_result(IA64PALResult result,
                                int64_t status)
{
    g_assert_cmpint(result.status, ==, status);
    g_assert_cmphex(result.v0, ==, 0);
    g_assert_cmphex(result.v1, ==, 0);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);
}

static void test_unimplemented_result(void)
{
    assert_error_result(ia64_pal_result_unimplemented(),
                        IA64_PAL_STATUS_UNIMPLEMENTED);
}

static void test_halt_result(void)
{
    IA64PALResult result = ia64_pal_result_halt();

    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmphex(result.v0, ==, 0);
    g_assert_cmphex(result.v1, ==, 0);
    g_assert_cmphex(result.v2, ==, 0);
    g_assert_cmpint(result.action, ==, IA64_PAL_ACTION_HALT);
}

static void test_cache_summary(void)
{
    IA64PALResult result = ia64_pal_result_cache_summary();

    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmpuint(result.v0, ==, IA64_PAL_CACHE_LEVELS);
    g_assert_cmpuint(result.v1, ==, IA64_PAL_UNIQUE_CACHES);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);
}

static void assert_cache_info(uint64_t level, uint64_t type,
                              bool unified, uint64_t size,
                              uint64_t attr, uint64_t assoc,
                              uint64_t line_size,
                              uint64_t tag_lsb,
                              uint64_t load_latency)
{
    IA64PALResult result = ia64_pal_result_cache_info(level, type);

    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmpuint(field(result.v0, 0, 0x1), ==, unified);
    g_assert_cmpuint(field(result.v0, 1, 0x3), ==, attr);
    g_assert_cmpuint(field(result.v0, 8, 0xff), ==, assoc);
    g_assert_cmpuint(field(result.v0, 16, 0xff), ==, line_size);
    g_assert_cmpuint(field(result.v0, 24, 0xff), ==, line_size);
    g_assert_cmpuint(field(result.v0, 40, 0xff), ==, load_latency);
    g_assert_cmpuint(field(result.v1, 0, UINT32_MAX), ==, size);
    g_assert_cmpuint(field(result.v1, 40, 0xff), ==, tag_lsb);
    g_assert_cmpuint(field(result.v1, 48, 0xff), ==, 43);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);
}

static void test_merced_cache_info(void)
{
    assert_cache_info(0, IA64_PAL_CACHE_TYPE_INSTRUCTION,
                      false, 16 * KiB, IA64_PAL_CACHE_ATTR_WT,
                      4, 5, 12, 1);
    assert_cache_info(0, IA64_PAL_CACHE_TYPE_DATA,
                      false, 16 * KiB, IA64_PAL_CACHE_ATTR_WT,
                      4, 5, 12, 2);
    assert_cache_info(1, IA64_PAL_CACHE_TYPE_DATA,
                      true, 96 * KiB, IA64_PAL_CACHE_ATTR_WB,
                      6, 6, 14, 6);
    assert_cache_info(2, IA64_PAL_CACHE_TYPE_DATA,
                      true, IA64_PAL_MERCED_L3_SIZE,
                      IA64_PAL_CACHE_ATTR_WB, 4, 6, 20, 21);
}

static void test_cache_info_invalid(void)
{
    assert_error_result(ia64_pal_result_cache_info(
                            IA64_PAL_CACHE_LEVELS,
                            IA64_PAL_CACHE_TYPE_DATA),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_cache_info(0, 0),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_cache_info(0, 3),
                        IA64_PAL_STATUS_EINVAL);
}

static void test_vm_summary(void)
{
    IA64PALResult result = ia64_pal_result_vm_summary(16, 16, 16);

    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmpuint(field(result.v0, 0, 0x1), ==, 1);
    g_assert_cmpuint(field(result.v0, 1, 0x7f), ==, 44);
    g_assert_cmpuint(field(result.v0, 8, 0xff), ==, 18);
    g_assert_cmpuint(field(result.v0, 16, 0xff), ==, 15);
    g_assert_cmpuint(field(result.v0, 24, 0xff), ==, 0);
    g_assert_cmpuint(field(result.v0, 32, 0xff), ==, 15);
    g_assert_cmpuint(field(result.v0, 40, 0xff), ==, 15);
    g_assert_cmpuint(field(result.v0, 48, 0xff), ==,
                     IA64_PAL_UNIQUE_TCS);
    g_assert_cmpuint(field(result.v0, 56, 0xff), ==,
                     IA64_PAL_TC_LEVELS);
    g_assert_cmpuint(field(result.v1, 0, 0xff), ==, 60);
    g_assert_cmpuint(field(result.v1, 8, 0xff), ==, 18);
    g_assert_cmpuint(field(result.v1, 16, 0xffff), ==, UINT16_MAX);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);
}

static void test_vm_summary_invalid(void)
{
    assert_error_result(ia64_pal_result_vm_summary(0, 16, 16),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_vm_summary(16, 0, 16),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_vm_summary(16, 16, 0),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_vm_summary(257, 16, 16),
                        IA64_PAL_STATUS_EINVAL);
}

static void test_vm_info(void)
{
    IA64PALResult result;

    result = ia64_pal_result_vm_info(
        0, IA64_PAL_CACHE_TYPE_INSTRUCTION, 128);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmpuint(field(result.v0, 0, 0xff), ==, 1);
    g_assert_cmpuint(field(result.v0, 8, 0xff), ==, 128);
    g_assert_cmpuint(field(result.v0, 16, 0xffff), ==, 128);
    g_assert_cmpuint(field(result.v0, 32, 0x7), ==, 0);
    g_assert_cmphex(result.v1, ==, IA64_PAL_VM_PAGE_SIZES);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);

    result = ia64_pal_result_vm_info(
        0, IA64_PAL_CACHE_TYPE_DATA, 128);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
}

static void test_vm_info_invalid(void)
{
    assert_error_result(ia64_pal_result_vm_info(
                            1, IA64_PAL_CACHE_TYPE_DATA, 128),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_vm_info(0, 0, 128),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_vm_info(
                            0, IA64_PAL_CACHE_TYPE_DATA, 0),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_vm_info(
                            0, IA64_PAL_CACHE_TYPE_DATA, 256),
                        IA64_PAL_STATUS_EINVAL);
}

static void test_vm_page_size(void)
{
    IA64PALResult result = ia64_pal_result_vm_page_size();

    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmphex(result.v0, ==, IA64_PAL_VM_PAGE_SIZES);
    g_assert_cmphex(result.v1, ==, IA64_PAL_VM_PAGE_SIZES);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);
}

static void test_proc_get_features(void)
{
    assert_error_result(ia64_pal_result_proc_get_features(0),
                        IA64_PAL_STATUS_EINVAL);
    assert_error_result(ia64_pal_result_proc_get_features(16),
                        IA64_PAL_STATUS_EINVAL);
}

static void assert_ratio(uint64_t value, uint32_t numerator,
                         uint32_t denominator)
{
    g_assert_cmpuint(value & UINT32_MAX, ==, denominator);
    g_assert_cmpuint(value >> 32, ==, numerator);
}

static void test_frequency_contract(void)
{
    IA64PALResult base = ia64_pal_result_freq_base();
    IA64PALResult ratios = ia64_pal_result_freq_ratios();

    g_assert_cmpint(base.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmpuint(base.v0, ==, IA64_PAL_PLATFORM_BASE_HZ);
    assert_return_action(base);

    g_assert_cmpint(ratios.status, ==, IA64_PAL_STATUS_SUCCESS);
    assert_ratio(ratios.v0, IA64_PAL_MERCED_PROC_RATIO_NUM,
                 IA64_PAL_MERCED_PROC_RATIO_DEN);
    assert_ratio(ratios.v1, IA64_PAL_MERCED_BUS_RATIO_NUM,
                 IA64_PAL_MERCED_BUS_RATIO_DEN);
    assert_ratio(ratios.v2, IA64_PAL_VIRTUAL_ITC_RATIO_NUM,
                 IA64_PAL_VIRTUAL_ITC_RATIO_DEN);
    g_assert_cmpuint(
        IA64_PAL_PLATFORM_BASE_HZ *
        IA64_PAL_MERCED_PROC_RATIO_NUM /
        IA64_PAL_MERCED_PROC_RATIO_DEN,
        ==, UINT64_C(800000000));
    g_assert_cmpuint(
        IA64_PAL_PLATFORM_BASE_HZ *
        IA64_PAL_VIRTUAL_ITC_RATIO_NUM /
        IA64_PAL_VIRTUAL_ITC_RATIO_DEN,
        ==, UINT64_C(1000000000));
    assert_return_action(ratios);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ia64/pal/unimplemented-result",
                    test_unimplemented_result);
    g_test_add_func("/ia64/pal/halt-result", test_halt_result);
    g_test_add_func("/ia64/pal/cache-summary", test_cache_summary);
    g_test_add_func("/ia64/pal/merced-cache-info",
                    test_merced_cache_info);
    g_test_add_func("/ia64/pal/cache-info-invalid",
                    test_cache_info_invalid);
    g_test_add_func("/ia64/pal/vm-summary", test_vm_summary);
    g_test_add_func("/ia64/pal/vm-summary-invalid",
                    test_vm_summary_invalid);
    g_test_add_func("/ia64/pal/vm-info", test_vm_info);
    g_test_add_func("/ia64/pal/vm-info-invalid",
                    test_vm_info_invalid);
    g_test_add_func("/ia64/pal/vm-page-size",
                    test_vm_page_size);
    g_test_add_func("/ia64/pal/proc-get-features",
                    test_proc_get_features);
    g_test_add_func("/ia64/pal/frequency-contract",
                    test_frequency_contract);
    return g_test_run();
}
