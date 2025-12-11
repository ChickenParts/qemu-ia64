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
#include "accel/tcg/cpu-ldst.h"
#include "qemu/log.h"
#include "exec/cpu-common.h"
#include "system/memory.h"
#include "system/address-spaces.h"

#define RR_RID(x)   extract64((x), 32, 24)
#define RR_PS(x)    extract64((x), 56, 6)
#define RR_VE(x)    extract64((x), 63, 1)
#define RR(idx)     env->rr[(idx) & 0x7]

/* PTA helpers */
#define PTA_VE(x)   extract64((x), 63, 1)
#define PTA_SIZE(x) extract64((x), 56, 6)
#define PTA_VF(x)   extract64((x), 55, 1)
#define PTA_BASE(x) extract64((x), 3, 46)
#define PTA_VRN(x)  extract64((x), 0, 3)

#define PTE_ED(x)   extract64((x), 11, 1)
#define PTE_AR(x)   extract64((x), 52, 3)
#define PTE_PL(x)   extract64((x), 55, 2)
#define PTE_D(x)    extract64((x), 57, 1)
#define PTE_A(x)    extract64((x), 58, 1)
#define PTE_MA(x)   extract64((x), 59, 3)
#define PTE_P(x)    extract64((x), 63, 1)
#define PTE_PPN(x)  extract64((x), 12, 38)

#define TAR_KEY(x)  extract64((x), 32, 24)
#define TAR_PS(x)   extract64((x), 56, 6)
#define TAR_P(x)    extract64((x), 63, 1)

#define IA64_PSR_CPL(psr) (((psr) >> 32) & 0x3)
#define IA64_PSR_IC       (1ULL << 13)

static bool ia64_fault(CPUState *cs, CPUIA64State *env, bool is_data,
                       bool write, uint32_t vec, uint64_t iim,
                       uintptr_t retaddr)
{
    /* Save interruption state */
    if (env->psr & IA64_PSR_IC) {
        env->cr_ipsr = env->psr;
        env->cr_iip = env->ip & ~0xFULL;
        env->cr_ifs = env->cfm;
    }
    /* Build ISR flags: X/W/R bits and code in low bits */
    uint64_t isr = vec;
    isr |= (!is_data ? 1ULL : 0ULL) << 31; /* X */
    isr |= (write ? 1ULL : 0ULL) << 30;    /* W */
    isr |= ((!write && is_data) || (!is_data) ? 1ULL : 0ULL) << 29; /* R */
    env->cr_isr = isr;
    env->cr_iim = iim;
    cs->exception_index = IA64_EXCP_BASE + vec;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64 fault vec=0x%x is_data=%d IIM=0x%lx IFA=0x%lx\n",
                  vec, is_data, iim, env->cr_ifa);
    cpu_loop_exit(cs);
    return false;
}

static bool ia64_check_perms(CPUIA64State *env, bool is_data, bool write,
                             uint8_t ar, uint8_t pl)
{
    uint8_t cpl = IA64_PSR_CPL(env->psr);
    if (cpl > pl) {
        return false;
    }
    /* Simplified: AR==0 => no access, otherwise allow. */
    if (ar == 0) {
        return false;
    }
    /* No PKR/key enforcement yet. */
    return true;
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

    /* Alignment/probe calls should not raise faults or log. */
    if (probe) {
        return false;
    }

    /* Region / page size info */
    uint8_t rr_idx = extract64(address, 61, 3);
    uint64_t rr = env->rr[rr_idx];
    uint32_t rid = RR_RID(rr);
    uint8_t ps = RR_PS(rr);
    if (ps == 0) {
        ps = 12;
    }

    env->cr_ifa = address;
    env->cr[21] = ((uint64_t)ps << 2) | ((uint64_t)rid << 8);

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
                phys_addr = env->itlb[i].pa + (address & ~mask);
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
                hit = true;
                break;
            }
        }
    }

    if (!hit) {
        uint64_t pta = env->cr[8]; /* cr.pta */
        if (PTA_VE(pta) && RR_VE(rr)) {
            uint64_t vhpt_addr = helper_thash(env);
            uint64_t pte = cpu_ldq_data(env, vhpt_addr);
            uint64_t tar = PTA_VF(pta) ? cpu_ldq_data(env, vhpt_addr + 8)
                                       : ((uint64_t)rid << 8) | ((uint64_t)ps << 2);
            uint64_t tag = PTA_VF(pta) ? cpu_ldq_data(env, vhpt_addr + 16) : 0;
            uint64_t expected = helper_ttag(env);
            if (!PTA_VF(pta) || tag == expected) {
                uint8_t trans_ps = TAR_PS(tar);
                hwaddr pbase = (PTE_PPN(pte) << 12);
                bool is_data = access_type != MMU_INST_FETCH;
                bool write = (access_type == MMU_DATA_STORE);
                if (!PTE_P(pte) || !TAR_P(tar)) {
                    return ia64_fault(cs, env, is_data, write,
                                      is_data ? IA64_VEC_DATA_PAGE_NOT_P
                                              : IA64_VEC_INST_PAGE_NOT_P,
                                      0, retaddr);
                }
                if (!PTE_A(pte)) {
                    return ia64_fault(cs, env, is_data, write,
                                      is_data ? IA64_VEC_DATA_ACCESS_RIGHTS
                                              : IA64_VEC_INST_ACCESS_RIGHTS,
                                      0, retaddr);
                }
                if (write && !PTE_D(pte)) {
                    return ia64_fault(cs, env, is_data, write,
                                      IA64_VEC_DATA_DIRTY, 0, retaddr);
                }
                if (!ia64_check_perms(env, is_data, write,
                                      PTE_AR(pte), PTE_PL(pte))) {
                    return ia64_fault(cs, env, is_data, write,
                                      is_data ? IA64_VEC_DATA_ACCESS_RIGHTS
                                              : IA64_VEC_INST_ACCESS_RIGHTS,
                                      0, retaddr);
                }
                ia64_insert_tlb(env, is_data, address, pbase,
                                rid, trans_ps, PTE_AR(pte), PTE_PL(pte),
                                PTE_D(pte), PTE_A(pte), PTE_P(pte), PTE_ED(pte));
                phys_addr = pbase | (address & ((1ULL << trans_ps) - 1));
                hit = true;
            }
        }
        if (!hit) {
            if (probe) {
                return false;
            }
            bool is_data = access_type != MMU_INST_FETCH;
            bool write = (access_type == MMU_DATA_STORE);
            return ia64_fault(cs, env, is_data, write,
                              access_type == MMU_INST_FETCH ? IA64_VEC_INST_TLB
                                                            : IA64_VEC_DATA_TLB,
                              0, retaddr);
        }
    }

    int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    tlb_set_page(cs, address & TARGET_PAGE_MASK,
                 phys_addr & TARGET_PAGE_MASK, prot,
                 mmu_idx, TARGET_PAGE_SIZE);

    return true;
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
    uint64_t nr = env->r[15];
    uint64_t arg0 = env->r[32];
    uint64_t arg2 = env->r[34];
    uint64_t arg3 = env->r[35];

    switch (nr) {
    case 0:
        /* Unknown/no-op SSC; ignore quietly. */
        return 0;
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
                ssc_files[i].fp = fopen(path, "rb");
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
        struct {
            uint64_t addr;
            uint32_t len;
        } req;
        ia64_ssc_read(env, arg2, &req, sizeof(req));
        fseeko(ssc_files[fd].fp, arg3, SEEK_SET);
        uint8_t *tmp = g_malloc(req.len);
        size_t n = fread(tmp, 1, req.len, ssc_files[fd].fp);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SSC_READ fd=%d addr=0x%lx len=%u off=0x%lx -> %zu\n",
                      fd + 3, (unsigned long)req.addr, req.len,
                      (unsigned long)arg3, n);
        ia64_ssc_write(env, req.addr, tmp, n);
        g_free(tmp);
        ssc_files[fd].last_count = n;
        return 0;
    }
    case 55: { /* SSC_WAIT_COMPLETION */
        struct {
            int32_t fd;
            uint32_t count;
        } stat = { .fd = arg0, .count = 0 };
        int fd = arg0 - 3;
        if (fd >= 0 && fd < 16) {
            stat.count = ssc_files[fd].last_count;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SSC_WAIT fd=%d last=%u\n", fd + 3, stat.count);
        ia64_ssc_write(env, arg0, &stat, sizeof(stat));
        return 0;
    }
    case 77: { /* SSC_GET_INITRAMFS */
        return 0;
    }
    case 66: { /* SSC_EXIT */
        exit(arg0);
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "SSC unhandled nr=%" PRIu64 " imm=%" PRIu64 "\n", nr, imm);
        return -1;
    }
}

static void ia64_insert_tlb(CPUIA64State *env, bool is_data, uint64_t va,
                            uint64_t pa, uint32_t rid, uint8_t ps,
                            uint8_t ar, uint8_t pl, uint8_t d, uint8_t a,
                            uint8_t p, uint8_t ed)
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
    uint8_t ps = (env->cr[21] >> 2) & 0x3f; /* ITIR.ps */
    if (ps == 0) {
        ps = 12; /* default to 4K */
    }
    uint8_t ar = extract64(src, 52, 3);
    uint8_t pl = extract64(src, 55, 2);
    uint8_t d  = extract64(src, 57, 1);
    uint8_t a  = extract64(src, 58, 1);
    uint8_t p  = extract64(src, 63, 1);
    uint8_t ed = extract64(src, 11, 1);
    uint32_t rid = RR_RID(env->rr[extract64(env->cr_ifa, 61, 3)]);
    ia64_insert_tlb(env, true, env->cr_ifa, src, rid, ps, ar, pl, d, a, p, ed);
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
    uint8_t ed = extract64(src, 11, 1);
    uint32_t rid = RR_RID(env->rr[extract64(env->cr_ifa, 61, 3)]);
    ia64_insert_tlb(env, false, env->cr_ifa, src, rid, ps, ar, pl, d, a, p, ed);
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
    ia64_ptc(env, false, false, false, va, tar);
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
    /* DTR insert */
    uint8_t slot = extract64(tar, 0, 4);
    uint8_t ps = TAR_PS(tar);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "itr.d slot=%u ps=%u pte=0x%" PRIx64 " tar=0x%" PRIx64
                  " cr_ifa=0x%" PRIx64 "\n",
                  slot, ps, pte, tar, env->cr_ifa);
    env->dtrs[slot].pte = pte;
    env->dtrs[slot].itr = tar;
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
    uint8_t slot = extract64(tar, 0, 4);
    uint8_t ps = TAR_PS(tar);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "itr.i slot=%u ps=%u pte=0x%" PRIx64 " tar=0x%" PRIx64
                  " cr_ifa=0x%" PRIx64 "\n",
                  slot, ps, pte, tar, env->cr_ifa);
    env->itrs[slot].pte = pte;
    env->itrs[slot].itr = tar;
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
