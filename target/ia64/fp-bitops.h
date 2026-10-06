/*
 * IA-64 F9 raw floating-register operations.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel SDM Vol. 3 rev. 2.3, F9 and the individual instruction operations.
 * These are bit operations, not host floating-point computations.  In
 * particular, a packed pair is an integer-format FR, not two scalar FRs.
 */
#ifndef TARGET_IA64_FP_BITOPS_H
#define TARGET_IA64_FP_BITOPS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct IA64FRBits {
    uint64_t significand;
    uint64_t sign_exp;
} IA64FRBits;

/* Operation keys retain major[0] and x6, making scalar/parallel distinct. */
enum {
    IA64_F9_INVALID = -1,
    IA64_F9_MERGE_S = 0x10,
    IA64_F9_MERGE_NS = 0x11,
    IA64_F9_MERGE_SE = 0x12,
    IA64_F9_PACK = 0x28,
    IA64_F9_AND = 0x2c,
    IA64_F9_ANDCM = 0x2d,
    IA64_F9_OR = 0x2e,
    IA64_F9_XOR = 0x2f,
    IA64_F9_SWAP = 0x34,
    IA64_F9_SWAP_NL = 0x35,
    IA64_F9_SWAP_NR = 0x36,
    IA64_F9_MIX_LR = 0x39,
    IA64_F9_MIX_R = 0x3a,
    IA64_F9_MIX_L = 0x3b,
    IA64_F9_SXT_R = 0x3c,
    IA64_F9_SXT_L = 0x3d,
    IA64_F9_PMERGE_S = 0x50,
    IA64_F9_PMERGE_NS = 0x51,
    IA64_F9_PMERGE_SE = 0x52,
};

static inline int ia64_f9_decode(uint64_t insn)
{
    unsigned major = (insn >> 37) & 15;
    unsigned x6 = (insn >> 27) & 63;

    if (major > 1 || ((insn >> 33) & 1)) {
        return IA64_F9_INVALID;
    }
    if (major == 1) {
        return x6 >= 0x10 && x6 <= 0x12 ? (int)(0x40 | x6) : IA64_F9_INVALID;
    }
    switch (x6) {
    case 0x10: case 0x11: case 0x12: case 0x28:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
    case 0x34: case 0x35: case 0x36:
    case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d:
        return x6;
    default:
        return IA64_F9_INVALID;
    }
}

static inline bool ia64_f9_is_natval(IA64FRBits v)
{
    /* Sign is significant: negative pseudo-zero is not NaTVal. */
    return v.significand == 0 && (v.sign_exp & 0x3ffff) == 0x1fffe;
}

/*
 * F3 fselect and F5 fclass are raw-register operations. Keep them out of
 * host floating point so NaTVal, unsupported encodings and unnormalized
 * values remain distinguishable.
 */
static inline IA64FRBits ia64_fselect_result(IA64FRBits when_true,
                                             IA64FRBits when_false,
                                             IA64FRBits selector)
{
    if (ia64_f9_is_natval(when_true) ||
        ia64_f9_is_natval(when_false) ||
        ia64_f9_is_natval(selector)) {
        return (IA64FRBits) { 0, 0x1fffe };
    }
    return (IA64FRBits) {
        (when_true.significand & selector.significand) |
        (when_false.significand & ~selector.significand),
        0x1003e
    };
}

/*
 * Fclass9 bits: 8 NaT, 7 qNaN, 6 sNaN, 5 inf, 4 normal, 3 unnormal,
 * 2 zero, 1 negative, 0 positive. Unsupported register-format values
 * match no class.
 */
static inline bool ia64_fclass_relation(IA64FRBits value, unsigned fclass9,
                                        bool *nat_clears)
{
    uint64_t significand = value.significand;
    unsigned exponent = value.sign_exp & 0x1ffff;
    bool sign = (value.sign_exp & 0x20000) != 0;
    bool integer_bit = (significand >> 63) != 0;
    bool nat = ia64_f9_is_natval(value);
    bool inf, qnan, snan, unsupported, zero, unorm, normal, sign_match;

    if (nat) {
        *nat_clears = (fclass9 & 0x100) == 0;
        return (fclass9 & 0x100) != 0;
    }
    *nat_clears = false;

    inf = exponent == 0x1ffff &&
          significand == UINT64_C(0x8000000000000000);
    qnan = exponent == 0x1ffff && integer_bit && !inf &&
           (significand & UINT64_C(0x4000000000000000));
    snan = exponent == 0x1ffff && integer_bit && !inf && !qnan;
    unsupported = exponent == 0x1ffff && !integer_bit;
    zero = !unsupported && !inf && !qnan && !snan &&
           significand == 0 && exponent == 0;
    unorm = !unsupported && !inf && !qnan && !snan && !zero &&
             ((significand == 0 && exponent != 0) ||
              (significand != 0 && (exponent == 0 || !integer_bit)));
    normal = !unsupported && !inf && !qnan && !snan && !zero && !unorm;
    sign_match = (!sign && (fclass9 & 0x001)) ||
                 (sign && (fclass9 & 0x002));

    return (sign_match &&
            ((zero && (fclass9 & 0x004)) ||
             (unorm && (fclass9 & 0x008)) ||
             (normal && (fclass9 & 0x010)) ||
             (inf && (fclass9 & 0x020)))) ||
           (snan && (fclass9 & 0x040)) ||
           (qnan && (fclass9 & 0x080));
}

static inline uint32_t ia64_f9_single_bits(IA64FRBits v)
{
    /*
     * Register -> single memory format (fp_single), without rounding.
     * The entire eight-bit exponent is gated by the explicit integer bit.
     * This preserves SP denormals (FR exponent 0xff81, integer bit clear).
     * Exponent[16] and [6:0] map to single exponent[7] and [6:0].
     */
    uint32_t exponent = 0;

    if (v.significand >> 63) {
        exponent = ((v.sign_exp >> 9) & 0x80) | (v.sign_exp & 0x7f);
    }
    return ((v.sign_exp & 0x20000) << 14) | (exponent << 23) |
           ((v.significand >> 40) & 0x7fffff);
}

static inline IA64FRBits ia64_f9_result(int op, IA64FRBits a, IA64FRBits b)
{
    IA64FRBits result = { 0, 0x1003e };
    uint64_t mask;

    if (ia64_f9_is_natval(a) || ia64_f9_is_natval(b)) {
        return (IA64FRBits) { 0, 0x1fffe };
    }
    switch (op) {
    case IA64_F9_MERGE_S:
    case IA64_F9_MERGE_NS:
        result.significand = b.significand;
        result.sign_exp = (b.sign_exp & 0x1ffff) | (a.sign_exp & 0x20000);
        if (op == IA64_F9_MERGE_NS) {
            result.sign_exp ^= 0x20000;
        }
        break;
    case IA64_F9_MERGE_SE:
        result.significand = b.significand;
        result.sign_exp = a.sign_exp & 0x3ffff;
        break;
    case IA64_F9_PACK:
        result.significand = (uint64_t)ia64_f9_single_bits(a) << 32 |
                             ia64_f9_single_bits(b);
        break;
    case IA64_F9_AND:
        result.significand = a.significand & b.significand;
        break;
    case IA64_F9_ANDCM:
        result.significand = a.significand & ~b.significand;
        break;
    case IA64_F9_OR:
        result.significand = a.significand | b.significand;
        break;
    case IA64_F9_XOR:
        result.significand = a.significand ^ b.significand;
        break;
    case IA64_F9_SWAP:
    case IA64_F9_SWAP_NL:
    case IA64_F9_SWAP_NR:
        result.significand = (b.significand << 32) | (a.significand >> 32);
        if (op == IA64_F9_SWAP_NL) {
            result.significand ^= UINT64_C(0x8000000000000000);
        } else if (op == IA64_F9_SWAP_NR) {
            result.significand ^= UINT64_C(0x80000000);
        }
        break;
    case IA64_F9_MIX_LR:
        result.significand = (a.significand & UINT64_C(0xffffffff00000000)) |
                             (uint32_t)b.significand;
        break;
    case IA64_F9_MIX_R:
        result.significand = (a.significand << 32) | (uint32_t)b.significand;
        break;
    case IA64_F9_MIX_L:
        result.significand = (a.significand & UINT64_C(0xffffffff00000000)) |
                             (b.significand >> 32);
        break;
    case IA64_F9_SXT_R:
        result.significand = (uint64_t)-(int64_t)((a.significand >> 31) & 1)
                            << 32 | (uint32_t)b.significand;
        break;
    case IA64_F9_SXT_L:
        result.significand = (uint64_t)-(int64_t)(a.significand >> 63)
                            << 32 | (b.significand >> 32);
        break;
    case IA64_F9_PMERGE_S:
    case IA64_F9_PMERGE_NS:
    case IA64_F9_PMERGE_SE:
        mask = op == IA64_F9_PMERGE_SE ? UINT64_C(0xff800000ff800000) :
                                        UINT64_C(0x8000000080000000);
        result.significand = (a.significand & mask) | (b.significand & ~mask);
        if (op == IA64_F9_PMERGE_NS) {
            result.significand ^= mask;
        }
        break;
    default:
        /* Caller must first decode; no guessed semantics for other opcodes. */
        __builtin_trap();
    }
    return result;
}

#endif /* TARGET_IA64_FP_BITOPS_H */
