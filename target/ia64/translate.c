/*
 * IA-64 translation
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "tcg/tcg.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"
#include "disas/disas.h"
#include "exec/translation-block.h"
#include "exec/memop.h"
#include "qemu/log.h"
#include <inttypes.h>
#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_psr;
static TCGv_i64 cpu_cfm;
static TCGv_i64 cpu_pr;
static TCGv_i64 cpu_b[8];
static TCGv_i64 cpu_r[128];
static TCGv_i64 cpu_rr[8];
static TCGv_i64 cpu_cr_iva;
static TCGv_i64 cpu_cr_iip;
static TCGv_i64 cpu_cr_ipsr;
static TCGv_i64 cpu_cr_ifs;
static TCGv_i64 cpu_cr_isr;
static TCGv_i64 cpu_cr_ifa;
static TCGv_i64 cpu_cr_iim;
static TCGv_i64 cpu_cr_iha;

void ia64_tcg_init(void)
{
    cpu_pc = tcg_global_mem_new_i64(tcg_env,
                                    offsetof(CPUIA64State, ip), "ip");
    cpu_psr = tcg_global_mem_new_i64(tcg_env,
                                     offsetof(CPUIA64State, psr), "psr");
    cpu_cfm = tcg_global_mem_new_i64(tcg_env,
                                     offsetof(CPUIA64State, cfm), "cfm");
    cpu_pr = tcg_global_mem_new_i64(tcg_env,
                                    offsetof(CPUIA64State, pr), "pr");
    cpu_cr_iip = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_iip),
                                        "cr_iip");
    cpu_cr_ipsr = tcg_global_mem_new_i64(tcg_env,
                                         offsetof(CPUIA64State, cr_ipsr),
                                         "cr_ipsr");
    cpu_cr_iva = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr[2]),
                                        "cr_iva");
    cpu_cr_ifs = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_ifs),
                                        "cr_ifs");
    cpu_cr_isr = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_isr),
                                        "cr_isr");
    cpu_cr_ifa = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_ifa),
                                        "cr_ifa");
    cpu_cr_iim = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_iim),
                                        "cr_iim");
    cpu_cr_iha = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_iha),
                                        "cr_iha");
    
    for (int i = 0; i < 8; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "b%d", i);
        cpu_b[i] = tcg_global_mem_new_i64(tcg_env,
                                          offsetof(CPUIA64State, b[i]),
                                          g_strdup(buf));
    }
    
    for (int i = 0; i < 128; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "r%d", i);
        cpu_r[i] = tcg_global_mem_new_i64(tcg_env,
                                          offsetof(CPUIA64State, r[i]),
                                          g_strdup(buf));
    }

    for (int i = 0; i < 8; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "rr%d", i);
        cpu_rr[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPUIA64State, rr[i]),
                                           g_strdup(buf));
    }
}

typedef struct DisasContext {
    DisasContextBase base;
    CPUIA64State *env;
    int ri;
    uint64_t extra_bits;
    int mem_idx;
} DisasContext;

enum SlotType {
    SLOT_M, SLOT_I, SLOT_F, SLOT_B, SLOT_L, SLOT_X, SLOT_RES
};

static const uint8_t template_table[32][3] = {
    { SLOT_M, SLOT_I, SLOT_I }, /* 00 */
    { SLOT_M, SLOT_I, SLOT_I }, /* 01 */
    { SLOT_M, SLOT_I, SLOT_I }, /* 02 */
    { SLOT_M, SLOT_I, SLOT_I }, /* 03 */
    { SLOT_M, SLOT_L, SLOT_X }, /* 04 */
    { SLOT_M, SLOT_L, SLOT_X }, /* 05 */
    { SLOT_RES, SLOT_RES, SLOT_RES }, /* 06 */
    { SLOT_RES, SLOT_RES, SLOT_RES }, /* 07 */
    { SLOT_M, SLOT_M, SLOT_I }, /* 08 */
    { SLOT_M, SLOT_M, SLOT_I }, /* 09 */
    { SLOT_M, SLOT_M, SLOT_I }, /* 0A */
    { SLOT_M, SLOT_M, SLOT_I }, /* 0B */
    { SLOT_M, SLOT_F, SLOT_I }, /* 0C */
    { SLOT_M, SLOT_F, SLOT_I }, /* 0D */
    { SLOT_M, SLOT_M, SLOT_F }, /* 0E */
    { SLOT_M, SLOT_M, SLOT_F }, /* 0F */
    { SLOT_M, SLOT_I, SLOT_B }, /* 10 */
    { SLOT_M, SLOT_I, SLOT_B }, /* 11 */
    { SLOT_M, SLOT_B, SLOT_B }, /* 12 */
    { SLOT_M, SLOT_B, SLOT_B }, /* 13 */
    { SLOT_RES, SLOT_RES, SLOT_RES }, /* 14 */
    { SLOT_RES, SLOT_RES, SLOT_RES }, /* 15 */
    { SLOT_B, SLOT_B, SLOT_B }, /* 16 */
    { SLOT_B, SLOT_B, SLOT_B }, /* 17 */
    { SLOT_M, SLOT_M, SLOT_B }, /* 18 */
    { SLOT_M, SLOT_M, SLOT_B }, /* 19 */
    { SLOT_RES, SLOT_RES, SLOT_RES }, /* 1A */
    { SLOT_RES, SLOT_RES, SLOT_RES }, /* 1B */
    { SLOT_M, SLOT_F, SLOT_B }, /* 1C */
    { SLOT_M, SLOT_F, SLOT_B }, /* 1D */
    { SLOT_RES, SLOT_RES, SLOT_RES }, /* 1E */
    { SLOT_RES, SLOT_RES, SLOT_RES }, /* 1F */
};

static void gen_unimpl(DisasContext *ctx, uint64_t insn, const char *msg)
{
    cpu_abort(env_cpu(ctx->env),
              "IA64 UNIMPL: pc=%016" PRIx64 " ri=%d insn=%011" PRIx64 " %s",
              ctx->base.pc_next, ctx->ri, insn, msg ?: "");
}

static MemOp memop_for_size_idx(unsigned size_idx)
{
    switch (size_idx & 0x3) {
    case 0:
        return MO_TE | MO_UB;
    case 1:
        return MO_TE | MO_UW;
    case 2:
        return MO_TE | MO_UL;
    case 3:
    default:
        return MO_TE | MO_64;
    }
}

static void gen_set_ri_const(uint8_t ri)
{
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_andi_i64(t, cpu_psr, ~PSR_RI_MASK);
    tcg_gen_ori_i64(t, t, ((uint64_t)(ri & 3) << PSR_RI_SHIFT));
    tcg_gen_mov_i64(cpu_psr, t);
}

static TCGLabel *gen_qp_skip(uint8_t qp)
{
    if (qp == 0) {
        return NULL;
    }
    TCGLabel *skip = gen_new_label();
    TCGv_i64 t_qp = tcg_temp_new_i64();
    tcg_gen_shri_i64(t_qp, cpu_pr, qp);
    tcg_gen_andi_i64(t_qp, t_qp, 1);
    tcg_gen_brcondi_i64(TCG_COND_EQ, t_qp, 0, skip);
    return skip;
}

static void gen_set_predicates(uint8_t p_true, uint8_t p_false, TCGv_i64 cond)
{
    /* Update predicate registers while keeping p0 untouched. */
    uint64_t mask = 0;
    if (p_true != 0) {
        mask |= 1ULL << p_true;
    }
    if (p_false != 0) {
        mask |= 1ULL << p_false;
    }
    if (mask == 0) {
        return;
    }

    TCGv_i64 t_pr = tcg_temp_new_i64();
    tcg_gen_mov_i64(t_pr, cpu_pr);

    /* Clear destination predicate bits first. */
    tcg_gen_andi_i64(t_pr, t_pr, ~mask);

    if (p_true != 0) {
        TCGv_i64 t = tcg_temp_new_i64();
        tcg_gen_andi_i64(t, cond, 1);
        tcg_gen_shli_i64(t, t, p_true);
        tcg_gen_or_i64(t_pr, t_pr, t);
    }
    if (p_false != 0) {
        TCGv_i64 t = tcg_temp_new_i64();
        tcg_gen_xori_i64(t, cond, 1);
        tcg_gen_andi_i64(t, t, 1);
        tcg_gen_shli_i64(t, t, p_false);
        tcg_gen_or_i64(t_pr, t_pr, t);
    }

    tcg_gen_mov_i64(cpu_pr, t_pr);
}

static TCGv_i64 gen_load_ar(uint8_t idx)
{
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_ld_i64(t, tcg_env, offsetof(CPUIA64State, ar) + idx * 8);
    return t;
}

static void gen_store_ar(uint8_t idx, TCGv_i64 v)
{
    tcg_gen_st_i64(v, tcg_env, offsetof(CPUIA64State, ar) + idx * 8);
}

static TCGv_i64 gen_load_cr_reg(uint8_t idx)
{
    switch (idx) {
    case 16: return cpu_cr_ipsr;
    case 17: return cpu_cr_isr;
    case 2:  return cpu_cr_iva;
    case 19: return cpu_cr_iip;
    case 20: return cpu_cr_ifa;
    case 23: return cpu_cr_ifs;
    case 24: return cpu_cr_iim;
    case 25: return cpu_cr_iha;
    default:
        break;
    }
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_ld_i64(t, tcg_env, offsetof(CPUIA64State, cr) + idx * 8);
    return t;
}

static void gen_store_cr_reg(uint8_t idx, TCGv_i64 v)
{
    switch (idx) {
    case 16: tcg_gen_mov_i64(cpu_cr_ipsr, v); break;
    case 17: tcg_gen_mov_i64(cpu_cr_isr, v); break;
    case 2:  tcg_gen_mov_i64(cpu_cr_iva, v); break;
    case 19: tcg_gen_mov_i64(cpu_cr_iip, v); break;
    case 20: tcg_gen_mov_i64(cpu_cr_ifa, v); break;
    case 23: tcg_gen_mov_i64(cpu_cr_ifs, v); break;
    case 24: tcg_gen_mov_i64(cpu_cr_iim, v); break;
    case 25: tcg_gen_mov_i64(cpu_cr_iha, v); break;
    case 21: /* cr.itir */ break;
    case 22: /* cr.iipa */ break;
    default:
        break;
    }
    tcg_gen_st_i64(v, tcg_env, offsetof(CPUIA64State, cr) + idx * 8);
}

static void gen_store_rr_reg(TCGv_i64 idx_reg, TCGv_i64 val)
{
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_shri_i64(tmp, idx_reg, 61);
    for (int i = 0; i < 8; i++) {
        TCGLabel *skip = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_NE, tmp, i, skip);
        tcg_gen_mov_i64(cpu_rr[i], val);
        gen_set_label(skip);
    }
}

static void gen_load_rr_reg(TCGv_i64 dest, TCGv_i64 idx_reg)
{
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_shri_i64(tmp, idx_reg, 61);
    for (int i = 0; i < 8; i++) {
        TCGLabel *skip = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_NE, tmp, i, skip);
        tcg_gen_mov_i64(dest, cpu_rr[i]);
        gen_set_label(skip);
    }
}

static void ia64_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    ctx->env = cpu_env(cs);
    ctx->ri = ctx->base.tb->flags & 3;
    ctx->extra_bits = 0;
    /*
     * Use the current PSR to select the data translation mode.
     * If DT is clear, data accesses use physical addressing.
     */
    if (!(ctx->env->psr & IA64_PSR_DT)) {
        ctx->mem_idx = MMU_PHYS_IDX;
    } else {
        ctx->mem_idx = (IA64_PSR_CPL(ctx->env->psr) == 3) ? MMU_USER_IDX
                                                          : MMU_KERNEL_IDX;
    }
}

static void ia64_tr_tb_start(DisasContextBase *db, CPUState *cpu)
{
    DisasContext *ctx = container_of(db, DisasContext, base);

    if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
        qemu_log_mask(CPU_LOG_EXEC, "IA64: TB start pc=%016" PRIx64 "\n",
                      ctx->base.pc_next);
    }
}

static void ia64_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->base.pc_next, ctx->ri);
}

static void decode_a_unit(DisasContext *ctx, uint64_t insn)
{
    uint8_t major = (insn >> 37) & 0xf;
    uint8_t x2a = (insn >> 34) & 0x3;
    uint8_t ve = (insn >> 33) & 0x1;
    uint8_t x4 = (insn >> 29) & 0xf;
    uint8_t x2b = (insn >> 27) & 0x3;
    uint8_t r3 = (insn >> 20) & 0x7f;
    uint8_t r2 = (insn >> 13) & 0x7f;
    uint8_t r1 = (insn >> 6) & 0x7f;
    uint8_t qp = insn & 0x3f;
    
    TCGLabel *skip_label = gen_qp_skip(qp);
    bool handled = false;

    if (major == 0x8 && x2a == 0) {
        /* A1 bitwise/add/sub/etc. */
        /* Immediate sub: sub r1 = imm7, r3  (imm7 is bits 13..19) */
        if (x4 == 0x9 && x2b == 1 && ve == 0) {
            uint64_t imm7 = r2;
            TCGv_i64 src = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(src, 0);
            } else {
                tcg_gen_mov_i64(src, cpu_r[r3]);
            }
            TCGv_i64 t = tcg_temp_new_i64();
            tcg_gen_movi_i64(t, imm7);
            tcg_gen_sub_i64(t, t, src);
            if (r1 != 0) {
                tcg_gen_mov_i64(cpu_r[r1], t);
            }
            handled = true;
        } else
        /* Immediate logical ops: and/andcm/or/xor r1 = imm7, r3 */
        if (x4 == 0xB && ve == 0) {
            int64_t simm7 = sextract64((uint64_t)r2, 0, 7);
            uint64_t imm = (uint64_t)simm7;
            TCGv_i64 src = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(src, 0);
            } else {
                tcg_gen_mov_i64(src, cpu_r[r3]);
            }
            TCGv_i64 t = tcg_temp_new_i64();
            switch (x2b) {
            case 0: /* and */
                tcg_gen_andi_i64(t, src, imm);
                break;
            case 1: /* andcm */
                tcg_gen_andi_i64(t, src, ~imm);
                break;
            case 2: /* or */
                tcg_gen_ori_i64(t, src, imm);
                break;
            case 3: /* xor */
                tcg_gen_xori_i64(t, src, imm);
                break;
            default:
                g_assert_not_reached();
            }
            if (r1 != 0) {
                tcg_gen_mov_i64(cpu_r[r1], t);
            }
            handled = true;
            /* handled */
        } else {
            TCGv_i64 t1 = tcg_temp_new_i64();
            TCGv_i64 t2 = tcg_temp_new_i64();
            if (r2 == 0) {
                tcg_gen_movi_i64(t1, 0);
            } else {
                tcg_gen_mov_i64(t1, cpu_r[r2]);
            }
            if (r3 == 0) {
                tcg_gen_movi_i64(t2, 0);
            } else {
                tcg_gen_mov_i64(t2, cpu_r[r3]);
            }

            bool a1_handled = true;
            switch (x4) {
            case 0: /* add */
                tcg_gen_add_i64(t1, t1, t2);
                break;
            case 1: /* sub */
                tcg_gen_sub_i64(t1, t1, t2);
                break;
            case 2: /* addp4: r1 = r2 + (r3 << 2) */
                tcg_gen_shli_i64(t2, t2, 2);
                tcg_gen_add_i64(t1, t1, t2);
                break;
            case 3:
                switch (x2b) {
                case 0: tcg_gen_and_i64(t1, t1, t2); break;
                case 1: tcg_gen_andc_i64(t1, t1, t2); break;
                case 2: tcg_gen_or_i64(t1, t1, t2); break;
                case 3: tcg_gen_xor_i64(t1, t1, t2); break;
                default: break;
                }
                break;
            case 4: { /* shladd: r1 = (r2 << (x2b+1)) + r3 */
                uint8_t sh = x2b + 1;
                tcg_gen_shli_i64(t1, t1, sh);
                tcg_gen_add_i64(t1, t1, t2);
                break;
            }
            default:
                a1_handled = false;
                break;
            }
            if (!a1_handled) {
                handled = false;
            } else {
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t1);
                }
                handled = true;
            }
        }
    } else if (major == 0x8 && x2a == 2 && ve == 0) {
        /* adds r1 = imm14, r3 (A4) */
        uint64_t imm =
            extract64(insn, 13, 7) |               /* imm7b */
            (extract64(insn, 27, 6) << 7) |         /* imm6d */
            (extract64(insn, 36, 1) << 13);         /* sign */
        int64_t simm = sextract64(imm, 0, 14);
        if (ctx->base.pc_next == 0xa000000100163b80ULL && ctx->ri == 1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "adds.A4 decode pc=%016" PRIx64 " ri=%d insn=%011" PRIx64
                          " r1=%u r3=%u imm7b=%llu imm6d=%llu s=%llu -> imm14=%lld\n",
                          ctx->base.pc_next, ctx->ri, insn,
                          r1, r3,
                          (unsigned long long)extract64(insn, 13, 7),
                          (unsigned long long)extract64(insn, 27, 6),
                          (unsigned long long)extract64(insn, 36, 1),
                          (long long)simm);
        }
        TCGv_i64 t = tcg_temp_new_i64();
        if (r3 == 0) {
            tcg_gen_movi_i64(t, simm);
        } else {
            tcg_gen_addi_i64(t, cpu_r[r3], simm);
        }
        if (r1 != 0) {
            tcg_gen_mov_i64(cpu_r[r1], t);
        }
        if (ctx->base.pc_next == 0xa000000100163b80ULL && ctx->ri == 1) {
            gen_helper_dbg_probe(tcg_env,
                                 tcg_constant_i64(ctx->base.pc_next),
                                 tcg_constant_i32(100 + ctx->ri));
        }
        handled = true;
    } else if (major == 0x8 && x2a == 3 && ve == 0 && x4 == 0xF) {
        /*
         * addp4 imm9, r3 (A3-ish): r1 = imm9 + (r3 << 2)
         * Encoding as observed in Linux: imm9 = imm7b | (x2b << 7).
         */
        uint64_t imm9 = extract64(insn, 13, 7) | (extract64(insn, 27, 2) << 7);
        int64_t simm = sextract64(imm9, 0, 9);

        TCGv_i64 t = tcg_temp_new_i64();
        if (r3 == 0) {
            tcg_gen_movi_i64(t, simm);
        } else {
            TCGv_i64 scaled = tcg_temp_new_i64();
            tcg_gen_shli_i64(scaled, cpu_r[r3], 2);
            tcg_gen_addi_i64(t, scaled, simm);
        }
        if (r1 != 0) {
            tcg_gen_mov_i64(cpu_r[r1], t);
        }
        handled = true;
    } else if ((major == 0xC || major == 0xD || major == 0xE) &&
               x2a == 0) {
        /* cmp.eq / cmp.ne variants (A6 reg-reg) */
        uint8_t p2 = extract64(insn, 27, 6);
        uint8_t p1 = extract64(insn, 6, 6);
        uint8_t c = (insn >> 12) & 1;
        bool is_ne = c;

        TCGv_i64 t1 = tcg_temp_new_i64();
        TCGv_i64 t2 = tcg_temp_new_i64();
        if (r2 == 0) {
            tcg_gen_movi_i64(t1, 0);
        } else {
            tcg_gen_mov_i64(t1, cpu_r[r2]);
        }
        if (r3 == 0) {
            tcg_gen_movi_i64(t2, 0);
        } else {
            tcg_gen_mov_i64(t2, cpu_r[r3]);
        }

        TCGv_i64 cond = tcg_temp_new_i64();
        if (is_ne) {
            tcg_gen_setcond_i64(TCG_COND_NE, cond, t1, t2);
        } else {
            tcg_gen_setcond_i64(TCG_COND_EQ, cond, t1, t2);
        }
        gen_set_predicates(p1, p2, cond);
        handled = true;
    } else if ((major == 0xC || major == 0xD || major == 0xE) &&
               x2a == 2) {
        /* cmp.eq / cmp.ne imm8, r3 (A8) */
        uint8_t p2 = extract64(insn, 27, 6);
        uint8_t p1 = extract64(insn, 6, 6);
        uint8_t c = (insn >> 12) & 1;
        uint64_t imm = extract64(insn, 13, 7) |
                       (extract64(insn, 36, 1) << 7);
        int64_t simm = sextract64(imm, 0, 8);
        bool is_ne = c;

        TCGv_i64 t2 = tcg_temp_new_i64();
        if (r3 == 0) {
            tcg_gen_movi_i64(t2, 0);
        } else {
            tcg_gen_mov_i64(t2, cpu_r[r3]);
        }
        TCGv_i64 cond = tcg_temp_new_i64();
        if (is_ne) {
            tcg_gen_setcondi_i64(TCG_COND_NE, cond, t2, simm);
        } else {
            tcg_gen_setcondi_i64(TCG_COND_EQ, cond, t2, simm);
        }
        gen_set_predicates(p1, p2, cond);
        handled = true;
    } else if ((major == 0xC || major == 0xD) &&
               (x2a == 1 || x2a == 3)) {
        /* cmp4.lt / cmp4.ltu reg-reg (A6) or imm8 (A8) */
        uint8_t p2 = extract64(insn, 27, 6);
        uint8_t p1 = extract64(insn, 6, 6);
        bool is_unsigned = (major == 0xD);

        TCGv_i64 cond = tcg_temp_new_i64();
        if (x2a == 1) {
            TCGv_i64 t1 = tcg_temp_new_i64();
            TCGv_i64 t2 = tcg_temp_new_i64();
            if (r2 == 0) {
                tcg_gen_movi_i64(t1, 0);
            } else {
                tcg_gen_mov_i64(t1, cpu_r[r2]);
            }
            if (r3 == 0) {
                tcg_gen_movi_i64(t2, 0);
            } else {
                tcg_gen_mov_i64(t2, cpu_r[r3]);
            }
            if (is_unsigned) {
                tcg_gen_ext32u_i64(t1, t1);
                tcg_gen_ext32u_i64(t2, t2);
                tcg_gen_setcond_i64(TCG_COND_LTU, cond, t1, t2);
            } else {
                tcg_gen_ext32s_i64(t1, t1);
                tcg_gen_ext32s_i64(t2, t2);
                tcg_gen_setcond_i64(TCG_COND_LT, cond, t1, t2);
            }
        } else {
            uint64_t imm = extract64(insn, 13, 7) |
                           (extract64(insn, 36, 1) << 7);
            int64_t simm = sextract64(imm, 0, 8);
            TCGv_i64 t2 = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(t2, 0);
            } else {
                tcg_gen_mov_i64(t2, cpu_r[r3]);
            }
            if (is_unsigned) {
                tcg_gen_ext32u_i64(t2, t2);
                tcg_gen_setcondi_i64(TCG_COND_GTU, cond, t2, (uint32_t)simm);
            } else {
                tcg_gen_ext32s_i64(t2, t2);
                tcg_gen_setcondi_i64(TCG_COND_GT, cond, t2, (int32_t)simm);
            }
        }
        gen_set_predicates(p1, p2, cond);
        handled = true;
    } else if (major == 0xE && (x2a == 1 || x2a == 3)) {
        /* cmp4.eq / cmp4.ne (A6/A8 style, 32-bit compare) */
        uint8_t p2 = extract64(insn, 27, 6);
        uint8_t p1 = extract64(insn, 6, 6);
        uint8_t ta = extract64(insn, 33, 1);
        bool is_ne = (insn >> 12) & 1;

        TCGv_i64 t2 = tcg_temp_new_i64();
        if (r3 == 0) {
            tcg_gen_movi_i64(t2, 0);
        } else {
            tcg_gen_mov_i64(t2, cpu_r[r3]);
        }
        tcg_gen_ext32u_i64(t2, t2);

        TCGv_i64 cond = tcg_temp_new_i64();
        if (x2a == 1) {
            TCGv_i64 t1 = tcg_temp_new_i64();
            if (r2 == 0) {
                tcg_gen_movi_i64(t1, 0);
            } else {
                tcg_gen_mov_i64(t1, cpu_r[r2]);
            }
            tcg_gen_ext32u_i64(t1, t1);
            if (is_ne) {
                tcg_gen_setcond_i64(TCG_COND_NE, cond, t1, t2);
            } else {
                tcg_gen_setcond_i64(TCG_COND_EQ, cond, t1, t2);
            }
        } else {
            uint64_t imm = extract64(insn, 13, 7) |
                           (extract64(insn, 36, 1) << 7);
            int64_t simm = sextract64(imm, 0, 8);
            if (is_ne) {
                tcg_gen_setcondi_i64(TCG_COND_NE, cond, t2, (uint32_t)simm);
            } else {
                tcg_gen_setcondi_i64(TCG_COND_EQ, cond, t2, (uint32_t)simm);
            }
        }
        if (ta == 0) {
            gen_set_predicates(p1, p2, cond);
        } else {
            /*
             * .or.andcm predicate combination (used heavily by Linux):
             *   p1 = p1 | cond
             *   p2 = p2 & ~cond
             */
            TCGv_i64 old1 = tcg_temp_new_i64();
            TCGv_i64 old2 = tcg_temp_new_i64();
            tcg_gen_shri_i64(old1, cpu_pr, p1);
            tcg_gen_andi_i64(old1, old1, 1);
            tcg_gen_shri_i64(old2, cpu_pr, p2);
            tcg_gen_andi_i64(old2, old2, 1);

            TCGv_i64 new1 = tcg_temp_new_i64();
            TCGv_i64 new2 = tcg_temp_new_i64();
            tcg_gen_or_i64(new1, old1, cond);
            tcg_gen_xori_i64(new2, cond, 1);
            tcg_gen_and_i64(new2, old2, new2);

            uint64_t mask = 0;
            if (p1 != 0) {
                mask |= 1ULL << p1;
            }
            if (p2 != 0) {
                mask |= 1ULL << p2;
            }
            if (mask) {
                TCGv_i64 t_pr = tcg_temp_new_i64();
                tcg_gen_mov_i64(t_pr, cpu_pr);
                tcg_gen_andi_i64(t_pr, t_pr, ~mask);

                if (p1 != 0) {
                    TCGv_i64 t = tcg_temp_new_i64();
                    tcg_gen_shli_i64(t, new1, p1);
                    tcg_gen_or_i64(t_pr, t_pr, t);
                }
                if (p2 != 0) {
                    TCGv_i64 t = tcg_temp_new_i64();
                    tcg_gen_shli_i64(t, new2, p2);
                    tcg_gen_or_i64(t_pr, t_pr, t);
                }
                tcg_gen_ori_i64(t_pr, t_pr, 1); /* p0 always true */
                tcg_gen_mov_i64(cpu_pr, t_pr);
            }
        }
        handled = true;
    }

    /* addl r1 = imm22, r3 (A5 format, op=9) */
    if (!handled && major == 0x9) {
        uint64_t imm =
            extract64(insn, 13, 7) |               /* bits 13..19 */
            (extract64(insn, 27, 9) << 7) |         /* bits 27..35 */
            (extract64(insn, 22, 5) << (7 + 9)) |   /* bits 22..26 */
            (extract64(insn, 36, 1) << (7 + 9 + 5));/* bit 36 (sign) */
        int64_t simm = sextract64(imm, 0, 22);
        TCGv_i64 t1 = tcg_temp_new_i64();
        if (r3 == 0) {
            tcg_gen_movi_i64(t1, simm);
        } else {
            tcg_gen_addi_i64(t1, cpu_r[r3], simm);
        }
        if (r1 != 0) {
            tcg_gen_mov_i64(cpu_r[r1], t1);
        }
        handled = true;
    }
    
    if (skip_label) {
        gen_set_label(skip_label);
    }
    if (!handled) {
        gen_unimpl(ctx, insn, "A-slot");
    }
}

static void decode_b_unit(DisasContext *ctx, uint64_t insn)
{
    if (insn == 0) {
        return;
    }
    uint8_t qp = insn & 0x3f;
    uint8_t major = (insn >> 37) & 0xf;
    uint8_t x6 = (insn >> 27) & 0x3f;

    TCGLabel *skip_label = gen_qp_skip(qp);
    static int bcall_log_count;

    /* rfi: op=0, x6=0x8 */
    if (major == 0x0 && x6 == 0x8) {
        gen_helper_rfi(tcg_env);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (major == 0x0 && (x6 == 0xC || x6 == 0xD)) {
        /* B8: bsw.0 / bsw.1 */
        gen_helper_bsw(tcg_env, tcg_constant_i32(x6 == 0xD));
    } else if (major == 0x0 && x6 == 0x2) {
        /* cover: RSE bookkeeping (we don't model RSE backing store yet) */
    } else if (major == 0x0 && (x6 == 0x20 || x6 == 0x21)) {
        /* B4: br.cond/br.ia b2 (x6=0x20) and br.ret b2 (x6=0x21). */
        uint8_t b2 = extract64(insn, 13, 3);
        if (x6 == 0x21) {
            gen_helper_ret_restore(tcg_env);
        }
        tcg_gen_andi_i64(cpu_pc, cpu_b[b2], ~0xFULL);
        gen_set_ri_const(0);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (major == 0x1) {
        /* br.call b1 = b2 */
        uint8_t b1 = extract64(insn, 6, 3);
        uint8_t b2 = extract64(insn, 13, 3);
        gen_helper_call(tcg_env);
        tcg_gen_movi_i64(cpu_b[b1], ctx->base.pc_next + 16);
        tcg_gen_andi_i64(cpu_pc, cpu_b[b2], ~0xFULL);
        gen_set_ri_const(0);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (major == 0x4) {
        /* Simple br.cond immediate (B1-style target25 << 4). */
        uint64_t imm = extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
        int64_t disp = sextract64(imm, 0, 21) << 4;
        tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next + disp);
        gen_set_ri_const(0);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (major == 0x5) {
        /* br.call target25 */
        uint8_t b1 = extract64(insn, 6, 3);
        uint64_t imm = extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
        int64_t disp = sextract64(imm, 0, 21) << 4;
        uint64_t tgt = ctx->base.pc_next + disp;
        if (bcall_log_count < 64) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "br.call pc=%016" PRIx64 " insn=%011" PRIx64
                          " b1=%u disp=%" PRId64 " tgt=%016" PRIx64 "\n",
                          ctx->base.pc_next, insn, b1, disp,
                          tgt);
            bcall_log_count++;
        }
        if (tgt == 0xa000000100880440ULL) {
            gen_helper_dbg_call(tcg_env, tcg_constant_i64(ctx->base.pc_next));
        }
        gen_helper_call(tcg_env);
        tcg_gen_movi_i64(cpu_b[b1], ctx->base.pc_next + 16);
        tcg_gen_movi_i64(cpu_pc, tgt);
        gen_set_ri_const(0);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (((insn >> 37) & 0xf) == 0x2 && ((insn >> 27) & 0x3f) == 0x0) {
        /* nop.b */
        /* nothing */
    } else {
        gen_unimpl(ctx, insn, "B-slot");
    }
    if (skip_label) {
        gen_set_label(skip_label);
    }
}

static void decode_insn(DisasContext *ctx, uint64_t insn, enum SlotType type)
{
    uint8_t major = (insn >> 37) & 0xf;
    
    if (type == SLOT_RES) {
        gen_unimpl(ctx, insn, "reserved template");
        return;
    }

    switch (type) {
    case SLOT_M:
        /* M-unit instructions */
        /*
         * IA-64 integer A-unit operations can be issued in M-slots as well
         * (e.g. "adds"/"mov" idioms in kernel prologues and MLX bundles).
         * Decode those here before falling back to memory/control decoding.
         */
        if (major >= 0x8 && major <= 0xE) {
            decode_a_unit(ctx, insn);
            break;
        }
        switch (major) {
        case 0x0: {
            /* Privileged/control ops keyed by x/x6 or x3/x4; crash on unknown. */
            uint8_t m0_x6 = extract64(insn, 30, 6);
            uint8_t m0_x = extract64(insn, 27, 1);
            uint8_t x3 = extract64(insn, 33, 3);
            uint8_t x4 = extract64(insn, 27, 4);
            uint8_t x2 = extract64(insn, 31, 2);

            if (x3 == 5) {
                /* M22: chk.a.clr r1, target25 (we don't model NaT yet; assume OK). */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            if (x3 == 0 && x4 == 0x2) {
                /* M24: mf (full memory fence) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            if (x3 == 0 && x4 == 0x0 && x2 == 0x1) {
                /* M24: invala (invalidate ALAT) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

	            if (x3 == 0 && (x4 == 0x6 || x4 == 0x7)) {
	                /* M44: ssm/rsm imm24 (per ski encoding.format + encoding.imm) */
	                TCGLabel *skip_label = gen_qp_skip(insn & 0x3f);
	                uint64_t imm21a = extract64(insn, 6, 21);
	                uint64_t i = extract64(insn, 36, 1);
	                uint64_t i2d = extract64(insn, 31, 2);
	                uint64_t imm24 = (i << 23) | (i2d << 21) | imm21a;
	                if (x4 == 0x6) {
	                    tcg_gen_ori_i64(cpu_psr, cpu_psr, imm24);
	                } else {
	                    TCGv_i64 mask = tcg_temp_new_i64();
	                    tcg_gen_movi_i64(mask, ~imm24);
	                    tcg_gen_and_i64(cpu_psr, cpu_psr, mask);
	                }
	                /* PSR update affects translation mode; end TB here. */
	                ctx->base.is_jmp = DISAS_TOO_MANY;
	                if (skip_label) {
	                    gen_set_label(skip_label);
	                }
	                break;
	            }
            if (m0_x == 1) {
                /* nop.m / hint.m with imm21 */
                break;
            }
            if (m0_x6 == 0x0) {
                /* nop.m */
            } else if (m0_x6 == 0x01) {
                /* flushrs */
                gen_helper_flushrs(tcg_env);
            } else if (m0_x6 == 0x05) {
                /* mov.m ar.rsc=imm (bootstrap clears RSE) */
                TCGv_i64 tmp = tcg_temp_new_i64();
                tcg_gen_movi_i64(tmp, extract64(insn, 14, 7));
                gen_store_ar(16, tmp); /* ar.rsc */
            } else if (m0_x6 == 0x06) {
                /* srlz.d */
                gen_helper_srlz_d(tcg_env);
            } else if (m0_x6 == 0x07) {
                /* srlz.i */
                gen_helper_srlz_i(tcg_env);
            } else {
                gen_unimpl(ctx, insn, "M-slot");
            }
            break;
        }
        case 0x1: {
            /* mov to/from control regs and region regs */
            uint8_t x3 = (insn >> 33) & 0x7;
            uint8_t x6 = (insn >> 27) & 0x3f;
            if (x3 == 6) {
                /* alloc r1 = ar.pfs, i, l, o, r (M34 format) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint64_t sof = extract64(insn, 13, 7);
                uint64_t sol = extract64(insn, 20, 7);
                uint64_t sor = extract64(insn, 27, 4);
                TCGv_i64 old_pfs = tcg_temp_new_i64();
                gen_helper_alloc(old_pfs, tcg_env,
                                 tcg_constant_i64(sof),
                                 tcg_constant_i64(sol),
                                 tcg_constant_i64(sor));
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], old_pfs);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x25) {
                /* M36: mov r1 = psr */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], cpu_psr);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x0) {
                /* mov rr[r3] = r2 */
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 idx = tcg_temp_new_i64();
                TCGv_i64 val = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(idx, 0);
                } else {
                    tcg_gen_mov_i64(idx, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(val, 0);
                } else {
                    tcg_gen_mov_i64(val, cpu_r[r2]);
                }
                gen_store_rr_reg(idx, val);
                break;
            } else if (x3 == 0 && x6 == 0x10) {
                /* mov r1 = rr[r3] */
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                if (r1 != 0) {
                    TCGv_i64 idx = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(idx, 0);
                    } else {
                        tcg_gen_mov_i64(idx, cpu_r[r3]);
                    }
                gen_load_rr_reg(cpu_r[r1], idx);
                }
                break;
            } else if (x3 == 0 && x6 == 0x1e) {
                /* tpa r1 = r3 (M46) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                TCGv_i64 src = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r3]);
                }
                TCGv_i64 dst = tcg_temp_new_i64();
                gen_helper_tpa(dst, tcg_env, src);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], dst);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x9) {
                /* ptc.l */
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r2]);
                }
                gen_helper_ptc_l(tcg_env, va, tar);
                break;
            } else if (x3 == 0 && x6 == 0xa) {
                /* ptc.g */
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r2]);
                }
                gen_helper_ptc_g(tcg_env, va, tar);
                break;
            } else if (x3 == 0 && x6 == 0xb) {
                /* ptc.ga */
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r2]);
                }
                gen_helper_ptc_ga(tcg_env, va, tar);
                break;
            } else if (x3 == 0 && x6 == 0xc) {
                /* ptr.d */
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 range = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(range, 0);
                } else {
                    tcg_gen_mov_i64(range, cpu_r[r2]);
                }
                gen_helper_ptr_d(tcg_env, va, range);
                break;
            } else if (x3 == 0 && x6 == 0xd) {
                /* ptr.i */
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 range = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(range, 0);
                } else {
                    tcg_gen_mov_i64(range, cpu_r[r2]);
                }
                gen_helper_ptr_i(tcg_env, va, range);
                break;
            } else if (x3 == 0 && x6 == 0xe) {
                /* itr.d */
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 pte = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(pte, 0);
                } else {
                    tcg_gen_mov_i64(pte, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r3]);
                }
                gen_helper_itr_d(tcg_env, pte, tar);
                break;
            } else if (x3 == 0 && x6 == 0xf) {
                /* itr.i */
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 pte = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(pte, 0);
                } else {
                    tcg_gen_mov_i64(pte, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r3]);
                }
                gen_helper_itr_i(tcg_env, pte, tar);
                break;
            } else if (x3 == 0 && (x6 == 0x2e || x6 == 0x2f)) {
                /* itc.d / itc.i */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                if (x6 == 0x2e) {
                    gen_helper_itc_d(tcg_env, cpu_r[r2]);
                } else {
                    gen_helper_itc_i(tcg_env, cpu_r[r2]);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x2c) {
                /* mov cr[r3] = r2 (M32 format) */
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t cr = extract64(insn, 20, 7);
                gen_store_cr_reg(cr, cpu_r[r2]);
                break;
            } else if (x3 == 0 && x6 == 0x24) {
                /* mov r1 = cr[r3] (M33 format) */
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t cr = extract64(insn, 20, 7);
                TCGv_i64 t = gen_load_cr_reg(cr);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                break;
            } else if (x3 == 0 && x6 == 0x22) {
                /* mov r1 = ar[r3] (M31 format) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t ar = extract64(insn, 20, 7);
                TCGv_i64 t = gen_load_ar(ar);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x2a) {
                /* mov ar[r3] = r2 (M29 format) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t ar = extract64(insn, 20, 7);
                TCGv_i64 t = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(t, 0);
                } else {
                    tcg_gen_mov_i64(t, cpu_r[r2]);
                }
                gen_store_ar(ar, t);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 7 && x6 == 0x38) {
                /* mov b[r3] = r2 */
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t b = extract64(insn, 20, 3);
                TCGv_i64 t = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(t, 0);
                } else {
                    tcg_gen_mov_i64(t, cpu_r[r2]);
                }
                tcg_gen_mov_i64(cpu_b[b], t);
                break;
            }
            gen_unimpl(ctx, insn, "M-slot");
            break;
        }
        case 0x4:
        case 0x5: {
            /* Integer loads/stores (M1/M2/M3 for loads, M4/M5 for stores) */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint8_t x6 = extract64(insn, 30, 6);
            uint8_t x = extract64(insn, 27, 1);
            uint8_t m = extract64(insn, 36, 1);
            uint8_t r3 = extract64(insn, 20, 7);
            uint8_t r2 = extract64(insn, 13, 7);
            uint8_t r1 = extract64(insn, 6, 7);
            bool is_imm = (major == 0x5);
            int64_t imm9 = 0;
            bool handled = false;

            /* M19: getf.sig r1 = f2 (op=4 m=0 x=1 x6=0x1c) */
            if (!is_imm && m == 0 && x == 1 && x6 == 0x1c) {
                uint8_t f2 = r2;
                TCGv_i64 t = tcg_temp_new_i64();
                gen_helper_getf_sig(t, tcg_env, tcg_constant_i32(f2));
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                handled = true;
            }
            if (handled) {
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            if (is_imm) {
                /* M3: imm in r2-field; M5: imm in r1-field. */
                uint64_t imm7 = (x6 <= 0x20) ? r2 : r1;
                imm9 = sextract64(imm7 |
                                  (extract64(insn, 27, 1) << 7) |
                                  (extract64(insn, 36, 1) << 8),
                                  0, 9);
            }

            /* Base address */
            TCGv_i64 base = tcg_temp_new_i64();
            TCGv_i64 addr = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(base, 0);
            } else {
                tcg_gen_mov_i64(base, cpu_r[r3]);
            }
            tcg_gen_mov_i64(addr, base);

            /* M2 reg+reg: m=1/x=1, use r2 as index (no scale) */
            bool reg_index = (!is_imm && x == 1 && ((insn >> 36) & 1));
            if (reg_index) {
                TCGv_i64 t = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_mov_i64(t, addr);
                } else {
                    tcg_gen_add_i64(t, addr, cpu_r[r2]);
                }
                tcg_gen_mov_i64(addr, t);
            }

            if (x6 <= 0x1f) {
                /* ld1/ld2/ld4/ld8 (plus hint/acq/a variants) */
                MemOp mop = memop_for_size_idx(x6);
                if (r1 != 0) {
                    tcg_gen_qemu_ld_i64(cpu_r[r1], addr, ctx->mem_idx, mop);
                }
                if (is_imm && r3 != 0 && imm9) {
                    /* M3: post-increment update by imm9. */
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x == 1 && !is_imm && x6 <= 0x7) {
                /* cmpxchg{1,2,4,8} (ignore acq/rel/hints) */
                MemOp mop = memop_for_size_idx(x6) | MO_ALIGN;
                TCGv_i64 cmp = gen_load_ar(32); /* ar.ccv */
                TCGv_i64 val = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(val, 0);
                } else {
                    tcg_gen_mov_i64(val, cpu_r[r2]);
                }
                TCGv_i64 old = tcg_temp_new_i64();
                tcg_gen_atomic_cmpxchg_i64(old, addr, cmp, val,
                                           ctx->mem_idx, mop);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], old);
                }
                handled = true;
            } else if (x == 0 && x6 == 0x10) {
                /* ld1.bias */
                if (r1 != 0) {
                    tcg_gen_qemu_ld_i64(cpu_r[r1], addr, ctx->mem_idx,
                                        MO_TE | MO_UB);
                }
                handled = true;
            } else if (x == 0 && (x6 == 0x20 || x6 == 0x28)) {
                /* ld1.c.clr{,.acq}: treat as zero-extended byte load */
                if (r1 != 0) {
                    tcg_gen_qemu_ld_i64(cpu_r[r1], addr, ctx->mem_idx,
                                        MO_TE | MO_UB);
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x == 0 && (x6 == 0x22 || x6 == 0x2a)) {
                /* ld4.c.clr{,.acq}: treat as zero-extended 32-bit load */
                if (r1 != 0) {
                    tcg_gen_qemu_ld_i64(cpu_r[r1], addr, ctx->mem_idx,
                                        MO_TE | MO_UL);
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x == 0 && x6 == 0x24) {
                /* ld1.c.nc variants: treat as zero-extended byte load */
                if (r1 != 0) {
                    tcg_gen_qemu_ld_i64(cpu_r[r1], addr, ctx->mem_idx,
                                        MO_TE | MO_UB);
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x6 >= 0x30 && x6 <= 0x3b) {
                /* st1/st2/st4/st8 (including .rel/.spill variants) */
                MemOp mop = memop_for_size_idx(x6);
                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                tcg_gen_qemu_st_i64(src, addr, ctx->mem_idx, mop);
                if (is_imm && r3 != 0 && imm9) {
                    /* M5: post-increment update by imm9. */
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            }
            if (!handled) {
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                gen_unimpl(ctx, insn, "M-slot");
                break;
            }
            if (skip_label) {
                gen_set_label(skip_label);
            }
            break;
        }
        case 0x8: /* A-unit */
        case 0x9: /* A-unit */
        case 0xA: /* A-unit */
        case 0xB: /* A-unit */
        case 0xC: /* A-unit */
        case 0xD: /* A-unit */
        case 0xE: /* A-unit */
            decode_a_unit(ctx, insn);
            break;
        case 0x6:
        case 0x7: {
            /* Assorted M-slot ops in the op=6/7 space. */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint8_t m = extract64(insn, 36, 1);
            uint8_t x = extract64(insn, 27, 1);
            uint8_t x6 = extract64(insn, 30, 6);
            uint8_t x3 = extract64(insn, 33, 3);
            bool handled = false;

            /*
             * lfetch* [r3] (M18-style encoding)
             * major=6, x3=5, m=0, x=0, x6={0x2c..0x2f}.
             * Treat as a hint; for .fault variants, perform a byte probe load
             * (discarded) so translation faults are raised in the right place.
             */
            if ((major == 0x6 || major == 0x7) && x3 == 5 &&
                (x6 == 0x2c || x6 == 0x2d || x6 == 0x2e || x6 == 0x2f)) {
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                int32_t simm8 = 0;
                if (major == 0x7) {
                    uint64_t raw = (uint64_t)r2 | ((uint64_t)x << 7);
                    simm8 = sextract64(raw, 0, 8);
                }

                if (x6 == 0x2e || x6 == 0x2f) {
                    /* .fault variants: raise translation faults at the hint site */
                    TCGv_i64 addr = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(addr, 0);
                    } else {
                        tcg_gen_mov_i64(addr, cpu_r[r3]);
                    }
                    TCGv_i64 tmp = tcg_temp_new_i64();
                    tcg_gen_qemu_ld_i64(tmp, addr, ctx->mem_idx, MO_TE | MO_UB);
                }
                if (r3 != 0 && simm8 != 0) {
                    tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], simm8);
                }
                handled = true;
            }

            /*
             * stf8 [r3]=f2{,imm8} (seen in memset/entry save areas)
             * - major=6/7, x3=6, x6=0x31
             * - r3 is base address
             * - r2 is FP register index
             * - imm8 is encoded as (r1 | (x << 7)); 0 => no update
             */
            if (!handled && (major == 0x6 || major == 0x7) && x3 == 6 &&
                m == 0 && x6 == 0x31) {
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t f2 = extract64(insn, 13, 7) & 0x7f;
                uint8_t r3 = extract64(insn, 20, 7);
                uint32_t imm8 = (uint32_t)r1 | ((uint32_t)x << 7);

                TCGv_i64 addr = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(addr, 0);
                } else {
                    tcg_gen_mov_i64(addr, cpu_r[r3]);
                }

                TCGv_i64 lo = tcg_temp_new_i64();
                tcg_gen_ld_i64(lo, tcg_env,
                               offsetof(CPUIA64State, f) + (f2 * 16) + 0);
                tcg_gen_qemu_st_i64(lo, addr, ctx->mem_idx, MO_TE | MO_64);

                if (r3 != 0 && imm8 != 0) {
                    tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], imm8);
                }
                handled = true;
            }

            /*
             * ldf.fill f1=[r3]{,imm8}
             * - major=6/7, x3=3, x6=0x1b
             * - r3 is base address
             * - r2 is the post-increment byte count low 7 bits
             * - imm8 is encoded as (r2 | (x << 7)); 0 => no update
             * Load 16 bytes (2x64) into the emulated FP register.
             */
            if (!handled && (major == 0x6 || major == 0x7) && x3 == 3 &&
                x6 == 0x1b) {
                uint8_t f1 = extract64(insn, 6, 7) & 0x7f;
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                int32_t simm8 = 0;
                if (major == 0x7) {
                    uint64_t raw = (uint64_t)r2 | ((uint64_t)x << 7);
                    simm8 = sextract64(raw, 0, 8);
                }

                TCGv_i64 addr = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(addr, 0);
                } else {
                    tcg_gen_mov_i64(addr, cpu_r[r3]);
                }

                TCGv_i64 lo = tcg_temp_new_i64();
                TCGv_i64 hi = tcg_temp_new_i64();
                tcg_gen_qemu_ld_i64(lo, addr, ctx->mem_idx, MO_TE | MO_64);
                TCGv_i64 addr2 = tcg_temp_new_i64();
                tcg_gen_addi_i64(addr2, addr, 8);
                tcg_gen_qemu_ld_i64(hi, addr2, ctx->mem_idx, MO_TE | MO_64);

                if (f1 != 0) {
                    tcg_gen_st_i64(lo, tcg_env,
                                   offsetof(CPUIA64State, f) + (f1 * 16) + 0);
                    tcg_gen_st_i64(hi, tcg_env,
                                   offsetof(CPUIA64State, f) + (f1 * 16) + 8);
                }

                if (r3 != 0 && simm8 != 0) {
                    tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], simm8);
                }
                handled = true;
            }

            /*
             * stf.spill{,.nta} [r3]=f2{,imm7} (used in Linux entry save area)
             * - major=6/7, x3=7, x6=0x3b
             * - r3 is the base address register
             * - r2 is the FP register index
             * - r1 is the post-increment byte count (0 => no update)
             */
            if (!handled && (major == 0x6 || major == 0x7) && x3 == 7 &&
                m == 0 && x == 0 && x6 == 0x3b) {
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t f2 = extract64(insn, 13, 7) & 0x7f;
                uint8_t r3 = extract64(insn, 20, 7);

                TCGv_i64 addr = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(addr, 0);
                } else {
                    tcg_gen_mov_i64(addr, cpu_r[r3]);
                }

                TCGv_i64 lo = tcg_temp_new_i64();
                TCGv_i64 hi = tcg_temp_new_i64();
                tcg_gen_ld_i64(lo, tcg_env,
                               offsetof(CPUIA64State, f) + (f2 * 16) + 0);
                tcg_gen_ld_i64(hi, tcg_env,
                               offsetof(CPUIA64State, f) + (f2 * 16) + 8);

                tcg_gen_qemu_st_i64(lo, addr, ctx->mem_idx, MO_TE | MO_64);
                TCGv_i64 addr2 = tcg_temp_new_i64();
                tcg_gen_addi_i64(addr2, addr, 8);
                tcg_gen_qemu_st_i64(hi, addr2, ctx->mem_idx, MO_TE | MO_64);

                if (r3 != 0 && r1 != 0) {
                    tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], r1);
                }
                handled = true;
            }

            /* M18: setf.sig f1 = r2 (op=6 m=0 x=1 x6=0x1c) */
            if (!handled && major == 0x6 && m == 0 && x == 1 && x6 == 0x1c) {
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t f1 = extract64(insn, 6, 7);
                TCGv_i64 val = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(val, 0);
                } else {
                    tcg_gen_mov_i64(val, cpu_r[r2]);
                }
                gen_helper_setf_sig(tcg_env, tcg_constant_i32(f1), val);
                handled = true;
            }

            if (!handled) {
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                gen_unimpl(ctx, insn, "M-slot op6/7");
                break;
            }
            if (skip_label) {
                gen_set_label(skip_label);
            }
            break;
        }
        default:
            gen_unimpl(ctx, insn, "M-slot");
            break;
        }
        break;
    case SLOT_I:
        /* I-unit instructions */
        switch (major) {
        case 0x0: {
            /* break / nop.i / hint.i / mov / mov pr */
            uint8_t x3 = extract64(insn, 33, 3);
            uint8_t x6 = extract64(insn, 27, 6);
            if (x3 == 0 && (x6 == 0x10 || x6 == 0x11 || x6 == 0x12 ||
                            x6 == 0x14 || x6 == 0x15 || x6 == 0x16)) {
                /* I29: zxt{1,2,4} / sxt{1,2,4} r1 = r3 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);

                if (r1 != 0) {
                    TCGv_i64 src = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r3]);
                    }

                    if (x6 == 0x10) {
                        tcg_gen_andi_i64(cpu_r[r1], src, 0xff);
                    } else if (x6 == 0x11) {
                        tcg_gen_andi_i64(cpu_r[r1], src, 0xffff);
                    } else if (x6 == 0x12) {
                        tcg_gen_andi_i64(cpu_r[r1], src, 0xffffffffULL);
                    } else if (x6 == 0x14) {
                        tcg_gen_ext8s_i64(cpu_r[r1], src);
                    } else if (x6 == 0x15) {
                        tcg_gen_ext16s_i64(cpu_r[r1], src);
                    } else {
                        tcg_gen_ext32s_i64(cpu_r[r1], src);
                    }
                }

                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x30) {
                /* mov r1 = ip (I25) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                if (r1 != 0) {
                    tcg_gen_movi_i64(cpu_r[r1], ctx->base.pc_next);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x33) {
                /* mov r1 = pr (I25) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], cpu_pr);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x1c) {
                /* czx1.r r1 = r3 (I??): index of first zero byte from right (0..8) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);

                TCGv_i64 src = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r3]);
                }

                /* has_zero_byte mask */
                TCGv_i64 t = tcg_temp_new_i64();
                TCGv_i64 tmp = tcg_temp_new_i64();
                tcg_gen_addi_i64(t, src, -0x0101010101010101ULL);
                tcg_gen_not_i64(tmp, src);
                tcg_gen_and_i64(t, t, tmp);
                tcg_gen_andi_i64(t, t, 0x8080808080808080ULL);

                TCGv_i64 idx = tcg_temp_new_i64();
                tcg_gen_ctzi_i64(idx, t, 64);
                tcg_gen_shri_i64(idx, idx, 3);

                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], idx);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x2a) {
                /* I26: mov.i ar3 = r2 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t ar = extract64(insn, 20, 7);
                TCGv_i64 t = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(t, 0);
                } else {
                    tcg_gen_mov_i64(t, cpu_r[r2]);
                }
                gen_store_ar(ar, t);
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x32) {
                /* I28: mov.i r1 = ar3 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t ar = extract64(insn, 13, 7);
                TCGv_i64 t = gen_load_ar(ar);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x01) {
                /* I18: nop.i / hint.i imm21 */
                break;
            }
            if (x3 == 7) {
                /* I21: mov{,.ret} b1 = r2, tag13 (we ignore tag/ih/hints for now) */
                static int movb_log_count;
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t b = extract64(insn, 6, 3);
                uint8_t r2 = extract64(insn, 13, 7);
                if (movb_log_count < 64) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "mov.b pc=%016" PRIx64 " insn=%011" PRIx64
                                  " b=%u r2=%u\n",
                                  ctx->base.pc_next, insn, b, r2);
                    movb_log_count++;
                }
                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                tcg_gen_mov_i64(cpu_b[b], src);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x31) {
                /* mov r1 = b2 (I22) */
                static int movrb_log_count;
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t b2 = extract64(insn, 13, 3);
                if (movrb_log_count < 64) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "mov.r=br pc=%016" PRIx64 " insn=%011" PRIx64
                                  " r1=%u b2=%u\n",
                                  ctx->base.pc_next, insn, r1, b2);
                    movrb_log_count++;
                }
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], cpu_b[b2]);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 3) {
                /* mov pr = r2, mask17 (I23) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint64_t s = extract64(insn, 36, 1);
                uint64_t mask8c = extract64(insn, 24, 8);
                uint64_t mask7a = extract64(insn, 6, 7);
                uint64_t imm = (s << 16) | (mask8c << 8) | (mask7a << 1);
                int64_t mask17 = sextract64(imm, 0, 17);
                uint64_t mask = (uint64_t)mask17;

                TCGv_i64 t_pr = tcg_temp_new_i64();
                TCGv_i64 t_src = tcg_temp_new_i64();
                TCGv_i64 t_masked = tcg_temp_new_i64();
                tcg_gen_mov_i64(t_pr, cpu_pr);
                if (r2 == 0) {
                    tcg_gen_movi_i64(t_src, 0);
                } else {
                    tcg_gen_mov_i64(t_src, cpu_r[r2]);
                }
                tcg_gen_andi_i64(t_masked, t_src, mask);
                tcg_gen_andi_i64(t_pr, t_pr, ~mask);
                tcg_gen_or_i64(t_pr, t_pr, t_masked);
                tcg_gen_ori_i64(t_pr, t_pr, 1); /* p0 always true */
                tcg_gen_mov_i64(cpu_pr, t_pr);
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0) {
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint64_t imm = (extract64(insn, 36, 1) << 20) | extract64(insn, 6, 20);
                if (imm == 0x80000 || imm == 0x80001) {
                    TCGv_i64 timm = tcg_temp_new_i64();
                    tcg_gen_movi_i64(timm, imm);
                    TCGv_i64 ret = tcg_temp_new_i64();
                    gen_helper_ssc(ret, tcg_env, timm);
                    tcg_gen_mov_i64(cpu_r[8], ret);
                } else {
                    gen_helper_breaki(tcg_env, tcg_constant_i64(imm));
                    if (qp == 0) {
                        ctx->base.is_jmp = DISAS_NORETURN;
                    }
                    tcg_gen_exit_tb(NULL, 0);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            gen_unimpl(ctx, insn, "I-slot");
            break;
        }
        case 0x7: {
            /* I-unit multimedia/shift group (op=7). Implement the basic shifts. */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);

            uint8_t za = extract64(insn, 36, 1);
            uint8_t zb = extract64(insn, 33, 1);
            uint8_t ve = extract64(insn, 32, 1);
            uint8_t x2a = extract64(insn, 34, 2);
            uint8_t x2b = extract64(insn, 28, 2);
            uint8_t x2c = extract64(insn, 30, 2);
            uint8_t r1 = extract64(insn, 6, 7);
            uint8_t r2 = extract64(insn, 13, 7);
            uint8_t r3 = extract64(insn, 20, 7);

            /* I3 mux1: za=0 zb=0 ve=0 x2a=3 x2b=2 x2c=2 : r1 = r2, mbtype4 */
            if (za == 0 && zb == 0 && ve == 0 && x2a == 3 && x2b == 2 && x2c == 2) {
                uint8_t mbt4c = extract64(insn, 20, 4);
                if (mbt4c == 0) { /* @brcst */
                    TCGv_i64 src = tcg_temp_new_i64();
                    TCGv_i64 b = tcg_temp_new_i64();
                    if (r2 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r2]);
                    }
                    tcg_gen_andi_i64(b, src, 0xff);
                    if (r1 != 0) {
                        TCGv_i64 t = tcg_temp_new_i64();
                        tcg_gen_muli_i64(t, b, 0x0101010101010101ULL);
                        tcg_gen_mov_i64(cpu_r[r1], t);
                    }
                    if (skip_label) {
                        gen_set_label(skip_label);
                    }
                    break;
                }
                gen_unimpl(ctx, insn, "mux1 mbtype");
            }

            /* I7 shl: za=1 zb=1 ve=0 x2a=0 x2b=0 x2c=1 : r1 = r2, r3 */
            if (za == 1 && zb == 1 && ve == 0 && x2a == 0 && x2b == 0 && x2c == 1) {
                TCGv_i64 src = tcg_temp_new_i64();
                TCGv_i64 cnt = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(cnt, 0);
                } else {
                    tcg_gen_mov_i64(cnt, cpu_r[r3]);
                }
                tcg_gen_andi_i64(cnt, cnt, 63);
                if (r1 != 0) {
                    tcg_gen_shl_i64(cpu_r[r1], src, cnt);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            /*
             * I5 shr/shr.u: za=1 zb=1 ve=0 x2a=0 x2c=0 : r1 = r3, r2
             * - x2b=2 => shr (arithmetic)
             * - x2b=0 => shr.u (logical)
             */
            if (za == 1 && zb == 1 && ve == 0 && x2a == 0 && x2c == 0 &&
                (x2b == 0 || x2b == 2)) {
                TCGv_i64 val = tcg_temp_new_i64();
                TCGv_i64 cnt = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(val, 0);
                } else {
                    tcg_gen_mov_i64(val, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(cnt, 0);
                } else {
                    tcg_gen_mov_i64(cnt, cpu_r[r2]);
                }
                tcg_gen_andi_i64(cnt, cnt, 63);
                if (r1 != 0) {
                    if (x2b == 0) {
                        tcg_gen_shr_i64(cpu_r[r1], val, cnt);
                    } else {
                        tcg_gen_sar_i64(cpu_r[r1], val, cnt);
                    }
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            gen_unimpl(ctx, insn, "I-slot");
            break;
        }
        case 0x4: {
            /* dep r1 = r2, r3, pos6, len4 (I15) */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = NULL;
            if (qp) {
                skip_label = gen_new_label();
                TCGv_i64 t_qp = tcg_temp_new_i64();
                tcg_gen_shri_i64(t_qp, cpu_pr, qp);
                tcg_gen_andi_i64(t_qp, t_qp, 1);
                tcg_gen_brcondi_i64(TCG_COND_EQ, t_qp, 0, skip_label);
            }
            uint8_t r1 = extract64(insn, 6, 7);
            uint8_t r2 = extract64(insn, 13, 7);
            uint8_t r3 = extract64(insn, 20, 7);
            uint8_t pos = extract64(insn, 31, 6);
            uint8_t len = extract64(insn, 27, 4);
            uint64_t mask = (len == 64) ? ~0ULL :
                ((len == 0) ? 0 : ((1ULL << len) - 1) << pos);
            TCGv_i64 src = tcg_temp_new_i64();
            if (r3 == 0) tcg_gen_movi_i64(src, 0);
            else tcg_gen_mov_i64(src, cpu_r[r3]);
            TCGv_i64 val = tcg_temp_new_i64();
            if (r2 == 0) tcg_gen_movi_i64(val, 0);
            else tcg_gen_mov_i64(val, cpu_r[r2]);
            if (mask) {
                tcg_gen_andi_i64(val, val, (mask >> pos));
                if (pos) {
                    tcg_gen_shli_i64(val, val, pos);
                }
                tcg_gen_andi_i64(src, src, ~mask);
                tcg_gen_or_i64(src, src, val);
            }
            if (r1 != 0) {
                tcg_gen_mov_i64(cpu_r[r1], src);
            }
            if (skip_label) {
                gen_set_label(skip_label);
            }
            break;
        }
        case 0x5: {
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);

            uint8_t x2a = extract64(insn, 34, 2);
            uint8_t ve = extract64(insn, 33, 1);
            bool handled = false;

            if (x2a == 0) {
                /* Test instructions: tbit.* / tnat.* */
                uint8_t p2 = extract64(insn, 27, 6);
                uint8_t p1 = extract64(insn, 6, 6);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t pos = extract64(insn, 14, 6);
                uint8_t tb = extract64(insn, 36, 1);
                uint8_t ta = extract64(insn, 33, 1);
                uint8_t c = extract64(insn, 12, 1);
                uint8_t bit13 = extract64(insn, 13, 1);

                if (bit13) {
                    /*
                     * tnat.* p1,p2 = r3
                     *
                     * We currently do not model NaT bits, so assume NaT(r3)=0.
                     * Linux uses tnat to augment predicates in exception paths.
                     *
                     * Encodings observed in Linux:
                     * - c selects z vs nz (0 => z, 1 => nz)
                     * - ta/tb select predicate combining:
                     *     ta=0 tb=0: .unc (default)
                     *     ta=0 tb=1: .and
                     *     ta=1 tb=0: .or
                     *     ta=1 tb=1: .or.andcm
                     */
                    bool is_nz = (c != 0);
                    TCGv_i64 cond = tcg_temp_new_i64();
                    tcg_gen_movi_i64(cond, is_nz ? 0 : 1);

                    if (ta == 0 && tb == 0) {
                        /* .unc: p1 = cond; p2 = ~cond */
                        gen_set_predicates(p1, p2, cond);
                        handled = true;
                    } else {
                        /* .and / .or / .or.andcm */
                        uint64_t mask = 0;
                        if (p1 != 0) {
                            mask |= 1ULL << p1;
                        }
                        if (p2 != 0) {
                            mask |= 1ULL << p2;
                        }
                        if (mask) {
                            TCGv_i64 pr = tcg_temp_new_i64();
                            tcg_gen_mov_i64(pr, cpu_pr);
                            tcg_gen_andi_i64(pr, pr, ~mask);

                            TCGv_i64 ncond = tcg_temp_new_i64();
                            tcg_gen_xori_i64(ncond, cond, 1);
                            tcg_gen_andi_i64(cond, cond, 1);
                            tcg_gen_andi_i64(ncond, ncond, 1);

                            if (p1 != 0) {
                                TCGv_i64 old1 = tcg_temp_new_i64();
                                TCGv_i64 new1 = tcg_temp_new_i64();
                                tcg_gen_shri_i64(old1, cpu_pr, p1);
                                tcg_gen_andi_i64(old1, old1, 1);
                                if (ta == 0) {
                                    tcg_gen_and_i64(new1, old1, cond);
                                } else {
                                    tcg_gen_or_i64(new1, old1, cond);
                                }
                                tcg_gen_shli_i64(new1, new1, p1);
                                tcg_gen_or_i64(pr, pr, new1);
                            }
                            if (p2 != 0) {
                                TCGv_i64 old2 = tcg_temp_new_i64();
                                TCGv_i64 new2 = tcg_temp_new_i64();
                                tcg_gen_shri_i64(old2, cpu_pr, p2);
                                tcg_gen_andi_i64(old2, old2, 1);
                                if (ta == 0 || tb) {
                                    /* .and or .or.andcm: p2 = p2 & ~cond */
                                    tcg_gen_and_i64(new2, old2, ncond);
                                } else {
                                    /* .or: p2 = p2 | ~cond */
                                    tcg_gen_or_i64(new2, old2, ncond);
                                }
                                tcg_gen_shli_i64(new2, new2, p2);
                                tcg_gen_or_i64(pr, pr, new2);
                            }
                            tcg_gen_mov_i64(cpu_pr, pr);
                        }
                        handled = true;
                    }
                } else {
                    /* tbit.* p1,p2 = r3, pos6 */
                    TCGv_i64 src = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r3]);
                    }

                    TCGv_i64 bit = tcg_temp_new_i64();
                    tcg_gen_shri_i64(bit, src, pos);
                    tcg_gen_andi_i64(bit, bit, 1);

                    /*
                     * Decode the encodings observed in early Linux boot:
                     * - For .or forms (ta=1), c selects z vs nz (0 => z, 1 => nz)
                     * - Otherwise treat as tbit.z for now.
                     */
                    bool is_z = (ta == 1) ? (c == 0) : true;
                    if (is_z) {
                        tcg_gen_xori_i64(bit, bit, 1);
                    }

                    gen_set_predicates(p1, p2, bit);
                    handled = true;
                }
            } else if (x2a == 1 && ve == 1) {
                /* shl r1 = r2, count6 (count encoded as 63 - imm6) */
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t inv = extract64(insn, 20, 6);
                uint8_t cnt = 63 - inv;

                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                if (r1 != 0) {
                    tcg_gen_shli_i64(cpu_r[r1], src, cnt);
                }
                handled = true;
            } else if (x2a == 1 && ve == 0) {
                /*
                 * Distinguish between:
                 * - shr/shr.u r1 = r3, count6 (right shift)
                 *   where (63-count) is redundantly encoded in bits 27..32
                 * - extr/extr.u r1 = r3, pos6, len6 (len encoded as len-1)
                 */
                uint8_t pos_or_cnt = extract64(insn, 14, 6);
                uint8_t inv = extract64(insn, 27, 6);

                if (inv == (uint8_t)(63 - pos_or_cnt)) {
                    /* shr / shr.u */
                    uint8_t r1 = extract64(insn, 6, 7);
                    uint8_t r3 = extract64(insn, 20, 7);
                    uint8_t cnt = pos_or_cnt;
                    bool is_arith = extract64(insn, 13, 1);

                    TCGv_i64 src = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r3]);
                    }
                    if (r1 != 0) {
                        if (is_arith) {
                            tcg_gen_sari_i64(cpu_r[r1], src, cnt);
                        } else {
                            tcg_gen_shri_i64(cpu_r[r1], src, cnt);
                        }
                    }
                    handled = true;
                } else {
                    /* extr / extr.u */
                    uint8_t r1 = extract64(insn, 6, 7);
                    uint8_t r3 = extract64(insn, 20, 7);
                    uint8_t pos = extract64(insn, 14, 6);
                    uint8_t len = extract64(insn, 27, 6) + 1;
                    bool is_signed = extract64(insn, 13, 1);

                    if (pos + len > 64 || len == 0) {
                        gen_unimpl(ctx, insn, "I-slot extr");
                        return;
                    }

                    TCGv_i64 src = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r3]);
                    }
                    TCGv_i64 t = tcg_temp_new_i64();
                    tcg_gen_shri_i64(t, src, pos);
                    if (len < 64) {
                        tcg_gen_andi_i64(t, t, (1ULL << len) - 1);
                        if (is_signed) {
                            tcg_gen_shli_i64(t, t, 64 - len);
                            tcg_gen_sari_i64(t, t, 64 - len);
                        }
                    }
                    if (r1 != 0) {
                        tcg_gen_mov_i64(cpu_r[r1], t);
                    }
                    handled = true;
                }
            } else if (x2a == 3 && ve == 1) {
                /* dep r1 = imm1, r3, pos6, len6 (I14) */
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t pos = extract64(insn, 14, 6);
                uint8_t len = extract64(insn, 27, 6);
                uint64_t mask = (len == 64) ? ~0ULL :
                    ((len == 0) ? 0 : ((1ULL << len) - 1) << pos);
                uint64_t imm1 = extract64(insn, 36, 1);

                TCGv_i64 src = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r3]);
                }
                if (mask) {
                    tcg_gen_andi_i64(src, src, ~mask);
                    TCGv_i64 tval = tcg_temp_new_i64();
                    tcg_gen_movi_i64(tval, imm1 ? mask : 0);
                    tcg_gen_or_i64(src, src, tval);
                }
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], src);
                }
                handled = true;
            }

            if (skip_label) {
                gen_set_label(skip_label);
            }
            if (!handled) {
                gen_unimpl(ctx, insn, "I-slot");
            }
            break;
        }
        case 0x8: /* A-unit */
        case 0x9: /* A-unit */
        case 0xA: /* A-unit */
        case 0xB: /* A-unit */
        case 0xC: /* A-unit */
        case 0xD: /* A-unit */
        case 0xE: /* A-unit */
            decode_a_unit(ctx, insn);
            break;
        default:
            gen_unimpl(ctx, insn, "I-slot");
            break;
        }
        break;
    case SLOT_F:
        /* F-unit instructions (minimal subset for kernel/libgcc helpers). */
        if (insn != 0) {
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip = gen_qp_skip(qp);
            uint8_t f_major = (insn >> 37) & 0xf;
            if (f_major == 0xE) {
                /* F2 xma.l: op=E x=1 x2=0 */
                uint8_t x = extract64(insn, 36, 1);
                uint8_t x2 = extract64(insn, 34, 2);
                if (x == 1 && x2 == 0) {
                    uint8_t f4 = extract64(insn, 27, 7);
                    uint8_t f3 = extract64(insn, 20, 7);
                    uint8_t f2 = extract64(insn, 13, 7);
                    uint8_t f1 = extract64(insn, 6, 7);
                    gen_helper_xma_l(tcg_env,
                                     tcg_constant_i32(f1),
                                     tcg_constant_i32(f3),
                                     tcg_constant_i32(f4),
                                     tcg_constant_i32(f2));
                    if (skip) {
                        gen_set_label(skip);
                    }
                    break;
                }
            }
            if (skip) {
                gen_set_label(skip);
            }
            gen_unimpl(ctx, insn, "F-slot");
        }
        break;
    case SLOT_B:
        /* B-unit instructions */
        decode_b_unit(ctx, insn);
        break;
    case SLOT_L:
        /* L-unit instructions (MLX template) */
        ctx->extra_bits = insn;
        break;
    case SLOT_X:
        if (insn == 0) {
            /* Treat zero X-slot as nop (e.g., MLX filler). */
            break;
        }
        if (major == 0x6) {
            /* movl r1 = imm64 (X2 format), uses prior L-slot as extra_bits */
            uint8_t r1 = extract64(insn, 6, 7);
            uint64_t imm = 0;
            uint64_t part;
            int off = 0;
            part = extract64(insn, 13, 7);
            imm |= part << off; off += 7;
            part = extract64(insn, 27, 9);
            imm |= part << off; off += 9;
            part = extract64(insn, 22, 5);
            imm |= part << off; off += 5;
            part = extract64(insn, 21, 1);
            imm |= part << off; off += 1;
            part = extract64(ctx->extra_bits, 0, 41);
            imm |= part << off; off += 41;
            part = extract64(insn, 36, 1);
            imm |= part << off;
            if (r1 != 0) {
                tcg_gen_movi_i64(cpu_r[r1], imm);
            }
        } else if (major == 0xC || major == 0xD) {
            /* X3/X4: brl.cond / brl.call target64, uses prior L-slot extra_bits */
            static int brl_log_count;
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint64_t i = extract64(insn, 36, 1);
            uint64_t imm20b = extract64(insn, 13, 20);
            uint64_t imm39 = extract64(ctx->extra_bits, 2, 39);
            uint64_t v = (i << 59) | (imm39 << 20) | imm20b;
            int64_t disp = sextract64(v, 0, 60) << 4;
            uint64_t tgt = ctx->base.pc_next + disp;
            if (major == 0xD) {
                uint8_t b1 = extract64(insn, 6, 3);
                if (brl_log_count < 32) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "brl.call pc=%016" PRIx64 " insn=%011" PRIx64
                                  " L=%011" PRIx64 " b1=%u tgt=%016" PRIx64 "\n",
                                  ctx->base.pc_next, insn, ctx->extra_bits, b1, tgt);
                    brl_log_count++;
                }
                if (ctx->base.pc_next == 0xa0000001000665c0ULL) {
                    gen_helper_dbg_call(tcg_env, tcg_constant_i64(ctx->base.pc_next));
                }
                gen_helper_call(tcg_env);
                tcg_gen_movi_i64(cpu_b[b1], ctx->base.pc_next + 16);
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                if (qp == 0) {
                    ctx->base.is_jmp = DISAS_NORETURN;
                }
                tcg_gen_exit_tb(NULL, 0);
            } else {
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                if (qp == 0) {
                    ctx->base.is_jmp = DISAS_NORETURN;
                }
                tcg_gen_exit_tb(NULL, 0);
            }
            if (skip_label) {
                gen_set_label(skip_label);
            }
        } else {
            gen_unimpl(ctx, insn, "X-slot");
        }
        break;
    default:
        gen_unimpl(ctx, insn, "bad slot");
        break;
    }
}

static void ia64_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    uint64_t low, high;
    uint64_t slot0, slot1, slot2;
    uint8_t template;
    
    low = translator_ldq(ctx->env, &ctx->base, ctx->base.pc_next);
    high = translator_ldq(ctx->env, &ctx->base, ctx->base.pc_next + 8);
    
    template = low & 0x1f;
    slot0 = (low >> 5) & 0x1ffffffffffULL;
    slot1 = ((low >> 46) | (high << 18)) & 0x1ffffffffffULL;
    slot2 = (high >> 23) & 0x1ffffffffffULL;

    if (ctx->ri == 0) {
        ctx->extra_bits = 0;
    }

    enum SlotType type = template_table[template][ctx->ri];
    uint64_t insn = 0;
    switch (ctx->ri) {
    case 0: insn = slot0; break;
    case 1: insn = slot1; break;
    case 2: insn = slot2; break;
    }

    if (type == SLOT_RES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64 reserved template tmpl=%02x pc=%016" PRIx64
                      " ri=%d low=%016" PRIx64 " high=%016" PRIx64 "\n",
                      template, ctx->base.pc_next, ctx->ri, low, high);
    }

    qemu_log_mask_and_addr(CPU_LOG_EXEC, ctx->base.pc_next,
                           "IA64: pc=%016" PRIx64 " ri=%d tmpl=%02x slot=%d insn=%011" PRIx64 "\n",
                           ctx->base.pc_next, ctx->ri, template, type, insn);

    /* Targeted probes for early boot bringup. */
    if (ctx->base.pc_next == 0xa000000100163ac0ULL && ctx->ri == 2) {
        gen_helper_dbg_probe(tcg_env,
                             tcg_constant_i64(ctx->base.pc_next),
                             tcg_constant_i32(ctx->ri));
    }
    if (ctx->base.pc_next == 0xa000000100163b80ULL && ctx->ri == 1) {
        gen_helper_dbg_probe(tcg_env,
                             tcg_constant_i64(ctx->base.pc_next),
                             tcg_constant_i32(ctx->ri));
    }
    if (ctx->base.pc_next == 0xa000000100163b80ULL && ctx->ri == 2) {
        gen_helper_dbg_probe(tcg_env,
                             tcg_constant_i64(ctx->base.pc_next),
                             tcg_constant_i32(ctx->ri));
    }

    decode_insn(ctx, insn, type);
    
    ctx->ri++;
    if (ctx->ri == 3) {
        ctx->ri = 0;
        ctx->base.pc_next += 16;
    }
    
    if (ctx->base.is_jmp == DISAS_NEXT) {
        if (ctx->ri == 0) {
            /* End of bundle, check for stops or just continue */
        }
    }
}

static void ia64_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    vaddr next_ip = ctx->base.pc_next;
    uint8_t next_ri = ctx->ri;

    /*
     * If we bailed out before completing a bundle, pc_next may still point
     * at the current bundle base. Ensure the TB accounts for the bytes
     * we actually consumed so translate-all does not see a zero-sized TB.
     */
    if (ctx->base.pc_next == ctx->base.pc_first) {
        ctx->base.pc_next += 16;
    }
    
    switch (ctx->base.is_jmp) {
    case DISAS_TOO_MANY:
    case DISAS_NEXT:
        /* Commit next IP + RI. */
        tcg_gen_movi_i64(cpu_pc, next_ip);
        gen_set_ri_const(next_ri);
        tcg_gen_exit_tb(NULL, 0);
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static bool ia64_tr_disas_log(const DisasContextBase *dcbase, CPUState *cpu,
                              FILE *logfile)
{
    fprintf(logfile, "IN: %s\n", lookup_symbol(dcbase->pc_first));
    target_disas(logfile, cpu, dcbase);
    return true;
}

static const TranslatorOps ia64_tr_ops = {
    .init_disas_context = ia64_tr_init_disas_context,
    .tb_start           = ia64_tr_tb_start,
    .insn_start         = ia64_tr_insn_start,
    .translate_insn     = ia64_tr_translate_insn,
    .tb_stop            = ia64_tr_tb_stop,
    .disas_log          = ia64_tr_disas_log,
};

void ia64_translate_code(CPUState *cpu, TranslationBlock *tb, int *max_insns,
                           vaddr pc, void *host_pc)
{
    DisasContext ctx;
    translator_loop(cpu, tb, max_insns, pc, host_pc, &ia64_tr_ops, &ctx.base);
}
