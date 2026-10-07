/*
 * IA-64 F6/F7 reciprocal approximation operations.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Integer-only implementation of frcpa, frsqrta, fprcpa and fprsqrta.
 * The 8-bit seed tables are the Itanium tables carried by Ski at pinned
 * revision dfc2902ea1423d9b32543a5daf8026213f2b37a1 (GPL-2.0-or-later).
 */
#ifndef TARGET_IA64_FP_APPROX_H
#define TARGET_IA64_FP_APPROX_H

#include "fp-bitops.h"

enum {
    IA64_F67_INVALID = -1,
    IA64_F67_FRCPA = 0,
    IA64_F67_FRSQRTA = 1,
    IA64_F67_FPRCPA = 2,
    IA64_F67_FPRSQRTA = 3,
    IA64_F67_V = 1,
    IA64_F67_D = 2,
    IA64_F67_Z = 4,
    IA64_F67_SWA = 8,
};

typedef enum IA64F67Class {
    IA64_F67_ZERO,
    IA64_F67_FINITE,
    IA64_F67_INF,
    IA64_F67_QNAN,
    IA64_F67_SNAN,
    IA64_F67_UNSUPPORTED,
} IA64F67Class;

typedef struct IA64F67Number {
    uint64_t sig;
    int exponent;
    unsigned raw_exp;
    bool sign;
    bool unnormal;
    IA64F67Class cls;
} IA64F67Number;

typedef struct IA64F67LaneResult {
    uint64_t value;
    uint64_t sign_exp;
    uint8_t flags;
    bool predicate;
    bool swa;
} IA64F67LaneResult;

typedef struct IA64F67Result {
    IA64FRBits value;
    uint32_t flags;
    uint16_t fault_code;
    bool predicate;
    bool write_value;
} IA64F67Result;

static const uint16_t ia64_f67_recip_table[256] = {
        0x7fc, 0x7f4, 0x7ec, 0x7e4, 0x7dd, 0x7d5, 0x7cd, 0x7c6,
        0x7be, 0x7b7, 0x7af, 0x7a8, 0x7a1, 0x799, 0x792, 0x78b,
        0x784, 0x77d, 0x776, 0x76f, 0x768, 0x761, 0x75b, 0x754,
        0x74d, 0x746, 0x740, 0x739, 0x733, 0x72c, 0x726, 0x720,
        0x719, 0x713, 0x70d, 0x707, 0x700, 0x6fa, 0x6f4, 0x6ee,
        0x6e8, 0x6e2, 0x6dc, 0x6d7, 0x6d1, 0x6cb, 0x6c5, 0x6bf,
        0x6ba, 0x6b4, 0x6af, 0x6a9, 0x6a3, 0x69e, 0x699, 0x693,
        0x68e, 0x688, 0x683, 0x67e, 0x679, 0x673, 0x66e, 0x669,
        0x664, 0x65f, 0x65a, 0x655, 0x650, 0x64b, 0x646, 0x641,
        0x63c, 0x637, 0x632, 0x62e, 0x629, 0x624, 0x61f, 0x61b,
        0x616, 0x611, 0x60d, 0x608, 0x604, 0x5ff, 0x5fb, 0x5f6,
        0x5f2, 0x5ed, 0x5e9, 0x5e5, 0x5e0, 0x5dc, 0x5d8, 0x5d4,
        0x5cf, 0x5cb, 0x5c7, 0x5c3, 0x5bf, 0x5bb, 0x5b6, 0x5b2,
        0x5ae, 0x5aa, 0x5a6, 0x5a2, 0x59e, 0x59a, 0x597, 0x593,
        0x58f, 0x58b, 0x587, 0x583, 0x57f, 0x57c, 0x578, 0x574,
        0x571, 0x56d, 0x569, 0x566, 0x562, 0x55e, 0x55b, 0x557,
        0x554, 0x550, 0x54d, 0x549, 0x546, 0x542, 0x53f, 0x53b,
        0x538, 0x534, 0x531, 0x52e, 0x52a, 0x527, 0x524, 0x520,
        0x51d, 0x51a, 0x517, 0x513, 0x510, 0x50d, 0x50a, 0x507,
        0x503, 0x500, 0x4fd, 0x4fa, 0x4f7, 0x4f4, 0x4f1, 0x4ee,
        0x4eb, 0x4e8, 0x4e5, 0x4e2, 0x4df, 0x4dc, 0x4d9, 0x4d6,
        0x4d3, 0x4d0, 0x4cd, 0x4ca, 0x4c8, 0x4c5, 0x4c2, 0x4bf,
        0x4bc, 0x4b9, 0x4b7, 0x4b4, 0x4b1, 0x4ae, 0x4ac, 0x4a9,
        0x4a6, 0x4a4, 0x4a1, 0x49e, 0x49c, 0x499, 0x496, 0x494,
        0x491, 0x48e, 0x48c, 0x489, 0x487, 0x484, 0x482, 0x47f,
        0x47c, 0x47a, 0x477, 0x475, 0x473, 0x470, 0x46e, 0x46b,
        0x469, 0x466, 0x464, 0x461, 0x45f, 0x45d, 0x45a, 0x458,
        0x456, 0x453, 0x451, 0x44f, 0x44c, 0x44a, 0x448, 0x445,
        0x443, 0x441, 0x43f, 0x43c, 0x43a, 0x438, 0x436, 0x433,
        0x431, 0x42f, 0x42d, 0x42b, 0x429, 0x426, 0x424, 0x422,
        0x420, 0x41e, 0x41c, 0x41a, 0x418, 0x415, 0x413, 0x411,
        0x40f, 0x40d, 0x40b, 0x409, 0x407, 0x405, 0x403, 0x401,
};

static const uint16_t ia64_f67_rsqrt_table[256] = {
        0x5a5, 0x5a0, 0x59a, 0x595, 0x58f, 0x58a, 0x585, 0x580,
        0x57a, 0x575, 0x570, 0x56b, 0x566, 0x561, 0x55d, 0x558,
        0x553, 0x54e, 0x54a, 0x545, 0x540, 0x53c, 0x538, 0x533,
        0x52f, 0x52a, 0x526, 0x522, 0x51e, 0x51a, 0x515, 0x511,
        0x50d, 0x509, 0x505, 0x501, 0x4fd, 0x4fa, 0x4f6, 0x4f2,
        0x4ee, 0x4ea, 0x4e7, 0x4e3, 0x4df, 0x4dc, 0x4d8, 0x4d5,
        0x4d1, 0x4ce, 0x4ca, 0x4c7, 0x4c3, 0x4c0, 0x4bd, 0x4b9,
        0x4b6, 0x4b3, 0x4b0, 0x4ad, 0x4a9, 0x4a6, 0x4a3, 0x4a0,
        0x49d, 0x49a, 0x497, 0x494, 0x491, 0x48e, 0x48b, 0x488,
        0x485, 0x482, 0x47f, 0x47d, 0x47a, 0x477, 0x474, 0x471,
        0x46f, 0x46c, 0x469, 0x467, 0x464, 0x461, 0x45f, 0x45c,
        0x45a, 0x457, 0x454, 0x452, 0x44f, 0x44d, 0x44a, 0x448,
        0x445, 0x443, 0x441, 0x43e, 0x43c, 0x43a, 0x437, 0x435,
        0x433, 0x430, 0x42e, 0x42c, 0x429, 0x427, 0x425, 0x423,
        0x420, 0x41e, 0x41c, 0x41a, 0x418, 0x416, 0x414, 0x411,
        0x40f, 0x40d, 0x40b, 0x409, 0x407, 0x405, 0x403, 0x401,
        0x7fc, 0x7f4, 0x7ec, 0x7e5, 0x7dd, 0x7d5, 0x7ce, 0x7c7,
        0x7bf, 0x7b8, 0x7b1, 0x7aa, 0x7a3, 0x79c, 0x795, 0x78e,
        0x788, 0x781, 0x77a, 0x774, 0x76d, 0x767, 0x761, 0x75a,
        0x754, 0x74e, 0x748, 0x742, 0x73c, 0x736, 0x730, 0x72b,
        0x725, 0x71f, 0x71a, 0x714, 0x70f, 0x709, 0x704, 0x6fe,
        0x6f9, 0x6f4, 0x6ee, 0x6e9, 0x6e4, 0x6df, 0x6da, 0x6d5,
        0x6d0, 0x6cb, 0x6c6, 0x6c1, 0x6bd, 0x6b8, 0x6b3, 0x6ae,
        0x6aa, 0x6a5, 0x6a1, 0x69c, 0x698, 0x693, 0x68f, 0x68a,
        0x686, 0x682, 0x67d, 0x679, 0x675, 0x671, 0x66d, 0x668,
        0x664, 0x660, 0x65c, 0x658, 0x654, 0x650, 0x64c, 0x649,
        0x645, 0x641, 0x63d, 0x639, 0x635, 0x632, 0x62e, 0x62a,
        0x627, 0x623, 0x620, 0x61c, 0x618, 0x615, 0x611, 0x60e,
        0x60a, 0x607, 0x604, 0x600, 0x5fd, 0x5f9, 0x5f6, 0x5f3,
        0x5f0, 0x5ec, 0x5e9, 0x5e6, 0x5e3, 0x5df, 0x5dc, 0x5d9,
        0x5d6, 0x5d3, 0x5d0, 0x5cd, 0x5ca, 0x5c7, 0x5c4, 0x5c1,
        0x5be, 0x5bb, 0x5b8, 0x5b5, 0x5b2, 0x5af, 0x5ac, 0x5aa,
};

static inline int ia64_f67_decode(uint64_t insn)
{
    unsigned major = (insn >> 37) & 15;

    if (major > 1 || !((insn >> 33) & 1)) {
        return IA64_F67_INVALID;
    }
    return (major << 1) | ((insn >> 36) & 1);
}

static inline IA64FRBits ia64_f67_ext_zero(bool sign)
{
    return (IA64FRBits) { 0, (uint64_t)sign << 17 };
}

static inline IA64FRBits ia64_f67_ext_inf(bool sign)
{
    return (IA64FRBits) { UINT64_C(1) << 63,
                          ((uint64_t)sign << 17) | 0x1ffff };
}

static inline IA64FRBits ia64_f67_ext_indefinite(void)
{
    return (IA64FRBits) { UINT64_C(0xc000000000000000), 0x3ffff };
}

static inline IA64FRBits ia64_f67_ext_quiet(IA64FRBits value)
{
    value.significand |= UINT64_C(0xc000000000000000);
    value.sign_exp = (value.sign_exp & UINT64_C(0x20000)) | 0x1ffff;
    return value;
}

static inline IA64F67Number ia64_f67_ext_number(IA64FRBits value)
{
    IA64F67Number number = {
        .sig = value.significand,
        .raw_exp = value.sign_exp & 0x1ffff,
        .sign = (value.sign_exp & 0x20000) != 0,
    };

    if (number.raw_exp == 0x1ffff) {
        if (!(number.sig >> 63)) {
            number.cls = IA64_F67_UNSUPPORTED;
        } else if (number.sig == (UINT64_C(1) << 63)) {
            number.cls = IA64_F67_INF;
        } else if (number.sig & (UINT64_C(1) << 62)) {
            number.cls = IA64_F67_QNAN;
        } else {
            number.cls = IA64_F67_SNAN;
        }
        return number;
    }
    if (!number.sig) {
        number.cls = IA64_F67_ZERO;
        return number;
    }
    number.cls = IA64_F67_FINITE;
    number.unnormal = number.raw_exp == 0 || !(number.sig >> 63);
    number.exponent = (int)(number.raw_exp ? number.raw_exp : 1) - 0xffff;
    if (!(number.sig >> 63)) {
        unsigned shift = __builtin_clzll(number.sig);
        number.sig <<= shift;
        number.exponent -= shift;
    }
    return number;
}

static inline IA64F67Number ia64_f67_lane_number(uint32_t value)
{
    unsigned exp = (value >> 23) & 255;
    uint32_t fraction = value & 0x7fffff;
    IA64F67Number number = {
        .sign = (value >> 31) != 0,
        .raw_exp = exp,
    };

    if (exp == 255) {
        if (!fraction) {
            number.cls = IA64_F67_INF;
        } else if (fraction & 0x400000) {
            number.cls = IA64_F67_QNAN;
        } else {
            number.cls = IA64_F67_SNAN;
        }
        number.sig = (UINT64_C(1) << 63) | ((uint64_t)fraction << 40);
        return number;
    }
    if (!exp && !fraction) {
        number.cls = IA64_F67_ZERO;
        return number;
    }
    number.cls = IA64_F67_FINITE;
    number.unnormal = exp == 0;
    if (exp) {
        number.sig = ((UINT64_C(1) << 23) | fraction) << 40;
        number.exponent = (int)exp - 127;
    } else {
        unsigned top = 31 - __builtin_clz(fraction);
        number.sig = (uint64_t)fraction << (63 - top);
        number.exponent = (int)top - 149;
    }
    return number;
}

static inline IA64FRBits ia64_f67_ext_seed(IA64F67Number source, bool rsqrt)
{
    uint16_t seed;
    int exponent;

    if (rsqrt) {
        unsigned index = ((source.exponent + 0xffff) & 1) << 7 |
                         ((source.sig >> 56) & 0x7f);
        seed = ia64_f67_rsqrt_table[index];
        int half = source.exponent >= 0 ? source.exponent / 2 :
                   -(((-source.exponent) + 1) / 2);
        exponent = -1 - half;
    } else {
        seed = ia64_f67_recip_table[(source.sig >> 55) & 0xff];
        exponent = -1 - source.exponent;
    }
    return (IA64FRBits) {
        (uint64_t)seed << 53,
        ((uint64_t)(rsqrt ? 0 : source.sign) << 17) |
            ((unsigned)(0xffff + exponent) & 0x1ffff),
    };
}

static inline uint32_t ia64_f67_lane_seed(IA64F67Number source, bool rsqrt)
{
    IA64FRBits seed = ia64_f67_ext_seed(source, rsqrt);
    int exponent = (int)(seed.sign_exp & 0x1ffff) - 0xffff;
    unsigned ieee_exp = exponent + 127;
    uint32_t fraction = (seed.significand >> 40) & 0x7fffff;

    if (ieee_exp >= 255) {
        return ((uint32_t)(seed.sign_exp >> 17) << 31) | 0x7f800000;
    }
    if (!ieee_exp) {
        return (uint32_t)(seed.sign_exp >> 17) << 31;
    }
    return ((uint32_t)(seed.sign_exp >> 17) << 31) |
           (ieee_exp << 23) | fraction;
}

static inline IA64F67LaneResult ia64_f67_scalar_recip(IA64FRBits numerator,
                                                       IA64FRBits denominator)
{
    IA64F67Number num = ia64_f67_ext_number(numerator);
    IA64F67Number den = ia64_f67_ext_number(denominator);
    IA64F67LaneResult result = { 0 };
    IA64FRBits value;

    if (num.cls == IA64_F67_UNSUPPORTED || den.cls == IA64_F67_UNSUPPORTED) {
        value = ia64_f67_ext_indefinite();
        result.flags = IA64_F67_V;
    } else if (num.cls == IA64_F67_SNAN) {
        value = ia64_f67_ext_quiet(numerator);
        result.flags = IA64_F67_V;
    } else if (num.cls == IA64_F67_QNAN) {
        value = numerator;
    } else if (den.cls == IA64_F67_SNAN) {
        value = ia64_f67_ext_quiet(denominator);
        result.flags = IA64_F67_V;
    } else if (den.cls == IA64_F67_QNAN) {
        value = denominator;
    } else if ((num.cls == IA64_F67_INF && den.cls == IA64_F67_INF) ||
               (num.cls == IA64_F67_ZERO && den.cls == IA64_F67_ZERO)) {
        value = ia64_f67_ext_indefinite();
        result.flags = IA64_F67_V;
    } else if (den.cls == IA64_F67_ZERO && num.cls != IA64_F67_INF) {
        value = ia64_f67_ext_inf(num.sign ^ den.sign);
        result.flags = IA64_F67_Z;
    } else if (num.cls != IA64_F67_INF && num.cls != IA64_F67_ZERO &&
               den.cls != IA64_F67_INF &&
               (den.raw_exp == 0 || den.raw_exp >= 0x1fffc ||
                (int)num.raw_exp - (int)den.raw_exp <= -65533 ||
                (int)num.raw_exp - (int)den.raw_exp >= 65535 ||
                num.raw_exp <= 64)) {
        result.swa = true;
        return result;
    } else {
        if (num.unnormal || den.unnormal) {
            result.flags |= IA64_F67_D;
        }
        if (num.cls == IA64_F67_INF) {
            value = ia64_f67_ext_inf(num.sign ^ den.sign);
        } else if (num.cls == IA64_F67_ZERO || den.cls == IA64_F67_INF) {
            value = ia64_f67_ext_zero(num.sign ^ den.sign);
        } else {
            value = ia64_f67_ext_seed(den, false);
            result.predicate = true;
        }
    }
    result.value = value.significand;
    result.sign_exp = value.sign_exp;
    return result;
}

static inline IA64F67LaneResult ia64_f67_scalar_rsqrt(IA64FRBits source)
{
    IA64F67Number number = ia64_f67_ext_number(source);
    IA64F67LaneResult result = { 0 };
    IA64FRBits value;

    if (number.cls == IA64_F67_UNSUPPORTED) {
        value = ia64_f67_ext_indefinite();
        result.flags = IA64_F67_V;
    } else if (number.cls == IA64_F67_SNAN) {
        value = ia64_f67_ext_quiet(source);
        result.flags = IA64_F67_V;
    } else if (number.cls == IA64_F67_QNAN) {
        value = source;
    } else if (number.sign && number.cls != IA64_F67_ZERO) {
        value = ia64_f67_ext_indefinite();
        result.flags = IA64_F67_V;
    } else if (number.cls == IA64_F67_INF) {
        value = ia64_f67_ext_inf(false);
    } else if (number.cls == IA64_F67_ZERO) {
        value = ia64_f67_ext_zero(number.sign);
    } else if (number.raw_exp <= 64) {
        result.swa = true;
        return result;
    } else {
        if (number.unnormal) {
            result.flags |= IA64_F67_D;
        }
        value = ia64_f67_ext_seed(number, true);
        result.predicate = true;
    }
    result.value = value.significand;
    result.sign_exp = value.sign_exp;
    return result;
}

static inline uint32_t ia64_f67_lane_qnan(uint32_t value)
{
    return value | 0x7fc00000;
}

static inline IA64F67LaneResult ia64_f67_packed_recip(uint32_t numerator,
                                                       uint32_t denominator)
{
    IA64F67Number num = ia64_f67_lane_number(numerator);
    IA64F67Number den = ia64_f67_lane_number(denominator);
    IA64F67LaneResult result = { 0 };
    uint32_t value;

    if (num.cls == IA64_F67_SNAN) {
        value = ia64_f67_lane_qnan(numerator);
        result.flags = IA64_F67_V;
    } else if (num.cls == IA64_F67_QNAN) {
        value = numerator;
    } else if (den.cls == IA64_F67_SNAN) {
        value = ia64_f67_lane_qnan(denominator);
        result.flags = IA64_F67_V;
    } else if (den.cls == IA64_F67_QNAN) {
        value = denominator;
    } else if ((num.cls == IA64_F67_INF && den.cls == IA64_F67_INF) ||
               (num.cls == IA64_F67_ZERO && den.cls == IA64_F67_ZERO)) {
        value = 0xffc00000;
        result.flags = IA64_F67_V;
    } else if (den.cls == IA64_F67_ZERO && num.cls != IA64_F67_INF) {
        value = ((uint32_t)(num.sign ^ den.sign) << 31) | 0x7f800000;
        result.flags = IA64_F67_Z;
    } else {
        if (num.unnormal || den.unnormal) {
            result.flags |= IA64_F67_D;
        }
        if (num.cls == IA64_F67_INF) {
            value = ((uint32_t)(num.sign ^ den.sign) << 31) | 0x7f800000;
        } else if (num.cls == IA64_F67_ZERO || den.cls == IA64_F67_INF) {
            value = (uint32_t)(num.sign ^ den.sign) << 31;
        } else if (den.unnormal) {
            value = ((uint32_t)den.sign << 31) | 0x7f800000;
        } else if (den.raw_exp >= 252) {
            value = (uint32_t)den.sign << 31;
        } else {
            value = ia64_f67_lane_seed(den, false);
            result.predicate = num.exponent - den.exponent > -125 &&
                               num.exponent - den.exponent < 127 &&
                               num.exponent > -103;
        }
    }
    result.value = value;
    return result;
}

static inline IA64F67LaneResult ia64_f67_packed_rsqrt(uint32_t source)
{
    IA64F67Number number = ia64_f67_lane_number(source);
    IA64F67LaneResult result = { 0 };
    uint32_t value;

    if (number.cls == IA64_F67_SNAN) {
        value = ia64_f67_lane_qnan(source);
        result.flags = IA64_F67_V;
    } else if (number.cls == IA64_F67_QNAN) {
        value = source;
    } else if (number.sign && number.cls != IA64_F67_ZERO) {
        value = 0xffc00000;
        result.flags = IA64_F67_V;
    } else if (number.cls == IA64_F67_INF) {
        value = 0;
    } else if (number.cls == IA64_F67_ZERO) {
        value = ((uint32_t)number.sign << 31) | 0x7f800000;
    } else {
        if (number.unnormal) {
            result.flags |= IA64_F67_D;
        }
        value = ia64_f67_lane_seed(number, true);
        result.predicate = number.exponent > -103;
    }
    result.value = value;
    return result;
}

static inline IA64F67Result ia64_f67_result(int op, IA64FRBits a,
                                             IA64FRBits b, uint64_t fpsr,
                                             unsigned sf)
{
    IA64F67Result result = { .write_value = true };
    IA64F67LaneResult lo, hi;
    unsigned controls, disabled;

    if (ia64_f9_is_natval(b) ||
        ((op == IA64_F67_FRCPA || op == IA64_F67_FPRCPA) &&
         ia64_f9_is_natval(a))) {
        result.value = (IA64FRBits) { 0, 0x1fffe };
        return result;
    }

    if (op == IA64_F67_FRCPA || op == IA64_F67_FRSQRTA) {
        lo = op == IA64_F67_FRCPA ? ia64_f67_scalar_recip(a, b) :
                                     ia64_f67_scalar_rsqrt(b);
        result.value = (IA64FRBits) { lo.value, lo.sign_exp };
        result.flags = lo.flags;
        result.predicate = lo.predicate;
        controls = (fpsr >> (6 + 13 * (sf & 3))) & 0x7f;
        disabled = (sf != 0 && (controls & 0x40)) ? 0x3f : fpsr & 0x3f;
        if (lo.swa) {
            result.fault_code = IA64_F67_SWA;
        } else {
            result.fault_code = lo.flags & ~disabled;
        }
    } else {
        if (op == IA64_F67_FPRCPA) {
            lo = ia64_f67_packed_recip((uint32_t)a.significand,
                                       (uint32_t)b.significand);
            hi = ia64_f67_packed_recip(a.significand >> 32,
                                       b.significand >> 32);
        } else {
            lo = ia64_f67_packed_rsqrt((uint32_t)b.significand);
            hi = ia64_f67_packed_rsqrt(b.significand >> 32);
        }
        result.value = (IA64FRBits) {
            ((uint64_t)(uint32_t)hi.value << 32) | (uint32_t)lo.value,
            0x1003e,
        };
        result.flags = lo.flags | hi.flags;
        result.predicate = lo.predicate && hi.predicate;
        disabled = fpsr & 0x3f;
        result.fault_code = (hi.flags & ~disabled) |
                            ((lo.flags & ~disabled) << 4);
    }
    if (result.fault_code) {
        result.write_value = false;
    }
    return result;
}

#endif /* TARGET_IA64_FP_APPROX_H */
