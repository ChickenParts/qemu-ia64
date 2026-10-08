/*
 * IA-64 instruction decode tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "target/ia64/decode.h"

static uint64_t munit_indexed_read(uint8_t x6)
{
    return (UINT64_C(0x1) << 37) | ((uint64_t)x6 << 27);
}

static void test_indexed_read_table(void)
{
    static const struct {
        uint8_t x6;
        IA64MUnitIndexedRegister reg;
    } cases[] = {
        { 0x10, IA64_MUNIT_INDEXED_RR },
        { 0x11, IA64_MUNIT_INDEXED_DBR },
        { 0x12, IA64_MUNIT_INDEXED_IBR },
        { 0x13, IA64_MUNIT_INDEXED_PKR },
        { 0x14, IA64_MUNIT_INDEXED_PMC },
        { 0x15, IA64_MUNIT_INDEXED_PMD },
        { 0x16, IA64_MUNIT_INDEXED_MSR },
        { 0x17, IA64_MUNIT_INDEXED_CPUID },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_cmpint(ia64_munit_decode_indexed_read(
                            munit_indexed_read(cases[i].x6)),
                        ==, cases[i].reg);
    }
}

static void test_firmware_dbr_read(void)
{
    /* Xen/IPF DXE: mov r5 = dbr[r3]. */
    uint64_t insn = UINT64_C(0x02088300140);

    g_assert_cmpint(ia64_munit_decode_indexed_read(insn), ==,
                    IA64_MUNIT_INDEXED_DBR);
    g_assert_cmpuint((insn >> 27) & 0x3f, ==, 0x11);
    g_assert_cmpuint((insn >> 6) & 0x7f, ==, 5);
    g_assert_cmpuint((insn >> 20) & 0x7f, ==, 3);
}

static void test_non_read_extensions_are_rejected(void)
{
    /* Only 0x10..0x17 are indexed reads in this decode class. */
    for (uint8_t x6 = 0; x6 < 0x40; x6++) {
        if (x6 >= 0x10 && x6 <= 0x17) {
            continue;
        }
        g_assert_cmpint(ia64_munit_decode_indexed_read(
                            munit_indexed_read(x6)),
                        ==, IA64_MUNIT_INDEXED_NONE);
    }
}

static void test_translation_operations_are_not_indexed_reads(void)
{
    /* ptc.l, ptc.g, ptc.ga, ptr.d, ptr.i, itr.d, and itr.i. */
    for (uint8_t x6 = 0x09; x6 <= 0x0f; x6++) {
        g_assert_cmpint(ia64_munit_decode_indexed_read(
                            munit_indexed_read(x6)),
                        ==, IA64_MUNIT_INDEXED_NONE);
    }
}

static void test_wrong_major_or_extension_rejected(void)
{
    uint64_t dbr = munit_indexed_read(0x11);

    g_assert_cmpint(ia64_munit_decode_indexed_read(
                        dbr ^ (UINT64_C(0x3) << 37)),
                    ==, IA64_MUNIT_INDEXED_NONE);
    g_assert_cmpint(ia64_munit_decode_indexed_read(
                        dbr | (UINT64_C(0x1) << 33)),
                    ==, IA64_MUNIT_INDEXED_NONE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ia64/decode/indexed-read-table",
                    test_indexed_read_table);
    g_test_add_func("/ia64/decode/firmware-dbr-read",
                    test_firmware_dbr_read);
    g_test_add_func("/ia64/decode/non-read-extensions",
                    test_non_read_extensions_are_rejected);
    g_test_add_func("/ia64/decode/translation-ops-not-indexed-reads",
                    test_translation_operations_are_not_indexed_reads);
    g_test_add_func("/ia64/decode/wrong-major-or-extension",
                    test_wrong_major_or_extension_rejected);

    return g_test_run();
}
