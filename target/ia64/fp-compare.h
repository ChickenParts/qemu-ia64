/*
 * IA-64 F4 floating-point compare operations.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel Itanium SDM Vol. 3 rev. 2.3, fcmp operation.
 * Evaluation uses architectural register-format bit patterns only; host
 * floating point is deliberately excluded from this path.
 */
#ifndef TARGET_IA64_FP_COMPARE_H
#define TARGET_IA64_FP_COMPARE_H

#include "fp-f8.h"

enum {
    IA64_FCMP_INVALID = -1,
    IA64_FCMP_EQ = 0,
    IA64_FCMP_LT = 1,
    IA64_FCMP_LE = 2,
    IA64_FCMP_UNORD = 3,
    IA64_FCMP_V = 1,
    IA64_FCMP_D = 2,
};

typedef struct IA64FCmpResult {
    uint8_t predicates;
    uint32_t flags;
    uint16_t fault_code;
} IA64FCmpResult;

static inline int ia64_fcmp_decode(uint64_t insn)
{
    if (((insn >> 37) & 0xf) != 4) {
        return IA64_FCMP_INVALID;
    }
    return (int)((((insn >> 33) & 1) << 1) | ((insn >> 36) & 1));
}

static inline IA64FCmpResult ia64_fcmp_result(int rel, IA64FRBits a,
                                              IA64FRBits b, uint64_t fpsr,
                                              unsigned sf, bool qual)
{
    IA64FCmpResult result = { 0 };
    IA64F8ExtClass ca, cb;
    unsigned controls, disabled;
    bool unordered, invalid_event;
    bool relation = false;
    int cmp = 0;

    if (!qual || ia64_f9_is_natval(a) || ia64_f9_is_natval(b)) {
        return result;
    }

    ca = ia64_f8_ext_class(a);
    cb = ia64_f8_ext_class(b);
    unordered = ca.invalid || cb.invalid;

    /*
     * Unsupported register-format values and signaling NaNs raise V for every
     * relation. Quiet NaNs raise V only for ordered LT/LE comparisons.
     * EQ and UNORD are quiet comparisons for qNaN.
     */
    invalid_event =
        (ca.invalid && !ca.qnan) || (cb.invalid && !cb.qnan) ||
        ((rel == IA64_FCMP_LT || rel == IA64_FCMP_LE) &&
         (ca.qnan || cb.qnan));
    if (invalid_event) {
        result.flags |= IA64_FCMP_V;
    }

    if (!unordered) {
        if (ca.unnormal || cb.unnormal) {
            result.flags |= IA64_FCMP_D;
        }
        cmp = ia64_f8_ext_cmp(a, b, ca, cb, false);
    }

    switch (rel) {
    case IA64_FCMP_EQ:
        relation = !unordered && cmp == 0;
        break;
    case IA64_FCMP_LT:
        relation = !unordered && cmp < 0;
        break;
    case IA64_FCMP_LE:
        relation = !unordered && cmp <= 0;
        break;
    case IA64_FCMP_UNORD:
        relation = unordered;
        break;
    default:
        __builtin_trap();
    }

    result.predicates = relation ? 1 : 2;
    controls = (fpsr >> (6 + 13 * (sf & 3))) & 0x7f;
    disabled = (sf != 0 && (controls & 0x40)) ? 0x3f :
               (unsigned)(fpsr & 0x3f);
    result.fault_code =
        result.flags & (IA64_FCMP_V | IA64_FCMP_D) & ~disabled;
    return result;
}

#endif /* TARGET_IA64_FP_COMPARE_H */
