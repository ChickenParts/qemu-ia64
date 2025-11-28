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

bool ia64_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                       MMUAccessType access_type, int mmu_idx,
                       bool probe, uintptr_t retaddr)
{
    /* IA64CPU *cpu = IA64_CPU(cs); */
    
    /* For now, just map everything 1:1 */
    hwaddr phys_addr = address;
    int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    
    tlb_set_page(cs, address & TARGET_PAGE_MASK,
                 phys_addr & TARGET_PAGE_MASK, prot,
                 mmu_idx, TARGET_PAGE_SIZE);
                 
    return true;
}

void helper_exception(CPUIA64State *env, int excp)
{
    CPUState *cs = env_cpu(env);
    cs->exception_index = excp;
    cpu_loop_exit(cs);
}
