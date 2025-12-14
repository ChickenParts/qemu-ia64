/*
 * IA-64 helper routines
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/target_page.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "accel/tcg/cpu-ldst.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "exec/cpu-common.h"
#include "system/memory.h"
#include "system/address-spaces.h"

/*
 * SKI BitfR/BitfX index from the MSB; QEMU extract64() indexes from the LSB.
 * Convert the architectural field layout accordingly.
 *
 * RR layout (see Linux head.S SET_ONE_RR):
 * - rid: bits 8..31
 * - ps : bits 2..7  (log2 page size)
 * - ve : bit 0      (VHPT enable for the region)
 */
#define RR_RID(x)   extract64((x), 8, 24)
#define RR_PS(x)    extract64((x), 2, 6)
#define RR_VE(x)    extract64((x), 0, 1)
#define RR(idx)     env->rr[(idx) & 0x7]

/* PTA helpers */
#define PTA_VE(x)   extract64((x), 0, 1)
#define PTA_SIZE(x) extract64((x), 2, 6)
#define PTA_VF(x)   extract64((x), 8, 1)
#define PTA_BASE(x) extract64((x), 15, 46)
#define PTA_VRN(x)  extract64((x), 61, 3)

#define PTE_ED(x)   extract64((x), 52, 1)
#define PTE_AR(x)   extract64((x), 9, 3)
#define PTE_PL(x)   extract64((x), 7, 2)
#define PTE_D(x)    extract64((x), 6, 1)
#define PTE_A(x)    extract64((x), 5, 1)
#define PTE_MA(x)   extract64((x), 2, 3)
#define PTE_P(x)    extract64((x), 0, 1)
#define PTE_PPN(x)  extract64((x), 12, 38)

#define TAR_KEY(x)  extract64((x), 8, 24)
#define TAR_PS(x)   extract64((x), 2, 6)
#define TAR_P(x)    extract64((x), 0, 1)

/* Interrupt Status Register (see Linux arch/ia64/include/asm/kregs.h). */
#define IA64_ISR_X_BIT 32 /* execute access */
#define IA64_ISR_W_BIT 33 /* write access */
#define IA64_ISR_R_BIT 34 /* read access */
#define IA64_ISR_CODE_MASK 0xf

/* RSE-related AR indices */
#define IA64_AR_RSC       16
#define IA64_AR_BSP       17
#define IA64_AR_BSPSTORE  18
#define IA64_AR_RNAT      19

/* ar.rsc loadrs field: bits 16..29, in bytes (see SKI ssDSym.c). */
#define IA64_RSC_LOADRS_SHIFT 16
#define IA64_RSC_LOADRS_MASK  0x3fffULL

static inline uint64_t ia64_rsc_get_loadrs(uint64_t rsc)
{
    return (rsc >> IA64_RSC_LOADRS_SHIFT) & IA64_RSC_LOADRS_MASK;
}

static inline uint64_t ia64_rsc_set_loadrs(uint64_t rsc, uint64_t loadrs_bytes)
{
    rsc &= ~(IA64_RSC_LOADRS_MASK << IA64_RSC_LOADRS_SHIFT);
    rsc |= (loadrs_bytes & IA64_RSC_LOADRS_MASK) << IA64_RSC_LOADRS_SHIFT;
    return rsc;
}

static inline uint64_t ia64_rse_slot_num(uint64_t addr)
{
    return (addr >> 3) & 0x3f;
}

static inline uint64_t ia64_rse_skip_regs(uint64_t addr, int64_t num_regs)
{
    /*
     * The backing store inserts an RNAT slot every 63 registers (slot#63).
     * This helper mirrors SKI's ia64_rse_skip_regs().
     */
    int64_t delta = (int64_t)ia64_rse_slot_num(addr) + num_regs;
    if (num_regs < 0) {
        delta -= 0x3e;
    }
    return addr + ((uint64_t)(num_regs + delta / 0x3f) << 3);
}

static void ia64_rse_push_window(CPUIA64State *env);
static bool ia64_rse_pop_window(CPUIA64State *env);
static void ia64_intr_push_window(CPUIA64State *env);
static bool ia64_intr_pop_window(CPUIA64State *env);

static void ia64_switch_banks(CPUIA64State *env)
{
    for (int i = 0; i < 16; i++) {
        uint64_t tmp = env->banked_r[i];
        env->banked_r[i] = env->r[16 + i];
        env->r[16 + i] = tmp;
    }
}

static bool ia64_fault(CPUState *cs, CPUIA64State *env, bool is_data,
                       bool write, uint32_t vec, uint64_t iim,
                       uintptr_t retaddr)
{
    static uint64_t last_ip;
    static uint32_t last_vec;
    static int log_count;

    if (retaddr) {
        cpu_restore_state(cs, retaddr);
    }

    /* Optional fail-fast: stack pointer should never point into kernel .text. */
    const char *sp_text = getenv("QEMU_IA64_SP_IN_TEXT");
    if (sp_text && *sp_text && env->kernel_stext && env->kernel_etext) {
        uint64_t sp = env->r[12];
        if (sp >= env->kernel_stext && sp < env->kernel_etext) {
            fprintf(stderr,
                    "IA64: SP in .text on fault vec=0x%x ip=%016" PRIx64
                    " sp=%016" PRIx64 " stext=%016" PRIx64 " etext=%016" PRIx64
                    " is_data=%d write=%d\n",
                    vec, env->ip, sp, env->kernel_stext, env->kernel_etext,
                    is_data ? 1 : 0, write ? 1 : 0);
            fflush(stderr);
            if (strcmp(sp_text, "1") == 0 || strcmp(sp_text, "abort") == 0) {
                cpu_abort(cs, "IA64: SP in .text (QEMU_IA64_SP_IN_TEXT=%s)", sp_text);
            }
        }
    }

    /* Save interruption state */
    ia64_intr_push_window(env);
    env->cr_ipsr = env->psr;
    env->cr_iip = env->ip & ~0xFULL;
    env->cr_ifs = env->cfm;
    /*
     * Build ISR flags (X/W/R) and leave isr.code at 0 for normal accesses.
     * Linux uses bit 32 (IA64_ISR_X_BIT) to distinguish instruction misses.
     */
    uint64_t isr = 0;
    if (!is_data) {
        isr |= 1ULL << IA64_ISR_X_BIT;
        isr |= 1ULL << IA64_ISR_R_BIT; /* treat instruction fetch as read */
    } else if (write) {
        isr |= 1ULL << IA64_ISR_W_BIT;
    } else {
        isr |= 1ULL << IA64_ISR_R_BIT;
    }
    isr |= 0 & IA64_ISR_CODE_MASK;
    env->cr_isr = isr;
    env->cr_iim = iim;
    cs->exception_index = IA64_EXCP_BASE + vec;
    if (log_count < 64 || vec != last_vec || env->ip != last_ip) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64 fault vec=0x%x ip=0x%" PRIx64 " psr=0x%" PRIx64
                      " is_data=%d IIM=0x%lx IFA=0x%lx"
                      " IHA=0x%lx PTA=0x%lx ITIR=0x%lx"
                      " LAST_BR from=%016" PRIx64 " to=%016" PRIx64
                      " kind=%" PRIu64 " insn=%011" PRIx64
                      " ar.k3=0x%" PRIx64 " ar.k4=0x%" PRIx64
                      " ar.k6=0x%" PRIx64 " ar.k7=0x%" PRIx64
                      " r1=0x%" PRIx64 " r12=0x%" PRIx64 " r13=0x%" PRIx64
                      " r16=0x%" PRIx64 " r17=0x%" PRIx64
                      " r20=0x%" PRIx64 " r32=0x%" PRIx64 " r45=0x%" PRIx64 "\n",
                      vec, env->ip, env->psr, is_data, iim, env->cr_ifa,
                      env->cr_iha, env->cr[8], env->cr[21],
                      env->last_branch_from, env->last_branch_to,
                      env->last_branch_kind, env->last_branch_insn,
                      env->ar[3], env->ar[4], env->ar[6], env->ar[7],
                      env->r[1], env->r[12], env->r[13],
                      env->r[16], env->r[17],
                      env->r[20], env->r[32], env->r[45]);
        log_count++;
        last_ip = env->ip;
        last_vec = vec;
    }
    cpu_loop_exit_restore(cs, retaddr);
    return false;
}

/*
 * Access rights enforcement.
 *
 * Mirror SKI's accessRights() behavior: AR encodes a combined read/write/exec
 * policy with additional privilege-level constraints.
 */
enum {
    IA64_EXECUTE_ACCESS = 0,
    IA64_READ_ACCESS = 1,
    IA64_WRITE_ACCESS = 2,
};

static bool ia64_access_rights(uint8_t ar, uint8_t pl, uint8_t cpl,
                               MMUAccessType access_type)
{
    unsigned atype = IA64_EXECUTE_ACCESS;
    if (access_type == MMU_DATA_LOAD) {
        atype = IA64_READ_ACCESS;
    } else if (access_type == MMU_DATA_STORE) {
        atype = IA64_READ_ACCESS | IA64_WRITE_ACCESS;
    }
    atype &= IA64_READ_ACCESS | IA64_WRITE_ACCESS;

    switch (ar & 7) {
    case 0:
        if (atype != IA64_READ_ACCESS || cpl > pl) {
            return false;
        }
        break;
    case 1:
        if ((atype & IA64_WRITE_ACCESS) || cpl > pl) {
            return false;
        }
        break;
    case 2:
        if (atype == IA64_EXECUTE_ACCESS || cpl > pl) {
            return false;
        }
        break;
    case 3:
        if (cpl > pl) {
            return false;
        }
        break;
    case 4:
        if (atype == IA64_EXECUTE_ACCESS || cpl > pl) {
            return false;
        }
        if ((atype & IA64_WRITE_ACCESS) && cpl && cpl == pl) {
            return false;
        }
        break;
    case 5:
        if (cpl > pl) {
            return false;
        }
        if ((atype & IA64_WRITE_ACCESS) && cpl) {
            return false;
        }
        break;
    case 6:
        if (cpl > pl) {
            return false;
        }
        if (atype == IA64_EXECUTE_ACCESS && (!cpl || cpl < pl)) {
            return false;
        }
        break;
    case 7:
        if (atype & IA64_WRITE_ACCESS) {
            return false;
        }
        if (atype == IA64_READ_ACCESS && cpl) {
            return false;
        }
        break;
    default:
        return false;
    }

    return true;
}

static uint64_t ia64_translate_tlb(CPUIA64State *env, bool is_data, uint64_t va,
                                   uint32_t rid, bool *hit)
{
    const int n = ARRAY_SIZE(env->dtlb);
    for (int i = 0; i < n; i++) {
        const typeof(env->dtlb[0]) *e = is_data ? &env->dtlb[i] : &env->itlb[i];
        if (!e->valid || e->rid != rid) {
            continue;
        }
        uint64_t mask = ~((1ULL << e->ps) - 1);
        if ((va & mask) == e->tag) {
            *hit = true;
            return e->pa + (va & ~mask);
        }
    }
    *hit = false;
    return 0;
}

uint64_t HELPER(tpa)(CPUIA64State *env, uint64_t va)
{
    uint8_t rr_idx = extract64(va, 61, 3);
    /*
     * Linux uses:
     *  - region 7 (bit60==0) as a direct mapping of physical memory (__va()),
     *  - region 6 as an identity-mapped uncached I/O region.
     *
     * These do not require a translation structure lookup.
     */
    if (rr_idx == 7 && extract64(va, 60, 1) == 0) {
        return va & ((1ULL << 61) - 1);
    }
    if (rr_idx == 6) {
        return va & ((1ULL << 61) - 1);
    }

    uint64_t rr = env->rr[rr_idx];
    uint32_t rid = RR_RID(rr);
    bool hit = false;

    uint64_t pa = ia64_translate_tlb(env, true, va, rid, &hit);
    if (hit) {
        return pa;
    }

    /* Fall back to VHPT if enabled. */
    uint64_t pta = env->cr[8]; /* cr.pta */
    if (PTA_VE(pta) && RR_VE(rr) && (env->psr & IA64_PSR_DT)) {
        env->cr_ifa = va;
        uint64_t vhpt_addr = helper_thash(env);
        uint64_t pte = cpu_ldq_data(env, vhpt_addr);
        uint64_t tar = PTA_VF(pta) ? cpu_ldq_data(env, vhpt_addr + 8)
                                   : ((uint64_t)rid << 8) | ((uint64_t)RR_PS(rr) << 2);
        uint64_t tag = PTA_VF(pta) ? cpu_ldq_data(env, vhpt_addr + 16) : 0;
        uint64_t expected = helper_ttag(env);
        if (!PTA_VF(pta) || tag == expected) {
            uint8_t trans_ps = TAR_PS(tar);
            hwaddr pbase = (PTE_PPN(pte) << 12);
            if (PTE_P(pte) && TAR_P(tar)) {
                return pbase | (va & ((1ULL << trans_ps) - 1));
            }
        }
    }

    CPUState *cs = env_cpu(env);
    uintptr_t retaddr = GETPC();
    env->cr_ifa = va;
    uint8_t ps = RR_PS(rr);
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    env->cr[21] = ((uint64_t)ps << 2) | ((uint64_t)rid << 8);
    env->cr_iha = helper_thash(env);

    /*
     * No translation: raise the architected (alt) DTLB fault and ensure the
     * OS miss handler sees the same CR state as for a normal data access.
     */
    uint32_t vec = (env->psr & IA64_PSR_DT) ? IA64_VEC_DATA_TLB
                                           : IA64_VEC_ALT_DATA_TLB;
    ia64_fault(cs, env, true, false, vec, 0, retaddr);
    return 0;
}

void HELPER(fc)(CPUIA64State *env, uint64_t va)
{
    /*
     * Flush cache line for address in va (fc/fc.i).
     *
     * QEMU doesn't model caches, but Linux relies on fc+sync.i+srlz.i around
     * patching to ensure updated instructions are visible to fetch. Mirror
     * SKI's clearPdecode() by invalidating translated code covering this
     * address.
     */
    uint64_t pa = va;
    if (env->psr & IA64_PSR_DT) {
        pa = helper_tpa(env, va);
    }

    pa &= ~0x1fULL;
    tb_invalidate_phys_range(env_cpu(env), pa, pa + 31);
}

static void ia64_rse_ensure(CPUIA64State *env, uint32_t need)
{
    if (env->rse_capacity >= need) {
        return;
    }
    uint32_t new_cap = env->rse_capacity ? env->rse_capacity : 16;
    while (new_cap < need) {
        new_cap *= 2;
    }
    env->rse_frames = g_realloc_n(env->rse_frames, new_cap,
                                  sizeof(*env->rse_frames));
    env->rse_capacity = new_cap;
}

static void ia64_intr_ensure(CPUIA64State *env, uint32_t need)
{
    if (env->intr_capacity >= need) {
        return;
    }
    uint32_t new_cap = env->intr_capacity ? env->intr_capacity : 8;
    while (new_cap < need) {
        new_cap *= 2;
    }
    env->intr_frames = g_realloc_n(env->intr_frames, new_cap,
                                   sizeof(*env->intr_frames));
    env->intr_capacity = new_cap;
}

static void ia64_intr_push_window(CPUIA64State *env)
{
    ia64_intr_ensure(env, env->intr_depth + 1);
    struct IA64IntrFrame *frame = &env->intr_frames[env->intr_depth++];
    memcpy(frame->r, &env->r[32], sizeof(frame->r));
    frame->ar_pfs = env->ar[64]; /* ar.pfs */
}

static bool ia64_intr_pop_window(CPUIA64State *env)
{
    if (env->intr_depth == 0) {
        return false;
    }
    struct IA64IntrFrame *frame = &env->intr_frames[--env->intr_depth];
    memcpy(&env->r[32], frame->r, sizeof(frame->r));
    env->ar[64] = frame->ar_pfs;
    return true;
}

static void ia64_rse_push_window(CPUIA64State *env)
{
    ia64_rse_ensure(env, env->rse_depth + 1);
    struct IA64RSEFrame *frame = &env->rse_frames[env->rse_depth++];
    memcpy(frame->r, &env->r[32], sizeof(frame->r));
    frame->ar_pfs = env->ar[64]; /* ar.pfs */
    frame->cfm = env->cfm;
}

static bool ia64_rse_pop_window(CPUIA64State *env)
{
    if (env->rse_depth == 0) {
        return false;
    }
    struct IA64RSEFrame *frame = &env->rse_frames[--env->rse_depth];
    memcpy(&env->r[32], frame->r, sizeof(frame->r));
    env->ar[64] = frame->ar_pfs;
    env->cfm = frame->cfm;
    return true;
}

void HELPER(bsw)(CPUIA64State *env, uint32_t bn)
{
    bn &= 1;
    uint32_t cur_bn = (env->psr & IA64_PSR_BN) ? 1 : 0;
    if (cur_bn != bn) {
        ia64_switch_banks(env);
    }
    env->psr &= ~IA64_PSR_BN;
    env->psr |= bn ? IA64_PSR_BN : 0;
}

void HELPER(rfi)(CPUIA64State *env)
{
    static int rfi_log_count;
    uint64_t new_psr = env->cr_ipsr;
    uint32_t cur_bn = (env->psr & IA64_PSR_BN) ? 1 : 0;
    uint32_t new_bn = (new_psr & IA64_PSR_BN) ? 1 : 0;
    if (cur_bn != new_bn) {
        ia64_switch_banks(env);
    }
    /*
     * rfi returns from an interruption and restores the interrupted context
     * from cr.ipsr/cr.iip/cr.ifs.  It must not unwind normal call frames.
     *
     * cr.ifs has a validity bit at 63 (set by cover when PSR.ic=0).
     */
    env->cfm = env->cr_ifs & ~(1ULL << 63);

    /*
     * Linux also uses rfi as a control transfer during early boot (to switch
     * into the final virtual mapping) without taking an actual interruption.
     * In that case we have no saved interrupt window to restore.
     */
    if (env->intr_depth > 0) {
        (void)ia64_intr_pop_window(env);
    }

    env->last_branch_from = env->ip;
    env->last_branch_to = env->cr_iip & ~0xFULL;
    env->last_branch_insn = 0;
    env->last_branch_kind = 7 | (((env->psr >> PSR_RI_SHIFT) & 3) << 8);
    if (rfi_log_count < 32 || env->cr_iip == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rfi ip=%016" PRIx64 " psr=%016" PRIx64
                      " -> cr_iip=%016" PRIx64 " cr_ipsr=%016" PRIx64
                      " cr_ifs=%016" PRIx64 " cr_iim=%016" PRIx64
                      " r12=%016" PRIx64 " r13=%016" PRIx64 " r28=%016" PRIx64 "\n",
                      env->ip, env->psr,
                      env->cr_iip, env->cr_ipsr, env->cr_ifs, env->cr_iim,
                      env->r[12], env->r[13], env->r[28]);
        rfi_log_count++;
    }
    env->psr = new_psr;
    env->ip = env->cr_iip & ~0xFULL;
}

static uint64_t ia64_dbg_next_call_pc;

void HELPER(dbg_call)(CPUIA64State *env, uint64_t pc)
{
    static int log_count;
    ia64_dbg_next_call_pc = pc;
    if (log_count >= 32) {
        return;
    }
    if (pc == 0xa0000001000665c0ULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dbg_call pc=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                      " pr=%016" PRIx64 " p2=%d"
                      " r13=%016" PRIx64 " r36=%016" PRIx64 " r38=%016" PRIx64
                      " r46(out0)=%016" PRIx64 "\n",
                      pc, env->psr, env->cfm, env->pr, (int)((env->pr >> 2) & 1),
                      env->r[13], env->r[36], env->r[38], env->r[46]);
    } else {
        uint8_t sof = env->cfm & 0x7f;
        uint8_t sol = (env->cfm >> 7) & 0x7f;
        uint8_t outs = (sof > sol) ? (sof - sol) : 0;
        uint8_t out0 = 32 + sol;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dbg_call pc=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                      " sof=%u sol=%u outs=%u out0=r%u"
                      " out0..4=%016" PRIx64 " %016" PRIx64 " %016" PRIx64
                      " %016" PRIx64 " %016" PRIx64 "\n",
                      pc, env->psr, env->cfm, sof, sol, outs, out0,
                      env->r[out0 + 0], env->r[out0 + 1], env->r[out0 + 2],
                      env->r[out0 + 3], env->r[out0 + 4]);
    }
    log_count++;
}

void HELPER(setf_sig)(CPUIA64State *env, uint32_t f1, uint64_t val)
{
    f1 &= 0x7f;
    if (f1 <= 1) {
        return;
    }
    env->f[f1][0] = val;
    /*
     * Match SKI's dword2freg(): treat the 64-bit payload as an unnormalized
     * significand with an initial exponent of bias+63.
     *
     * fnorm + getf.exp then yields bias+msb_index, which Linux uses for fls().
     */
    env->f[f1][1] = IA64_FP_SEXP(0, IA64_FP_EXP_INTEGER);
}

uint64_t HELPER(getf_sig)(CPUIA64State *env, uint32_t f2)
{
    f2 &= 0x7f;
    return env->f[f2][0];
}

void HELPER(xma_l)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                   uint32_t f4, uint32_t f2)
{
    f1 &= 0x7f;
    f2 &= 0x7f;
    f3 &= 0x7f;
    f4 &= 0x7f;
    if (f1 <= 1) {
        return;
    }
    __uint128_t prod = (__uint128_t)env->f[f3][0] * (__uint128_t)env->f[f4][0];
    __uint128_t sum = prod + (__uint128_t)env->f[f2][0];
    env->f[f1][0] = (uint64_t)sum;
    env->f[f1][1] = 0;
}

void HELPER(xma_h)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                   uint32_t f4, uint32_t f2)
{
    f1 &= 0x7f;
    f2 &= 0x7f;
    f3 &= 0x7f;
    f4 &= 0x7f;
    if (f1 <= 1) {
        return;
    }
    __int128 prod = (__int128)(int64_t)env->f[f3][0] * (__int128)(int64_t)env->f[f4][0];
    __int128 sum = prod + (__int128)(int64_t)env->f[f2][0];
    env->f[f1][0] = (uint64_t)(sum >> 64);
    env->f[f1][1] = 0;
}

void HELPER(xma_hu)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                    uint32_t f4, uint32_t f2)
{
    f1 &= 0x7f;
    f2 &= 0x7f;
    f3 &= 0x7f;
    f4 &= 0x7f;
    if (f1 <= 1) {
        return;
    }
    __uint128_t prod = (__uint128_t)env->f[f3][0] * (__uint128_t)env->f[f4][0];
    __uint128_t sum = prod + (__uint128_t)env->f[f2][0];
    env->f[f1][0] = (uint64_t)(sum >> 64);
    env->f[f1][1] = 0;
}

/*
 * libgcc integer division helpers.
 *
 * IA-64 toolchains often lower 32/64-bit division/modulus to calls into these
 * helper functions. The out-of-line implementations use floating-point
 * instructions (fnorm/frcpa/fma/...) which we don't model yet.
 *
 * Emulate the architectural effect directly so that the kernel and runtime
 * can make forward progress while the full FP unit is brought up.
 */
uint64_t HELPER(divdi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    int64_t sa = (int64_t)a;
    int64_t sb = (int64_t)b;
    if (sb == 0) {
        return 0;
    }
    return (uint64_t)(sa / sb);
}

uint64_t HELPER(udivdi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    if (b == 0) {
        return 0;
    }
    return a / b;
}

uint64_t HELPER(moddi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    int64_t sa = (int64_t)a;
    int64_t sb = (int64_t)b;
    if (sb == 0) {
        return 0;
    }
    return (uint64_t)(sa % sb);
}

uint64_t HELPER(umoddi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    if (b == 0) {
        return 0;
    }
    return a % b;
}

uint64_t HELPER(divsi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    int32_t sa = (int32_t)a;
    int32_t sb = (int32_t)b;
    if (sb == 0) {
        return 0;
    }
    return (uint64_t)(int64_t)(sa / sb);
}

uint64_t HELPER(udivsi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;
    if (ub == 0) {
        return 0;
    }
    return (uint64_t)(ua / ub);
}

uint64_t HELPER(modsi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    int32_t sa = (int32_t)a;
    int32_t sb = (int32_t)b;
    if (sb == 0) {
        return 0;
    }
    return (uint64_t)(int64_t)(sa % sb);
}

uint64_t HELPER(umodsi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;
    if (ub == 0) {
        return 0;
    }
    return (uint64_t)(ua % ub);
}

void HELPER(breaki)(CPUIA64State *env, uint64_t iim)
{
    CPUState *cs = env_cpu(env);
    ia64_fault(cs, env, false, false, IA64_VEC_BREAK, iim, GETPC());
}

void HELPER(dbg_probe)(CPUIA64State *env, uint64_t pc, uint32_t ri)
{
    typedef struct {
        uint64_t pc;
        uint32_t count;
    } DbgProbeCount;

    static DbgProbeCount probes[64];
    static uint32_t nprobes;

    bool abort_panic = false;
    uint32_t idx;
    for (idx = 0; idx < nprobes; idx++) {
        if (probes[idx].pc == pc) {
            break;
        }
    }
    if (idx == nprobes) {
        if (nprobes >= ARRAY_SIZE(probes)) {
            return;
        }
        probes[idx].pc = pc;
        probes[idx].count = 0;
        nprobes++;
    }

    if (probes[idx].count++ >= 8) {
        return;
    }

    /*
     * Some probes are placed on bundles that contain predicated call sites.
     * Filter those down to the interesting (predicate-true) cases to avoid
     * drowning in noise.
     */
    if (pc == 0xa000000101197f30ULL) { /* format_decode WARN_ONCE bundle */
        if (((env->pr >> 7) & 1) == 0) {
            return;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "dbg_probe pc=%016" PRIx64 " ri=%u"
                  " psr=%016" PRIx64 " cfm=%016" PRIx64 " depth=%u"
                  " cr_ifa=%016" PRIx64 " cr_iha=%016" PRIx64
                  " pta=%016" PRIx64 " itir=%016" PRIx64
                  " ar.k6=%016" PRIx64
                  " r1=%016" PRIx64 " r8=%016" PRIx64 " r9=%016" PRIx64
                  " r10=%016" PRIx64 " r11=%016" PRIx64
                  " r12=%016" PRIx64 " r14=%016" PRIx64
                  " r16=%016" PRIx64 " r17=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64
                  " b0=%016" PRIx64 " b6=%016" PRIx64 " r45=%016" PRIx64 "\n",
                  pc, ri,
                  env->psr, env->cfm, env->rse_depth,
                  env->cr_ifa, env->cr_iha, env->cr[8], env->cr[21],
                  env->ar[6],
                  env->r[1], env->r[8], env->r[9], env->r[10], env->r[11],
                  env->r[12], env->r[14], env->r[16], env->r[17],
                  env->r[32], env->r[33], env->r[34], env->r[35],
                  env->r[36], env->r[37], env->b[0], env->b[6], env->r[45]);

    if (pc == 0xa000000100073da0ULL || pc == 0xa000000100073620ULL) {
        static int abort_on_panic = -1;
        if (abort_on_panic == -1) {
            const char *s = getenv("QEMU_IA64_ABORT_PANIC");
            abort_on_panic = (s && *s) ? 1 : 0;
        }
        abort_panic = abort_on_panic;

        qemu_log_mask(LOG_GUEST_ERROR,
                      "panic_trace pc=%016" PRIx64 " ri=%u ip=%016" PRIx64
                      " psr=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                      pc, ri, env->ip, env->psr, env->cfm);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "panic_trace last_br from=%016" PRIx64 " to=%016" PRIx64
                      " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                      env->last_branch_from, env->last_branch_to,
                      env->last_branch_kind, env->last_branch_insn);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "panic_trace last_b0_write pc=%016" PRIx64 " val=%016" PRIx64
                      " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                      env->last_b0_write_pc, env->last_b0_write_val,
                      env->last_b0_write_kind, env->last_b0_write_insn);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "panic_trace prev_b0_write pc=%016" PRIx64 " val=%016" PRIx64
                      " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                      env->prev_b0_write_pc, env->prev_b0_write_val,
                      env->prev_b0_write_kind, env->prev_b0_write_insn);
        for (int i = 0; i < 16; i++) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "panic_trace b0_trace[%02d] pc=%016" PRIx64 " val=%016" PRIx64
                          " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                          i, env->b0_trace_pc[i], env->b0_trace_val[i],
                          env->b0_trace_kind[i], env->b0_trace_insn[i]);
        }
    }

    if (pc == 0xa000000101197f30ULL) {
        uint64_t ptr = env->r[32];
        uint8_t b[8];
        for (int i = 0; i < 8; i++) {
            b[i] = cpu_ldub_data(env, ptr - 4 + i);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "format_decode_warn fmt.str=%016" PRIx64 " r15=%016" PRIx64
                      " bytes[-4..+3]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                      ptr, env->r[15],
                      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }

    if (pc == 0xa00000010002de00ULL) { /* die() */
        uint64_t str = env->r[32];
        char msg[160];
        size_t n = 0;
        for (; n + 1 < sizeof(msg); n++) {
            uint8_t c = cpu_ldub_data(env, str + n);
            if (c == 0) {
                break;
            }
            if (c < 0x20 || c > 0x7e) {
                msg[n] = '.';
            } else {
                msg[n] = (char)c;
            }
        }
        msg[n] = '\0';
        qemu_log_mask(LOG_GUEST_ERROR,
                      "die_str pc=%016" PRIx64 " str=%016" PRIx64 " \"%s\"\n",
                      pc, str, msg);
    }

    if (pc == 0xa000000100073da0ULL || pc == 0xa000000100073620ULL) {
        uint64_t fmt = env->r[32];
        char msg[160];
        size_t n = 0;
        for (; n + 1 < sizeof(msg); n++) {
            uint8_t c = cpu_ldub_data(env, fmt + n);
            if (c == 0) {
                break;
            }
            if (c < 0x20 || c > 0x7e) {
                msg[n] = '.';
            } else {
                msg[n] = (char)c;
            }
        }
        msg[n] = '\0';
        qemu_log_mask(LOG_GUEST_ERROR,
                      "panic_fmt pc=%016" PRIx64 " fmt=%016" PRIx64 " \"%s\"\n",
                      pc, fmt, msg);

        if (abort_panic) {
            CPUState *cs = env_cpu(env);
            cpu_restore_state(cs, GETPC());
            cpu_abort(cs, "IA64: aborting on guest panic pc=%016" PRIx64, pc);
        }
    }
}

void HELPER(record_b0_trace)(CPUIA64State *env, uint64_t pc, uint64_t insn,
                             uint64_t kind, uint64_t val)
{
    uint32_t idx = env->b0_trace_idx & 15U;
    env->b0_trace_pc[idx] = pc;
    env->b0_trace_insn[idx] = insn;
    env->b0_trace_kind[idx] = kind;
    env->b0_trace_val[idx] = val;
    env->b0_trace_idx = (idx + 1) & 15U;
}

uint64_t HELPER(alloc)(CPUIA64State *env, uint64_t sof, uint64_t sol, uint64_t sor)
{
    /*
     * The register stack engine (RSE) creates a new register frame of size SOF
     * with SOL locals and SOR rotating registers.  We model the architectural
     * register numbers as a window (r32..r127) and rely on helper_call() to
     * move caller OUT registers into callee IN registers.
     *
     * alloc itself only changes CFM/ar.pfs and (optionally) clears newly
     * allocated stacked registers.
     */
    uint64_t old_pfs = env->ar[64]; /* ar.pfs */
    uint64_t old_cfm = env->cfm;
    uint8_t old_sof = old_cfm & 0x7f;

    env->cfm = (sof & 0x7f) | ((sol & 0x7f) << 7) | ((sor & 0xf) << 14);

    /* Clear newly allocated stacked regs (beyond previous SOF). */
    if (sof > old_sof) {
        uint8_t n = MIN((uint8_t)sof, (uint8_t)96);
        for (uint8_t i = old_sof; i < n; i++) {
            env->r[32 + i] = 0;
        }
    }

    return old_pfs;
}

void HELPER(call)(CPUIA64State *env)
{
    /*
     * On a call, the caller's OUT registers become the callee's IN registers.
     * OUT0 starts at r32+SOL and there are (SOF-SOL) outputs in the current
     * frame.
     *
     * We snapshot the full stacked window for return, then build a new window
     * with IN regs copied from the caller's OUT regs.  The callee will run its
     * own alloc to set up locals/outs.
     */
    uint64_t caller_cfm = env->cfm;
    uint8_t sof = caller_cfm & 0x7f;
    uint8_t sol = (caller_cfm >> 7) & 0x7f;
    uint8_t outs = (sof > sol) ? (sof - sol) : 0;
    uint64_t tmp[96] = { 0 };
    uint64_t dbg_pc = ia64_dbg_next_call_pc;
    if (dbg_pc) {
        uint8_t out0 = 32 + sol;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "call_map pc=%016" PRIx64 " cfm=%016" PRIx64 " sof=%u sol=%u outs=%u out0=r%u"
                      " out0..4=%016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
                      dbg_pc, env->cfm, sof, sol, outs, out0,
                      env->r[out0 + 0], env->r[out0 + 1], env->r[out0 + 2],
                      env->r[out0 + 3], env->r[out0 + 4]);
    }

    ia64_rse_push_window(env);

    outs = MIN(outs, (uint8_t)96);
    if (sol < 96) {
        uint8_t max_copy = MIN(outs, (uint8_t)(96 - sol));
        for (uint8_t i = 0; i < max_copy; i++) {
            tmp[i] = env->r[32 + sol + i];
        }
    }
    memcpy(&env->r[32], tmp, sizeof(tmp));
    if (dbg_pc) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "call_map pc=%016" PRIx64 " mapped in0..4=%016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
                      dbg_pc, env->r[32], env->r[33], env->r[34], env->r[35], env->r[36]);
        ia64_dbg_next_call_pc = 0;
    }

    /* Pre-alloc CFM for callee: treat all IN regs as locals. */
    env->cfm = (outs & 0x7f) | ((outs & 0x7f) << 7);

    /*
     * br.call seeds ar.pfs for the callee. Model this as the caller's CFM,
     * which alloc will return in r1 and then replace with the callee's
     * pre-alloc CFM.
     */
    env->ar[64] = caller_cfm;
}

void HELPER(ret_restore)(CPUIA64State *env)
{
    static int log_count;
    if (log_count < 64) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ret_restore ip=0x%" PRIx64 " b0=0x%" PRIx64
                      " cfm=0x%" PRIx64 " depth=%u\n",
                      env->ip, env->b[0], env->cfm, env->rse_depth);
        log_count++;
    }
    (void)ia64_rse_pop_window(env);
}

void HELPER(ret_restore_b0)(CPUIA64State *env)
{
    /*
     * Some code sequences (notably PAL call paths) use "br.ia b0" as a return
     * control transfer without a matching br.call that created a new stacked
     * register frame.  Only unwind our modeled RSE window if b0 was last
     * written by br.call/brl.call.
     */
    uint8_t kind = env->last_b0_write_kind & 0xff;
    bool do_pop = (kind == 1);
    static int log_count;

    if (log_count < 64) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ret_restore_b0 ip=0x%" PRIx64 " b0=0x%" PRIx64
                      " last_b0_kind=%u depth=%u pop=%d\n",
                      env->ip, env->b[0], kind, env->rse_depth, do_pop);
        log_count++;
    }

    if (do_pop) {
        (void)ia64_rse_pop_window(env);
    }
}

static void ia64_insert_tlb(CPUIA64State *env, bool is_data, uint64_t va,
                            uint64_t pa, uint32_t rid, uint8_t ps,
                            uint8_t ar, uint8_t pl, uint8_t d, uint8_t a,
                            uint8_t p, uint8_t ed);

bool ia64_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                       MMUAccessType access_type, int mmu_idx,
                       bool probe, uintptr_t retaddr)
{
    if (probe) {
        return false;
    }

    /* IA64CPU *cpu = IA64_CPU(cs); */
    CPUIA64State *env = cpu_env(cs);
    bool is_fetch = (access_type == MMU_INST_FETCH);

    /*
     * Linux uses region 7 addresses (__va()) as a direct map of physical memory.
     * However, region 7 also contains other (non-identity) kernel addresses
     * such as the negative percpu range. Only treat region 7 with bit60==0
     * (i.e. VA in [RGN_BASE(7), RGN_BASE(7)+2^60)) as identity-mapped.
     */
    if (extract64(address, 61, 3) == 7 && extract64(address, 60, 1) == 0) {
        hwaddr phys_addr = address & ((1ULL << 61) - 1);
        int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        tlb_set_page(cs, address & TARGET_PAGE_MASK,
                     phys_addr & TARGET_PAGE_MASK, prot,
                     mmu_idx, TARGET_PAGE_SIZE);
        return true;
    }

    /*
     * Linux uses region 6 as an identity-mapped uncached I/O region
     * (RGN_UNCACHED). Treat it as a direct physical mapping.
     */
    if (extract64(address, 61, 3) == 6) {
        hwaddr phys_addr = address & ((1ULL << 61) - 1);
        int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        tlb_set_page(cs, address & TARGET_PAGE_MASK,
                     phys_addr & TARGET_PAGE_MASK, prot,
                     mmu_idx, TARGET_PAGE_SIZE);
        return true;
    }

    /*
     * Physical mode: if translation is disabled for this access, treat the
     * address as a physical address and never raise translation faults.
     *
     * IA-64 Linux head.S expects to start with IT/DT off and will install TRs
     * before switching into virtual mode.
     */
    if ((is_fetch && !(env->psr & IA64_PSR_IT)) ||
        (!is_fetch && !(env->psr & IA64_PSR_DT))) {
        hwaddr phys_addr = address;
        int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        tlb_set_page(cs, address & TARGET_PAGE_MASK,
                     phys_addr & TARGET_PAGE_MASK, prot,
                     mmu_idx, TARGET_PAGE_SIZE);
        return true;
    }

    /*
     * Linux IA-64 kernels may place the percpu PT_LOAD segment in region 7
     * high addresses (e.g. 0xfffffffffffc0000). If present, map it directly
     * to the segment's physical address to avoid early-boot fault recursion
     * before the kernel installs its final translation structures.
     */
    if (env->percpu_size) {
        uint64_t va = env->percpu_va_base;
        uint64_t sz = env->percpu_size;
        if (address >= va && address < va + sz) {
            hwaddr phys_addr = env->percpu_pa_base + (address - va);
            int prot = PAGE_READ | PAGE_WRITE;
            if (is_fetch) {
                prot |= PAGE_EXEC;
            }
            tlb_set_page(cs, address & TARGET_PAGE_MASK,
                         phys_addr & TARGET_PAGE_MASK, prot,
                         mmu_idx, TARGET_PAGE_SIZE);
            return true;
        }
    }

    /* Region / page size info */
    uint8_t rr_idx = extract64(address, 61, 3);
    uint64_t rr = env->rr[rr_idx];
    uint32_t rid = RR_RID(rr);
    uint8_t ps = RR_PS(rr);
    if (ps == 0) {
        ps = 12;
    }

    hwaddr phys_addr = address;
    bool hit = false;
    const typeof(env->dtlb[0]) *match = NULL;

    /*
     * Check instruction vs data TLBs. Rid must match; page mask uses ps.
     */
    if (access_type == MMU_INST_FETCH) {
        for (int i = 0; i < ARRAY_SIZE(env->itlb); i++) {
            if (!env->itlb[i].valid) {
                continue;
            }
            if (env->itlb[i].rid != rid) {
                continue;
            }
            uint64_t mask = ~((1ULL << env->itlb[i].ps) - 1);
            if ((address & mask) == env->itlb[i].tag) {
                phys_addr = env->itlb[i].pa + (address & ~mask);
                match = &env->itlb[i];
                hit = true;
                break;
            }
        }
    } else {
        for (int i = 0; i < ARRAY_SIZE(env->dtlb); i++) {
            if (!env->dtlb[i].valid) {
                continue;
            }
            if (env->dtlb[i].rid != rid) {
                continue;
            }
            uint64_t mask = ~((1ULL << env->dtlb[i].ps) - 1);
            if ((address & mask) == env->dtlb[i].tag) {
                phys_addr = env->dtlb[i].pa + (address & ~mask);
                match = &env->dtlb[i];
                hit = true;
                break;
            }
        }
    }

    if (!hit) {
        bool is_data = access_type != MMU_INST_FETCH;
        bool write = (access_type == MMU_DATA_STORE);
        uint32_t vec;

        /*
         * Determine the precise faulting PC for nested-miss heuristics.
         * (ia64_fault() will restore state again; this is fine for now.)
         */
        if (retaddr) {
            cpu_restore_state(cs, retaddr);
        }

        if (is_data) {
            vec = (env->psr & IA64_PSR_DT) ? IA64_VEC_DATA_TLB
                                           : IA64_VEC_ALT_DATA_TLB;
        } else {
            vec = (env->psr & IA64_PSR_IT) ? IA64_VEC_INST_TLB
                                           : IA64_VEC_ALT_INST_TLB;
        }

        /*
         * Nested DTLB misses: Linux's IVT miss/ABit/DBit handlers expect a
         * nested-DTLB vector when the VMLPT access faults.
         *
         * Heuristic: any data TLB miss while executing inside the first
         * (0x2c00) bytes of the IVT is treated as nested.
         */
        bool nested = false;
        if (is_data && vec == IA64_VEC_DATA_TLB && env->cr[2]) {
            uint64_t iva = env->cr[2];
            uint64_t ip = env->ip & ~0xFULL;
            if (ip >= iva + IA64_VEC_INST_TLB && ip < iva + IA64_VEC_BREAK) {
                nested = true;
                vec = IA64_VEC_DATA_NESTED_TLB;
            }
        }

        if (!nested) {
            /*
             * These control registers are architecturally updated only on
             * translation faults (not on QEMU softmmu host TLB refills).
             */
            env->cr_ifa = address;
            env->cr[21] = ((uint64_t)ps << 2) | ((uint64_t)rid << 8);
            env->cr_iha = helper_thash(env);

            /*
             * VHPT translation vector: if the VHPT walker is enabled for this
             * region and the virtual page table page is not mapped yet, Linux
             * expects to take the VHPT vector (entry 0) rather than recurse.
             */
            uint64_t pta = env->cr[8]; /* cr.pta */
            if (PTA_VE(pta) && RR_VE(rr)) {
                uint64_t iha = env->cr_iha;
                uint8_t iha_rr_idx = extract64(iha, 61, 3);
                uint32_t iha_rid = RR_RID(env->rr[iha_rr_idx]);
                bool iha_hit = false;
                (void)ia64_translate_tlb(env, true, iha, iha_rid, &iha_hit);
                if (!iha_hit) {
                    vec = IA64_VEC_VHPT_INST;
                }
            }
        }

        return ia64_fault(cs, env, is_data, write, vec, 0, retaddr);
    }

    /*
     * Enforce access/dirty rights from the inserted TLB entry.
     *
     * Linux relies on access-bit/dirty-bit faults to set A/D in software and
     * expects kernel text to be non-writable. Without enforcement, deep fault
     * recursion can silently corrupt .opd/.text and derail control flow.
     */
    uint8_t cpl = IA64_PSR_CPL(env->psr);
    bool is_data = access_type != MMU_INST_FETCH;
    bool write = access_type == MMU_DATA_STORE;
    uint32_t vec = 0;

    if (match && !match->p) {
        vec = IA64_VEC_DATA_PAGE_NOT_P;
    } else if (match && !match->a) {
        vec = is_data ? IA64_VEC_DATA_ACCESS_BIT : IA64_VEC_INST_ACCESS_BIT;
    } else if (write && match && !match->d) {
        vec = IA64_VEC_DATA_DIRTY;
    } else if (match &&
               !ia64_access_rights(match->ar, match->pl, cpl, access_type)) {
        vec = is_data ? IA64_VEC_DATA_ACCESS_RIGHTS : IA64_VEC_INST_ACCESS_RIGHTS;
    }

    if (vec) {
        if (retaddr) {
            cpu_restore_state(cs, retaddr);
        }
        env->cr_ifa = address;
        env->cr[21] = ((uint64_t)ps << 2) | ((uint64_t)rid << 8);
        env->cr_iha = helper_thash(env);
        return ia64_fault(cs, env, is_data, write, vec, 0, retaddr);
    }

    int prot = 0;
    if (match && match->a &&
        ia64_access_rights(match->ar, match->pl, cpl, MMU_DATA_LOAD)) {
        prot |= PAGE_READ;
    }
    if (match && match->a && match->d &&
        ia64_access_rights(match->ar, match->pl, cpl, MMU_DATA_STORE)) {
        prot |= PAGE_WRITE;
    }
    if (match && match->a &&
        ia64_access_rights(match->ar, match->pl, cpl, MMU_INST_FETCH)) {
        prot |= PAGE_EXEC;
    }
    if (!prot) {
        prot = PAGE_READ;
    }

    /* Bringup: confirm kernel .data mapping for console_srcu page. */
    if ((address & TARGET_PAGE_MASK) == 0xa000000101f54000ULL) {
        static int log_count;
        if (log_count < 16) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tlb_map console_srcu_page va=%016" PRIx64
                          " -> pa=%016" HWADDR_PRIx " ps=%u rid=0x%x prot=%x\n",
                          (uint64_t)address, phys_addr, match ? match->ps : 0,
                          rid, prot);
            log_count++;
        }
    }
    tlb_set_page(cs, address & TARGET_PAGE_MASK,
                 phys_addr & TARGET_PAGE_MASK, prot,
                 mmu_idx, TARGET_PAGE_SIZE);

    return true;
}

void HELPER(check_null_branch)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                               uint64_t insn, uint64_t to)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_ABORT_NULL_BRANCH");
        enabled = (s && *s) ? 1 : 0;
    }
    if (!enabled) {
        return;
    }
    if (to == 0) {
        CPUState *cs = env_cpu(env);
        cpu_restore_state(cs, GETPC());
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: null branch pc=%016" PRIx64 " ri=%u insn=%011" PRIx64
                      " ip=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                      " b0=%016" PRIx64 " b6=%016" PRIx64
                      " r12=%016" PRIx64 " r14=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                      " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64 "\n",
                      pc, ri, insn,
                      env->ip, env->psr, env->cfm,
                      env->b[0], env->b[6],
                      env->r[12], env->r[14],
                      env->r[32], env->r[33], env->r[34], env->r[35],
                      env->r[36], env->r[37]);
        cpu_abort(cs,
                  "IA64: null branch target pc=%016" PRIx64 " ri=%u insn=%011" PRIx64,
                  pc, ri, insn);
    }
}

typedef struct {
    FILE *fp;
    uint64_t last_count;
} SscFile;

static SscFile ssc_files[16];

static void ia64_ssc_write(CPUIA64State *env, uint64_t addr, const void *buf,
                           size_t len)
{
    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
}

static void ia64_ssc_read(CPUIA64State *env, uint64_t addr, void *buf,
                          size_t len)
{
    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
}

uint64_t HELPER(ssc)(CPUIA64State *env, uint64_t imm)
{
    /* SKI convention: break imm selects SSC dispatcher, r15 holds SSC number. */
    uint64_t nr = env->r[15];
    uint64_t arg0 = env->r[32];
    uint64_t arg1 = env->r[33];
    uint64_t arg2 = env->r[34];
    uint64_t arg3 = env->r[35];

    switch (nr) {
    case 0:
        /* SSC_STOP */
        cpu_restore_state(env_cpu(env), GETPC());
        cpu_abort(env_cpu(env), "IA64: SSC_STOP imm=%" PRIu64, imm);
    case 20: /* SSC_CONSOLE_INIT */
        return 0;
    case 31: /* SSC_PUTCHAR */
        fputc((int)arg0 & 0xff, stderr);
        fflush(stderr);
        return 0;
    case 75: { /* SSC_GET_ARGS */
        /* Return kernel path and args as single string. */
        static const char args[] = "stuff/vmlinux-ia64-main";
        ia64_ssc_write(env, arg0, args, sizeof(args));
        return sizeof(args);
    }
    case 50: { /* SSC_OPEN */
        char path[512];
        ia64_ssc_read(env, arg0, path, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        qemu_log_mask(LOG_GUEST_ERROR, "SSC_OPEN '%s'\n", path);
        for (int i = 0; i < 16; i++) {
            if (!ssc_files[i].fp) {
                const char *mode = "rb";
                if (arg1 == 2) { /* SSC_WRITE_ACCESS */
                    mode = "wb";
                } else if (arg1 == 3) { /* SSC_READ_ACCESS | SSC_WRITE_ACCESS */
                    mode = "r+b";
                }
                ssc_files[i].fp = fopen(path, mode);
                if (!ssc_files[i].fp) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "SSC_OPEN failed errno=%d\n", errno);
                    return -1;
                }
                qemu_log_mask(LOG_GUEST_ERROR, "SSC_OPEN fd=%d\n", i + 3);
                return i + 3;
            }
        }
        return -1;
    }
    case 51: { /* SSC_CLOSE */
        int fd = arg0 - 3;
        if (fd >= 0 && fd < 16 && ssc_files[fd].fp) {
            fclose(ssc_files[fd].fp);
            ssc_files[fd].fp = NULL;
            ssc_files[fd].last_count = 0;
            return 0;
        }
        return -1;
    }
    case 52: { /* SSC_READ */
        int fd = arg0 - 3;
        if (fd < 0 || fd >= 16 || !ssc_files[fd].fp) {
            return -1;
        }
        struct QEMU_PACKED {
            uint64_t addr;
            uint32_t len;
            uint32_t pad;
        } req;
        uint64_t req_ptr = arg2;
        uint64_t off = arg3;
        uint64_t total = 0;
        fseeko(ssc_files[fd].fp, off, SEEK_SET);
        for (uint64_t i = 0; i < arg1; i++, req_ptr += sizeof(req)) {
            ia64_ssc_read(env, req_ptr, &req, sizeof(req));
            if (req.len == 0) {
                continue;
            }
            uint8_t *tmp = g_malloc(req.len);
            size_t n = fread(tmp, 1, req.len, ssc_files[fd].fp);
            ia64_ssc_write(env, req.addr, tmp, n);
            g_free(tmp);
            total += n;
            if (n < req.len) {
                break;
            }
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SSC_READ fd=%d nreq=%" PRIu64 " req=0x%lx off=0x%lx -> %" PRIu64 "\n",
                      fd + 3, arg1, (unsigned long)arg2, (unsigned long)arg3,
                      total);
        ssc_files[fd].last_count = total;
        return 0;
    }
    case 55: { /* SSC_WAIT_COMPLETION */
        struct QEMU_PACKED {
            int32_t fd;
            uint32_t count;
        } stat = { .fd = -1, .count = 0 };
        ia64_ssc_read(env, arg0, &stat, sizeof(stat));
        int fd = stat.fd - 3;
        if (fd >= 0 && fd < 16 && ssc_files[fd].fp) {
            stat.count = (uint32_t)ssc_files[fd].last_count;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SSC_WAIT fd=%d last=%u\n", stat.fd, stat.count);
        ia64_ssc_write(env, arg0, &stat, sizeof(stat));
        return 0;
    }
    case 77: { /* SSC_GET_INITRAMFS */
        return 0;
    }
    case 66: { /* SSC_EXIT */
        exit(arg0);
    }
    case 448: { /* platform-specific; ignore */
        return 0;
    }
    case 96: /* legacy/unknown: treat as SSC_WRITE */
    case 53: { /* SSC_WRITE */
        int fd = arg0 - 3;
        if (fd < 0 || fd >= 16 || !ssc_files[fd].fp) {
            return (uint64_t)-1;
        }
        struct QEMU_PACKED {
            uint64_t addr;
            uint32_t len;
            uint32_t pad;
        } req;
        uint64_t req_ptr = arg2;
        uint64_t off = arg3;
        uint64_t total = 0;
        fseeko(ssc_files[fd].fp, off, SEEK_SET);
        for (uint64_t i = 0; i < arg1; i++, req_ptr += sizeof(req)) {
            ia64_ssc_read(env, req_ptr, &req, sizeof(req));
            if (req.len == 0) {
                continue;
            }
            uint8_t *tmp = g_malloc(req.len);
            ia64_ssc_read(env, req.addr, tmp, req.len);
            size_t n = fwrite(tmp, 1, req.len, ssc_files[fd].fp);
            g_free(tmp);
            total += n;
            if (n < req.len) {
                break;
            }
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SSC_WRITE fd=%d nreq=%" PRIu64 " req=0x%lx off=0x%lx -> %" PRIu64 "\n",
                      fd + 3, arg1, (unsigned long)arg2, (unsigned long)arg3,
                      total);
        ssc_files[fd].last_count = total;
        return 0;
    }
    default:
        cpu_restore_state(env_cpu(env), GETPC());
        cpu_abort(env_cpu(env), "IA64: SSC unhandled nr=%" PRIu64 " imm=%" PRIu64,
                  nr, imm);
    }
}

static void ia64_insert_tlb(CPUIA64State *env, bool is_data, uint64_t va,
                            uint64_t pa, uint32_t rid, uint8_t ps,
                            uint8_t ar, uint8_t pl, uint8_t d, uint8_t a,
                            uint8_t p, uint8_t ed)
{
    if (!p) {
        return;
    }
    uint64_t mask = (1ULL << ps) - 1;
    uint64_t tag = va & ~mask;
    uint64_t pbase = pa & ~mask;
    uint8_t idx = is_data ? env->dtlb_next : env->itlb_next;
    if (is_data) {
        env->dtlb[idx].tag = tag;
        env->dtlb[idx].pa = pbase;
        env->dtlb[idx].ps = ps;
        env->dtlb[idx].rid = rid;
        env->dtlb[idx].ar = ar;
        env->dtlb[idx].pl = pl;
        env->dtlb[idx].d = d;
        env->dtlb[idx].a = a;
        env->dtlb[idx].p = p;
        env->dtlb[idx].ed = ed;
        env->dtlb[idx].valid = 1;
        env->dtlb_next = (idx + 1) & 127;
    } else {
        env->itlb[idx].tag = tag;
        env->itlb[idx].pa = pbase;
        env->itlb[idx].ps = ps;
        env->itlb[idx].rid = rid;
        env->itlb[idx].ar = ar;
        env->itlb[idx].pl = pl;
        env->itlb[idx].d = d;
        env->itlb[idx].a = a;
        env->itlb[idx].p = p;
        env->itlb[idx].ed = ed;
        env->itlb[idx].valid = 1;
        env->itlb_next = (idx + 1) & 127;
    }
}

void HELPER(itc_d)(CPUIA64State *env, uint64_t src)
{
    static int log_count;
    uint8_t ps = (env->cr[21] >> 2) & 0x3f; /* ITIR.ps */
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    uint64_t pa = (PTE_PPN(src) << 12);
    uint8_t ar = PTE_AR(src);
    uint8_t pl = PTE_PL(src);
    uint8_t d  = PTE_D(src);
    uint8_t a  = PTE_A(src);
    uint8_t p  = PTE_P(src);
    uint8_t ed = PTE_ED(src);
    uint32_t rid = RR_RID(env->rr[extract64(env->cr_ifa, 61, 3)]);
    if (log_count < 16) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "itc.d ip=0x%" PRIx64 " cr_ifa=0x%" PRIx64
                      " ps=%u rid=0x%x src=0x%" PRIx64 " pa=0x%" PRIx64 "\n",
                      env->ip, env->cr_ifa, ps, rid, src, pa);
        log_count++;
    }
    ia64_insert_tlb(env, true, env->cr_ifa, pa, rid, ps, ar, pl, d, a, p, ed);
}

void HELPER(itc_i)(CPUIA64State *env, uint64_t src)
{
    static int log_count;
    uint8_t ps = (env->cr[21] >> 2) & 0x3f; /* ITIR.ps */
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    uint64_t pa = (PTE_PPN(src) << 12);
    uint8_t ar = PTE_AR(src);
    uint8_t pl = PTE_PL(src);
    uint8_t d  = PTE_D(src);
    uint8_t a  = PTE_A(src);
    uint8_t p  = PTE_P(src);
    uint8_t ed = PTE_ED(src);
    uint32_t rid = RR_RID(env->rr[extract64(env->cr_ifa, 61, 3)]);
    if (log_count < 16) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "itc.i ip=0x%" PRIx64 " cr_ifa=0x%" PRIx64
                      " ps=%u rid=0x%x src=0x%" PRIx64 " pa=0x%" PRIx64 "\n",
                      env->ip, env->cr_ifa, ps, rid, src, pa);
        log_count++;
    }
    ia64_insert_tlb(env, false, env->cr_ifa, pa, rid, ps, ar, pl, d, a, p, ed);
}

uint64_t HELPER(thash)(CPUIA64State *env)
{
    static int log_count;
    uint64_t va = env->cr_ifa;
    uint64_t pta = env->cr[8]; /* cr.pta stored in cr[8] */
    uint8_t rr = extract64(va, 61, 3);
    uint8_t ps = RR_PS(env->rr[rr]);
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    uint64_t mask = (1ULL << PTA_SIZE(pta)) - 1;
    mask = extract64(mask, 15, 46);
    uint64_t va_61 = va & ((1ULL << 61) - 1);
    uint64_t hpn = va_61 >> ps;
    uint64_t offset;
    uint64_t addr;

    if (PTA_VF(pta)) {
        offset = (hpn ^ RR_RID(env->rr[rr])) << 5;
        addr = (uint64_t)PTA_VRN(pta) << 61;
    } else {
        offset = hpn << 3;
        addr = (uint64_t)rr << 61;
    }
    addr |= ((PTA_BASE(pta) & ~mask) | (extract64(offset, 15, 46) & mask)) << 15
            | extract64(offset, 0, 15);
    if (log_count < 16) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "thash ifa=0x%" PRIx64 " rr=%u pta=0x%" PRIx64
                      " -> 0x%" PRIx64 "\n",
                      va, rr, pta, addr);
        log_count++;
    }
    return addr;
}

uint64_t HELPER(ttag)(CPUIA64State *env)
{
    uint64_t va = env->cr_ifa;
    uint8_t rr = extract64(va, 61, 3);
    uint8_t ps = RR_PS(env->rr[rr]);
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    uint64_t va_61 = va & ((1ULL << 61) - 1);
    return ((va_61 >> ps) ^ ((uint64_t)RR_RID(env->rr[rr]) << 39));
}

static void ia64_purge_tc_range(CPUIA64State *env, bool is_data,
                                uint64_t va, uint8_t ps)
{
    uint64_t mask = ~((1ULL << ps) - 1);
    uint64_t tag = va & mask;
    uint32_t rid = RR_RID(RR(extract64(va, 61, 3)));

    if (is_data) {
        for (int i = 0; i < 128; i++) {
            if (!env->dtlb[i].valid) {
                continue;
            }
            if (env->dtlb[i].rid == rid &&
                env->dtlb[i].tag == tag &&
                env->dtlb[i].ps == ps) {
                env->dtlb[i].valid = 0;
            }
        }
        for (int i = 0; i < 128; i++) {
            if (!env->dtcs[i].valid) {
                continue;
            }
            if (env->dtcs[i].rid == rid &&
                env->dtcs[i].tag == tag &&
                env->dtcs[i].ps == ps) {
                env->dtcs[i].valid = 0;
            }
        }
    } else {
        for (int i = 0; i < 128; i++) {
            if (!env->itlb[i].valid) {
                continue;
            }
            if (env->itlb[i].rid == rid &&
                env->itlb[i].tag == tag &&
                env->itlb[i].ps == ps) {
                env->itlb[i].valid = 0;
            }
        }
        for (int i = 0; i < 128; i++) {
            if (!env->itcs[i].valid) {
                continue;
            }
            if (env->itcs[i].rid == rid &&
                env->itcs[i].tag == tag &&
                env->itcs[i].ps == ps) {
                env->itcs[i].valid = 0;
            }
        }
    }
}

/* Purge TC entry for va/range using ps from tar. */
static void ia64_ptc(CPUIA64State *env, bool is_global, bool is_dirty,
                     bool is_rid, uint64_t va, uint64_t tar)
{
    uint8_t ps = TAR_PS(tar);
    ia64_purge_tc_range(env, is_dirty, va, ps);
}

void HELPER(ptc_l)(CPUIA64State *env, uint64_t va, uint64_t tar)
{
    ia64_ptc(env, false, true, false, va, tar);
}

void HELPER(ptc_e)(CPUIA64State *env, uint64_t va, uint64_t tar)
{
    /*
     * ptc.e: entry purge.
     *
     * SKI models ptc.e as an overpurge of the translation caches (flush all
     * ITC/DTC entries).  This is sufficient for Linux local_flush_tlb_all(),
     * which uses ptc.e in a loop based on PAL ptce_* parameters.
     */
    (void)va;
    (void)tar;
    for (int i = 0; i < 128; i++) {
        env->itlb[i].valid = 0;
        env->dtlb[i].valid = 0;
        env->itcs[i].valid = 0;
        env->dtcs[i].valid = 0;
    }
    tlb_flush(env_cpu(env));
}

void HELPER(ptc_g)(CPUIA64State *env, uint64_t va, uint64_t tar)
{
    ia64_ptc(env, true, true, true, va, tar);
}

void HELPER(ptc_ga)(CPUIA64State *env, uint64_t va, uint64_t tar)
{
    ia64_ptc(env, true, true, true, va, tar);
}

void HELPER(itr_d)(CPUIA64State *env, uint64_t pte, uint64_t tar)
{
    static uint64_t last_pte;
    static uint64_t last_ifa;
    static uint8_t last_slot;
    static int log_count;

    /* DTR insert: slot in low bits of tar operand, uses cr.ifa + cr.itir. */
    uint8_t slot = tar & 0x7f;
    slot &= 0xf;
    uint8_t ps = (env->cr[21] >> 2) & 0x3f; /* ITIR.ps */
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    if (log_count < 64 || pte != last_pte || env->cr_ifa != last_ifa ||
        slot != last_slot) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "itr.d ip=0x%" PRIx64 " slot=%u ps=%u pte=0x%" PRIx64
                      " sel=0x%" PRIx64 " cr_ifa=0x%" PRIx64
                      " r2=0x%" PRIx64 " r3=0x%" PRIx64 "\n",
                      env->ip, slot, ps, pte, tar, env->cr_ifa,
                      env->r[2], env->r[3]);
        log_count++;
        last_pte = pte;
        last_ifa = env->cr_ifa;
        last_slot = slot;
    }
    env->dtrs[slot].pte = pte;
    env->dtrs[slot].itr = env->cr[21];
    env->dtrs[slot].tag = env->cr_ifa & ~((1ULL << ps) - 1);
    env->dtrs[slot].pa = (PTE_PPN(pte) << 12) & ~((1ULL << ps) - 1);
    env->dtrs[slot].rid = RR_RID(RR(extract64(env->cr_ifa, 61, 3)));
    env->dtrs[slot].ps = ps;
    env->dtrs[slot].valid = 1;
    ia64_insert_tlb(env, true, env->cr_ifa, env->dtrs[slot].pa,
                    env->dtrs[slot].rid, ps, PTE_AR(pte), PTE_PL(pte),
                    PTE_D(pte), PTE_A(pte), PTE_P(pte), PTE_ED(pte));
}

void HELPER(itr_i)(CPUIA64State *env, uint64_t pte, uint64_t tar)
{
    static uint64_t last_pte;
    static uint64_t last_ifa;
    static uint8_t last_slot;
    static int log_count;

    uint8_t slot = tar & 0x7f;
    slot &= 0xf;
    uint8_t ps = (env->cr[21] >> 2) & 0x3f; /* ITIR.ps */
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    if (log_count < 64 || pte != last_pte || env->cr_ifa != last_ifa ||
        slot != last_slot) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "itr.i ip=0x%" PRIx64 " slot=%u ps=%u pte=0x%" PRIx64
                      " sel=0x%" PRIx64 " cr_ifa=0x%" PRIx64
                      " r2=0x%" PRIx64 " r3=0x%" PRIx64 "\n",
                      env->ip, slot, ps, pte, tar, env->cr_ifa,
                      env->r[2], env->r[3]);
        log_count++;
        last_pte = pte;
        last_ifa = env->cr_ifa;
        last_slot = slot;
    }
    env->itrs[slot].pte = pte;
    env->itrs[slot].itr = env->cr[21];
    env->itrs[slot].tag = env->cr_ifa & ~((1ULL << ps) - 1);
    env->itrs[slot].pa = (PTE_PPN(pte) << 12) & ~((1ULL << ps) - 1);
    env->itrs[slot].rid = RR_RID(RR(extract64(env->cr_ifa, 61, 3)));
    env->itrs[slot].ps = ps;
    env->itrs[slot].valid = 1;
    ia64_insert_tlb(env, false, env->cr_ifa, env->itrs[slot].pa,
                    env->itrs[slot].rid, ps, PTE_AR(pte), PTE_PL(pte),
                    PTE_D(pte), PTE_A(pte), PTE_P(pte), PTE_ED(pte));
}

void HELPER(ptr_d)(CPUIA64State *env, uint64_t va, uint64_t range)
{
    uint8_t ps = extract64(range, 24, 6);
    ia64_purge_tc_range(env, true, va, ps);
}

void HELPER(ptr_i)(CPUIA64State *env, uint64_t va, uint64_t range)
{
    uint8_t ps = extract64(range, 24, 6);
    ia64_purge_tc_range(env, false, va, ps);
}

void HELPER(flushrs)(CPUIA64State *env)
{
    /*
     * For now, model an eager RSE with an empty dirty partition: flushrs
     * synchronizes ar.bspstore with ar.bsp and clears ar.rsc.loadrs.
     *
     * Linux uses enforced-lazy + flushrs when switching stacks/modes and
     * expects the loadrs field to report the dirty-partition size.
     */
    env->ar[IA64_AR_BSPSTORE] = env->ar[IA64_AR_BSP] & ~0x7ULL;
    env->ar[IA64_AR_RSC] = ia64_rsc_set_loadrs(env->ar[IA64_AR_RSC], 0);
}

void HELPER(loadrs)(CPUIA64State *env)
{
    /*
     * Minimal loadrs implementation sufficient for Linux head.S early boot
     * usage (loadrs with ar.rsc=0 to clear any residual dirty partition).
     *
     * If a non-zero loadrs field is requested, fail fast so we can fill in
     * the full RSE load behavior when needed.
     */
    uint64_t rsc = env->ar[IA64_AR_RSC];
    uint64_t loadrs_bytes = ia64_rsc_get_loadrs(rsc);
    if (loadrs_bytes != 0) {
        cpu_abort(env_cpu(env),
                  "IA64 UNIMPL: loadrs with ar.rsc.loadrs=%" PRIu64 " (pc=%016" PRIx64 ")",
                  loadrs_bytes, env->ip);
    }
    env->ar[IA64_AR_BSPSTORE] = env->ar[IA64_AR_BSP] & ~0x7ULL;
    env->ar[IA64_AR_RSC] = ia64_rsc_set_loadrs(rsc, 0);
}

void HELPER(cover)(CPUIA64State *env)
{
    static int log_count;
    uint64_t old_cfm = env->cfm;
    uint8_t sof = old_cfm & 0x7f;
    uint64_t bsp = env->ar[IA64_AR_BSP];

    if (bsp == 0) {
        bsp = env->ar[IA64_AR_BSPSTORE];
    }
    bsp &= ~0x7ULL;

    uint64_t new_bsp = ia64_rse_skip_regs(bsp, sof);
    uint64_t cover_bytes = new_bsp - bsp;

    if (log_count < 64) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cover ip=%016" PRIx64 " psr=%016" PRIx64
                      " cfm=%016" PRIx64 " sof=%u bsp=%016" PRIx64
                      " -> new_bsp=%016" PRIx64 " bytes=%" PRIu64 "\n",
                      env->ip, env->psr, old_cfm, sof, bsp,
                      new_bsp, cover_bytes);
        log_count++;
    }

    /*
     * When interruption collection is off, cover also updates cr.ifs (V=1).
     * Set V unconditionally since Linux relies on it for rfi paths.
     */
    env->cr_ifs = old_cfm | (1ULL << 63);

    /* Extend the dirty-partition size reported by ar.rsc.loadrs. */
    uint64_t rsc = env->ar[IA64_AR_RSC];
    uint64_t loadrs_bytes = ia64_rsc_get_loadrs(rsc);
    env->ar[IA64_AR_RSC] = ia64_rsc_set_loadrs(rsc, loadrs_bytes + cover_bytes);

    env->ar[IA64_AR_BSP] = new_bsp;

    /* Create the covering frame: no stacked regs/rotations. */
    env->cfm = 0;
}

void HELPER(set_bspstore)(CPUIA64State *env, uint64_t bspstore)
{
    /*
     * Writing ar.bspstore is used by Linux to establish a new RSE backing
     * store (e.g. during head.S and mode switches). In enforced-lazy mode,
     * the kernel ensures the dirty partition is empty before doing this.
     *
     * Model this as resetting both ar.bspstore and ar.bsp and clearing
     * ar.rsc.loadrs.
     */
    bspstore &= ~0x7ULL;
    env->ar[IA64_AR_BSPSTORE] = bspstore;
    env->ar[IA64_AR_BSP] = bspstore;
    env->ar[IA64_AR_RSC] = ia64_rsc_set_loadrs(env->ar[IA64_AR_RSC], 0);
    env->ar[IA64_AR_RNAT] = 0;
}

uint64_t HELPER(get_itc)(CPUIA64State *env)
{
    /*
     * ar.itc: interval time counter.
     *
     * Linux uses ar.itc for udelay() and various timekeeping paths very early.
     * Model it as the QEMU virtual clock (ns) plus a guest-programmable offset.
     *
     * Store the offset in env->ar[IA64_AR_ITC] (signed) so that mov.m ar.itc = X
     * can be implemented without adding a separate state field.
     */
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t off = (int64_t)env->ar[IA64_AR_ITC];
    return now + off;
}

void HELPER(set_itc)(CPUIA64State *env, uint64_t val)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    env->ar[IA64_AR_ITC] = (uint64_t)((int64_t)val - (int64_t)now);
}

uint64_t HELPER(get_cpuid)(CPUIA64State *env, uint64_t idx)
{
    if (idx < ARRAY_SIZE(env->cpuid)) {
        return env->cpuid[idx];
    }
    return 0;
}

void HELPER(srlz_d)(CPUIA64State *env)
{
    /* Serialization is a no-op in this model. */
}

void HELPER(srlz_i)(CPUIA64State *env)
{
    /* Serialization is a no-op in this model. */
}
