/*
 * IA-64 Processor Abstraction Layer contract tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "target/ia64/pal.h"

static void test_unimplemented_result(void)
{
    IA64PALResult result = ia64_pal_result_unimplemented();

    g_assert_cmpint(result.status, ==,
                    IA64_PAL_STATUS_UNIMPLEMENTED);
    g_assert_cmphex(result.v0, ==, 0);
    g_assert_cmphex(result.v1, ==, 0);
    g_assert_cmphex(result.v2, ==, 0);
    g_assert_cmpint(result.action, ==, IA64_PAL_ACTION_RETURN);
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ia64/pal/unimplemented-result",
                    test_unimplemented_result);
    g_test_add_func("/ia64/pal/halt-result", test_halt_result);
    return g_test_run();
}
