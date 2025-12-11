/*
 * IA-64 CPU header for QEMU
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef IA64_CPU_H
#define IA64_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-defs.h"
#include "exec/cpu-common.h"

typedef struct CPUArchState {
    uint64_t r[128];     /* General Registers */
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
    uint64_t cr_ifa;     /* Interruption faulting address */
    uint64_t cr_iim;     /* Interruption immediate */
    uint64_t cr_iha;     /* Interruption handler address */
    uint64_t cr[128];
    uint64_t ar[128];
    uint64_t rr[8];

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
        uint8_t valid;  /* entry contains translation */
    } itlb[128], dtlb[128];
    uint8_t itlb_next;
    uint8_t dtlb_next;
} CPUIA64State;

struct ArchCPU {
    /*< private >*/
    CPUState parent_obj;
    /*< public >*/

    CPUIA64State env;
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

void ia64_cpu_list(void);
void ia64_tcg_init(void);
void ia64_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);
bool ia64_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                       MMUAccessType access_type, int mmu_idx,
                       bool probe, uintptr_t retaddr);

#define cpu_signal_handler cpu_ia64_signal_handler
#define cpu_mmu_index cpu_ia64_mmu_index

static inline int cpu_ia64_mmu_index(CPUIA64State *env, bool ifetch)
{
    return MMU_KERNEL_IDX; // Placeholder
}

#define TARGET_INSN_START_EXTRA_WORDS 1

#define PSR_RI_SHIFT 41
#define PSR_RI_MASK  (3ULL << PSR_RI_SHIFT)

#endif
