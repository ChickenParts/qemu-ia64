/*
 * IA-64 instruction decode helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef TARGET_IA64_DECODE_H
#define TARGET_IA64_DECODE_H

#include <stdint.h>

typedef enum IA64MUnitIndexedRegister {
    IA64_MUNIT_INDEXED_NONE = 0,
    IA64_MUNIT_INDEXED_RR,
    IA64_MUNIT_INDEXED_DBR,
    IA64_MUNIT_INDEXED_IBR,
    IA64_MUNIT_INDEXED_PKR,
    IA64_MUNIT_INDEXED_PMC,
    IA64_MUNIT_INDEXED_PMD,
    IA64_MUNIT_INDEXED_MSR,
    IA64_MUNIT_INDEXED_CPUID,
} IA64MUnitIndexedRegister;

IA64MUnitIndexedRegister ia64_munit_decode_indexed_read(uint64_t insn);

#endif /* TARGET_IA64_DECODE_H */
