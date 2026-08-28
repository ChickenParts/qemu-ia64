/*
 * IA-64 System Abstraction Layer contract tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "target/ia64/sal.h"

static void assert_frequency(uint64_t selector,
                             uint64_t expected)
{
    IA64SALResult result = ia64_sal_result_freq_base(selector);

    g_assert_cmpint(result.status, ==, IA64_SAL_STATUS_SUCCESS);
    g_assert_cmpuint(result.v0, ==, expected);
    g_assert_cmphex(result.v1, ==, IA64_SAL_NO_DRIFT_INFO);
    g_assert_cmphex(result.v2, ==, 0);
}

static void test_freq_base(void)
{
    assert_frequency(IA64_SAL_FREQ_BASE_PLATFORM,
                     IA64_PLATFORM_BASE_HZ);
    assert_frequency(IA64_SAL_FREQ_BASE_INTERVAL_TIMER,
                     IA64_ITC_HZ);
    assert_frequency(IA64_SAL_FREQ_BASE_REALTIME_CLOCK,
                     IA64_RTC_HZ);
}

static void test_freq_base_invalid(void)
{
    IA64SALResult result = ia64_sal_result_freq_base(3);

    g_assert_cmpint(result.status, ==, IA64_SAL_STATUS_EINVAL);
    g_assert_cmphex(result.v0, ==, 0);
    g_assert_cmphex(result.v1, ==, 0);
    g_assert_cmphex(result.v2, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ia64/sal/freq-base", test_freq_base);
    g_test_add_func("/ia64/sal/freq-base-invalid",
                    test_freq_base_invalid);
    return g_test_run();
}
