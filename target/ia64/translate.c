/*
 * IA-64 translation
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "exec/translator.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "disas/disas.h"
#include "exec/translation-block.h"
#include "qemu/log.h"
#include <inttypes.h>

static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_psr;
static TCGv_i64 cpu_cfm;
static TCGv_i64 cpu_pr;
static TCGv_i64 cpu_b[8];
static TCGv_i64 cpu_r[128];
static TCGv_i64 cpu_cr_iip;
static TCGv_i64 cpu_cr_ipsr;
static TCGv_i64 cpu_cr_ifs;

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
    cpu_cr_ifs = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_ifs),
                                        "cr_ifs");
    
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
}

typedef struct DisasContext {
    DisasContextBase base;
    CPUIA64State *env;
    int ri;
    uint64_t extra_bits;
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

static void ia64_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    ctx->env = cpu_env(cs);
    ctx->ri = ctx->base.tb->flags & 3;
    ctx->extra_bits = 0;
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
    uint8_t x2a = (insn >> 34) & 0x7;
    uint8_t x4 = (insn >> 29) & 0xf;
    uint8_t x2b = (insn >> 27) & 0x3;
    uint8_t r3 = (insn >> 20) & 0x7f;
    uint8_t r2 = (insn >> 13) & 0x7f;
    uint8_t r1 = (insn >> 6) & 0x7f;
    uint8_t qp = insn & 0x3f;
    
    TCGLabel *skip_label = NULL;
    
    if (qp != 0) {
        skip_label = gen_new_label();
        TCGv_i64 t_qp = tcg_temp_new_i64();
        tcg_gen_shri_i64(t_qp, cpu_pr, qp);
        tcg_gen_andi_i64(t_qp, t_qp, 1);
        tcg_gen_brcondi_i64(TCG_COND_EQ, t_qp, 0, skip_label);
    }

    if (major == 0x8 && x2a == 0 && x4 == 0 && x2b == 0) {
        /* add r1 = r2, r3 */
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
    }
    
    if (skip_label) {
        gen_set_label(skip_label);
    }

    /* Anything else in A-unit currently treated as a no-op placeholder. */
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
    } else {
        /* Simple br.cond immediate (B1-style target25 << 4). */
        uint64_t imm = extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
        int64_t disp = (int64_t)(imm << 4);
        tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next + disp);
        ctx->base.is_jmp = DISAS_NORETURN;
        tcg_gen_exit_tb(NULL, 0);
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
            if (x3 == 0 && x6 == 0x2c) {
                /* mov cr[r3] = r2 (M32 format) */
                uint8_t r2 = extract64(insn, 20, 7);
                uint8_t cr = extract64(insn, 13, 7);
                if (cr == 4) {
                    tcg_gen_mov_i64(cpu_cr_ipsr, cpu_r[r2]);
                } else if (cr == 6) {
                    tcg_gen_mov_i64(cpu_cr_iip, cpu_r[r2]);
                } else if (cr == 10) {
                    tcg_gen_mov_i64(cpu_cr_ifs, cpu_r[r2]);
                }
                break;
            } else if (x3 == 0 && x6 == 0x24) {
                /* mov r1 = cr[r3] (M33 format) */
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t cr = extract64(insn, 20, 7);
                TCGv_i64 t = tcg_temp_new_i64();
                if (cr == 4) {
                    tcg_gen_mov_i64(t, cpu_cr_ipsr);
                } else if (cr == 6) {
                    tcg_gen_mov_i64(t, cpu_cr_iip);
                } else if (cr == 10) {
                    tcg_gen_mov_i64(t, cpu_cr_ifs);
                } else {
                    tcg_gen_movi_i64(t, 0);
                }
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
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
            gen_unimpl(ctx, insn, "M-slot");
            break;
        }
        break;
    case SLOT_I:
        /* I-unit instructions */
        switch (major) {
        case 0x0:
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
