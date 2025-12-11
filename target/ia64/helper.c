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

    /*
     * Temporary mapping policy (fail-fast outside known image):
     *  - Identity map low addresses.
     *  - Kernel text/data is linked at 0xa000000100000000 with an LMA
     *    around 0x04000000; map that region by subtracting the bias.
     *  - Abort on anything outside a conservative physical window.
     */
    hwaddr phys_addr = address;
    /* ELF vmlinux links text at 0xa000000100000000, LMA around 0x04000000. */
    const uint64_t kernel_bias = 0xa0000000fc000000ULL;
    const uint64_t phys_limit = 0x08000000ULL; /* ~128MB window */

    if (address >= 0xa000000000000000ULL) {
        phys_addr = address - kernel_bias;
    }
    if (phys_addr >= phys_limit) {
        return false;
    }
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
