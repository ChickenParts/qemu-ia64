/*
 * IA-64 F8 min/max and packed-compare operations.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel SDM Vol. 3 rev. 2.3, F8 and individual operation definitions.
 * Evaluation uses architectural register/single bit patterns only; host
 * floating point is deliberately not part of this path.
 */
#ifndef TARGET_IA64_FP_F8_H
#define TARGET_IA64_FP_F8_H

#include "fp-bitops.h"

enum {
    IA64_F8_INVALID = -1,
    IA64_F8_MIN = 0x14,
    IA64_F8_MAX = 0x15,
    IA64_F8_AMIN = 0x16,
    IA64_F8_AMAX = 0x17,
    IA64_F8_PMIN = 0x54,
    IA64_F8_PMAX = 0x55,
    IA64_F8_PAMIN = 0x56,
    IA64_F8_PAMAX = 0x57,
    IA64_F8_PCMP_EQ = 0x70,
    IA64_F8_PCMP_LT = 0x71,
    IA64_F8_PCMP_LE = 0x72,
    IA64_F8_PCMP_UNORD = 0x73,
    IA64_F8_PCMP_NEQ = 0x74,
    IA64_F8_PCMP_NLT = 0x75,
    IA64_F8_PCMP_NLE = 0x76,
    IA64_F8_PCMP_ORD = 0x77,
    IA64_F8_V = 1,
    IA64_F8_D = 2,
};

typedef struct IA64F8Result {
    IA64FRBits value;
    uint32_t flags;
    uint16_t fault_code;
} IA64F8Result;

typedef struct IA64F8ExtClass {
    bool nat;
    bool invalid;
    bool qnan;
    bool snan;
    bool infinity;
    bool zero;
    bool unnormal;
    bool sign;
} IA64F8ExtClass;

static inline int ia64_f8_decode(uint64_t insn)
{
    unsigned major = (insn >> 37) & 15;
    unsigned x = (insn >> 33) & 1;
    unsigned x6 = (insn >> 27) & 63;

    if (x || major > 1) {
        return IA64_F8_INVALID;
    }
    if (x6 >= 0x14 && x6 <= 0x17) {
        return (int)((major << 6) | x6);
    }
    if (major == 1 && x6 >= 0x30 && x6 <= 0x37) {
        return (int)(0x40 | x6);
    }
    return IA64_F8_INVALID;
}

static inline IA64F8ExtClass ia64_f8_ext_class(IA64FRBits value)
{
    IA64F8ExtClass c = { 0 };
    uint64_t sig = value.significand;
    unsigned exp = value.sign_exp & 0x1ffff;
    bool integer_bit = (sig >> 63) != 0;

    c.sign = (value.sign_exp & 0x20000) != 0;
    if (ia64_f9_is_natval(value)) {
        c.nat = true;
        return c;
    }
    if (exp == 0x1ffff) {
        if (!integer_bit) {
            c.invalid = true; /* unsupported register-format value */
        } else if (sig == UINT64_C(0x8000000000000000)) {
            c.infinity = true;
        } else if (sig & UINT64_C(0x4000000000000000)) {
            c.qnan = true;
            c.invalid = true;
        } else {
            c.snan = true;
            c.invalid = true;
        }
        return c;
    }

    c.zero = sig == 0;
    c.unnormal = (sig == 0 && exp != 0) ||
                 (sig != 0 && (exp == 0 || !integer_bit));
    return c;
}

/*
 * Compare finite extended magnitudes exactly.  The explicit significand means
 * value ~= sig * 2^(exp-bias-63).  exp==0 uses the denormal exponent 1-bias.
 * Normalize only for comparison; the selected operand is returned unchanged.
 */
static inline int ia64_f8_ext_mag_cmp(IA64FRBits a, IA64FRBits b)
{
    uint64_t as = a.significand;
    uint64_t bs = b.significand;
    int ae, be;
    unsigned alz, blz;

    if (!as || !bs) {
        return as ? 1 : bs ? -1 : 0;
    }

    alz = __builtin_clzll(as);
    blz = __builtin_clzll(bs);
    ae = (int)((a.sign_exp & 0x1ffff) ?: 1) - (int)alz;
    be = (int)((b.sign_exp & 0x1ffff) ?: 1) - (int)blz;
    if (ae != be) {
        return ae < be ? -1 : 1;
    }
    as <<= alz;
    bs <<= blz;
    return as < bs ? -1 : as > bs ? 1 : 0;
}

static inline int ia64_f8_ext_cmp(IA64FRBits a, IA64FRBits b,
                                  IA64F8ExtClass ca, IA64F8ExtClass cb,
                                  bool absolute)
{
    int cmp;

    if (ca.infinity || cb.infinity) {
        if (ca.infinity && cb.infinity) {
            cmp = 0;
        } else {
            cmp = ca.infinity ? 1 : -1;
        }
    } else {
        cmp = ia64_f8_ext_mag_cmp(a, b);
    }
    if (absolute) {
        return cmp;
    }
    if (ca.zero && cb.zero) {
        return 0;
    }
    if (ca.sign != cb.sign) {
        return ca.sign ? -1 : 1;
    }
    return ca.sign ? -cmp : cmp;
}

static inline bool ia64_f8_is_min(int op)
{
    return op == IA64_F8_MIN || op == IA64_F8_AMIN ||
           op == IA64_F8_PMIN || op == IA64_F8_PAMIN;
}

static inline bool ia64_f8_is_absolute(int op)
{
    return op == IA64_F8_AMIN || op == IA64_F8_AMAX ||
           op == IA64_F8_PAMIN || op == IA64_F8_PAMAX;
}

static inline IA64FRBits ia64_f8_scalar_minmax(int op, IA64FRBits a,
                                               IA64FRBits b,
                                               uint32_t *flags)
{
    IA64F8ExtClass ca = ia64_f8_ext_class(a);
    IA64F8ExtClass cb = ia64_f8_ext_class(b);
    int cmp;

    *flags = 0;
    if (ca.nat || cb.nat) {
        return (IA64FRBits) { 0, 0x1fffe };
    }
    /* Invalid special values select f3 when the fault is masked. */
    if (ca.invalid || cb.invalid) {
        *flags = IA64_F8_V;
        return b;
    }
    if (ca.unnormal || cb.unnormal) {
        *flags |= IA64_F8_D;
    }

    cmp = ia64_f8_ext_cmp(a, b, ca, cb, ia64_f8_is_absolute(op));
    if (ia64_f8_is_min(op)) {
        return cmp < 0 ? a : b;
    }
    return cmp > 0 ? a : b;
}

static inline bool ia64_f8_single_nan(uint32_t v)
{
    return (v & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) &&
           (v & UINT32_C(0x007fffff));
}

static inline bool ia64_f8_single_snan(uint32_t v)
{
    return ia64_f8_single_nan(v) && !(v & UINT32_C(0x00400000));
}

static inline bool ia64_f8_single_denormal(uint32_t v)
{
    return !(v & UINT32_C(0x7f800000)) && (v & UINT32_C(0x007fffff));
}

static inline int ia64_f8_single_cmp(uint32_t a, uint32_t b, bool absolute)
{
    uint32_t am = a & UINT32_C(0x7fffffff);
    uint32_t bm = b & UINT32_C(0x7fffffff);
    bool as = (a >> 31) != 0;
    bool bs = (b >> 31) != 0;

    if (absolute) {
        return am < bm ? -1 : am > bm ? 1 : 0;
    }
    if (am == 0 && bm == 0) {
        return 0;
    }
    if (as != bs) {
        return as ? -1 : 1;
    }
    if (am == bm) {
        return 0;
    }
    if (!as) {
        return am < bm ? -1 : 1;
    }
    return am < bm ? 1 : -1;
}

static inline uint32_t ia64_f8_single_minmax(int op, uint32_t a, uint32_t b,
                                             uint32_t *flags)
{
    int cmp;

    *flags = 0;
    if (ia64_f8_single_nan(a) || ia64_f8_single_nan(b)) {
        *flags = IA64_F8_V;
        return b;
    }
    if (ia64_f8_single_denormal(a) || ia64_f8_single_denormal(b)) {
        *flags |= IA64_F8_D;
    }
    cmp = ia64_f8_single_cmp(a, b, ia64_f8_is_absolute(op));
    if (ia64_f8_is_min(op)) {
        return cmp < 0 ? a : b;
    }
    return cmp > 0 ? a : b;
}

static inline bool ia64_f8_compare_ordered_relation(int op)
{
    return op == IA64_F8_PCMP_LT || op == IA64_F8_PCMP_LE ||
           op == IA64_F8_PCMP_NLT || op == IA64_F8_PCMP_NLE;
}

static inline bool ia64_f8_single_compare(int op, uint32_t a, uint32_t b,
                                          uint32_t *flags)
{
    bool anan = ia64_f8_single_nan(a);
    bool bnan = ia64_f8_single_nan(b);
    bool unordered = anan || bnan;
    bool snan = ia64_f8_single_snan(a) || ia64_f8_single_snan(b);
    int cmp = 0;
    bool result;

    *flags = 0;
    if (snan || (unordered && ia64_f8_compare_ordered_relation(op))) {
        *flags |= IA64_F8_V;
    }
    if (!unordered &&
        (ia64_f8_single_denormal(a) || ia64_f8_single_denormal(b))) {
        *flags |= IA64_F8_D;
    }
    if (!unordered) {
        cmp = ia64_f8_single_cmp(a, b, false);
    }

    switch (op) {
    case IA64_F8_PCMP_EQ:
        result = !unordered && cmp == 0;
        break;
    case IA64_F8_PCMP_LT:
        result = !unordered && cmp < 0;
        break;
    case IA64_F8_PCMP_LE:
        result = !unordered && cmp <= 0;
        break;
    case IA64_F8_PCMP_UNORD:
        result = unordered;
        break;
    case IA64_F8_PCMP_NEQ:
        result = unordered || cmp != 0;
        break;
    case IA64_F8_PCMP_NLT:
        result = unordered || cmp >= 0;
        break;
    case IA64_F8_PCMP_NLE:
        result = unordered || cmp > 0;
        break;
    case IA64_F8_PCMP_ORD:
        result = !unordered;
        break;
    default:
        __builtin_trap();
    }
    return result;
}

static inline IA64F8Result ia64_f8_result(int op, IA64FRBits a, IA64FRBits b,
                                          uint64_t fpsr, unsigned sf)
{
    IA64F8Result result = { 0 };
    unsigned controls = (fpsr >> (6 + 13 * (sf & 3))) & 0x7f;
    unsigned disabled = (sf != 0 && (controls & 0x40)) ? 0x3f :
                        (unsigned)(fpsr & 0x3f);
    bool packed = op >= 0x40;

    if (ia64_f9_is_natval(a) || ia64_f9_is_natval(b)) {
        result.value = (IA64FRBits) { 0, 0x1fffe };
        return result;
    }

    if (!packed) {
        result.value = ia64_f8_scalar_minmax(op, a, b, &result.flags);
        result.fault_code = result.flags & (IA64_F8_V | IA64_F8_D) & ~disabled;
        return result;
    }

    {
        uint32_t ah = a.significand >> 32;
        uint32_t al = a.significand;
        uint32_t bh = b.significand >> 32;
        uint32_t bl = b.significand;
        uint32_t rh, rl, hf = 0, lf = 0;

        if (op >= IA64_F8_PCMP_EQ) {
            rh = ia64_f8_single_compare(op, ah, bh, &hf) ? UINT32_MAX : 0;
            rl = ia64_f8_single_compare(op, al, bl, &lf) ? UINT32_MAX : 0;
        } else {
            rh = ia64_f8_single_minmax(op, ah, bh, &hf);
            rl = ia64_f8_single_minmax(op, al, bl, &lf);
        }

        result.value.significand = ((uint64_t)rh << 32) | rl;
        result.value.sign_exp = 0x1003e;
        result.flags = hf | lf;
        result.fault_code =
            (hf & (IA64_F8_V | IA64_F8_D) & ~disabled) |
            ((lf & (IA64_F8_V | IA64_F8_D) & ~disabled) << 4);
    }
    return result;
}

#endif /* TARGET_IA64_FP_F8_H */
