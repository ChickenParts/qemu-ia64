/*
 * IA-64 instruction decode helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "decode.h"

IA64MUnitIndexedRegister ia64_munit_decode_indexed_read(uint64_t insn)
{
    uint8_t major = (insn >> 37) & 0xf;
    uint8_t x3 = (insn >> 33) & 0x7;
    uint8_t x6 = (insn >> 27) & 0x3f;

    if (major != 0x1 || x3 != 0) {
        return IA64_MUNIT_INDEXED_NONE;
    }

    switch (x6) {
    case 0x10:
        return IA64_MUNIT_INDEXED_RR;
    case 0x11:
        return IA64_MUNIT_INDEXED_DBR;
    case 0x12:
        return IA64_MUNIT_INDEXED_IBR;
    case 0x13:
        return IA64_MUNIT_INDEXED_PKR;
    case 0x14:
        return IA64_MUNIT_INDEXED_PMC;
    case 0x15:
        return IA64_MUNIT_INDEXED_PMD;
    case 0x16:
        return IA64_MUNIT_INDEXED_MSR;
    case 0x17:
        return IA64_MUNIT_INDEXED_CPUID;
    default:
        return IA64_MUNIT_INDEXED_NONE;
    }
}
