/*
 * IA-64 F1 parallel single-precision multiply-add operations.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_IA64_FP_F1_H
#define TARGET_IA64_FP_F1_H

#include "fpu/softfloat.h"
#include "fp-bitops.h"

enum {
    IA64_F1_INVALID = -1,
    IA64_F1_FPMA = 0,
    IA64_F1_FPMS = 1,
    IA64_F1_FPNMA = 2,
};

typedef struct IA64F1Result {
    IA64FRBits value;
    uint32_t flags;
    uint16_t fault_code;
    uint16_t trap_code;
} IA64F1Result;

static inline int ia64_f1_decode(uint64_t insn)
{
    unsigned major = (insn >> 37) & 15;
    unsigned x = (insn >> 36) & 1;
    unsigned pc;

    if (major < 8 || major > 13) {
        return IA64_F1_INVALID;
    }
    pc = ((major & 1) << 1) | x;
    return pc == 3 ? (int)((major - 8) >> 1) : IA64_F1_INVALID;
}

static inline bool ia64_f1_single_denormal(uint32_t value)
{
    return !(value & UINT32_C(0x7f800000)) &&
           (value & UINT32_C(0x007fffff));
}

static inline FloatRoundMode ia64_f1_rounding(unsigned controls)
{
    static const FloatRoundMode modes[4] = {
        float_round_nearest_even,
        float_round_down,
        float_round_up,
        float_round_to_zero,
    };

    return modes[(controls >> 4) & 3];
}

static inline void ia64_f1_status_init(float_status *status,
                                        unsigned controls,
                                        unsigned disabled)
{
    *status = (float_status) { 0 };
    set_float_exception_flags(0, status);
    /* IA-64 canonical indefinite is a negative quiet NaN. */
    set_float_2nan_prop_rule(float_2nan_prop_ab, status);
    set_float_3nan_prop_rule(float_3nan_prop_abc, status);
    set_float_infzeronan_rule(
        float_infzeronan_dnan_never |
        float_infzeronan_suppress_invalid, status);
    set_float_default_nan_pattern(0b11000000, status);
    set_float_rounding_mode(ia64_f1_rounding(controls), status);
    set_float_detect_tininess(float_tininess_before_rounding,
                              status);
    set_float_ftz_detection(float_ftz_before_rounding, status);
    set_flush_inputs_to_zero(false, status);
    set_flush_to_zero((controls & 1) != 0, status);
    status->rebias_overflow = !(disabled & (1U << 3));
    status->rebias_underflow = !(disabled & (1U << 4));
}

static inline uint32_t ia64_f1_lane_result(uint32_t addend_bits,
                                            uint32_t left_bits,
                                            uint32_t right_bits,
                                            int op,
                                            bool pure_multiply,
                                            float_status *status)
{
    float32 left = make_float32(left_bits);
    float32 right = make_float32(right_bits);
    float32 addend = make_float32(addend_bits);
    float32 result;

    if (pure_multiply) {
        Float2NaNPropRule saved = get_float_2nan_prop_rule(status);

        if (op == IA64_F1_FPNMA &&
            !float32_is_any_nan(left) &&
            !float32_is_any_nan(right)) {
            /* Negate the exact product before directed rounding. */
            left = float32_chs(left);
        }
        /* SoftFloat B is architectural f4, ahead of f3. */
        set_float_2nan_prop_rule(float_2nan_prop_ba, status);
        result = float32_mul(left, right, status);
        set_float_2nan_prop_rule(saved, status);
    } else {
        Float3NaNPropRule saved = get_float_3nan_prop_rule(status);
        int flags = 0;

        if (op == IA64_F1_FPMS) {
            flags = float_muladd_negate_c;
        } else if (op == IA64_F1_FPNMA) {
            flags = float_muladd_negate_product;
        }
        /* SoftFloat A, B, C map to architectural f3, f4, f2. */
        set_float_3nan_prop_rule(float_3nan_prop_bca, status);
        result = float32_muladd(left, right, addend, flags, status);
        set_float_3nan_prop_rule(saved, status);
    }

    return float32_val(result);
}

static inline uint32_t ia64_f1_lane_eval(uint32_t addend_bits,
                                          uint32_t left_bits,
                                          uint32_t right_bits,
                                          int op,
                                          bool pure_multiply,
                                          unsigned controls,
                                          unsigned disabled,
                                          int *soft_out,
                                          bool *fpa)
{
    float_status status;
    uint32_t result;
    int soft;

    ia64_f1_status_init(&status, controls, disabled);
    result = ia64_f1_lane_result(addend_bits, left_bits, right_bits,
                                 op, pure_multiply, &status);
    soft = get_float_exception_flags(&status);
    if ((!pure_multiply && ia64_f1_single_denormal(addend_bits)) ||
        ia64_f1_single_denormal(left_bits) ||
        ia64_f1_single_denormal(right_bits)) {
        soft |= float_flag_input_denormal_used;
    }

    /* FTZ reports both U and I for a flushed tiny result. */
    if (get_flush_to_zero(&status) &&
        float32_is_zero(make_float32(result)) &&
        (soft & float_flag_inexact) &&
        !(soft & (float_flag_invalid | float_flag_overflow))) {
        float_raise(float_flag_output_denormal_flushed, &status);
        soft |= float_flag_output_denormal_flushed;
    }

    *fpa = false;
    if (!(soft & float_flag_invalid) &&
        (soft & (float_flag_overflow | float_flag_underflow |
                 float_flag_inexact |
                 float_flag_output_denormal_flushed))) {
        float_status truncated_status = status;
        uint32_t truncated;

        set_float_exception_flags(0, &truncated_status);
        set_float_rounding_mode(float_round_to_zero,
                                &truncated_status);
        truncated = ia64_f1_lane_result(
            addend_bits, left_bits, right_bits, op, pure_multiply,
            &truncated_status);
        *fpa = (result & UINT32_C(0x7fffffff)) >
               (truncated & UINT32_C(0x7fffffff));
    }

    *soft_out = soft;
    return result;
}

static inline unsigned ia64_f1_soft_flags(int soft)
{
    unsigned flags = 0;

    if (soft & float_flag_invalid) {
        flags |= 1U << 0;
    }
    if (soft & (float_flag_input_denormal_flushed |
                float_flag_input_denormal_used)) {
        flags |= 1U << 1;
    }
    if (soft & float_flag_divbyzero) {
        flags |= 1U << 2;
    }
    if (soft & float_flag_overflow) {
        flags |= 1U << 3;
    }
    if (soft & (float_flag_underflow |
                float_flag_output_denormal_flushed)) {
        flags |= 1U << 4;
    }
    if (soft & (float_flag_inexact |
                float_flag_output_denormal_flushed)) {
        flags |= 1U << 5;
    }
    return flags;
}

static inline unsigned ia64_f1_completion_traps(unsigned flags,
                                                 unsigned disabled)
{
    unsigned enabled = flags & ~disabled & 0x38;

    /* Figure 5-12 reports concurrent I with enabled O/U. */
    if (enabled & 0x18) {
        enabled |= flags & (1U << 5);
    }
    return enabled;
}

static inline IA64F1Result ia64_f1_result(int op,
                                           IA64FRBits addend,
                                           IA64FRBits left,
                                           IA64FRBits right,
                                           bool pure_multiply,
                                           uint64_t fpsr,
                                           unsigned sf)
{
    IA64F1Result result = { 0 };
    unsigned controls = (fpsr >> (6 + 13 * (sf & 3))) & 0x7f;
    unsigned disabled = (sf != 0 && (controls & 0x40)) ? 0x3f :
                        (unsigned)(fpsr & 0x3f);
    uint32_t addend_hi = addend.significand >> 32;
    uint32_t addend_lo = addend.significand;
    uint32_t left_hi = left.significand >> 32;
    uint32_t left_lo = left.significand;
    uint32_t right_hi = right.significand >> 32;
    uint32_t right_lo = right.significand;
    uint32_t out_hi, out_lo;
    int hi_soft, lo_soft;
    bool hi_fpa, lo_fpa;
    unsigned hi_flags, lo_flags, hi_fault, lo_fault;
    unsigned hi_trap, lo_trap;

    if (ia64_f9_is_natval(addend) || ia64_f9_is_natval(left) ||
        ia64_f9_is_natval(right)) {
        result.value = (IA64FRBits) { 0, 0x1fffe };
        return result;
    }

    out_hi = ia64_f1_lane_eval(
        addend_hi, left_hi, right_hi, op, pure_multiply, controls,
        disabled, &hi_soft, &hi_fpa);
    out_lo = ia64_f1_lane_eval(
        addend_lo, left_lo, right_lo, op, pure_multiply, controls,
        disabled, &lo_soft, &lo_fpa);
    hi_flags = ia64_f1_soft_flags(hi_soft);
    lo_flags = ia64_f1_soft_flags(lo_soft);
    hi_fault = hi_flags & ~disabled & 0x7;
    lo_fault = lo_flags & ~disabled & 0x7;
    hi_trap = ia64_f1_completion_traps(hi_flags, disabled);
    lo_trap = ia64_f1_completion_traps(lo_flags, disabled);

    result.value.significand = ((uint64_t)out_hi << 32) | out_lo;
    result.value.sign_exp = 0x1003e;
    result.flags = hi_flags | lo_flags;
    /* Parallel FP-HI is ISR.code[3:0], LO is [7:4]. */
    result.fault_code = (hi_fault & 0xf) |
                        ((lo_fault & 0xf) << 4);
    if (hi_trap || lo_trap) {
        /* LO O/U/I/FPA is [10:7], HI is [14:11]. */
        result.trap_code = 1U |
                           ((lo_trap & 0x38) << 4) |
                           ((hi_trap & 0x38) << 8);
        if (lo_fpa && lo_trap) {
            result.trap_code |= 1U << 10;
        }
        if (hi_fpa && hi_trap) {
            result.trap_code |= 1U << 14;
        }
    }
    return result;
}

#endif /* TARGET_IA64_FP_F1_H */
