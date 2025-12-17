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
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "exec/cpu-common.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/ioport.h"
#include <math.h>

static inline hwaddr ia64_phys_mode_addr(uint64_t addr)
{
    /*
     * IA-64 uses region-encoded virtual addresses even in physical mode.
     * Firmware also commonly forms sign-extended 32-bit addresses (e.g.
     * 0xffffffffffE00000) via addl/adds from a small GP value.
     *
     * - If the address is canonically sign-extended 32-bit, treat it as a
     *   32-bit physical address.
     * - Otherwise fall back to the low 61 bits (ignore the region number).
     */
    uint64_t hi32 = addr & 0xffffffff00000000ULL;
    if (hi32 == 0 || hi32 == 0xffffffff00000000ULL) {
        return (hwaddr)(uint32_t)addr;
    }
    return (hwaddr)(addr & ((1ULL << 61) - 1));
}

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

/* CFM fields */
#define IA64_CFM_SOR_SHIFT 14
#define IA64_CFM_SOR_MASK  0xfULL

/* ar.rsc loadrs field: bits 16..29, in bytes (see SKI ssDSym.c). */
#define IA64_RSC_LOADRS_SHIFT 16
#define IA64_RSC_LOADRS_MASK  0x3fffULL

/* Linux/ia64 canonical per-cpu range: [-PERCPU_PAGE_SIZE, 0). */
#define IA64_PERCPU_VA_BASE   0xfffffffffffc0000ULL
#define IA64_PERCPU_PAGE_SIZE (1ULL << 18) /* 256KiB (PERCPU_PAGE_SHIFT=18) */
#define IA64_KR_PER_CPU_DATA  3            /* ar.k3 */

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
static bool ia64_intr_pop_window(CPUIA64State *env);

#ifndef CONFIG_USER_ONLY
static bool ia64_pc_in_sym(const CPUIA64State *env, uint64_t pc,
                           uint64_t sym_va, uint64_t sym_size)
{
    const uint64_t bias = env->kernel_bias;
    if (!sym_va || !sym_size) {
        return false;
    }
    if (pc >= sym_va && pc < sym_va + sym_size) {
        return true;
    }
    if (bias) {
        uint64_t sym_pa = sym_va - bias;
        if (pc >= sym_pa && pc < sym_pa + sym_size) {
            return true;
        }
    }
    return false;
}

static bool ia64_is_task_switch_pc(const CPUIA64State *env, uint64_t pc)
{
    /*
     * Only switch our modeled call-frame stack when Linux is actually
     * switching tasks (ia64_switch_to/load_switch_stack). The IVT and various
     * PAL/SAL/EFI paths also touch ar.bspstore but must not affect our per-task
     * bookkeeping.
     */
    if (ia64_pc_in_sym(env, pc, env->dbg_ia64_switch_to_va,
                       env->dbg_ia64_switch_to_size)) {
        return true;
    }
    if (ia64_pc_in_sym(env, pc, env->dbg_load_switch_stack_va,
                       env->dbg_load_switch_stack_size)) {
        return true;
    }
    return false;
}
#endif

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
    static uint32_t break_log_count;

    if (retaddr) {
        cpu_restore_state(cs, retaddr);
    }

    /*
     * Debug helper: repair_env_string() BUG() in init/main.c is usually a
     * symptom of command-line parsing seeing unexpected memory contents.
     * When a BREAK is taken, opportunistically dump the current param/value
     * pointers (r32/r33) when they are in the region-7 direct map, so the
     * caller can see what string triggered the BUG.
     */
    if (vec == IA64_VEC_BREAK && break_log_count < 16) {
        const char *s = getenv("QEMU_IA64_LOG_BREAK_STR");
        if (s && *s) {
            uint64_t param_va = env->r[32];
            uint64_t val_va = env->r[33];
            bool param_direct = (extract64(param_va, 61, 3) == 7 &&
                                 extract64(param_va, 60, 1) == 0);
            bool val_direct = (!val_va) || (extract64(val_va, 61, 3) == 7 &&
                                            extract64(val_va, 60, 1) == 0);
            if (param_direct && val_direct) {
                uint64_t param_pa = param_va & ((1ULL << 61) - 1);
                uint64_t val_pa = val_va ? (val_va & ((1ULL << 61) - 1)) : 0;
                char pbuf[128];
                char vbuf[128];
                size_t plen = sizeof(pbuf) - 1, vlen = sizeof(vbuf) - 1;

                MemTxResult r1 = address_space_read(&address_space_memory, param_pa,
                                                    MEMTXATTRS_UNSPECIFIED,
                                                    (uint8_t *)pbuf, sizeof(pbuf));
                if (r1 == MEMTX_OK) {
                    for (size_t i = 0; i + 1 < sizeof(pbuf); i++) {
                        if (pbuf[i] == '\0') {
                            plen = i;
                            break;
                        }
                        if ((uint8_t)pbuf[i] < 0x20 || (uint8_t)pbuf[i] > 0x7e) {
                            pbuf[i] = '.';
                        }
                    }
                    pbuf[MIN(plen, sizeof(pbuf) - 1)] = '\0';
                } else {
                    strcpy(pbuf, "<unreadable>");
                }

                if (val_va) {
                    MemTxResult r2 = address_space_read(&address_space_memory, val_pa,
                                                        MEMTXATTRS_UNSPECIFIED,
                                                        (uint8_t *)vbuf, sizeof(vbuf));
                    if (r2 == MEMTX_OK) {
                        for (size_t i = 0; i + 1 < sizeof(vbuf); i++) {
                            if (vbuf[i] == '\0') {
                                vlen = i;
                                break;
                            }
                            if ((uint8_t)vbuf[i] < 0x20 || (uint8_t)vbuf[i] > 0x7e) {
                                vbuf[i] = '.';
                            }
                        }
                        vbuf[MIN(vlen, sizeof(vbuf) - 1)] = '\0';
                    } else {
                        strcpy(vbuf, "<unreadable>");
                    }
                } else {
                    strcpy(vbuf, "<null>");
                }

                int64_t diff = (int64_t)(val_va - param_va);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64 break iim=0x%" PRIx64 " ip=%016" PRIx64
                              " r32(param)=%016" PRIx64 "(pa=%016" PRIx64 ") \"%s\""
                              " r33(val)=%016" PRIx64 "(pa=%016" PRIx64 ") \"%s\""
                              " diff=%" PRId64 " plen=%zu expect={%zu,%zu}\n",
                              iim, env->ip,
                              param_va, param_pa, pbuf,
                              val_va, val_pa, vbuf,
                              diff, plen, plen + 1, plen + 2);
                break_log_count++;
            }
        }
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
    /*
     * Translation registers (ITR/DTR) are pinned and must survive TC purges.
     * Consult them first (see SKI's TR semantics and Linux's IVT setup).
     */
    const typeof(env->itrs[0]) *trs = is_data ? env->dtrs : env->itrs;
    for (int i = 0; i < ARRAY_SIZE(env->itrs); i++) {
        if (!trs[i].valid || trs[i].rid != rid) {
            continue;
        }
        if (!PTE_P(trs[i].pte)) {
            continue;
        }
        uint64_t mask = ~((1ULL << trs[i].ps) - 1);
        if ((va & mask) == trs[i].tag) {
            *hit = true;
            return trs[i].pa + (va & ~mask);
        }
    }

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
    if (rr_idx == 6 && extract64(va, 60, 1) == 0) {
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

void ia64_intr_push_window(CPUIA64State *env)
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
    struct IA64RSEFrame *frame = &env->rse_frames[env->rse_depth - 1];

    /*
     * Model the shared physical registers between caller OUT and callee IN.
     *
     * Real IA-64 calls do not preserve the caller's OUT registers: they are
     * the callee's IN registers, so any callee writes must be visible to the
     * caller after the return.
     *
     * Capture the current IN values (env->r[32..]) before restoring the saved
     * caller frame, and then copy them back into the caller's OUT window.
     */
    uint64_t outvals[96] = { 0 };
    uint8_t caller_sof = frame->cfm & 0x7f;
    uint8_t caller_sol = (frame->cfm >> 7) & 0x7f;
    uint8_t caller_outs = (caller_sof > caller_sol) ? (caller_sof - caller_sol) : 0;
    caller_outs = MIN(caller_outs, (uint8_t)96);
    for (uint8_t i = 0; i < caller_outs; i++) {
        outvals[i] = env->r[32 + i];
    }

    frame = &env->rse_frames[--env->rse_depth];
    memcpy(&env->r[32], frame->r, sizeof(frame->r));
    env->ar[64] = frame->ar_pfs;
    env->cfm = frame->cfm;

    if (caller_outs && caller_sol < 96) {
        uint8_t max_copy = MIN(caller_outs, (uint8_t)(96 - caller_sol));
        uint8_t out0 = 32 + caller_sol;
        for (uint8_t i = 0; i < max_copy; i++) {
            env->r[out0 + i] = outvals[i];
        }
    }
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
    static int log_limit = -1;
    if (log_limit == -1) {
        log_limit = 32;
        const char *s = getenv("QEMU_IA64_DBG_CALL_LIMIT");
        if (s && *s) {
            log_limit = atoi(s);
        }
        if (log_limit < 0) {
            log_limit = 0;
        }
    }
    if (log_count >= log_limit) {
        return;
    }
    ia64_dbg_next_call_pc = pc;
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

static long double ia64_fp_to_ld(const CPUIA64State *env, uint32_t f)
{
    f &= 0x7f;
    uint64_t mant = env->f[f][0];
    uint64_t expw = env->f[f][1];

    if (mant == 0) {
        return 0.0L;
    }

    int sign = (expw & 0x20000ULL) ? 1 : 0;
    uint64_t exp = expw & 0x1ffffULL;
    if (exp == 0) {
        exp = IA64_FP_EXP_INTEGER;
    }

    int64_t e = (int64_t)exp - (int64_t)IA64_FP_EXP_BIAS;
    long double sig = (long double)mant / (long double)(1ULL << 63);
    long double val = ldexpl(sig, (int)e);
    return sign ? -val : val;
}

static void ia64_ld_to_fp(CPUIA64State *env, uint32_t f, long double val)
{
    f &= 0x7f;
    if (f <= 1) {
        return;
    }

    if (val == 0.0L || isnan(val) || isinf(val)) {
        env->f[f][0] = 0;
        env->f[f][1] = 0;
        return;
    }

    int sign = (val < 0.0L) ? 1 : 0;
    long double abs = fabsl(val);

    int e = 0;
    long double frac = frexpl(abs, &e); /* abs = frac * 2^e, frac in [0.5,1) */
    frac *= 2.0L;
    e -= 1;

    long double scaled = ldexpl(frac, 63);
    uint64_t mant = (uint64_t)(scaled + 0.5L);

    uint64_t exp = (uint64_t)((int64_t)IA64_FP_EXP_BIAS + (int64_t)e);
    exp &= 0x1ffffULL;
    uint64_t expw = (sign ? 0x20000ULL : 0) | exp;

    env->f[f][0] = mant;
    env->f[f][1] = expw;
}

void HELPER(frcpa_s1)(CPUIA64State *env, uint32_t f1, uint32_t p2,
                      uint32_t f2, uint32_t f3)
{
    f1 &= 0x7f;
    f2 &= 0x7f;
    f3 &= 0x7f;
    p2 &= 0x3f;

    long double den = ia64_fp_to_ld(env, f3);
    bool ok = (den != 0.0L);
    long double res = ok ? (1.0L / den) : 0.0L;

    ia64_ld_to_fp(env, f1, res);

    if (p2 != 0) {
        if (ok) {
            env->pr |= (1ULL << p2);
        } else {
            env->pr &= ~(1ULL << p2);
        }
        env->pr |= 1ULL; /* p0 is always true */
    }
}

void HELPER(fma_s1)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                    uint32_t f4, uint32_t f2)
{
    long double a = ia64_fp_to_ld(env, f3);
    long double b = ia64_fp_to_ld(env, f4);
    long double c = ia64_fp_to_ld(env, f2);
    ia64_ld_to_fp(env, f1, fmal(a, b, c));
}

void HELPER(fms_s1)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                    uint32_t f4, uint32_t f2)
{
    long double a = ia64_fp_to_ld(env, f3);
    long double b = ia64_fp_to_ld(env, f4);
    long double c = ia64_fp_to_ld(env, f2);
    ia64_ld_to_fp(env, f1, fmal(a, b, -c));
}

void HELPER(fnma_s1)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                     uint32_t f4, uint32_t f2)
{
    long double a = ia64_fp_to_ld(env, f3);
    long double b = ia64_fp_to_ld(env, f4);
    long double c = ia64_fp_to_ld(env, f2);
    ia64_ld_to_fp(env, f1, fmal(-a, b, c));
}

static void ia64_fcvt_invalid(CPUIA64State *env, const char *op,
                              uint32_t fsrc, uint64_t expw, uint64_t mant)
{
    static uint32_t count;

    if (count++ >= 16) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: %s invalid ip=%016" PRIx64 " f%u expw=%05" PRIx64
                  " mant=%016" PRIx64 "\n",
                  op, env->ip, fsrc & 0x7f, (uint64_t)(expw & 0x3ffffULL), mant);
}

void HELPER(fcvt_fxu_trunc_s1)(CPUIA64State *env, uint32_t f1, uint32_t f2)
{
    f1 &= 0x7f;
    f2 &= 0x7f;
    if (f1 <= 1) {
        return;
    }

    uint64_t mant = env->f[f2][0];
    uint64_t expw = env->f[f2][1];
    uint64_t res = 0;

    if (mant != 0) {
        bool sign = (expw & 0x20000ULL) != 0;
        uint64_t exp = expw & 0x1ffffULL;
        if (exp == 0) {
            exp = IA64_FP_EXP_INTEGER;
        }

        if (sign) {
            res = 0x8000000000000000ULL;
            ia64_fcvt_invalid(env, "fcvt.fxu.trunc.s1", f2, expw, mant);
        } else {
            int64_t shift = (int64_t)exp - (int64_t)IA64_FP_EXP_BIAS - 63;
            if (shift <= -64) {
                res = 0;
            } else if (shift >= 64) {
                res = 0x8000000000000000ULL;
                ia64_fcvt_invalid(env, "fcvt.fxu.trunc.s1", f2, expw, mant);
            } else if (shift >= 0) {
                __uint128_t v = (__uint128_t)mant << shift;
                if (v > UINT64_MAX) {
                    res = 0x8000000000000000ULL;
                    ia64_fcvt_invalid(env, "fcvt.fxu.trunc.s1", f2, expw, mant);
                } else {
                    res = (uint64_t)v;
                }
            } else {
                res = mant >> (-shift);
            }
        }
    }

    env->f[f1][0] = res;
    env->f[f1][1] = IA64_FP_SEXP(0, IA64_FP_EXP_INTEGER);
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
static bool ia64_divhelp_log_enabled(void)
{
    static int inited;
    static bool enabled;

    if (!inited) {
        const char *s = getenv("QEMU_IA64_DIVHELP_LOG");
        enabled = (s && *s);
        inited = 1;
    }
    return enabled;
}

static void ia64_divhelp_log(CPUIA64State *env, const char *name,
                             uint64_t a, uint64_t b, uint64_t res)
{
    static uint32_t count;

    if (!ia64_divhelp_log_enabled()) {
        return;
    }
    if (count++ >= 64) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "ia64: divhelp %s ip=%016" PRIx64
                  " a=%016" PRIx64 " b=%016" PRIx64 " -> %016" PRIx64 "\n",
                  name, env ? env->ip : 0, a, b, res);
}

uint64_t HELPER(divdi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    int64_t sa = (int64_t)a;
    int64_t sb = (int64_t)b;
    if (sb == 0) {
        return 0;
    }
    uint64_t res = (uint64_t)(sa / sb);
    ia64_divhelp_log(env, "divdi3", a, b, res);
    return res;
}

uint64_t HELPER(udivdi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    if (b == 0) {
        return 0;
    }
    uint64_t res = a / b;
    ia64_divhelp_log(env, "udivdi3", a, b, res);
    return res;
}

uint64_t HELPER(moddi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    int64_t sa = (int64_t)a;
    int64_t sb = (int64_t)b;
    if (sb == 0) {
        return 0;
    }
    uint64_t res = (uint64_t)(sa % sb);
    ia64_divhelp_log(env, "moddi3", a, b, res);
    return res;
}

uint64_t HELPER(umoddi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    if (b == 0) {
        return 0;
    }
    uint64_t res = a % b;
    ia64_divhelp_log(env, "umoddi3", a, b, res);
    return res;
}

uint64_t HELPER(divsi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    int32_t sa = (int32_t)a;
    int32_t sb = (int32_t)b;
    if (sb == 0) {
        return 0;
    }
    uint64_t res = (uint64_t)(int64_t)(sa / sb);
    ia64_divhelp_log(env, "divsi3", a, b, res);
    return res;
}

uint64_t HELPER(udivsi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;
    if (ub == 0) {
        return 0;
    }
    uint64_t res = (uint64_t)(ua / ub);
    ia64_divhelp_log(env, "udivsi3", a, b, res);
    return res;
}

uint64_t HELPER(modsi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    int32_t sa = (int32_t)a;
    int32_t sb = (int32_t)b;
    if (sb == 0) {
        return 0;
    }
    uint64_t res = (uint64_t)(int64_t)(sa % sb);
    ia64_divhelp_log(env, "modsi3", a, b, res);
    return res;
}

uint64_t HELPER(umodsi3)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;
    if (ub == 0) {
        return 0;
    }
    uint64_t res = (uint64_t)(ua % ub);
    ia64_divhelp_log(env, "umodsi3", a, b, res);
    return res;
}

void HELPER(breaki)(CPUIA64State *env, uint64_t iim)
{
    CPUState *cs = env_cpu(env);
    ia64_fault(cs, env, false, false, IA64_VEC_BREAK, iim, GETPC());
}

uint64_t HELPER(fw_break0)(CPUIA64State *env, uint64_t pc)
{
    /*
     * Xenipf firmware and some EDK components use break(0) as a last-resort
     * trap/breakpoint. In our bringup environment, a missing handler would
     * otherwise recurse into the empty break vector (0x2c00) and hang.
     *
     * Heuristic: treat break(0) as a fail-fast call that returns to b0 when
     * the CPU is currently in a br.call-created frame. Otherwise, just
     * advance to the next bundle.
     */
    uint8_t kind = env->last_b0_write_kind & 0xff;
    bool in_call = (kind == 1);
    static int dump_enabled = -1;
    static int dump_len = -1;

    if (dump_enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_BREAK0_DUMP");
        dump_enabled = (s && *s) ? 1 : 0;
    }
    if (dump_len == -1) {
        dump_len = 4096;
        const char *s = getenv("QEMU_IA64_FW_BREAK0_DUMP_LEN");
        if (s && *s) {
            dump_len = atoi(s);
        }
        if (dump_len < 0) {
            dump_len = 0;
        }
        if (dump_len > 65536) {
            dump_len = 65536;
        }
    }

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_break0 pc=%016" PRIx64 " in_call=%d b0=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                      " r35=%016" PRIx64 " r36=%016" PRIx64 "\n",
                      pc, in_call, env->b[0],
                      env->r[32], env->r[33], env->r[34], env->r[35], env->r[36]);
    }

    if (dump_enabled && dump_len > 0 && env->r[36]) {
        CPUState *cs = env_cpu(env);
        g_autofree uint8_t *buf = g_malloc0((size_t)dump_len);
        if (cpu_memory_rw_debug(cs, env->r[36], buf, (size_t)dump_len, false) == 0) {
            size_t i = 0;
            while (i < (size_t)dump_len) {
                while (i < (size_t)dump_len) {
                    unsigned char c = buf[i];
                    if (c >= 0x20 && c < 0x7f) {
                        break;
                    }
                    i++;
                }
                size_t start = i;
                while (i < (size_t)dump_len) {
                    unsigned char c = buf[i];
                    if (!(c >= 0x20 && c < 0x7f)) {
                        break;
                    }
                    i++;
                }
                size_t len = i - start;
                if (len >= 8) {
                    char s[256];
                    size_t n = len < (sizeof(s) - 1) ? len : (sizeof(s) - 1);
                    memcpy(s, &buf[start], n);
                    s[n] = '\0';
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_break0_str +0x%zx \"%s\"\n",
                                  start, s);
                }
            }
        }
    }

    if (in_call && env->b[0]) {
        uint64_t ret = env->b[0] & ~0xFULL;
        HELPER(ret_restore_b0)(env);
        env->psr &= ~PSR_RI_MASK;
        return ret;
    }

    return (pc & ~0xFULL) + 16;
}

void HELPER(dbg_probe)(CPUIA64State *env, uint64_t pc, uint32_t ri)
{
    typedef struct {
        uint64_t pc;
        uint32_t count;
    } DbgProbeCount;

    static DbgProbeCount probes[64];
    static uint32_t nprobes;
    static int dbg_probe_limit = -1;

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

    if (dbg_probe_limit == -1) {
        dbg_probe_limit = 8;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_LIMIT");
        if (s && *s) {
            dbg_probe_limit = atoi(s);
        }
        if (dbg_probe_limit < 0) {
            dbg_probe_limit = 0;
        }
    }

    if (probes[idx].count++ >= (uint32_t)dbg_probe_limit) {
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
                  " psr=%016" PRIx64 " cfm=%016" PRIx64 " pr=%016" PRIx64 " depth=%u"
                  " cr_ifa=%016" PRIx64 " cr_iha=%016" PRIx64
                  " pta=%016" PRIx64 " itir=%016" PRIx64
                  " ar.rsc=%016" PRIx64 " ar.bsp=%016" PRIx64 " ar.bspstore=%016" PRIx64
                  " ar.rnat=%016" PRIx64 " ar.pfs=%016" PRIx64
                  " ar.k6=%016" PRIx64 " ar.lc=%016" PRIx64 " ar.ec=%016" PRIx64
                  " r0=%016" PRIx64 " r1=%016" PRIx64 " r2=%016" PRIx64 " r3=%016" PRIx64
                  " r8=%016" PRIx64 " r9=%016" PRIx64
                  " r10=%016" PRIx64 " r11=%016" PRIx64
                  " r12=%016" PRIx64 " r14=%016" PRIx64 " r15=%016" PRIx64
                  " r18=%016" PRIx64
                  " r19=%016" PRIx64 " r22=%016" PRIx64 " r23=%016" PRIx64 " r27=%016" PRIx64
                  " r24=%016" PRIx64
                  " r28=%016" PRIx64 " r29=%016" PRIx64
                  " r30=%016" PRIx64 " r31=%016" PRIx64
                  " r43=%016" PRIx64
                  " r59=%016" PRIx64
                  " r62=%016" PRIx64
                  " r16=%016" PRIx64 " r17=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64
                  " r38=%016" PRIx64 " r39=%016" PRIx64 " r40=%016" PRIx64
                  " r47=%016" PRIx64 " r48=%016" PRIx64 " r49=%016" PRIx64
                  " b0=%016" PRIx64 " b6=%016" PRIx64
                  " r45=%016" PRIx64 " r46=%016" PRIx64 "\n",
                  pc, ri,
                  env->psr, env->cfm, env->pr, env->rse_depth,
                  env->cr_ifa, env->cr_iha, env->cr[8], env->cr[21],
                  env->ar[IA64_AR_RSC], env->ar[IA64_AR_BSP],
                  env->ar[IA64_AR_BSPSTORE], env->ar[IA64_AR_RNAT],
                  env->ar[IA64_AR_PFS], env->ar[6], env->ar[65], env->ar[66],
                  env->r[0], env->r[1], env->r[2], env->r[3],
                  env->r[8], env->r[9], env->r[10], env->r[11],
                  env->r[12], env->r[14], env->r[15],
                  env->r[18], env->r[19], env->r[22], env->r[23], env->r[27],
                  env->r[24], env->r[28], env->r[29],
                  env->r[30], env->r[31],
                  env->r[43],
                  env->r[59],
                  env->r[62],
                  env->r[16], env->r[17],
                  env->r[32], env->r[33], env->r[34], env->r[35],
                  env->r[36], env->r[37], env->r[38], env->r[39], env->r[40],
                  env->r[47], env->r[48], env->r[49],
                  env->b[0], env->b[6], env->r[45], env->r[46]);

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

void HELPER(dbg_cmp)(CPUIA64State *env, uint64_t pc, uint64_t lhs, uint64_t rhs,
                     uint32_t p1, uint32_t p2)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "dbg_cmp pc=%016" PRIx64 " p1=%u p2=%u lhs=%016" PRIx64 " rhs=%016" PRIx64
                  " pr=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                  pc, p1, p2, lhs, rhs, env->pr, env->cfm);
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

static inline bool ia64_ranges_overlap(uint64_t a1, uint32_t s1,
                                       uint64_t a2, uint32_t s2)
{
    if (s1 == 0 || s2 == 0) {
        return false;
    }
    uint64_t e1 = a1 + (uint64_t)s1;
    uint64_t e2 = a2 + (uint64_t)s2;
    return !(e1 <= a2 || e2 <= a1);
}

static void ia64_alat_clear_entries(struct IA64ALATEntry *entries,
                                   uint32_t *count, size_t n)
{
    if (*count == 0) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        entries[i].valid = 0;
    }
    *count = 0;
}

void HELPER(alat_invalidate_all)(CPUIA64State *env)
{
    ia64_alat_clear_entries(env->alat_gr, &env->alat_gr_valid,
                            ARRAY_SIZE(env->alat_gr));
    ia64_alat_clear_entries(env->alat_fr, &env->alat_fr_valid,
                            ARRAY_SIZE(env->alat_fr));
}

void HELPER(alat_record_gr)(CPUIA64State *env, uint32_t reg,
                            uint64_t addr, uint32_t size)
{
    reg &= 0x7f;
    if (reg == 0 || size == 0) {
        return;
    }
    struct IA64ALATEntry *e = &env->alat_gr[reg];
    if (!e->valid) {
        env->alat_gr_valid++;
    }
    e->addr = addr;
    e->size = size;
    e->valid = 1;
}

void HELPER(alat_record_fr)(CPUIA64State *env, uint32_t reg,
                            uint64_t addr, uint32_t size)
{
    reg &= 0x7f;
    if (reg <= 1 || size == 0) {
        return;
    }
    struct IA64ALATEntry *e = &env->alat_fr[reg];
    if (!e->valid) {
        env->alat_fr_valid++;
    }
    e->addr = addr;
    e->size = size;
    e->valid = 1;
}

uint64_t HELPER(alat_check_gr)(CPUIA64State *env, uint32_t reg, uint32_t clr)
{
    reg &= 0x7f;
    if (reg == 0) {
        return 1;
    }
    struct IA64ALATEntry *e = &env->alat_gr[reg];
    uint64_t miss = e->valid ? 0 : 1;
    if (clr && e->valid) {
        e->valid = 0;
        if (env->alat_gr_valid) {
            env->alat_gr_valid--;
        }
    }
    if (clr && miss) {
        e->valid = 0;
    }
    return miss;
}

uint64_t HELPER(alat_check_fr)(CPUIA64State *env, uint32_t reg, uint32_t clr)
{
    reg &= 0x7f;
    if (reg <= 1) {
        return 1;
    }
    struct IA64ALATEntry *e = &env->alat_fr[reg];
    uint64_t miss = e->valid ? 0 : 1;
    if (clr && e->valid) {
        e->valid = 0;
        if (env->alat_fr_valid) {
            env->alat_fr_valid--;
        }
    }
    if (clr && miss) {
        e->valid = 0;
    }
    return miss;
}

void HELPER(alat_invalidate)(CPUIA64State *env, uint64_t addr, uint32_t size)
{
    if (size == 0) {
        return;
    }

    if (env->alat_gr_valid) {
        for (uint32_t r = 1; r < 128; r++) {
            struct IA64ALATEntry *e = &env->alat_gr[r];
            if (!e->valid) {
                continue;
            }
            if (ia64_ranges_overlap(addr, size, e->addr, e->size)) {
                e->valid = 0;
                env->alat_gr_valid--;
            }
        }
    }

    if (env->alat_fr_valid) {
        for (uint32_t f = 2; f < 128; f++) {
            struct IA64ALATEntry *e = &env->alat_fr[f];
            if (!e->valid) {
                continue;
            }
            if (ia64_ranges_overlap(addr, size, e->addr, e->size)) {
                e->valid = 0;
                env->alat_fr_valid--;
            }
        }
    }
}

uint64_t HELPER(alloc)(CPUIA64State *env, uint64_t sof, uint64_t sol, uint64_t sor)
{
    /*
     * The register stack engine (RSE) creates a new register frame of size SOF
     * with SOL locals and SOR rotating registers.  We model the architectural
     * register numbers as a window (r32..r127) and rely on helper_call() to
     * move caller OUT registers into callee IN registers.
     *
     * alloc changes CFM and updates ar.pfs to the previous CFM; the previous
     * ar.pfs value is returned in the destination register.
     */
    uint64_t old_pfs = env->ar[64]; /* ar.pfs */
    uint64_t old_cfm = env->cfm;
    uint8_t old_sof = old_cfm & 0x7f;

    env->cfm = (sof & 0x7f) | ((sol & 0x7f) << 7) | ((sor & 0xf) << 14);
    env->ar[64] = old_cfm;

    /* Clear newly allocated stacked regs (beyond previous SOF). */
    if (sof > old_sof) {
        uint8_t n = MIN((uint8_t)sof, (uint8_t)96);
        for (uint8_t i = old_sof; i < n; i++) {
            env->r[32 + i] = 0;
        }
    }

    return old_pfs;
}

void HELPER(rotate_grs)(CPUIA64State *env)
{
    /*
     * Software pipelining rotates the *mapping* of GR32..(GR32+SOR-1) via RRBG.
     *
     * Our translator currently indexes stacked registers directly by their
     * architectural number. Emulate the same architectural behavior by
     * rotating the values in the rotating window whenever rotate_regs()
     * updates RRBG.
     *
     * See SKI's PHYS_GR() mapping and rotate_regs() behavior.
     */
    uint32_t sor8 = extract64(env->cfm, IA64_CFM_SOR_SHIFT, 4);
    if (sor8 == 0) {
        return;
    }

    uint32_t sor_regs = sor8 << 3;
    sor_regs = MIN(sor_regs, 96U);
    sor_regs = MIN(sor_regs, (uint32_t)(env->cfm & 0x7fU));
    if (sor_regs <= 1) {
        return;
    }

    uint64_t last = env->r[32 + sor_regs - 1];
    for (uint32_t i = sor_regs - 1; i > 0; i--) {
        env->r[32 + i] = env->r[32 + i - 1];
    }
    env->r[32] = last;
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

    /* ar.pfs is updated by the callee's alloc. */
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

void HELPER(fw_enter_kernel)(CPUIA64State *env)
{
    if (!env->fw_preboot_active || !env->fw_preboot_ip) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64 FW: handoff to kernel entry=%016" PRIx64
                  " r28=%016" PRIx64 "\n",
                  env->fw_preboot_ip, env->fw_preboot_r28);

    /*
     * Reset the CPU core state so the kernel starts from the same baseline as
     * direct -kernel boot, while keeping any firmware-written memory intact.
     */
    CPUState *cs = env_cpu(env);
    uint64_t entry = env->fw_preboot_ip;
    uint64_t boot_r28 = env->fw_preboot_r28;
    uint64_t kernel_low = env->fw_preboot_kernel_low;
    uint64_t ar_k0 = env->ar[0];

    /* Snapshot the boot parameters after firmware ran. */
    {
        typedef struct ia64_boot_param {
            uint64_t command_line;
            uint64_t efi_systab;
            uint64_t efi_memmap;
            uint64_t efi_memmap_size;
            uint64_t efi_memdesc_size;
            uint32_t efi_memdesc_version;
            struct {
                uint16_t num_cols;
                uint16_t num_rows;
                uint16_t orig_x;
                uint16_t orig_y;
            } console_info;
            uint64_t fpswa;
            uint64_t initrd_start;
            uint64_t initrd_size;
            uint64_t domain_start;
            uint64_t domain_size;
        } IpfBootParam;

        IpfBootParam bp = { 0 };
        MemTxResult r =
            address_space_read(&address_space_memory, boot_r28,
                               MEMTXATTRS_UNSPECIFIED, &bp, sizeof(bp));
        if (r == MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64 FW: boot_param cmdline=%016" PRIx64
                          " efi_systab=%016" PRIx64
                          " efi_memmap=%016" PRIx64 " memmap_size=%" PRIu64
                          " desc_size=%" PRIu64 " desc_ver=%u"
                          " initrd=%016" PRIx64 "+%" PRIu64
                          " domain=%016" PRIx64 "+%" PRIu64 "\n",
                          bp.command_line, bp.efi_systab,
                          bp.efi_memmap, bp.efi_memmap_size,
                          bp.efi_memdesc_size, bp.efi_memdesc_version,
                          bp.initrd_start, bp.initrd_size,
                          bp.domain_start, bp.domain_size);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64 FW: boot_param read failed at %016" PRIx64 "\n",
                          boot_r28);
        }
    }

    cpu_reset(cs);

    /* Preserve the legacy I/O port window base across the reset. */
    env->ar[0] = ar_k0;

    /* Provide a deterministic initial stack pointer in physical mode. */
    uint64_t stack_top = kernel_low;
    if (stack_top > (1ULL << 20)) {
        stack_top -= (1ULL << 20); /* 1MiB below kernel base */
    } else {
        stack_top = (1ULL << 20);
    }
    stack_top &= ~0xFULL;
    env->r[12] = stack_top;

    /*
     * Seed cr.pta so Linux/ia64's early IVT itlb/dtlb miss handlers can locate
     * PTEs via cr.iha before ia64_mmu_init() programs the final VMLPT layout.
     *
     * Model a CPU with a 61-bit implemented VA space and Linux/ia64 64K pages:
     *   vmlpt_bits = impl_va_bits - PAGE_SHIFT + pte_bits = 61 - 16 + 3 = 48
     *   pta_base   = 2^61 - 2^vmlpt_bits
     *
     * Keep PTA.VE=0 so early Linux uses the software TLB miss handlers.
     */
    const uint64_t impl_va_bits = 61;
    const uint64_t page_shift = 16;
    const uint64_t pte_bits = 3;
    const uint64_t vmlpt_bits = impl_va_bits - page_shift + pte_bits; /* 48 */
    uint64_t pta_base = (1ULL << 61) - (1ULL << vmlpt_bits);
    uint64_t pta = 0;
    pta |= pta_base;
    pta |= (vmlpt_bits << 2); /* SIZE */
    env->cr[8] = pta;

    env->r[28] = boot_r28;
    env->ip = entry;

    env->fw_preboot_active = 0;
}

#ifndef CONFIG_USER_ONLY
static bool ia64_fw_fastpath_copy(uint64_t dst, uint64_t src, uint64_t len)
{
    while (len) {
        hwaddr src_len = len;
        hwaddr dst_len = len;
        void *src_p = cpu_physical_memory_map(src, &src_len, false);
        if (!src_p) {
            return false;
        }
        void *dst_p = cpu_physical_memory_map(dst, &dst_len, true);
        if (!dst_p) {
            cpu_physical_memory_unmap(src_p, src_len, false, 0);
            return false;
        }

        hwaddr chunk = MIN(src_len, dst_len);
        memmove(dst_p, src_p, chunk);

        cpu_physical_memory_unmap(src_p, src_len, false, chunk);
        cpu_physical_memory_unmap(dst_p, dst_len, true, chunk);

        src += chunk;
        dst += chunk;
        len -= chunk;
    }
    return true;
}

static bool ia64_fw_fastpath_fill(uint64_t dst, uint64_t len, int value)
{
    while (len) {
        hwaddr dst_len = len;
        void *dst_p = cpu_physical_memory_map(dst, &dst_len, true);
        if (!dst_p) {
            return false;
        }

        hwaddr chunk = dst_len;
        memset(dst_p, value, chunk);
        cpu_physical_memory_unmap(dst_p, dst_len, true, chunk);

        dst += chunk;
        len -= chunk;
    }
    return true;
}
#endif /* !CONFIG_USER_ONLY */

uint32_t HELPER(fw_fastpath)(CPUIA64State *env, uint64_t pc, uint32_t ri)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)ri;
    return 0;
#else
    /*
     * Fast-path for common bytewise memset/memcpy loops in the Xen/EDK guest
     * firmware. This massively speeds up PEI/DXE memory init under TCG.
     *
     * The patterns below were identified by disassembling the hot loops
     * surfaced by the ia64 hang detector (see QEMU_IA64_HANG_ABORT).
     */
    static int trace_enabled = -1;
    static int trace_count;
    if (trace_enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_FASTPATH_TRACE");
        trace_enabled = (s && *s) ? 1 : 0;
    }
    if (ri != 0) {
        return 0;
    }
    if (env->psr & IA64_PSR_DT) {
        /* Only handle physical-mode loops. */
        return 0;
    }

    hwaddr phys_pc = ia64_phys_mode_addr(pc);

    uint8_t buf[32];
    cpu_physical_memory_read(phys_pc, buf, sizeof(buf));
    uint64_t low0 = ldq_le_p(buf + 0);
    uint64_t high0 = ldq_le_p(buf + 8);
    uint64_t low1 = ldq_le_p(buf + 16);
    uint64_t high1 = ldq_le_p(buf + 24);

    /*
     * Bytewise memcpy loop (forward):
     *   struct { dst, count, src } at r12 offsets 0,8,16.
     *   exit at pc + 0xc0.
     */
    if (low0 == 0x41e021001820f811ULL &&
        high0 == 0x2000000000420030ULL &&
        low1 == 0x00f010183e00f80bULL &&
        high1 == 0x84006083e070007cULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t tmp[8];
        cpu_physical_memory_read(frame + 0, tmp, sizeof(tmp));
        uint64_t dst_raw = ldq_le_p(tmp);
        cpu_physical_memory_read(frame + 8, tmp, sizeof(tmp));
        uint64_t count = ldq_le_p(tmp);
        cpu_physical_memory_read(frame + 16, tmp, sizeof(tmp));
        uint64_t src_raw = ldq_le_p(tmp);

        /*
         * Firmware passes region-encoded pointers even in physical mode.
         * Match ia64_cpu_tlb_fill()'s physical-mode masking.
         */
        hwaddr dst_phys = ia64_phys_mode_addr(dst_raw);
        hwaddr src_phys = ia64_phys_mode_addr(src_raw);

        if (count && !ia64_fw_fastpath_copy(dst_phys, src_phys, count)) {
            return 0;
        }

        if (trace_enabled && trace_count++ < 64) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath memcpy pc=%016" PRIx64
                          " dst=%016" PRIx64 " src=%016" PRIx64
                          " len=%" PRIu64 "\n",
                          pc, dst_raw, src_raw, count);
        }

        dst_raw += count;
        src_raw += count;
        /*
         * Mirror the loop's final stored count value: when count reaches 0 it
         * stores -1 and exits (decrement before the exit branch).
         */
        count = UINT64_MAX;

        stq_le_p(tmp, dst_raw);
        cpu_physical_memory_write(frame + 0, tmp, sizeof(tmp));
        stq_le_p(tmp, count);
        cpu_physical_memory_write(frame + 8, tmp, sizeof(tmp));
        stq_le_p(tmp, src_raw);
        cpu_physical_memory_write(frame + 16, tmp, sizeof(tmp));

        /* Mirror the loop's (p15) mov r8=r0 success return. */
        env->r[8] = 0;
        env->ip = pc + 0xc0;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    if ((low0 == 0xc1e021001860f811ULL &&
         high0 == 0x2000000000420030ULL &&
         low1 == 0x00f010183e00f80bULL &&
         high1 == 0x84006183e070007cULL) ||
        (low0 == 0x000011983c7c0019ULL &&
         (high0 == 0x48ffff8800000200ULL ||
          high0 == 0x48ffff9800000200ULL))) {
        /*
         * Bytewise memset/memfill loops:
         *   struct { dst, ..., count, fill_byte } in r12 frame.
         *
         * Some hot loops enter at the tail bundle (store + br.many backedge).
         * Detect that and match against the loop head.
         */
        uint64_t fill_head_pc = pc;
        hwaddr fill_head_phys_pc = phys_pc;
        if (low0 == 0x000011983c7c0019ULL &&
            (high0 == 0x48ffff8800000200ULL || high0 == 0x48ffff9800000200ULL)) {
            fill_head_pc = pc - 0x80;
            fill_head_phys_pc = ia64_phys_mode_addr(fill_head_pc);
        }

        uint8_t fbuf[80];
        cpu_physical_memory_read(fill_head_phys_pc, fbuf, sizeof(fbuf));
        uint64_t f_low0 = ldq_le_p(fbuf + 0);
        uint64_t f_high0 = ldq_le_p(fbuf + 8);
        uint64_t f_low1 = ldq_le_p(fbuf + 16);
        uint64_t f_high1 = ldq_le_p(fbuf + 24);
        uint64_t f_high3 = ldq_le_p(fbuf + 56);
        uint64_t f_low4 = ldq_le_p(fbuf + 64);

        if (f_low0 == 0xc1e021001860f811ULL &&
            f_high0 == 0x2000000000420030ULL &&
            f_low1 == 0x00f010183e00f80bULL &&
            f_high1 == 0x84006183e070007cULL) {
            hwaddr frame = ia64_phys_mode_addr(env->r[12]);
            uint8_t tmp[8];
            cpu_physical_memory_read(frame + 0, tmp, sizeof(tmp));
            uint64_t dst_raw = ldq_le_p(tmp);
            cpu_physical_memory_read(frame + 24, tmp, sizeof(tmp));
            uint64_t count = ldq_le_p(tmp);
            hwaddr dst_phys = ia64_phys_mode_addr(dst_raw);

            uint32_t exit_off = 0;
            int fill_val = 0;
            if (f_high3 == 0x4200005807800200ULL &&
                f_low4 == 0x01e021001800f811ULL) {
                /* Zero fill, exit at pc + 0x80. */
                exit_off = 0x80;
                fill_val = 0;
            } else if (f_high3 == 0x4200006807800200ULL &&
                       f_low4 == 0x01e021001880f811ULL) {
                /* Byte fill, exit at pc + 0x90. */
                exit_off = 0x90;
                uint8_t b;
                cpu_physical_memory_read(frame + 32, &b, 1);
                fill_val = b;
            } else {
                return 0;
            }

            if (count && !ia64_fw_fastpath_fill(dst_phys, count, fill_val)) {
                return 0;
            }

            if (trace_enabled && trace_count++ < 64) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "fw_fastpath fill pc=%016" PRIx64
                              " dst=%016" PRIx64
                              " len=%" PRIu64 " val=0x%02x\n",
                              fill_head_pc, dst_raw, count, fill_val & 0xff);
            }

            dst_raw += count;
            /* Mirror the loop's final stored count value (see memcpy case). */
            count = UINT64_MAX;

            stq_le_p(tmp, dst_raw);
            cpu_physical_memory_write(frame + 0, tmp, sizeof(tmp));
            stq_le_p(tmp, count);
            cpu_physical_memory_write(frame + 24, tmp, sizeof(tmp));

            env->ip = fill_head_pc + exit_off;
            env->psr &= ~PSR_RI_MASK;
            return 1;
        }
    }

    return 0;
#endif
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
     * Canonical per-cpu region mapping.
     *
     * Linux/ia64 keeps per-cpu variables at a fixed canonical virtual range
     * (-PERCPU_PAGE_SIZE..0).  The OS maintains a runtime per-cpu area
     * elsewhere and exposes it via ar.k3 (IA64_KR_PER_CPU_DATA) so that
     * physical addresses can be computed as:
     *
     *   phys = ar.k3 + &per_cpu_var
     *
     * The canonical virtual mapping is normally established by IVT handlers,
     * but for bringup we map it directly to the current per-cpu area so that
     * local_cpu_data accesses see the initialized runtime values.
     */
    if ((uint64_t)address >= IA64_PERCPU_VA_BASE) {
        uint64_t k3 = env->ar[IA64_KR_PER_CPU_DATA];
        if (k3) {
            hwaddr phys_addr = (uint64_t)address + k3;
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

    /*
     * Per-cpu alias in region 6.
     *
     * Some early boot code uses per_cpu() pointers computed as:
     *   ptr = __per_cpu_offset[cpu] + &percpu_symbol
     *
     * On ia64 this can yield a region-6 address whose bits are the canonical
     * per-cpu virtual address with the region number decremented (i.e. a
     * 0xdfff... alias of a 0xffff... canonical address). Map this alias to the
     * current per-cpu physical area using ar.k3, mirroring the direct
     * canonical mapping above.
     */
    if (extract64(address, 61, 3) == 6 && extract64(address, 60, 1) == 1) {
        uint64_t canon = (uint64_t)address | (1ULL << 61); /* -> region 7 */
        if (canon >= IA64_PERCPU_VA_BASE) {
            uint64_t k3 = env->ar[IA64_KR_PER_CPU_DATA];
            if (k3) {
                hwaddr phys_addr = canon + k3;
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
    }

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
    if (extract64(address, 61, 3) == 6 && extract64(address, 60, 1) == 0) {
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
        /*
         * Physical mode: the architecture still uses the region-encoded 64-bit
         * address form. Handle both region-encoded and sign-extended 32-bit
         * physical addresses produced by firmware, so accesses into the 32-bit
         * GFW window (e.g. 0x80000000ffffffb0 and 0xffffffffffE00000) map
         * correctly.
         */
        hwaddr phys_addr = ia64_phys_mode_addr(address);
        int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        tlb_set_page(cs, address & TARGET_PAGE_MASK,
                     phys_addr & TARGET_PAGE_MASK, prot,
                     mmu_idx, TARGET_PAGE_SIZE);
        return true;
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
    typeof(env->dtlb[0]) tr_match;

    /*
     * Check instruction vs data TLBs. Rid must match; page mask uses ps.
     */
    if (access_type == MMU_INST_FETCH) {
        for (int i = 0; i < ARRAY_SIZE(env->itrs); i++) {
            if (!env->itrs[i].valid || env->itrs[i].rid != rid) {
                continue;
            }
            if (!PTE_P(env->itrs[i].pte)) {
                continue;
            }
            uint64_t mask = ~((1ULL << env->itrs[i].ps) - 1);
            if ((address & mask) == env->itrs[i].tag) {
                phys_addr = env->itrs[i].pa + (address & ~mask);
                tr_match.tag = env->itrs[i].tag;
                tr_match.pa = env->itrs[i].pa;
                tr_match.ps = env->itrs[i].ps;
                tr_match.rid = rid;
                tr_match.ar = PTE_AR(env->itrs[i].pte);
                tr_match.pl = PTE_PL(env->itrs[i].pte);
                tr_match.d  = PTE_D(env->itrs[i].pte);
                tr_match.a  = PTE_A(env->itrs[i].pte);
                tr_match.p  = 1;
                tr_match.ed = PTE_ED(env->itrs[i].pte);
                tr_match.valid = 1;
                match = &tr_match;
                hit = true;
                break;
            }
        }
    } else {
        for (int i = 0; i < ARRAY_SIZE(env->dtrs); i++) {
            if (!env->dtrs[i].valid || env->dtrs[i].rid != rid) {
                continue;
            }
            if (!PTE_P(env->dtrs[i].pte)) {
                continue;
            }
            uint64_t mask = ~((1ULL << env->dtrs[i].ps) - 1);
            if ((address & mask) == env->dtrs[i].tag) {
                phys_addr = env->dtrs[i].pa + (address & ~mask);
                tr_match.tag = env->dtrs[i].tag;
                tr_match.pa = env->dtrs[i].pa;
                tr_match.ps = env->dtrs[i].ps;
                tr_match.rid = rid;
                tr_match.ar = PTE_AR(env->dtrs[i].pte);
                tr_match.pl = PTE_PL(env->dtrs[i].pte);
                tr_match.d  = PTE_D(env->dtrs[i].pte);
                tr_match.a  = PTE_A(env->dtrs[i].pte);
                tr_match.p  = 1;
                tr_match.ed = PTE_ED(env->dtrs[i].pte);
                tr_match.valid = 1;
                match = &tr_match;
                hit = true;
                break;
            }
        }
    }

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

void HELPER(dbg_mem_watch)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                           uint64_t addr, uint32_t size, uint64_t val)
{
    static int log_limit = -1;
    static int log_count;

    if (log_limit == -1) {
        log_limit = 64;
        const char *s = getenv("QEMU_IA64_WATCH_STORE_LIMIT");
        if (s && *s) {
            log_limit = atoi(s);
        }
        if (log_limit < 0) {
            log_limit = 0;
        }
    }

    if (log_count++ >= log_limit) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "store_watch pc=%016" PRIx64 " ri=%u"
                  " ip=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                  " addr=%016" PRIx64 " size=%u val=%016" PRIx64 "\n",
                  pc, ri, env->ip, env->psr, env->cfm, addr, size, val);
}

void HELPER(hang_abort)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                        uint64_t threshold)
{
    if (threshold == 0) {
        return;
    }

    env->dbg_tb_total++;

    uint64_t lc = env->ar[65];
    uint64_t ec = env->ar[66];

    if (pc == env->dbg_tb_last_pc && ri == env->dbg_tb_last_ri &&
        lc == env->dbg_tb_last_lc && ec == env->dbg_tb_last_ec) {
        env->dbg_tb_same1++;
        env->dbg_tb_same2 = 0;
    } else if (pc == env->dbg_tb_last2_pc && ri == env->dbg_tb_last2_ri &&
               lc == env->dbg_tb_last2_lc && ec == env->dbg_tb_last2_ec) {
        env->dbg_tb_same2++;
        env->dbg_tb_same1 = 0;
    } else {
        env->dbg_tb_same1 = 0;
        env->dbg_tb_same2 = 0;
    }

    env->dbg_tb_last2_pc = env->dbg_tb_last_pc;
    env->dbg_tb_last2_ri = env->dbg_tb_last_ri;
    env->dbg_tb_last2_lc = env->dbg_tb_last_lc;
    env->dbg_tb_last2_ec = env->dbg_tb_last_ec;
    env->dbg_tb_last_pc = pc;
    env->dbg_tb_last_ri = ri;
    env->dbg_tb_last_lc = lc;
    env->dbg_tb_last_ec = ec;

    if (env->dbg_tb_same1 < threshold && env->dbg_tb_same2 < threshold) {
        return;
    }

    CPUState *cs = env_cpu(env);
    cpu_restore_state(cs, GETPC());

    /* Dump bundles at the stuck TB start to aid bringup/debugging. */
    for (int i = 0; i < 8; i++) {
        uint64_t bpc = pc + (uint64_t)i * 16;
        uint8_t bundle[16];
        if (cpu_memory_rw_debug(cs, bpc, bundle, sizeof(bundle), false) != 0) {
            break;
        }
        uint64_t low, high;
        memcpy(&low, &bundle[0], sizeof(low));
        memcpy(&high, &bundle[8], sizeof(high));
        low = le64_to_cpu(low);
        high = le64_to_cpu(high);
        uint8_t template = low & 0x1f;
        uint64_t slot0 = (low >> 5) & 0x1ffffffffffULL;
        uint64_t slot1 = ((low >> 46) | (high << 18)) & 0x1ffffffffffULL;
        uint64_t slot2 = (high >> 23) & 0x1ffffffffffULL;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64 hang_abort bundle[%d] pc=%016" PRIx64
                      " low=%016" PRIx64 " high=%016" PRIx64
                      " tmpl=%u s0=%011" PRIx64 " s1=%011" PRIx64
                      " s2=%011" PRIx64 "\n",
                      i, bpc, low, high, template, slot0, slot1, slot2);
    }

    uint32_t con_ok = 0;
    uint8_t con_waiter = 0;
    uint64_t con_owner = 0;
    uint64_t con_waiter_va = env->dbg_console_waiter_va;
    uint64_t con_owner_va = env->dbg_console_owner_va;
    if (env->kernel_bias) {
        if (con_waiter_va) {
            hwaddr pa = (hwaddr)(con_waiter_va - env->kernel_bias);
            if (address_space_read(&address_space_memory, pa,
                                   MEMTXATTRS_UNSPECIFIED,
                                   &con_waiter, sizeof(con_waiter)) == MEMTX_OK) {
                con_ok |= 1;
            }
        }
        if (con_owner_va) {
            hwaddr pa = (hwaddr)(con_owner_va - env->kernel_bias);
            uint64_t le = 0;
            if (address_space_read(&address_space_memory, pa,
                                   MEMTXATTRS_UNSPECIFIED,
                                   &le, sizeof(le)) == MEMTX_OK) {
                con_owner = le64_to_cpu(le);
                con_ok |= 2;
            }
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64 hang_abort threshold=%" PRIu64 " total=%" PRIu64
                  " tbpc=%016" PRIx64 " ri=%u"
                  " same1=%" PRIu64 " same2=%" PRIu64
                  " ip=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                  " pr=%016" PRIx64
                  " ar.lc=%016" PRIx64 " ar.ec=%016" PRIx64
                  " last_branch from=%016" PRIx64 " to=%016" PRIx64
                  " kind=%" PRIu64 " insn=%011" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64 " r13=%016" PRIx64
                  " r24=%016" PRIx64 " r27=%016" PRIx64
                  " r28=%016" PRIx64 " r29=%016" PRIx64
                  " r31=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r52=%016" PRIx64 " r53=%016" PRIx64
                  " kbias=%016" PRIx64
                  " con_waiter_va=%016" PRIx64 " con_waiter=%02x"
                  " con_owner_va=%016" PRIx64 " con_owner=%016" PRIx64
                  " con_ok=%u"
                  " b0=%016" PRIx64 " b6=%016" PRIx64 "\n",
                  threshold, env->dbg_tb_total, pc, ri,
                  env->dbg_tb_same1, env->dbg_tb_same2,
                  env->ip, env->psr, env->cfm, env->pr,
                  env->ar[65], env->ar[66],
                  env->last_branch_from, env->last_branch_to,
                  env->last_branch_kind, env->last_branch_insn,
                  env->r[1], env->r[12], env->r[13],
                  env->r[24], env->r[27], env->r[28], env->r[29],
                  env->r[31],
                  env->r[32], env->r[33], env->r[34],
                  env->r[52], env->r[53],
                  env->kernel_bias,
                  con_waiter_va, (unsigned)con_waiter,
                  con_owner_va, con_owner,
                  con_ok,
                  env->b[0], env->b[6]);
    cpu_abort(cs, "IA64: hang detected at tbpc=%016" PRIx64 " ri=%u", pc, ri);
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

static bool ia64_fw_log_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_LOG");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

#define IA64_PAL_STATUS_SUCCESS        0
#define IA64_PAL_STATUS_UNIMPLEMENTED  (-1)

#define IA64_PAL_CACHE_FLUSH     1
#define IA64_PAL_CACHE_INFO      2
#define IA64_PAL_CACHE_INIT      3
#define IA64_PAL_CACHE_SUMMARY   4
#define IA64_PAL_MEM_ATTRIB      5
#define IA64_PAL_PTCE_INFO       6
#define IA64_PAL_VM_SUMMARY      8
#define IA64_PAL_FREQ_BASE       13
#define IA64_PAL_FREQ_RATIOS     14
#define IA64_PAL_RSE_INFO        19
#define IA64_PAL_VM_PAGE_SIZE    34
#define IA64_PAL_HALT            28
#define IA64_PAL_HALT_LIGHT      29
#define IA64_PAL_LOGICAL_TO_PHYSICAL 42
#define IA64_PAL_CACHE_SHARED_INFO   43
#define IA64_PAL_BRAND_INFO          274

void HELPER(fw_pal)(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    uint64_t idx = env->r[28];
    bool from_call = ((env->last_b0_write_kind & 0xff) == 1);
    uint64_t a1 = from_call ? env->r[33] : env->r[29];
    uint64_t a2 = from_call ? env->r[34] : env->r[30];
    uint64_t a3 = from_call ? env->r[35] : env->r[31];

    int64_t status = IA64_PAL_STATUS_SUCCESS;
    uint64_t v0 = 0, v1 = 0, v2 = 0;

    switch (idx) {
    case IA64_PAL_CACHE_FLUSH:
    case IA64_PAL_CACHE_INIT:
        /* Nothing to do for the software model. */
        break;
    case IA64_PAL_CACHE_SUMMARY:
        /* levels=3, unique_caches=5 (enough for Linux cache init). */
        v0 = 3;
        v1 = 5;
        break;
    case IA64_PAL_CACHE_INFO: {
        /*
         * Return basic cache geometry.
         *
         * Inputs:
         *   a1: cache_level
         *   a2: cache_type (1=instruction, 2=data_or_unified)
         */
        uint8_t level = (uint8_t)a1;
        uint8_t type = (uint8_t)a2;

        uint8_t unified = (type == 2) ? 1 : 0;
        uint8_t attr = 1;      /* PAL_CACHE_ATTR_WB */
        uint8_t assoc = 8;
        uint8_t line_size = 6; /* log2(64) */
        uint8_t stride = 6;    /* 64-byte stride */

        uint32_t cache_size = 0;
        switch (level) {
        case 0:
            cache_size = unified ? (64 * 1024) : (32 * 1024);
            break;
        case 1:
            cache_size = 256 * 1024;
            break;
        default:
            cache_size = 1024 * 1024;
            break;
        }

        /* pal_cache_config_info_1_t::pcci1_data */
        v0 |= (uint64_t)(unified & 1) << 0;
        v0 |= (uint64_t)(attr & 3) << 1;
        v0 |= (uint64_t)assoc << 8;
        v0 |= (uint64_t)line_size << 16;
        v0 |= (uint64_t)stride << 24;

        /* pal_cache_config_info_2_t::pcci2_data */
        v1 = (uint64_t)cache_size;
        v2 = 0;
        break;
    }
    case IA64_PAL_MEM_ATTRIB:
        /* bitmask; Linux uses low byte. */
        v0 = 0xF1;
        break;
    case IA64_PAL_PTCE_INFO:
        /*
         * local_flush_tlb_all() uses ptc.e in a PAL-driven loop. Our ptc.e
         * helper overpurges and flushes all translation caches, so the loop
         * bounds are not important as long as they execute at least once.
         */
        v0 = 0;                  /* base */
        v1 = (1ULL << 32) | 1ULL; /* count[0]=1,count[1]=1 */
        v2 = 0;                  /* stride[0]=0,stride[1]=0 */
        break;
    case IA64_PAL_VM_SUMMARY: {
        /* Provide sane VA/PA limits and TC parameters. */
        uint8_t vw = 1;
        uint8_t phys_add_size = 44;
        uint8_t key_size = 18;
        uint8_t max_pkr = 16;
        uint8_t hash_tag_id = 0;
        uint8_t max_dtr_entry = 7;
        uint8_t max_itr_entry = 7;
        uint8_t max_unique_tcs = 1;
        uint8_t num_tc_levels = 1;

        uint8_t impl_va_msb = 60;
        uint8_t rid_size = 18;
        uint16_t max_purges = 0xFFFF;

        v0 |= (uint64_t)(vw & 1) << 0;
        v0 |= (uint64_t)(phys_add_size & 0x7F) << 1;
        v0 |= (uint64_t)key_size << 8;
        v0 |= (uint64_t)max_pkr << 16;
        v0 |= (uint64_t)hash_tag_id << 24;
        v0 |= (uint64_t)max_dtr_entry << 32;
        v0 |= (uint64_t)max_itr_entry << 40;
        v0 |= (uint64_t)max_unique_tcs << 48;
        v0 |= (uint64_t)num_tc_levels << 56;

        v1 |= (uint64_t)impl_va_msb << 0;
        v1 |= (uint64_t)rid_size << 8;
        v1 |= (uint64_t)max_purges << 16;
        break;
    }
    case IA64_PAL_VM_PAGE_SIZE:
        v0 = 0x115557000ULL;
        v1 = 0x115557000ULL;
        break;
    case IA64_PAL_FREQ_BASE:
        v0 = 100000000ULL;
        break;
    case IA64_PAL_FREQ_RATIOS: {
        /* u64 packing of struct pal_freq_ratio { u32 den, num; } */
        uint32_t den = 1;
        uint32_t num = 3;
        v0 = (uint64_t)den | ((uint64_t)num << 32); /* proc ratio */
        v1 = (uint64_t)den | ((uint64_t)1 << 32);   /* bus ratio */
        v2 = (uint64_t)den | ((uint64_t)num << 32); /* itc ratio */
        break;
    }
    case IA64_PAL_HALT:
    case IA64_PAL_HALT_LIGHT:
        /*
         * Enter (light) halt state.
         *
         * For bringup under TCG we do not model low-power suspension here.
         * Linux uses PAL_HALT_LIGHT in arch_safe_halt() when idling; returning
         * success keeps the CPU running and avoids stalling boot if interrupt
         * delivery is not yet fully modeled.
         */
        break;
    case IA64_PAL_RSE_INFO:
        v0 = 96;
        v1 = 0;
        break;
    case IA64_PAL_LOGICAL_TO_PHYSICAL: {
        /*
         * Logical-to-physical mapping query.
         *
         * Linux calls this with proc_number=-1 during early CPU topology init.
         * Provide a simple single-core/single-thread topology.
         */
        uint16_t la = (a1 == UINT64_MAX) ? 0 : (uint16_t)a1;
        uint16_t num_log = 1;
        uint8_t tpc = 1;
        uint8_t cpp = 1;
        uint8_t ppid = 0;

        v0 = 0;
        v0 |= (uint64_t)num_log;
        v0 |= (uint64_t)tpc << 16;
        v0 |= (uint64_t)cpp << 32;
        v0 |= (uint64_t)ppid << 48;

        v1 = 0; /* tid=0,cid=0 */
        v2 = (uint64_t)la;
        break;
    }
    case IA64_PAL_CACHE_SHARED_INFO:
        /* Report cache not shared beyond this logical processor. */
        v0 = 1; /* num_shared */
        v1 = 0; /* tid=0,cid=0 */
        v2 = 0; /* la=0 */
        break;
    case IA64_PAL_BRAND_INFO: {
        /*
         * Processor branding string.
         *
         * Kernel passes a pointer to a 128-byte buffer (on the stack) in a2.
         * For bringup, report this call as unimplemented so Linux falls back
         * to family/model-based naming without requiring buffer writes.
         */
        status = IA64_PAL_STATUS_UNIMPLEMENTED;
        break;
    }
    default:
        if (ia64_fw_log_enabled()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: PAL idx=%" PRIu64 " a1=%" PRIu64 " a2=%" PRIu64
                          " a3=%" PRIu64 " from_call=%d\n",
                          idx, a1, a2, a3, from_call);
        }
        cpu_restore_state(cs, GETPC());
        cpu_abort(cs, "IA64: PAL unhandled idx=%" PRIu64, idx);
    }

    if (ia64_fw_log_enabled()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: PAL idx=%" PRIu64 " -> status=%" PRId64
                      " v0=%016" PRIx64 " v1=%016" PRIx64 " v2=%016" PRIx64 "\n",
                      idx, status, v0, v1, v2);
    }

    env->r[8] = (uint64_t)status;
    env->r[9] = v0;
    env->r[10] = v1;
    env->r[11] = v2;
}

#define IA64_SAL_SET_VECTORS          0x01000000ULL
#define IA64_SAL_GET_STATE_INFO       0x01000001ULL
#define IA64_SAL_GET_STATE_INFO_SIZE  0x01000002ULL
#define IA64_SAL_CLEAR_STATE_INFO     0x01000003ULL
#define IA64_SAL_MC_RENDEZ            0x01000004ULL
#define IA64_SAL_MC_SET_PARAMS        0x01000005ULL
#define IA64_SAL_REGISTER_PHYS_ADDR   0x01000006ULL
#define IA64_SAL_CACHE_FLUSH          0x01000008ULL
#define IA64_SAL_CACHE_INIT           0x01000009ULL
#define IA64_SAL_PCI_CONFIG_READ      0x01000010ULL
#define IA64_SAL_PCI_CONFIG_WRITE     0x01000011ULL
#define IA64_SAL_FREQ_BASE            0x01000012ULL
#define IA64_SAL_PHYSICAL_ID_INFO     0x01000013ULL
#define IA64_SAL_UPDATE_PAL           0x01000020ULL

void HELPER(fw_sal)(CPUIA64State *env)
{
    uint64_t func_raw = env->r[32];
    /*
     * Some IA-64 guest firmware uses the low 24-bit SAL function numbers
     * without the 0x01000000 prefix that Linux uses (e.g. 0x10 instead of
     * 0x01000010 for PCI_CONFIG_READ). Normalize those calls here.
     */
    uint64_t func = func_raw;
    if ((func & 0xff000000ULL) == 0) {
        func |= 0x01000000ULL;
    }

    int64_t status = 0;
    uint64_t v0 = 0, v1 = 0, v2 = 0;

    switch (func) {
    case IA64_SAL_SET_VECTORS:
    case IA64_SAL_CACHE_FLUSH:
    case IA64_SAL_CACHE_INIT:
    case IA64_SAL_MC_SET_PARAMS:
    case IA64_SAL_REGISTER_PHYS_ADDR:
        status = 0;
        break;
    case IA64_SAL_GET_STATE_INFO:
        /*
         * Match SKI/Xen firmware expectations: report no state info available
         * (SAL_STATUS_NO_ENTRY == -5 in common implementations).
         */
        status = -5;
        break;
    case IA64_SAL_GET_STATE_INFO_SIZE:
        status = 0;
        v0 = 1;
        break;
    case IA64_SAL_CLEAR_STATE_INFO:
    case IA64_SAL_MC_RENDEZ:
        status = 0;
        break;
    case IA64_SAL_FREQ_BASE:
        status = 0;
        v0 = 100000000ULL; /* ticks per second */
        v1 = ~0ULL;        /* drift info: -1 */
        v2 = 0;
        break;
    case IA64_SAL_PCI_CONFIG_READ: {
        /*
         * SAL-based PCI config space access (see Linux arch/ia64/pci/pci.c).
         *
         * The xenipf guest firmware expects these calls to succeed; implement
         * them via PCI config mechanism #1 (0xcf8/0xcfc) which is provided by
         * the IPF machine.
         *
         * Arguments:
         *   r33: encoded pci_config_addr
         *   r34: access size (1/2/4[/8])
         *   r35: type/mode (0 = legacy, 1 = extended)
         */
        uint64_t pci_addr = env->r[33];
        uint64_t size = env->r[34];
        uint64_t mode = env->r[35];

        uint16_t seg;
        uint8_t bus, devfn;
        uint16_t reg;

        if (mode == 0) {
            seg = (pci_addr >> 24) & 0xff;
            bus = (pci_addr >> 16) & 0xff;
            devfn = (pci_addr >> 8) & 0xff;
            reg = pci_addr & 0xff;
        } else if (mode == 1) {
            seg = (pci_addr >> 28) & 0xffff;
            bus = (pci_addr >> 20) & 0xff;
            devfn = (pci_addr >> 12) & 0xff;
            reg = pci_addr & 0xfff;
        } else {
            status = -1;
            break;
        }

        if (seg != 0 || reg > 0xff) {
            status = -1;
            break;
        }

        uint32_t cfgaddr = 0x80000000U | ((uint32_t)bus << 16) |
                           ((uint32_t)devfn << 8) | (reg & ~3U);
        cpu_outl(0xcf8, cfgaddr);

        /*
         * Match SKI/Xen firmware expectations: size==1/2 selects byte/word,
         * anything else reads a dword.
         */
        if (size == 1) {
            v0 = cpu_inb(0xcfc + (reg & 3));
            status = 0;
        } else if (size == 2) {
            v0 = cpu_inw(0xcfc + (reg & 2));
            status = 0;
        } else {
            v0 = cpu_inl(0xcfc);
            status = 0;
        }

        if (ia64_fw_log_enabled()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: SAL_PCI_CONFIG_READ mode=%" PRIu64
                          " seg=%u bus=%u devfn=%u reg=0x%x size=%" PRIu64
                          " -> status=%" PRId64 " v0=%016" PRIx64 "\n",
                          mode, seg, bus, devfn, reg, size, status, v0);
        }
        break;
    }
    case IA64_SAL_PCI_CONFIG_WRITE: {
        /*
         * Arguments:
         *   r33: encoded pci_config_addr
         *   r34: access size (1/2/4[/8])
         *   r35: value
         *   r36: type/mode (0 = legacy, 1 = extended)
         */
        uint64_t pci_addr = env->r[33];
        uint64_t size = env->r[34];
        uint64_t value = env->r[35];
        uint64_t mode = env->r[36];

        uint16_t seg;
        uint8_t bus, devfn;
        uint16_t reg;

        if (mode == 0) {
            seg = (pci_addr >> 24) & 0xff;
            bus = (pci_addr >> 16) & 0xff;
            devfn = (pci_addr >> 8) & 0xff;
            reg = pci_addr & 0xff;
        } else if (mode == 1) {
            seg = (pci_addr >> 28) & 0xffff;
            bus = (pci_addr >> 20) & 0xff;
            devfn = (pci_addr >> 12) & 0xff;
            reg = pci_addr & 0xfff;
        } else {
            status = -1;
            break;
        }

        if (seg != 0 || reg > 0xff) {
            status = -1;
            break;
        }

        uint32_t cfgaddr = 0x80000000U | ((uint32_t)bus << 16) |
                           ((uint32_t)devfn << 8) | (reg & ~3U);
        cpu_outl(0xcf8, cfgaddr);

        /*
         * Match SKI/Xen firmware expectations: size==1/2 selects byte/word,
         * anything else writes a dword.
         */
        if (size == 1) {
            cpu_outb(0xcfc + (reg & 3), (uint8_t)value);
            status = 0;
        } else if (size == 2) {
            cpu_outw(0xcfc + (reg & 2), (uint16_t)value);
            status = 0;
        } else {
            cpu_outl(0xcfc, (uint32_t)value);
            status = 0;
        }

        if (ia64_fw_log_enabled()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: SAL_PCI_CONFIG_WRITE mode=%" PRIu64
                          " seg=%u bus=%u devfn=%u reg=0x%x size=%" PRIu64
                          " value=%016" PRIx64 " -> status=%" PRId64 "\n",
                          mode, seg, bus, devfn, reg, size, value, status);
        }
        break;
    }
    case IA64_SAL_PHYSICAL_ID_INFO:
    case IA64_SAL_UPDATE_PAL:
    default:
        status = -1;
        break;
    }

    if (ia64_fw_log_enabled()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: SAL func=%016" PRIx64 " (raw=%016" PRIx64 ") -> status=%" PRId64
                      " v0=%016" PRIx64 " v1=%016" PRIx64 " v2=%016" PRIx64 "\n",
                      func, func_raw, status, v0, v1, v2);
    }

    env->r[8] = (uint64_t)status;
    env->r[9] = v0;
    env->r[10] = v1;
    env->r[11] = v2;
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
     * Minimal loadrs implementation sufficient for reaching userspace.
     *
     * We do not model the full RSE backing-store/dirty-partition machinery
     * yet. Treat loadrs as synchronizing bspstore with bsp and clearing the
     * loadrs field, which is enough for Linux's rse_clear_invalid path when
     * starting the first user process.
     */
    uint64_t rsc = env->ar[IA64_AR_RSC];
    (void)ia64_rsc_get_loadrs(rsc);
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
#ifndef CONFIG_USER_ONLY
    if (ia64_is_task_switch_pc(env, env->ip)) {
        ia64_rse_switch_bspstore(env, bspstore);
    }
#endif
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
#ifndef CONFIG_USER_ONLY
    ia64_itm_update(env);
#endif
}

void HELPER(set_itm)(CPUIA64State *env, uint64_t val)
{
    env->cr[1] = val; /* cr.itm */
#ifndef CONFIG_USER_ONLY
    ia64_itm_update(env);
#endif
}

void HELPER(set_itv)(CPUIA64State *env, uint64_t val)
{
    env->cr[72] = val; /* cr.itv */
#ifndef CONFIG_USER_ONLY
    ia64_itm_update(env);
#endif
}

void HELPER(eoi)(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    env->cr[65] = IA64_SPURIOUS_INT_VECTOR; /* cr.ivr */
#ifndef CONFIG_USER_ONLY
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
#endif
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
