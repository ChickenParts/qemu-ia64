/*
 * IA-64 F10 floating-to-integer data paths and FPSR event classification.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel SDM Vol. 3 rev. 2.3: fcvt.fx/fpcvt.fx and format F10.
 * Vol. 1: register encodings and FPSR (chapter 5).
 * No host floating point, target-width casts, or out-of-range shifts.
 * Event delivery is deliberately separate from numerical evaluation.
 */
#ifndef TARGET_IA64_FP_CONVERT_H
#define TARGET_IA64_FP_CONVERT_H

#include "fp-bitops.h"

enum {
    IA64_F10_INVALID = -1,
    IA64_F10_UNSIGNED = 1,
    IA64_F10_TRUNCATE = 2,
    IA64_F10_PARALLEL = 4,
    IA64_F10_V = 1,
    IA64_F10_D = 2,
    IA64_F10_I = 32,
};

typedef struct IA64IntConversion {
    uint64_t value;
    uint32_t flags;
    bool rounded_up;
} IA64IntConversion;

typedef struct IA64F10Result {
    IA64FRBits value;
    uint32_t flags;
    uint32_t enabled;
    uint16_t fault_code;
    uint16_t trap_code;
} IA64F10Result;

static inline int ia64_f10_decode(uint64_t insn)
{
    unsigned major = (insn >> 37) & 15;
    unsigned x6 = (insn >> 27) & 63;

    if (major > 1 || ((insn >> 33) & 1) || x6 < 0x18 || x6 > 0x1b) {
        return IA64_F10_INVALID;
    }
    return (major << 2) | (x6 & 3);
}

/* Round |significand * 2^shift|, then check the destination integer range. */
static inline IA64IntConversion ia64_f10_integer(uint64_t significand,
                                                int shift, bool negative,
                                                unsigned width,
                                                bool unsigned_result,
                                                unsigned rounding,
                                                bool unnormal)
{
    uint64_t magnitude, remainder = 0;
    uint64_t indefinite = UINT64_C(1) << (width - 1);
    uint64_t limit = unsigned_result ? (width == 64 ? UINT64_MAX : UINT32_MAX) :
                     (negative ? indefinite : indefinite - 1);
    int half_compare = -1;
    bool increment;
    IA64IntConversion invalid = { indefinite, IA64_F10_V, false };

    if (!significand) {
        return (IA64IntConversion) { 0, unnormal ? IA64_F10_D : 0, false };
    }
    if (shift >= 0) {
        if (shift >= (int)width || significand > (limit >> shift)) {
            return invalid;
        }
        magnitude = significand << shift;
    } else {
        unsigned distance = -shift;

        if (distance > 64) {
            magnitude = 0;
            remainder = significand;
        } else if (distance == 64) {
            magnitude = 0;
            remainder = significand;
            half_compare = significand > (UINT64_C(1) << 63) ? 1 :
                           significand == (UINT64_C(1) << 63) ? 0 : -1;
        } else {
            uint64_t half = UINT64_C(1) << (distance - 1);

            magnitude = significand >> distance;
            remainder = significand & ((UINT64_C(1) << distance) - 1);
            half_compare = remainder > half ? 1 : remainder == half ? 0 : -1;
        }
        increment = remainder &&
                    ((rounding == 0 && (half_compare > 0 ||
                                         (half_compare == 0 && (magnitude & 1)))) ||
                     (rounding == 1 && negative) ||
                     (rounding == 2 && !negative));
        if (magnitude > limit || (increment && magnitude == limit)) {
            return invalid;
        }
        magnitude += increment;
    }
    /* A negative value rounded to zero is valid even for unsigned forms. */
    if (unsigned_result && negative && magnitude) {
        return invalid;
    }
    return (IA64IntConversion) {
        (negative ? UINT64_C(0) - magnitude : magnitude) &
            (width == 64 ? UINT64_MAX : UINT32_MAX),
        (unnormal ? IA64_F10_D : 0) | (remainder ? IA64_F10_I : 0),
        increment
    };
}

static inline IA64IntConversion ia64_f10_scalar(IA64FRBits source,
                                               bool unsigned_result,
                                               unsigned rounding)
{
    unsigned exponent = source.sign_exp & 0x1ffff;
    bool negative = (source.sign_exp & 0x20000) != 0;
    bool unnormal = (exponent == 0 && source.significand != 0) ||
                    (exponent != 0 && !(source.significand >> 63));

    /* Every NaN/infinity encoding is invalid for integer conversion. */
    if (exponent == 0x1ffff) {
        return (IA64IntConversion) { UINT64_C(1) << 63, IA64_F10_V, false };
    }
    /* exp=0 nonzero inputs are double-extended (pseudo-)denormals. */
    if (exponent == 0) {
        exponent = 0xc001;
    }
    return ia64_f10_integer(source.significand, (int)exponent - 0xffff - 63,
                           negative, 64, unsigned_result, rounding, unnormal);
}

static inline IA64IntConversion ia64_f10_lane(uint32_t source,
                                             bool unsigned_result,
                                             unsigned rounding)
{
    unsigned exponent = (source >> 23) & 255;
    uint64_t significand = source & 0x7fffff;
    bool negative = (source >> 31) != 0;
    bool unnormal = exponent == 0 && significand != 0;

    if (exponent == 255) {
        return (IA64IntConversion) { UINT64_C(1) << 31, IA64_F10_V, false };
    }
    if (exponent) {
        significand |= UINT64_C(1) << 23;
    } else {
        exponent = 1;
    }
    return ia64_f10_integer(significand, (int)exponent - 127 - 23,
                           negative, 32, unsigned_result, rounding, unnormal);
}

static inline IA64F10Result ia64_f10_result(int op, IA64FRBits source,
                                           uint64_t fpsr, unsigned sf)
{
    IA64F10Result result = { .value = { 0, 0x1003e } };
    unsigned controls = (fpsr >> (6 + 13 * sf)) & 127;
    unsigned rounding = op & IA64_F10_TRUNCATE ? 3 : (controls >> 4) & 3;
    unsigned disabled = (sf != 0 && (controls & 64)) ? 63 : fpsr & 63;
    IA64IntConversion lo, hi;

    if (ia64_f9_is_natval(source)) {
        result.value = (IA64FRBits) { 0, 0x1fffe };
        return result;
    }
    if (op & IA64_F10_PARALLEL) {
        lo = ia64_f10_lane(source.significand, op & IA64_F10_UNSIGNED, rounding);
        hi = ia64_f10_lane(source.significand >> 32,
                          op & IA64_F10_UNSIGNED, rounding);
        result.value.significand = (hi.value << 32) | lo.value;
        result.flags = hi.flags | lo.flags;
        result.fault_code = (hi.flags & (IA64_F10_V | IA64_F10_D) & ~disabled) |
                            ((lo.flags & (IA64_F10_V | IA64_F10_D) & ~disabled) << 4);
        if ((hi.flags & IA64_F10_I) & ~disabled) {
            result.trap_code |= 1u << 13;
            if (hi.rounded_up) {
                result.trap_code |= 1u << 14;
            }
        }
        if ((lo.flags & IA64_F10_I) & ~disabled) {
            result.trap_code |= 1u << 9;
            if (lo.rounded_up) {
                result.trap_code |= 1u << 10;
            }
        }
    } else {
        lo = ia64_f10_scalar(source, op & IA64_F10_UNSIGNED, rounding);
        result.value.significand = lo.value;
        result.flags = lo.flags;
        result.fault_code = lo.flags & (IA64_F10_V | IA64_F10_D) & ~disabled;
        if ((lo.flags & IA64_F10_I) & ~disabled) {
            result.trap_code |= 1u << 13;
            if (lo.rounded_up) {
                result.trap_code |= 1u << 14;
            }
        }
    }
    result.enabled = result.flags & ~disabled;
    if (result.trap_code) {
        result.trap_code |= 1u; /* ISR.code.fp: floating-point exception trap. */
    }
    return result;
}

#endif /* TARGET_IA64_FP_CONVERT_H */
