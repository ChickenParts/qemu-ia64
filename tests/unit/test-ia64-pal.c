/*
 * IA-64 Processor Abstraction Layer contract tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "target/ia64/pal.h"

static uint64_t field(uint64_t value, unsigned int shift, uint64_t mask)
{
    return (value >> shift) & mask;
}

static void assert_return_action(IA64PALResult result)
{
    g_assert_cmpint(result.action, ==, IA64_PAL_ACTION_RETURN);
}

static void test_unimplemented_result(void)
{
    IA64PALResult result = ia64_pal_result_unimplemented();

    g_assert_cmpint(result.status, ==,
                    IA64_PAL_STATUS_UNIMPLEMENTED);
    g_assert_cmphex(result.v0, ==, 0);
    g_assert_cmphex(result.v1, ==, 0);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);
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
                              bool unified, uint64_t size)
{
    IA64PALResult result = ia64_pal_result_cache_info(level, type);

    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmpuint(field(result.v0, 0, 0x1), ==, unified);
    g_assert_cmpuint(field(result.v0, 1, 0x3), ==, 1);
    g_assert_cmpuint(field(result.v0, 8, 0xff), ==, 8);
    g_assert_cmpuint(field(result.v0, 16, 0xff), ==, 6);
    g_assert_cmpuint(field(result.v0, 24, 0xff), ==, 6);
    g_assert_cmpuint(result.v1, ==, size);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);
}

static void test_cache_info(void)
{
    assert_cache_info(0, IA64_PAL_CACHE_TYPE_INSTRUCTION,
                      false, 32 * KiB);
    assert_cache_info(0, IA64_PAL_CACHE_TYPE_DATA,
                      false, 64 * KiB);
    assert_cache_info(1, IA64_PAL_CACHE_TYPE_INSTRUCTION,
                      false, 256 * KiB);
    assert_cache_info(1, IA64_PAL_CACHE_TYPE_DATA,
                      false, 256 * KiB);
    assert_cache_info(2, IA64_PAL_CACHE_TYPE_INSTRUCTION,
                      true, 1 * MiB);
    assert_cache_info(2, IA64_PAL_CACHE_TYPE_DATA,
                      true, 1 * MiB);
}

static void test_cache_info_invalid(void)
{
    IA64PALResult result;

    result = ia64_pal_result_cache_info(
        IA64_PAL_CACHE_LEVELS, IA64_PAL_CACHE_TYPE_DATA);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_cache_info(0, 0);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_cache_info(0, 3);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
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
    IA64PALResult result;

    result = ia64_pal_result_vm_summary(0, 16, 16);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_vm_summary(16, 0, 16);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_vm_summary(16, 16, 0);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_vm_summary(257, 16, 16);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
}

static void assert_vm_info(uint64_t type)
{
    IA64PALResult result = ia64_pal_result_vm_info(0, type, 128);

    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_SUCCESS);
    g_assert_cmpuint(field(result.v0, 0, 0xff), ==, 1);
    g_assert_cmpuint(field(result.v0, 8, 0xff), ==, 128);
    g_assert_cmpuint(field(result.v0, 16, 0xffff), ==, 128);
    g_assert_cmpuint(field(result.v0, 32, 0x1), ==, 0);
    g_assert_cmpuint(field(result.v0, 33, 0x1), ==, 0);
    g_assert_cmpuint(field(result.v0, 34, 0x1), ==, 0);
    g_assert_cmphex(result.v1, ==, IA64_PAL_VM_PAGE_SIZES);
    g_assert_cmphex(result.v2, ==, 0);
    assert_return_action(result);
}

static void test_vm_info(void)
{
    assert_vm_info(IA64_PAL_TC_TYPE_INSTRUCTION);
    assert_vm_info(IA64_PAL_TC_TYPE_DATA);
}

static void test_vm_info_invalid(void)
{
    IA64PALResult result;

    result = ia64_pal_result_vm_info(1, IA64_PAL_TC_TYPE_DATA, 128);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_vm_info(0, 0, 128);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_vm_info(0, IA64_PAL_TC_TYPE_DATA, 0);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_vm_info(0, IA64_PAL_TC_TYPE_DATA, 256);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
}

static void test_proc_get_features(void)
{
    IA64PALResult result;

    result = ia64_pal_result_proc_get_features(0);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
    result = ia64_pal_result_proc_get_features(16);
    g_assert_cmpint(result.status, ==, IA64_PAL_STATUS_EINVAL);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ia64/pal/unimplemented-result",
                    test_unimplemented_result);
    g_test_add_func("/ia64/pal/halt-result", test_halt_result);
    g_test_add_func("/ia64/pal/cache-summary", test_cache_summary);
    g_test_add_func("/ia64/pal/cache-info", test_cache_info);
    g_test_add_func("/ia64/pal/cache-info-invalid",
                    test_cache_info_invalid);
    g_test_add_func("/ia64/pal/vm-summary", test_vm_summary);
    g_test_add_func("/ia64/pal/vm-summary-invalid",
                    test_vm_summary_invalid);
    g_test_add_func("/ia64/pal/vm-info", test_vm_info);
    g_test_add_func("/ia64/pal/vm-info-invalid", test_vm_info_invalid);
    g_test_add_func("/ia64/pal/proc-get-features",
                    test_proc_get_features);
    return g_test_run();
}
