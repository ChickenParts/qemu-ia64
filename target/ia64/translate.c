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

static TCGv_i64 gen_load_fp(uint8_t idx, uint8_t part)
{
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_ld_i64(t, tcg_env,
                   offsetof(CPUIA64State, f) + (idx * 2 + part) * 8);
    return t;
}

static void ia64_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    ctx->env = cpu_env(cs);
    ctx->ri = ctx->base.tb->flags & 3;
    ctx->extra_bits = 0;
    ctx->mem_idx = MMU_KERNEL_IDX;
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
    uint8_t x4 = (insn >> 29) & 0xf;
    uint8_t x2b = (insn >> 27) & 0x3;
    uint8_t r3 = (insn >> 20) & 0x7f;
    uint8_t r2 = (insn >> 13) & 0x7f;
    uint8_t r1 = (insn >> 6) & 0x7f;
    uint8_t qp = insn & 0x3f;
    
    TCGLabel *skip_label = gen_qp_skip(qp);
    bool handled = false;
    
    if (major == 0x8 && x2a == 0 && x4 == 0 && x2b == 0) {
        TCGv_i64 t1 = tcg_temp_new_i64();
        TCGv_i64 t2 = tcg_temp_new_i64();
        
        /* Read r2 and r3 */
        if (r2 == 0) tcg_gen_movi_i64(t1, 0);
        else tcg_gen_mov_i64(t1, cpu_r[r2]);
        
        if (r3 == 0) tcg_gen_movi_i64(t2, 0);
        else tcg_gen_mov_i64(t2, cpu_r[r3]);
        
        /* Do add */
        tcg_gen_add_i64(t1, t1, t2);
        
        /* Write r1 */
        if (r1 != 0) {
            tcg_gen_mov_i64(cpu_r[r1], t1);
        }
        handled = true;
    } else if (major == 0x8 && x2a == 0 && x4 == 3 && x2b == 2) {
        /* or r1 = r2, r3 */
        TCGv_i64 t1 = tcg_temp_new_i64();
        TCGv_i64 t2 = tcg_temp_new_i64();
        if (r2 == 0) tcg_gen_movi_i64(t1, 0);
        else tcg_gen_mov_i64(t1, cpu_r[r2]);
        if (r3 == 0) tcg_gen_movi_i64(t2, 0);
        else tcg_gen_mov_i64(t2, cpu_r[r3]);
        tcg_gen_or_i64(t1, t1, t2);
        if (r1 != 0) {
            tcg_gen_mov_i64(cpu_r[r1], t1);
        }
        handled = true;
    } else if (major == 0x8 && x2a == 2) {
        /* adds r1 = imm14, r3 (A4) */
        uint64_t imm =
            extract64(insn, 13, 7) |               /* imm7b */
            (extract64(insn, 27, 6) << 7) |         /* imm6d */
            (extract64(insn, 36, 1) << 13);         /* sign */
        int64_t simm = sextract64(imm, 0, 14);
        TCGv_i64 t = tcg_temp_new_i64();
        if (r3 == 0) {
            tcg_gen_movi_i64(t, simm);
        } else {
            tcg_gen_addi_i64(t, cpu_r[r3], simm);
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
            int64_t imm = sextract64(insn, 13, 8);
            TCGv_i64 t2 = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(t2, 0);
            } else {
                tcg_gen_mov_i64(t2, cpu_r[r3]);
            }
            if (is_unsigned) {
                tcg_gen_ext32u_i64(t2, t2);
                tcg_gen_setcondi_i64(TCG_COND_LTU, cond, t2, (uint32_t)imm);
            } else {
                tcg_gen_ext32s_i64(t2, t2);
                tcg_gen_setcondi_i64(TCG_COND_LT, cond, t2, (int32_t)imm);
            }
        }
        gen_set_predicates(p1, p2, cond);
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
    uint8_t qp = insn & 0x3f;
    
    TCGLabel *skip_label = NULL;
    
    if (qp != 0) {
        skip_label = gen_new_label();
        TCGv_i64 t_qp = tcg_temp_new_i64();
        tcg_gen_shri_i64(t_qp, cpu_pr, qp);
        tcg_gen_andi_i64(t_qp, t_qp, 1);
        tcg_gen_brcondi_i64(TCG_COND_EQ, t_qp, 0, skip_label);
    }

    /* rfi: op=0, x6=0x8 */
    if (((insn >> 37) & 0xf) == 0x0 && ((insn >> 27) & 0x3f) == 0x8) {
        tcg_gen_mov_i64(cpu_pc, cpu_cr_iip);
        ctx->base.is_jmp = DISAS_NORETURN;
        tcg_gen_exit_tb(NULL, 0);
    } else if (((insn >> 37) & 0xf) == 0x4) {
        /* Simple br.cond immediate (B1-style target25 << 4). */
        uint64_t imm = extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
        int64_t disp = sextract64(imm, 0, 21) << 4;
        tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next + disp);
        ctx->base.is_jmp = DISAS_NORETURN;
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
        switch (major) {
        case 0x0:
            /* nop/rsm and other privileged ops: treat as nop for now */
            break;
        case 0x1: {
            /* mov to/from control regs and region regs */
            uint8_t x3 = (insn >> 33) & 0x7;
            uint8_t x6 = (insn >> 27) & 0x3f;
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
            } else if (x3 == 0 && x6 == 0xa) {
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
            }
            /* mov rX = rr[rY] : ignore RR content, just clear destination */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = NULL;
            if (qp) {
                skip_label = gen_new_label();
                TCGv_i64 t_qp = tcg_temp_new_i64();
                tcg_gen_shri_i64(t_qp, cpu_pr, qp);
                tcg_gen_andi_i64(t_qp, t_qp, 1);
                tcg_gen_brcondi_i64(TCG_COND_EQ, t_qp, 0, skip_label);
            }
            uint8_t r1 = (insn >> 6) & 0x7f;
            if (r1 != 0) {
                tcg_gen_movi_i64(cpu_r[r1], 0);
            }
            if (skip_label) {
                gen_set_label(skip_label);
            }
            break;
        }
        case 0x4:
        case 0x5: {
            /* Integer loads/stores (M4/M5) */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint8_t x6 = extract64(insn, 30, 6);
            uint8_t x = extract64(insn, 27, 1);
            uint8_t r3 = extract64(insn, 20, 7);
            uint8_t r2 = extract64(insn, 13, 7);
            uint8_t r1 = extract64(insn, 6, 7);
            bool is_imm = (major == 0x5);
            int64_t imm9 = 0;

            if (is_imm) {
                uint64_t imm = extract64(insn, 6, 7) |
                               (extract64(insn, 27, 1) << 7) |
                               (extract64(insn, 36, 1) << 8);
                imm9 = sextract64(imm, 0, 9);
            }

            /* Base address */
            TCGv_i64 addr = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(addr, 0);
            } else {
                tcg_gen_mov_i64(addr, cpu_r[r3]);
            }
            if (is_imm && imm9) {
                tcg_gen_addi_i64(addr, addr, imm9);
            }

            if (x == 0 && (x6 == 0x3 || x6 == 0x7 || x6 == 0xb || x6 == 0xf)) {
                /* ld8 variants */
                if (r1 != 0) {
                    tcg_gen_qemu_ld_i64(cpu_r[r1], addr, ctx->mem_idx,
                                        MO_TE | MO_64);
                }
            } else if (x == 0 && x6 == 0x24) {
                /* ld1.c.nc variants: treat as zero-extended byte load */
                if (r1 != 0) {
                    tcg_gen_qemu_ld_i64(cpu_r[r1], addr, ctx->mem_idx,
                                        MO_TE | MO_UB);
                }
            } else if (x == 0 && (x6 == 0x33 || x6 == 0x37 || x6 == 0x3b)) {
                /* st8 / st8.rel / st8.spill variants */
                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                tcg_gen_qemu_st_i64(src, addr, ctx->mem_idx, MO_TE | MO_64);
                if (is_imm && r3 != 0 && imm9) {
                    /* Post-increment writeback */
                    tcg_gen_mov_i64(cpu_r[r3], addr);
                }
            } else {
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
            /* FP memory (M9/M10): handle stfe* using lower 64 bits. */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint8_t x6 = extract64(insn, 30, 6);
            uint8_t r3 = extract64(insn, 20, 7);
            uint8_t f2 = extract64(insn, 13, 7);
            uint8_t r2 = f2;
            bool is_imm = (major == 0x7);
            int64_t imm9 = 0;
            if (is_imm) {
                uint64_t imm = extract64(insn, 6, 7) |
                               (extract64(insn, 27, 1) << 7) |
                               (extract64(insn, 36, 1) << 8);
                imm9 = sextract64(imm, 0, 9);
            }
            TCGv_i64 addr = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(addr, 0);
            } else {
                tcg_gen_mov_i64(addr, cpu_r[r3]);
            }
            if (is_imm && imm9) {
                tcg_gen_addi_i64(addr, addr, imm9);
            }
            if (x6 == 0x30) {
                /* stfe/stfe.imm: store low 64 bits of f2 */
                TCGv_i64 src = gen_load_fp(f2, 0);
                tcg_gen_qemu_st_i64(src, addr, ctx->mem_idx, MO_TE | MO_64);
            } else if (x6 == 0x33 || x6 == 0x37 || x6 == 0x3b) {
                /* Some firmwares use st8.spill via M10 encoding; emulate. */
                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                tcg_gen_qemu_st_i64(src, addr, ctx->mem_idx, MO_TE | MO_64);
            } else {
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                gen_unimpl(ctx, insn, "M-slot fp");
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
        case 0x0:
            /* break / nop.i / loadrs */
            if (((insn >> 33) & 0x7) == 0 && ((insn >> 27) & 0x3f) == 0) {
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint64_t imm = ((insn >> 36) & 1ULL) << 20 | ((insn >> 6) & 0xfffff);
                TCGv_i64 timm = tcg_temp_new_i64();
                tcg_gen_movi_i64(timm, imm);
                TCGv_i64 ret = tcg_temp_new_i64();
                gen_helper_ssc(ret, tcg_env, timm);
                tcg_gen_mov_i64(cpu_r[8], ret);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (((insn >> 27) & 0x3f) == 0x2) {
                /* loadrs: we do not model RSE yet; treat as nop but emit a temp op */
                TCGv_i64 tmp = tcg_temp_new_i64();
                tcg_gen_movi_i64(tmp, 0);
                break;
            } else if (((insn >> 27) & 0x3f) == 0x1) {
                /* srlz.i placeholder */
                break;
            } else if (((insn >> 27) & 0x3f) == 0x8) {
                /* srlz.d placeholder */
                break;
            }
            /* nop.i class */
            break;
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
            uint8_t x2 = (insn >> 34) & 0x3;
            uint8_t x = (insn >> 33) & 0x1;
            if (x2 == 3 && x == 1) {
                /* dep r1 = imm1, r3, pos6, len6 (I14) */
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
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t pos = extract64(insn, 14, 6);
                uint8_t len = extract64(insn, 27, 6);
                uint64_t mask = (len == 64) ? ~0ULL :
                    ((len == 0) ? 0 : ((1ULL << len) - 1) << pos);
                uint64_t imm1 = extract64(insn, 36, 1);
                TCGv_i64 src = tcg_temp_new_i64();
                if (r3 == 0) tcg_gen_movi_i64(src, 0);
                else tcg_gen_mov_i64(src, cpu_r[r3]);
                if (mask) {
                    uint64_t chunk = (imm1 & ((len == 64) ? ~0ULL : ((1ULL << len) - 1))) << pos;
                    tcg_gen_andi_i64(src, src, ~mask);
                    TCGv_i64 tval = tcg_temp_new_i64();
                    tcg_gen_movi_i64(tval, chunk);
                    tcg_gen_or_i64(src, src, tval);
                }
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], src);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
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
        /* F-unit instructions */
        gen_unimpl(ctx, insn, "F-slot");
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

    if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
        qemu_log_mask(CPU_LOG_EXEC,
                      "IA64: pc=%016" PRIx64 " ri=%d tmpl=%02x slot=%d insn=%011" PRIx64 "\n",
                      ctx->base.pc_next, ctx->ri, template, type, insn);
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
        /* Update PSR.ri and IP */
        /* We need to save the new RI and IP */
        /* For now, just exit to main loop */
        tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
        /* We need a way to save RI. cpu_psr? */
        /* tcg_gen_andi_i64(cpu_psr, cpu_psr, ~PSR_RI_MASK); */
        /* tcg_gen_ori_i64(cpu_psr, cpu_psr, (uint64_t)ctx->ri << PSR_RI_SHIFT); */
        
        /* For now, assume we always exit at bundle boundary (ri=0) for simplicity in this skeleton */
        if (ctx->ri != 0) {
             /* If we stop in the middle of a bundle, we must save RI */
        }
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
