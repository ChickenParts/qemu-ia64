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

#define RR_RID(x)   extract64((x), 32, 24)
#define RR_PS(x)    extract64((x), 56, 6)
#define RR_VE(x)    extract64((x), 63, 1)

/* PTA helpers */
#define PTA_VE(x)   extract64((x), 63, 1)
#define PTA_SIZE(x) extract64((x), 56, 6)
#define PTA_VF(x)   extract64((x), 55, 1)
#define PTA_BASE(x) extract64((x), 3, 46)
#define PTA_VRN(x)  extract64((x), 0, 3)

bool ia64_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                       MMUAccessType access_type, int mmu_idx,
                       bool probe, uintptr_t retaddr)
{
    /* IA64CPU *cpu = IA64_CPU(cs); */
    CPUIA64State *env = cpu_env(cs);

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
                phys_addr = env->itlb[i].pa | (address & ~mask);
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
                phys_addr = env->dtlb[i].pa | (address & ~mask);
                hit = true;
                break;
            }
        }
    }

    if (!hit) {
        /* Temporary fallback: identity map and kernel bias region. */
        const uint64_t kernel_bias = 0xa0000000fc000000ULL;
        if (address >= 0xa000000000000000ULL) {
            phys_addr = address - kernel_bias;
        } else {
            return false;
        }
    }

    int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    tlb_set_page(cs, address & TARGET_PAGE_MASK,
                 phys_addr & TARGET_PAGE_MASK, prot,
                 mmu_idx, TARGET_PAGE_SIZE);
                 
    return true;
}

static void ia64_insert_tlb(CPUIA64State *env, bool is_data, uint64_t va,
                            uint64_t pa, uint32_t rid, uint8_t ps,
                            uint8_t ar, uint8_t pl, uint8_t d, uint8_t a,
                            uint8_t p)
{
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
        env->dtlb[idx].valid = 1;
        env->dtlb_next = (idx + 1) & 63;
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
        env->itlb[idx].valid = 1;
        env->itlb_next = (idx + 1) & 63;
    }
}

void HELPER(itc_d)(CPUIA64State *env, uint64_t src)
{
    uint8_t ps = (env->cr[21] >> 2) & 0x3f; /* ITIR.ps */
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    uint8_t ar = extract64(src, 52, 3);
    uint8_t pl = extract64(src, 55, 2);
    uint8_t d  = extract64(src, 57, 1);
    uint8_t a  = extract64(src, 58, 1);
    uint8_t p  = extract64(src, 63, 1);
    uint32_t rid = RR_RID(env->rr[extract64(env->cr_ifa, 61, 3)]);
    ia64_insert_tlb(env, true, env->cr_ifa, src, rid, ps, ar, pl, d, a, p);
}

void HELPER(itc_i)(CPUIA64State *env, uint64_t src)
{
    uint8_t ps = (env->cr[21] >> 2) & 0x3f; /* ITIR.ps */
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    uint8_t ar = extract64(src, 52, 3);
    uint8_t pl = extract64(src, 55, 2);
    uint8_t d  = extract64(src, 57, 1);
    uint8_t a  = extract64(src, 58, 1);
    uint8_t p  = extract64(src, 63, 1);
    uint32_t rid = RR_RID(env->rr[extract64(env->cr_ifa, 61, 3)]);
    ia64_insert_tlb(env, false, env->cr_ifa, src, rid, ps, ar, pl, d, a, p);
}

uint64_t HELPER(thash)(CPUIA64State *env)
{
    uint64_t va = env->cr_ifa;
    uint64_t pta = env->cr[8]; /* cr.pta stored in cr[8] */
    uint8_t rr = extract64(va, 61, 3);
    uint64_t mask = (1ULL << PTA_SIZE(pta)) - 1;
    mask = extract64(mask, 3, 46);
    uint64_t hpn = extract64(va, 3 + (61 - 1 - 3), 61 - (RR_PS(env->rr[rr])));
    uint64_t offset;
    uint64_t addr;

    if (PTA_VF(pta)) {
        offset = (hpn ^ RR_RID(env->rr[rr])) << 5;
        addr = (uint64_t)PTA_VRN(pta) << 61;
    } else {
        offset = hpn << 3;
        addr = (uint64_t)rr << 61;
    }
    addr |= ((PTA_BASE(pta) & ~mask) | (extract64(offset, 3, 46) & mask)) << 15
            | extract64(offset, 49, 15);
    return addr;
}

uint64_t HELPER(ttag)(CPUIA64State *env)
{
    uint64_t va = env->cr_ifa;
    uint8_t rr = extract64(va, 61, 3);
    return (extract64(va, 3 + (61 - 1 - 3), 61 - RR_PS(env->rr[rr])) ^
            (uint64_t)RR_RID(env->rr[rr]) << 39);
}
