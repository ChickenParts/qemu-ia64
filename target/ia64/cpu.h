/*
 * IA-64 CPU header for QEMU
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef IA64_CPU_H
#define IA64_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-interrupt.h"
#include "exec/cpu-defs.h"
#include "exec/cpu-common.h"

typedef struct QEMUTimer QEMUTimer;

typedef struct CPUArchState {
    uint64_t r[128];     /* General Registers */
    uint8_t nat[128];    /* NaT bits for general registers */
    uint64_t banked_r[16]; /* Banked GRs r16..r31 (PSR.BN selects bank) */
    uint8_t banked_nat[16];
    uint64_t f[128][2];  /* Floating Point Registers (82-bit, stored as 2x64 for now) */
    uint64_t pr;         /* Predicate Registers (64 bits) */
    uint64_t b[8];       /* Branch Registers */
    uint64_t ip;         /* Instruction Pointer */
    uint64_t psr;        /* Processor Status Register */
    uint64_t cfm;        /* Current Frame Marker */
    uint64_t cr_ipsr;    /* Processor status saved by interrupt */
    uint64_t cr_iip;     /* Saved instruction pointer */
    uint64_t cr_ifs;     /* Function state */
    uint64_t cr_isr;     /* Interrupt status */
    uint64_t cr_iipa;    /* Interruption instruction pointer address */
    uint64_t cr_ifa;     /* Interruption faulting address */
    uint64_t cr_iim;     /* Interruption immediate */
    uint64_t cr_iha;     /* Interruption handler address */
    uint64_t cr[128];
    uint64_t ar[128];
    uint64_t rr[8];
    uint64_t cpuid[5];

    /*
     * Advanced Load Address Table (ALAT), used by ld*.a + chk.a.{nc,clr}.
     *
     * For bringup we only model enough to satisfy Linux' usage patterns:
     * - Record ld*.a destinations with their load address + size.
     * - Invalidate overlapping entries on stores.
     * - chk.a.{nc,clr} branches on miss; .clr also clears the entry.
     */
    struct IA64ALATEntry {
        uint64_t addr;
        uint32_t size;
        uint8_t valid;
    } alat_gr[128];
    uint32_t alat_gr_valid;

    struct IA64ALATEntry alat_fr[128];
    uint32_t alat_fr_valid;

    struct {
        uint64_t tag;   /* virtual base aligned to page */
        uint64_t pa;    /* physical base aligned to page */
        uint32_t rid;   /* region id selector */
        uint8_t ps;     /* page-size log2 */
        uint8_t ar;     /* access rights */
        uint8_t pl;     /* privilege level */
        uint8_t d;      /* dirty */
        uint8_t a;      /* accessed */
        uint8_t p;      /* present */
        uint8_t ed;     /* exception deferral */
        uint8_t valid;  /* entry contains translation */
    } itlb[128], dtlb[128];
    uint8_t itlb_next;
    uint8_t dtlb_next;

    struct {
        uint64_t pte;   /* raw PTE for attributes */
        uint64_t itr;   /* raw ITR/DTR TAR value */
        uint64_t tag;
        uint64_t pa;
        uint32_t rid;
        uint8_t ps;
        uint8_t valid;
    } itrs[16], dtrs[16], itcs[128], dtcs[128];

    struct IA64RSEFrame {
        uint64_t r[96];     /* r32..r127 */
        uint8_t nat[96];
        uint64_t ar_pfs;
        uint64_t cfm;
        uint64_t ret_addr;  /* caller return address (bundle PC) */
    } *rse_frames;
    uint32_t rse_depth;
    uint32_t rse_capacity;

    /*
     * Interrupt snapshots for the stacked register window.
     *
     * Keep this separate from rse_frames (used for br.call/br.ret windowing)
     * so that rfi restores the interrupted context without interfering with
     * normal call/return bookkeeping performed inside IVT handlers.
     */
    struct IA64IntrFrame {
        uint64_t r[96];     /* r32..r127 */
        uint8_t nat[96];
        uint64_t ar_pfs;
    } *intr_frames;
    uint32_t intr_depth;
    uint32_t intr_capacity;

    /* Debug: last write to b0 (rp). */
    uint64_t last_b0_write_pc;
    uint64_t last_b0_write_insn;
    uint64_t last_b0_write_val;
    uint64_t last_b0_write_kind; /* 1=br.call/brl.call, 2=mov.b, 3=other */
    uint64_t prev_b0_write_pc;
    uint64_t prev_b0_write_insn;
    uint64_t prev_b0_write_val;
    uint64_t prev_b0_write_kind;

    /* Debug: small ring buffer of recent b0 writes. */
    uint64_t b0_trace_pc[16];
    uint64_t b0_trace_insn[16];
    uint64_t b0_trace_val[16];
    uint64_t b0_trace_kind[16];
    uint32_t b0_trace_idx;

    /* Debug: last control-flow change (branch/call/ret/rfi). */
    uint64_t last_branch_from;
    uint64_t last_branch_to;
    uint64_t last_branch_insn;
    uint64_t last_branch_kind; /* kind | (ri << 8) */

    /* Optional kernel text range for fail-fast diagnostics. */
    uint64_t kernel_stext;
    uint64_t kernel_etext;

    /* Optional kernel virtual-to-physical bias for region-5 linear mapping. */
    uint64_t kernel_bias;

    /* Optional kernel percpu PT_LOAD mapping (virtual -> physical). */
    uint64_t percpu_va_base;
    uint64_t percpu_pa_base;
    uint64_t percpu_size;

    /* Optional kernel symbol addresses for targeted bringup debugging. */
    uint64_t dbg_console_owner_va;
    uint64_t dbg_console_waiter_va;
    uint64_t dbg_switch_mode_phys_va;
    uint64_t dbg_switch_mode_virt_va;
    uint64_t dbg_switch_mode_phys_size;
    uint64_t dbg_switch_mode_virt_size;
    uint64_t dbg_ia64_switch_to_va;
    uint64_t dbg_ia64_switch_to_size;
    uint64_t dbg_load_switch_stack_va;
    uint64_t dbg_load_switch_stack_size;

    /* Debug: fail-fast hang detector (TB hot loop). */
    uint64_t dbg_tb_last_pc;
    uint64_t dbg_tb_last2_pc;
    uint64_t dbg_tb_last_lc;
    uint64_t dbg_tb_last2_lc;
    uint64_t dbg_tb_last_ec;
    uint64_t dbg_tb_last2_ec;
    uint64_t dbg_tb_total;
    uint64_t dbg_tb_same1;
    uint64_t dbg_tb_same2;
    uint32_t dbg_tb_last_ri;
    uint32_t dbg_tb_last2_ri;
    uint64_t dbg_fw_r8_last;
    uint64_t dbg_fw_r8_logged;
    uint64_t dbg_fw_call_ret_pc[8];
    uint64_t dbg_fw_call_entry_pc[8];
    uint32_t dbg_fw_call_depth;

    /*
     * Firmware-preboot handoff state.
     *
     * When enabled by the IPF machine, guest firmware runs first and then
     * transfers control to the loaded -kernel ELF once the firmware returns.
     */
    uint64_t fw_preboot_active;
    uint64_t fw_preboot_ip;
    uint64_t fw_preboot_r28;
    uint64_t fw_preboot_kernel_low;

    /* Firmware bringup: PEI handoff pointers for early call fixups. */
    uint64_t fw_pei_handoff;
    uint64_t fw_pei_ppi;
    uint64_t fw_pei_stack_count;

    /* Debug: last observed EFI PHIT values (xenipf/EDK firmware bringup). */
    uint64_t fw_phit_mem_top;
    uint64_t fw_phit_mem_bottom;
    uint64_t fw_phit_free_top;
    uint64_t fw_phit_free_bottom;
    uint64_t fw_hob_reloc_base;

    /* Firmware bringup: cached EFI system table physical address. */
    uint64_t fw_efi_systab_pa;

    /* Firmware bringup: one-way SAL systab config-table install guard. */
    uint64_t fw_sal_systab_installed;

    /* Cache flush throttle: last page invalidated by fc/fc.i. */
    uint64_t fc_last_page;
} CPUIA64State;

struct ArchCPU {
    /*< private >*/
    CPUState parent_obj;
    /*< public >*/

    CPUIA64State env;

#ifndef CONFIG_USER_ONLY
    QEMUTimer *itm_timer;
    struct IA64RSEContext *rse_ctxs;
    uint32_t rse_ctxs_len;
    uint32_t rse_ctxs_cap;
#endif
};

struct IA64CPUClass {
    /*< private >*/
    CPUClass parent_class;
    /*< public >*/
    
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

#ifndef CONFIG_USER_ONLY
#include "exec/hwaddr.h"
#endif

#define cpu_list ia64_cpu_list

#define CPU_RESOLVING_TYPE TYPE_IA64_CPU

#define MMU_USER_IDX 0
#define MMU_KERNEL_IDX 1
#define MMU_PHYS_IDX 2

/*
 * IPF machine firmware interface stubs.
 *
 * When booting a Linux kernel directly via -kernel (bypassing real firmware),
 * hw/ia64/ipf.c synthesizes an EFI environment and a minimal SAL system table.
 * Linux discovers the SAL systab via an EFI config table entry, then uses the
 * SAL entrypoint descriptor to find the PAL and SAL procedure addresses.
 *
 * We provide small emulated PAL/SAL entry points at fixed physical addresses in
 * low RAM (covered by the "reserved" EFI memory range) and a dedicated EFI
 * PAL_CODE memory descriptor so Linux installs a PAL ITR mapping.
 */
#define IA64_IPF_FW_SAL_SYSTAB_ADDR 0x0000000000017000ULL
#define IA64_IPF_FW_SAL_PROC_ADDR   0x0000000000018000ULL
#define IA64_IPF_FW_PAL_PROC_ADDR   0x0000000000019000ULL
#define IA64_IPF_FW_SAL_GP_ADDR     0x000000000001A000ULL
#define IA64_IPF_FW_PAL_SIZE        0x0000000000001000ULL

#define IA64_RGN_BASE(r)   ((uint64_t)(r) << 61)
#define IA64_RGN7_BASE     IA64_RGN_BASE(7)

/*
 * PSR bit positions (LSB indexing, matching the IA-64 SDM and SKI's BitfR()
 * extraction when converted to LSB indexing):
 *   AC  bit 3
 *   RI  bits 42:41
 *   CPL bits 33:32
 *   DT  bit 17
 *   IC  bit 13
 */
#define IA64_PSR_AC       (1ULL << 3)
#define IA64_PSR_IC       (1ULL << 13)
#define IA64_PSR_I        (1ULL << 14)
#define IA64_PSR_IT       (1ULL << 36)
#define IA64_PSR_DT       (1ULL << 17)
#define IA64_PSR_BN       (1ULL << 44)
#define IA64_PSR_CPL(psr) (((psr) >> 32) & 0x3)

#define IA64_SPURIOUS_INT_VECTOR 0x0f
#define IA64_ITV_MASK            (1ULL << 16)

/*
 * Floating-point helper conventions (minimal subset).
 *
 * The kernel uses getf.exp as part of ia64_fls().  Hardware returns the
 * biased exponent (17 bits) plus a sign bit packed in bit 17.
 */
#define IA64_FP_EXP_BIAS      0xFFFFU
#define IA64_FP_EXP_INTEGER   (IA64_FP_EXP_BIAS + 63U)
#define IA64_FP_SEXP(sign, exp) \
    ((((uint64_t)(sign) & 1ULL) << 17) | ((uint64_t)(exp) & 0x1FFFFULL))

/* Application Register indices (subset). */
#define IA64_AR_RSC       16
#define IA64_AR_BSP       17
#define IA64_AR_BSPSTORE  18
#define IA64_AR_RNAT      19
#define IA64_AR_FPSR      40
#define IA64_AR_ITC       44
#define IA64_AR_PFS       64

#define IA64_FPSR_DEFAULT 0x0009804c0270033fULL

#define IA64_EXCP_BASE    0x200
#define IA64_EXCP_VHPT_TRANS  (IA64_EXCP_BASE + 0x0)
#define IA64_EXCP_ITLB_MISS   (IA64_EXCP_BASE + 0x1)
#define IA64_EXCP_DTLB_MISS   (IA64_EXCP_BASE + 0x2)
#define IA64_EXCP_PAGE_NOT_P  (IA64_EXCP_BASE + 0x3)
#define IA64_EXCP_MA          (IA64_EXCP_BASE + 0x4)
#define IA64_EXCP_PAGE_ACC    (IA64_EXCP_BASE + 0x5)
#define IA64_EXCP_PAGE_DIRTY  (IA64_EXCP_BASE + 0x6)
#define IA64_EXCP_INST_ACCESS (IA64_EXCP_BASE + 0x52) /* access rights */
#define IA64_EXCP_DATA_ACCESS (IA64_EXCP_BASE + 0x53) /* access rights */

#define IA64_VEC_VHPT_INST            0x0000
#define IA64_VEC_INST_TLB             0x0400
#define IA64_VEC_DATA_TLB             0x0800
#define IA64_VEC_ALT_DATA_TLB         0x1000
#define IA64_VEC_DATA_NESTED_TLB      0x1400
#define IA64_VEC_INST_KEY_MISS        0x1800
#define IA64_VEC_DATA_KEY_MISS        0x1C00
#define IA64_VEC_DATA_DIRTY           0x2000
#define IA64_VEC_INST_ACCESS_BIT      0x2400
#define IA64_VEC_DATA_ACCESS_BIT      0x2800
#define IA64_VEC_BREAK                0x2C00
#define IA64_VEC_EXTERNAL_INTERRUPT   0x3000
#define IA64_VEC_ALT_INST_TLB         0x0C00
#define IA64_VEC_INST_PAGE_NOT_P      0x5000
#define IA64_VEC_DATA_PAGE_NOT_P      0x5000
#define IA64_VEC_INST_ACCESS_RIGHTS   0x5200
#define IA64_VEC_DATA_ACCESS_RIGHTS   0x5300

void ia64_cpu_list(void);
void ia64_tcg_init(void);
void ia64_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);
bool ia64_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                       MMUAccessType access_type, int mmu_idx,
                       bool probe, uintptr_t retaddr);
void ia64_intr_push_window(CPUIA64State *env);
#ifndef CONFIG_USER_ONLY
void ia64_itm_update(CPUIA64State *env);
void ia64_rse_switch_bspstore(CPUIA64State *env, uint64_t new_bspstore);
#endif

#define cpu_signal_handler cpu_ia64_signal_handler
#ifdef CONFIG_USER_ONLY
#define cpu_mmu_index cpu_ia64_mmu_index
#endif

static inline int cpu_ia64_mmu_index(CPUState *cs, bool ifetch)
{
    CPUIA64State *env = cpu_env(cs);
    (void)env;
    return MMU_KERNEL_IDX; // Placeholder
}

#define TARGET_INSN_START_EXTRA_WORDS 1

#define PSR_RI_SHIFT 41
#define PSR_RI_MASK  (3ULL << PSR_RI_SHIFT)

#endif
