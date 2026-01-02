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
#include "exec/tb-flush.h"
#include "accel/tcg/cpu-ldst.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "exec/cpu-common.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/ioport.h"
#include "hw/boards.h"
#include <math.h>
#include <zlib.h>

static bool ia64_fw_log_enabled(void);
static bool ia64_env_truthy(const char *s);
static uint64_t ia64_fw_pei_cached_hob_base;
static uint64_t ia64_fw_pei_cached_temp_base;
static uint64_t ia64_fw_pei_cached_temp_size;

#define IA64_EFI_FFS_FILE_HEADER_SIZE  24
#define IA64_EFI_FFS_FILE_HEADER2_SIZE 32
#define IA64_EFI_SECTION_PE32          0x10
#define IA64_EFI_SECTION_TE            0x12
#define IA64_EFI_TE_IMAGE_HEADER_SIGNATURE 0x5A56

typedef struct {
    uint8_t Size[3];
    uint8_t Type;
} IA64EfiCommonSectionHeader;

static inline hwaddr ia64_phys_mode_addr(uint64_t addr)
{
    /*
     * IA-64 uses region-encoded virtual addresses even in physical mode.
     * Firmware also commonly forms sign-extended 32-bit addresses (e.g.
     * 0xffffffffffE00000) via addl/adds from a small GP value.
     *
     * - If the address is canonically sign-extended 32-bit, or matches the
     *   common firmware pattern that clears bit63 but leaves bits 62..32 set
     *   (0x7fffffffXXXXXXXX), treat it as a 32-bit physical address.
     * - Otherwise fall back to the low 61 bits (ignore the region number).
     */
    uint64_t hi32 = addr & 0xffffffff00000000ULL;
    if (hi32 == 0 ||
        hi32 == 0xffffffff00000000ULL ||
        hi32 == 0x7fffffff00000000ULL) {
        return (hwaddr)(uint32_t)addr;
    }
    return (hwaddr)(addr & ((1ULL << 61) - 1));
}


static inline uint64_t ia64_fw_encode_addr(uint64_t template, uint64_t phys)
{
    uint64_t hi32 = template & 0xffffffff00000000ULL;
    if (hi32 == 0 ||
        hi32 == 0xffffffff00000000ULL ||
        hi32 == 0x7fffffff00000000ULL) {
        if (phys > 0xffffffffULL) {
            return phys;
        }
        return hi32 | (uint32_t)phys;
    }
    return (template & ~((1ULL << 61) - 1)) | (phys & ((1ULL << 61) - 1));
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
#define IA64_CFM_RRBF_SHIFT 25
#define IA64_CFM_RRBF_MASK  0x7fULL

#define IA64_FR_ROT_BASE 32
#define IA64_FR_ROT_SIZE 96

/* ar.rsc loadrs field: bits 16..29, in bytes (see SKI ssDSym.c). */
#define IA64_RSC_LOADRS_SHIFT 16
#define IA64_RSC_LOADRS_MASK  0x3fffULL
#define IA64_RSC_MODE_MASK    0x3ULL

/* Linux/ia64 canonical per-cpu range: [-PERCPU_PAGE_SIZE, 0). */
#define IA64_PERCPU_VA_BASE   0xfffffffffffc0000ULL
#define IA64_PERCPU_PAGE_SIZE (1ULL << 18) /* 256KiB (PERCPU_PAGE_SHIFT=18) */
#define IA64_KR_PER_CPU_DATA  3            /* ar.k3 */

/*
 * Firmware volume access (flash) base/size for xenipf/EDK firmware.
 * Keep in sync with hw/ia64/gfw.h (GFW_START/GFW_SIZE).
 */
#define IA64_IPF_FW_FLASH_BASE        0x00000000ff000000ULL
#define IA64_IPF_FW_FLASH_SIZE        (16ULL << 20)
#define IA64_IPF_FW_FLASH_BLOCK_SIZE  (64ULL << 10)
#define IA64_IPF_FW_FLASH_ATTRS       0x00100C36U
#define IA64_EFI_FVH_SIGNATURE        0x4856465fU /* "_FVH" */
#define IA64_EFI_VARSTORE_SIGNATURE   0x53535624U /* "$VSS" */
#define IA64_EFI_FLASH_SUBFV          0x0002U
#define IA64_EFI_FLASH_MEMMAPPED_FV   0x0004U
#define IA64_EFI_FLASH_EFI_VARIABLES  0x0b
/* Match hw/ia64/ipf.c firmware work RAM window. */
#define IA64_IPF_FW_WORKRAM_BASE      0x0000000100000000ULL
#define IA64_IPF_FW_WORKRAM_SIZE      (16ULL << 20)
/* Match hw/ia64/gfw.h GFW_HOB_START/GFW_HOB_SIZE. */
#define IA64_IPF_GFW_HOB_BASE         0x00000000ff200000ULL
#define IA64_IPF_GFW_HOB_SIZE         (1ULL << 20)

static uint64_t ia64_fw_flash_base(void)
{
    static uint64_t base;
    static bool inited;
    if (!inited) {
        base = IA64_IPF_FW_FLASH_BASE;
        const char *s = getenv("QEMU_IA64_FW_FLASH_BASE");
        if (s && *s) {
            char *endp = NULL;
            uint64_t val = strtoull(s, &endp, 0);
            if (endp && endp != s) {
                base = val;
            }
        }
        inited = true;
    }
    return base;
}

static uint64_t ia64_fw_flash_size(void)
{
    static uint64_t size;
    static bool inited;
    if (!inited) {
        size = IA64_IPF_FW_FLASH_SIZE;
        const char *s = getenv("QEMU_IA64_FW_FLASH_SIZE");
        if (s && *s) {
            char *endp = NULL;
            uint64_t val = strtoull(s, &endp, 0);
            if (endp && endp != s && val) {
                size = val;
            }
        }
        size = QEMU_ALIGN_DOWN(size, IA64_IPF_FW_FLASH_BLOCK_SIZE);
        if (size < IA64_IPF_FW_FLASH_BLOCK_SIZE) {
            size = IA64_IPF_FW_FLASH_BLOCK_SIZE;
        }
        inited = true;
    }
    return size;
}

static uint64_t ia64_fw_flash_end(void)
{
    uint64_t base = ia64_fw_flash_base();
    uint64_t size = ia64_fw_flash_size();
    uint64_t end = base + size;
    if (end < base) {
        return UINT64_MAX;
    }
    return end;
}

static uint64_t ia64_fw_flash_blocks(void)
{
    return ia64_fw_flash_size() / IA64_IPF_FW_FLASH_BLOCK_SIZE;
}

static bool ia64_fw_addr_in_flash(uint64_t phys)
{
    uint64_t base = ia64_fw_flash_base();
    uint64_t end = ia64_fw_flash_end();
    if (end <= base) {
        return false;
    }
    return phys >= base && phys < end;
}

static const uint8_t ia64_efi_ffs_guid[16] = {
    0xd9, 0x54, 0x93, 0x7a, 0x68, 0x04, 0x4a, 0x44,
    0x81, 0xce, 0x0b, 0xf6, 0x17, 0xd8, 0x90, 0xdf,
};

static inline uint64_t ia64_rsc_get_loadrs(uint64_t rsc)
{
    return (rsc >> IA64_RSC_LOADRS_SHIFT) & IA64_RSC_LOADRS_MASK;
}

static inline uint64_t ia64_rsc_get_mode(uint64_t rsc)
{
    return rsc & IA64_RSC_MODE_MASK;
}

static inline bool ia64_rse_is_lazy(const CPUIA64State *env)
{
    return ia64_rsc_get_mode(env->ar[IA64_AR_RSC]) == 0;
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

static inline bool ia64_gr_nat_get(const CPUIA64State *env, uint32_t gr)
{
    if (gr == 0 || gr >= 128) {
        return false;
    }
    return env->nat[gr] != 0;
}

static inline void ia64_gr_nat_set(CPUIA64State *env, uint32_t gr, bool nat)
{
    if (gr == 0 || gr >= 128) {
        return;
    }
    env->nat[gr] = nat ? 1 : 0;
}

static inline uint64_t ia64_rse_get_bsp(const CPUIA64State *env)
{
    uint64_t bsp = env->ar[IA64_AR_BSP];
    if (bsp == 0) {
        bsp = env->ar[IA64_AR_BSPSTORE];
    }
    return bsp & ~0x7ULL;
}

static inline void ia64_rse_update_loadrs(CPUIA64State *env, uint64_t bsp)
{
    uint64_t rsc = env->ar[IA64_AR_RSC];
    if (ia64_rsc_get_mode(rsc) != 0) {
        env->ar[IA64_AR_RSC] = ia64_rsc_set_loadrs(rsc, 0);
        return;
    }
    uint64_t bspstore = env->ar[IA64_AR_BSPSTORE] & ~0x7ULL;
    uint64_t bytes = (bsp > bspstore) ? (bsp - bspstore) : 0;
    env->ar[IA64_AR_RSC] = ia64_rsc_set_loadrs(env->ar[IA64_AR_RSC], bytes);
}

static void ia64_rse_write_mem(CPUIA64State *env, uint64_t addr, uint64_t val)
{
    cpu_stq_data(env, addr, val);
}

static uint64_t ia64_rse_read_mem(CPUIA64State *env, uint64_t addr)
{
    return cpu_ldq_data(env, addr);
}

static void ia64_rse_store_frame(CPUIA64State *env, uint64_t bsp, uint8_t sof)
{
    uint8_t count = MIN(sof, (uint8_t)96);
    if (count == 0) {
        env->ar[IA64_AR_RNAT] = 0;
        return;
    }

    bsp &= ~0x7ULL;
    uint64_t addr = ia64_rse_skip_regs(bsp, -(int64_t)count);
    uint32_t reg_idx = 0;
    uint64_t rnat = 0;
    uint32_t rnat_bit = 0;

    while (reg_idx < count) {
        if (ia64_rse_slot_num(addr) == 0x3f) {
            ia64_rse_write_mem(env, addr, rnat);
            rnat = 0;
            rnat_bit = 0;
            addr += 8;
            continue;
        }

        uint64_t val = env->r[32 + reg_idx];
        ia64_rse_write_mem(env, addr, val);
        if (ia64_gr_nat_get(env, 32 + reg_idx)) {
            rnat |= (1ULL << rnat_bit);
        }
        rnat_bit++;
        reg_idx++;
        addr += 8;
    }

    env->ar[IA64_AR_RNAT] = rnat;
}

static void ia64_rse_load_frame(CPUIA64State *env, uint64_t bsp, uint8_t sof)
{
    uint8_t count = MIN(sof, (uint8_t)96);
    if (count == 0) {
        return;
    }

    bsp &= ~0x7ULL;
    uint64_t addr = ia64_rse_skip_regs(bsp, -(int64_t)count);
    uint32_t reg_idx = 0;
    uint32_t group_start = 0;
    uint32_t group_count = 0;

    while (reg_idx < count) {
        if (ia64_rse_slot_num(addr) == 0x3f) {
            uint64_t rnat = ia64_rse_read_mem(env, addr);
            for (uint32_t i = 0; i < group_count; i++) {
                ia64_gr_nat_set(env, 32 + group_start + i,
                                (rnat >> i) & 1);
            }
            group_start = reg_idx;
            group_count = 0;
            addr += 8;
            continue;
        }

        uint64_t val = ia64_rse_read_mem(env, addr);
        env->r[32 + reg_idx] = val;
        group_count++;
        reg_idx++;
        addr += 8;
    }

    if (group_count) {
        uint64_t rnat = env->ar[IA64_AR_RNAT];
        for (uint32_t i = 0; i < group_count; i++) {
            ia64_gr_nat_set(env, 32 + group_start + i, (rnat >> i) & 1);
        }
    }

    for (uint32_t i = count; i < 96; i++) {
        env->nat[32 + i] = 0;
    }
}

static inline uint32_t ia64_fr_phys(const CPUIA64State *env, uint32_t f)
{
    f &= 0x7f;
    if (f < IA64_FR_ROT_BASE) {
        return f;
    }
    uint32_t rrbf = (env->cfm >> IA64_CFM_RRBF_SHIFT) & IA64_CFM_RRBF_MASK;
    uint32_t off = (f - IA64_FR_ROT_BASE) + rrbf;
    if (off >= IA64_FR_ROT_SIZE) {
        off -= IA64_FR_ROT_SIZE;
    }
    return IA64_FR_ROT_BASE + off;
}

static void ia64_rse_push_window(CPUIA64State *env, uint64_t ret_addr);
static bool ia64_rse_pop_window(CPUIA64State *env);
static bool ia64_intr_pop_window(CPUIA64State *env);

#ifndef CONFIG_USER_ONLY
static void ia64_fw_dump_code(CPUIA64State *env, const char *tag,
                              uint64_t pc, int bundles);
#ifndef CONFIG_USER_ONLY
static void ia64_dbg_probe_dump_mem(CPUIA64State *env, uint64_t pc,
                                    const char *tag, uint64_t addr, int nbytes);
#endif
static void ia64_fw_r8_log_pei_hob(CPUIA64State *env, uint64_t pc,
                                   uint64_t peis_ptr);
static void ia64_fw_trace_dump(void);
#endif

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

        uint8_t tmp_nat = env->banked_nat[i];
        env->banked_nat[i] = env->nat[16 + i];
        env->nat[16 + i] = tmp_nat;
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
    static uint32_t break_basic_count;

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
        static int break_basic_enabled = -1;
        if (break_basic_enabled == -1) {
            const char *s = getenv("QEMU_IA64_LOG_BREAK");
            break_basic_enabled = (s && *s) ? 1 : 0;
        }
        if (break_basic_enabled && break_basic_count < 16) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: break iim=0x%" PRIx64 " ip=%016" PRIx64
                          " r28=%016" PRIx64 " r32=%016" PRIx64
                          " r33=%016" PRIx64 " r34=%016" PRIx64
                          " b0=%016" PRIx64 " b6=%016" PRIx64 "\n",
                          iim, env->ip, env->r[28], env->r[32],
                          env->r[33], env->r[34], env->b[0], env->b[6]);
            break_basic_count++;
        }
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
    /*
     * IFS = CFM{38:0} | AR.EC{5:0} << 52 | V (bit 63 set by cover).
     * Save CFM and AR.EC together so rfi can restore both.
     */
    env->cr_ifs = env->cfm | ((env->ar[IA64_AR_EC] & 0x3fULL) << 52);
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

void ia64_cpu_do_unaligned_access(CPUState *cs, vaddr addr,
                                  MMUAccessType access_type,
                                  int mmu_idx, uintptr_t retaddr)
{
#ifdef CONFIG_USER_ONLY
    (void)mmu_idx;
    cpu_abort(cs, "IA64: unaligned access addr=%016" PRIx64, (uint64_t)addr);
#else
    (void)mmu_idx;
    CPUIA64State *env = cpu_env(cs);
    bool is_data = (access_type != MMU_INST_FETCH);
    bool is_write = (access_type == MMU_DATA_STORE);

    env->cr_ifa = addr;
    env->cr_iha = helper_thash(env);
    ia64_fault(cs, env, is_data, is_write,
               IA64_VEC_UNALIGNED_DATA_REFERENCE, 0, retaddr);
#endif
    g_assert_not_reached();
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

static bool ia64_try_translate(CPUIA64State *env, uint64_t va, hwaddr *pa)
{
    uint8_t rr_idx = extract64(va, 61, 3);

    if (rr_idx == 7 && extract64(va, 60, 1) == 0) {
        *pa = va & ((1ULL << 61) - 1);
        return true;
    }
    if (rr_idx == 6 && extract64(va, 60, 1) == 0) {
        *pa = va & ((1ULL << 61) - 1);
        return true;
    }

    if (!(env->psr & IA64_PSR_DT)) {
        *pa = ia64_phys_mode_addr(va);
        return true;
    }

    uint64_t rr = env->rr[rr_idx];
    uint32_t rid = RR_RID(rr);
    bool hit = false;
    uint64_t tpa = ia64_translate_tlb(env, true, va, rid, &hit);
    if (hit) {
        *pa = tpa;
        return true;
    }

    uint64_t pta = env->cr[8];
    if (PTA_VE(pta) && RR_VE(rr)) {
        env->cr_ifa = va;
        uint64_t vhpt_addr = helper_thash(env);
        uint64_t pte = cpu_ldq_data(env, vhpt_addr);
        uint64_t tar = PTA_VF(pta) ? cpu_ldq_data(env, vhpt_addr + 8)
                                   : ((uint64_t)rid << 8) |
                                     ((uint64_t)RR_PS(rr) << 2);
        uint64_t tag = PTA_VF(pta) ? cpu_ldq_data(env, vhpt_addr + 16) : 0;
        uint64_t expected = helper_ttag(env);
        if (!PTA_VF(pta) || tag == expected) {
            uint8_t trans_ps = TAR_PS(tar);
            hwaddr pbase = (PTE_PPN(pte) << 12);
            if (PTE_P(pte) && TAR_P(tar)) {
                *pa = pbase | (va & ((1ULL << trans_ps) - 1));
                return true;
            }
        }
    }

    return false;
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

uint64_t HELPER(ld_s)(CPUIA64State *env, uint64_t addr, uint32_t size,
                      uint32_t reg, uint32_t advanced)
{
    uint64_t val = 0;
    bool ok = true;

    if (size == 0 || size > 8) {
        ok = false;
    }

    hwaddr pa = 0;
    if (ok && !ia64_try_translate(env, addr, &pa)) {
        ok = false;
    }

    if (ok) {
        uint8_t buf[8] = { 0 };
        MemTxResult res = address_space_read(&address_space_memory, pa,
                                             MEMTXATTRS_UNSPECIFIED,
                                             buf, size);
        if (res != MEMTX_OK) {
            ok = false;
        } else {
            for (uint32_t i = 0; i < size; i++) {
                val |= (uint64_t)buf[i] << (i * 8);
            }
        }
    }

    if (reg != 0) {
        ia64_gr_nat_set(env, reg, !ok);
        if (advanced) {
            uint32_t r = reg & 0x7f;
            struct IA64ALATEntry *e = &env->alat_gr[r];
            if (!ok) {
                if (e->valid) {
                    e->valid = 0;
                    if (env->alat_gr_valid) {
                        env->alat_gr_valid--;
                    }
                }
            } else {
                if (!e->valid) {
                    env->alat_gr_valid++;
                }
                e->addr = addr;
                e->size = size;
                e->valid = 1;
            }
        }
    }

    return ok ? val : 0;
}

void HELPER(unimpl)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                    uint64_t insn, uint64_t msg_ptr)
{
    const char *msg = (const char *)(uintptr_t)msg_ptr;
    uint64_t last_kind = env->last_branch_kind;
    uint32_t last_ri = last_kind >> 8;
    uint32_t last_kind_id = last_kind & 0xff;
    static int dump_enabled = -1;
    static int dump_bundles = 8;
    if (dump_enabled == -1) {
        const char *s = getenv("QEMU_IA64_UNIMPL_DUMP");
        dump_enabled = (s && *s) ? 1 : 0;
        const char *b = getenv("QEMU_IA64_UNIMPL_DUMP_BUNDLES");
        if (b && *b) {
            int v = atoi(b);
            if (v > 0) {
                dump_bundles = v;
            }
        }
    }
    if (dump_enabled) {
        CPUState *cs = env_cpu(env);
        uint64_t base = pc & ~0xFULL;
        g_mkdir_with_parents("scratch/ia64_logs", 0755);
        char path[256];
        snprintf(path, sizeof(path),
                 "scratch/ia64_logs/unimpl_%016" PRIx64 ".bin", base);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            for (int i = 0; i < dump_bundles; i++) {
                uint8_t bundle[16];
                uint64_t bpc = base + (uint64_t)i * 16;
                if (cpu_memory_rw_debug(cs, bpc, bundle, sizeof(bundle), false) != 0) {
                    break;
                }
                fwrite(bundle, 1, sizeof(bundle), fp);
            }
            fclose(fp);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: unimpl dump pc=%016" PRIx64 " bundles=%d file=%s\n",
                          base, dump_bundles, path);
        }
    }
    cpu_abort(env_cpu(env),
              "IA64 UNIMPL: pc=%016" PRIx64 " ri=%u insn=%011" PRIx64 " %s"
              " last_branch from=%016" PRIx64 " to=%016" PRIx64
              " kind=%u ri=%u insn=%011" PRIx64,
              pc, ri, insn, msg ? msg : "",
              env->last_branch_from, env->last_branch_to,
              last_kind_id, last_ri, env->last_branch_insn);
}

enum {
    IA64_B7_WRITE_MOV_M = 0,
    IA64_B7_WRITE_MOV_I = 1,
    IA64_B7_WRITE_BRCALL_REG = 2,
    IA64_B7_WRITE_BRCALL_IMM = 3,
};

void HELPER(dbg_movb)(CPUIA64State *env, uint64_t pc, uint32_t b,
                      uint32_t r2, uint64_t val)
{
    static int log_limit = -1;
    static int log_count;
    static int filter_b = -2;
    static int dump_bundle = -1;
    if (log_limit == -1) {
        log_limit = 64;
        const char *s = getenv("QEMU_IA64_MOVB_LOG_LIMIT");
        if (s && *s) {
            log_limit = atoi(s);
        }
        if (log_limit < 0) {
            log_limit = 0;
        }
    }
    if (filter_b == -2) {
        filter_b = -1;
        const char *s = getenv("QEMU_IA64_MOVB_LOG_B");
        if (s && *s) {
            filter_b = atoi(s);
        }
    }
    if (dump_bundle == -1) {
        dump_bundle = 0;
        const char *s = getenv("QEMU_IA64_MOVB_LOG_DUMP");
        if (s && *s) {
            dump_bundle = atoi(s) ? 1 : 0;
        }
    }
    if (log_limit == 0 || log_count >= log_limit) {
        return;
    }
    if (filter_b >= 0 && (int)b != filter_b) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "mov.b pc=%016" PRIx64 " b=%u r2=%u val=%016" PRIx64
                  " cfm=%016" PRIx64 " bsp=%016" PRIx64 "\n",
                  pc, b, r2, val, env->cfm, env->ar[IA64_AR_BSP]);
    if (dump_bundle) {
        CPUState *cs = env_cpu(env);
        uint8_t bundle[16];
        uint64_t base = pc & ~0xFULL;
        if (cpu_memory_rw_debug(cs, base, bundle, sizeof(bundle), false) == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mov.b bundle pc=%016" PRIx64
                          " %02x %02x %02x %02x %02x %02x %02x %02x"
                          " %02x %02x %02x %02x %02x %02x %02x %02x\n",
                          base,
                          bundle[0], bundle[1], bundle[2], bundle[3],
                          bundle[4], bundle[5], bundle[6], bundle[7],
                          bundle[8], bundle[9], bundle[10], bundle[11],
                          bundle[12], bundle[13], bundle[14], bundle[15]);
        }
    }
    log_count++;
}

static const char *ia64_fw_b7_kind_name(uint32_t kind)
{
    switch (kind) {
    case IA64_B7_WRITE_MOV_M:
        return "mov.m";
    case IA64_B7_WRITE_MOV_I:
        return "mov.i";
    case IA64_B7_WRITE_BRCALL_REG:
        return "br.call(reg)";
    case IA64_B7_WRITE_BRCALL_IMM:
        return "br.call(imm)";
    default:
        return "unknown";
    }
}

void HELPER(fw_b7_write)(CPUIA64State *env, uint64_t pc, uint32_t kind,
                         uint32_t aux, uint64_t val)
{
    static int enabled = -1;
    static int limit = -1;
    static int count;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_B7_TRACE");
        enabled = (s && *s) ? 1 : 0;
    }
    if (!enabled) {
        return;
    }
    if (limit == -1) {
        limit = 128;
        const char *s = getenv("QEMU_IA64_B7_TRACE_LIMIT");
        if (s && *s) {
            limit = atoi(s);
        }
        if (limit < 0) {
            limit = 0;
        }
    }
    if (limit == 0 || count >= limit) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: b7_write pc=%016" PRIx64 " kind=%s aux=%u"
                  " old=%016" PRIx64 " val=%016" PRIx64
                  " cfm=%016" PRIx64 " bsp=%016" PRIx64 "\n",
                  pc, ia64_fw_b7_kind_name(kind), aux, env->b[7], val,
                  env->cfm, env->ar[IA64_AR_BSP]);
    count++;
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

    pa &= TARGET_PAGE_MASK;
    if (pa == env->fc_last_page) {
        return;
    }
    env->fc_last_page = pa;
    tb_invalidate_phys_range(env_cpu(env), pa, pa + TARGET_PAGE_SIZE - 1);
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
    memcpy(frame->nat, &env->nat[32], sizeof(frame->nat));
    frame->ar_pfs = env->ar[64]; /* ar.pfs */
}

static bool ia64_intr_pop_window(CPUIA64State *env)
{
    if (env->intr_depth == 0) {
        return false;
    }
    struct IA64IntrFrame *frame = &env->intr_frames[--env->intr_depth];
    memcpy(&env->r[32], frame->r, sizeof(frame->r));
    memcpy(&env->nat[32], frame->nat, sizeof(frame->nat));
    env->ar[64] = frame->ar_pfs;
    return true;
}

static void ia64_rse_push_window(CPUIA64State *env, uint64_t ret_addr)
{
    ia64_rse_ensure(env, env->rse_depth + 1);
    struct IA64RSEFrame *frame = &env->rse_frames[env->rse_depth++];
    memcpy(frame->r, &env->r[32], sizeof(frame->r));
    memcpy(frame->nat, &env->nat[32], sizeof(frame->nat));
    frame->ar_pfs = env->ar[64]; /* ar.pfs */
    frame->cfm = env->cfm;
    frame->ret_addr = ret_addr & ~0xFULL;
    frame->share_outs = 1;
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
    uint8_t outnats[96] = { 0 };
    uint8_t caller_sol = 0;
    uint8_t caller_outs = 0;
    if (frame->share_outs) {
        uint8_t caller_sof = frame->cfm & 0x7f;
        caller_sol = (frame->cfm >> 7) & 0x7f;
        caller_outs = (caller_sof > caller_sol) ?
            (caller_sof - caller_sol) : 0;
        caller_outs = MIN(caller_outs, (uint8_t)96);
        for (uint8_t i = 0; i < caller_outs; i++) {
            outvals[i] = env->r[32 + i];
            outnats[i] = env->nat[32 + i];
        }
    }

    frame = &env->rse_frames[--env->rse_depth];
    memcpy(&env->r[32], frame->r, sizeof(frame->r));
    memcpy(&env->nat[32], frame->nat, sizeof(frame->nat));
    env->ar[64] = frame->ar_pfs;
    env->cfm = frame->cfm;

    if (frame->share_outs && caller_outs && caller_sol < 96) {
        uint8_t max_copy = MIN(caller_outs, (uint8_t)(96 - caller_sol));
        uint8_t out0 = 32 + caller_sol;
        for (uint8_t i = 0; i < max_copy; i++) {
            env->r[out0 + i] = outvals[i];
            env->nat[out0 + i] = outnats[i];
        }
    }
    return true;
}

static inline void ia64_restore_ec_from_pfs(CPUIA64State *env)
{
    /*
     * br.ret restores EC_CNT from the previous function state (ar.pfs).
     * Keep the EC_CNT field in ar.ec synchronized with the caller's PFS.PEC.
     */
    uint64_t pec = (env->ar[IA64_AR_PFS] >> 52) & 0x3f;
    uint64_t ec = env->ar[66];
    ec &= ~(0x3fULL << 58);
    ec |= (pec << 58);
    env->ar[66] = ec;
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
     * The EC field (bits 52-57) is restored to AR.EC per IA-64 SDM.
     */
    uint64_t ifs = env->cr_ifs;
    if (ifs & (1ULL << 63)) {
        env->cfm = ifs & ~(1ULL << 63);
        /* Restore AR.EC from IFS.ec (bits 52-57) */
        env->ar[IA64_AR_EC] = (ifs >> 52) & 0x3f;
    }

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

    {
        /*
         * Dump the raw bundle words for the call site so mismatches between
         * decoded control flow and the firmware image can be diagnosed without
         * a full disassembly log.
         */
        CPUState *cs = env_cpu(env);
        uint64_t base = pc & ~0xFULL;
        uint8_t bundle[16];
        if (cpu_memory_rw_debug(cs, base, bundle, sizeof(bundle), false) == 0) {
            uint64_t low = 0;
            uint64_t high = 0;
            memcpy(&low, &bundle[0], sizeof(low));
            memcpy(&high, &bundle[8], sizeof(high));

            uint8_t tmpl = low & 0x1f;
            uint64_t s0 = (low >> 5) & 0x1ffffffffffULL;
            uint64_t s1 = ((low >> 46) | (high << 18)) & 0x1ffffffffffULL;
            uint64_t s2 = (high >> 23) & 0x1ffffffffffULL;

            qemu_log_mask(LOG_GUEST_ERROR,
                          "dbg_call_bundle pc=%016" PRIx64
                          " low=%016" PRIx64 " high=%016" PRIx64
                          " tmpl=%02x s0=%011" PRIx64 " s1=%011" PRIx64
                          " s2=%011" PRIx64 "\n",
                          base, low, high, tmpl, s0, s1, s2);
        }
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
        if (pc == 0x000000001ff726d0ULL) {
            /*
             * CoreAcquireLockOrFail() call site: out0 is the lock's TPL loaded
             * from *Lock (EFI_LOCK.Tpl at offset 0). The xenipf firmware
             * occasionally reaches this call with a zero TPL, which causes
             * CoreRaiseTpl() to ASSERT. Dump the lock pointer/fields to help
             * identify which lock is uninitialized or corrupted.
             */
            uint64_t out0v = env->r[out0];
            if (out0v == 0) {
                CPUState *cs = env_cpu(env);
                uint64_t lockp = env->r[31];
                uint64_t tpl = 0, owner = 0, cnt = 0;
                bool ok_tpl = false, ok_owner = false, ok_cnt = false;
                if (lockp >= 0x1000) {
                    uint8_t buf[24];
                    if (cpu_memory_rw_debug(cs, lockp, buf, sizeof(buf), false) == 0) {
                        memcpy(&tpl, &buf[0], 8);
                        memcpy(&owner, &buf[8], 8);
                        memcpy(&cnt, &buf[16], 8);
                        ok_tpl = ok_owner = ok_cnt = true;
                    }
                }

                qemu_log_mask(LOG_GUEST_ERROR,
                              "dbg_call_lock pc=%016" PRIx64 " sp=%016" PRIx64
                              " b0=%016" PRIx64
                              " lockp=%016" PRIx64 " out0(Tpl)=%016" PRIx64
                              " mem{Tpl=%016" PRIx64 "%s Owner=%016" PRIx64 "%s Lock=%016" PRIx64 "%s}\n",
                              pc, env->r[12], env->b[0], lockp, out0v,
                              tpl, ok_tpl ? "" : "?",
                              owner, ok_owner ? "" : "?",
                              cnt, ok_cnt ? "" : "?");
            }
        }
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

void HELPER(dbg_r12)(CPUIA64State *env, uint64_t pc, uint64_t new_val)
{
    static int enabled = -1;
    static int log_limit = -1;
    static int log_count;
    static int dump_bundle = -1;

    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_R12");
        enabled = (s && *s) ? 1 : 0;
    }
    if (!enabled) {
        return;
    }
    if (dump_bundle == -1) {
        dump_bundle = getenv("QEMU_IA64_DBG_R12_DUMP") ? 1 : 0;
    }
    if (log_limit == -1) {
        log_limit = 64;
        const char *s = getenv("QEMU_IA64_DBG_R12_LIMIT");
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
                  "dbg_r12 pc=%016" PRIx64
                  " old=%016" PRIx64 " new=%016" PRIx64
                  " psr=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                  pc, env->r[12], new_val, env->psr, env->cfm);
    if (dump_bundle) {
        CPUState *cs = env_cpu(env);
        uint64_t base = pc & ~0xFULL;
        uint8_t bundle[16];
        if (cpu_memory_rw_debug(cs, ia64_phys_mode_addr(base),
                                bundle, sizeof(bundle), false) == 0) {
            uint64_t low = 0, high = 0;
            memcpy(&low, &bundle[0], sizeof(low));
            memcpy(&high, &bundle[8], sizeof(high));
            uint8_t tmpl = low & 0x1f;
            uint64_t s0 = (low >> 5) & 0x1ffffffffffULL;
            uint64_t s1 = ((low >> 46) | (high << 18)) & 0x1ffffffffffULL;
            uint64_t s2 = (high >> 23) & 0x1ffffffffffULL;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "dbg_r12_bundle pc=%016" PRIx64
                          " low=%016" PRIx64 " high=%016" PRIx64
                          " tmpl=%02x s0=%011" PRIx64 " s1=%011" PRIx64
                          " s2=%011" PRIx64 "\n",
                          base, low, high, tmpl, s0, s1, s2);
        }
    }
}

uint64_t HELPER(fr_get_lo)(CPUIA64State *env, uint32_t f)
{
    uint32_t pf = ia64_fr_phys(env, f);
    return env->f[pf][0];
}

uint64_t HELPER(fr_get_hi)(CPUIA64State *env, uint32_t f)
{
    uint32_t pf = ia64_fr_phys(env, f);
    return env->f[pf][1];
}

void HELPER(fr_set_lo)(CPUIA64State *env, uint32_t f, uint64_t val)
{
    uint32_t pf = ia64_fr_phys(env, f);
    if (pf <= 1) {
        return;
    }
    env->f[pf][0] = val;
}

void HELPER(fr_set_hi)(CPUIA64State *env, uint32_t f, uint64_t val)
{
    uint32_t pf = ia64_fr_phys(env, f);
    if (pf <= 1) {
        return;
    }
    env->f[pf][1] = val;
}

uint64_t HELPER(gr_nat)(CPUIA64State *env, uint32_t gr)
{
    return ia64_gr_nat_get(env, gr) ? 1 : 0;
}

void HELPER(gr_nat_set)(CPUIA64State *env, uint32_t gr, uint64_t nat)
{
    ia64_gr_nat_set(env, gr, nat != 0);
}

static inline uint8_t ia64_sat_u8(long long v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 0xff) {
        return 0xff;
    }
    return (uint8_t)v;
}

static inline uint8_t ia64_sat_s8(long long v)
{
    if (v < -128) {
        return 0x80;
    }
    if (v > 127) {
        return 0x7f;
    }
    return (uint8_t)(int8_t)v;
}

static inline uint16_t ia64_sat_u16(long long v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 0xffff) {
        return 0xffff;
    }
    return (uint16_t)v;
}

static inline uint16_t ia64_sat_s16(long long v)
{
    if (v < -32768) {
        return 0x8000;
    }
    if (v > 32767) {
        return 0x7fff;
    }
    return (uint16_t)(int16_t)v;
}

uint64_t HELPER(padd1)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t av = (a >> (i * 8)) & 0xff;
        uint8_t bv = (b >> (i * 8)) & 0xff;
        uint8_t rv = av + bv;
        res |= (uint64_t)rv << (i * 8);
    }
    return res;
}

uint64_t HELPER(padd1_sss)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 8; i++) {
        int8_t av = (int8_t)((a >> (i * 8)) & 0xff);
        int8_t bv = (int8_t)((b >> (i * 8)) & 0xff);
        uint8_t rv = ia64_sat_s8((long long)av + (long long)bv);
        res |= (uint64_t)rv << (i * 8);
    }
    return res;
}

uint64_t HELPER(padd1_uuu)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t av = (a >> (i * 8)) & 0xff;
        uint8_t bv = (b >> (i * 8)) & 0xff;
        uint8_t rv = ia64_sat_u8((long long)av + (long long)bv);
        res |= (uint64_t)rv << (i * 8);
    }
    return res;
}

uint64_t HELPER(padd1_uus)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t av = (a >> (i * 8)) & 0xff;
        int8_t bv = (int8_t)((b >> (i * 8)) & 0xff);
        uint8_t rv = ia64_sat_u8((long long)av + (long long)bv);
        res |= (uint64_t)rv << (i * 8);
    }
    return res;
}

uint64_t HELPER(padd2)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t av = (a >> (i * 16)) & 0xffff;
        uint16_t bv = (b >> (i * 16)) & 0xffff;
        uint16_t rv = av + bv;
        res |= (uint64_t)rv << (i * 16);
    }
    return res;
}

uint64_t HELPER(padd2_sss)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 4; i++) {
        int16_t av = (int16_t)((a >> (i * 16)) & 0xffff);
        int16_t bv = (int16_t)((b >> (i * 16)) & 0xffff);
        uint16_t rv = ia64_sat_s16((long long)av + (long long)bv);
        res |= (uint64_t)rv << (i * 16);
    }
    return res;
}

uint64_t HELPER(padd2_uuu)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t av = (a >> (i * 16)) & 0xffff;
        uint16_t bv = (b >> (i * 16)) & 0xffff;
        uint16_t rv = ia64_sat_u16((long long)av + (long long)bv);
        res |= (uint64_t)rv << (i * 16);
    }
    return res;
}

uint64_t HELPER(padd2_uus)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t av = (a >> (i * 16)) & 0xffff;
        int16_t bv = (int16_t)((b >> (i * 16)) & 0xffff);
        uint16_t rv = ia64_sat_u16((long long)av + (long long)bv);
        res |= (uint64_t)rv << (i * 16);
    }
    return res;
}

uint64_t HELPER(padd4)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint32_t lo = (uint32_t)a + (uint32_t)b;
    uint32_t hi = (uint32_t)(a >> 32) + (uint32_t)(b >> 32);
    return ((uint64_t)hi << 32) | lo;
}

uint64_t HELPER(psub1)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t av = (a >> (i * 8)) & 0xff;
        uint8_t bv = (b >> (i * 8)) & 0xff;
        uint8_t rv = av - bv;
        res |= (uint64_t)rv << (i * 8);
    }
    return res;
}

uint64_t HELPER(psub1_sss)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 8; i++) {
        int8_t av = (int8_t)((a >> (i * 8)) & 0xff);
        int8_t bv = (int8_t)((b >> (i * 8)) & 0xff);
        uint8_t rv = ia64_sat_s8((long long)av - (long long)bv);
        res |= (uint64_t)rv << (i * 8);
    }
    return res;
}

uint64_t HELPER(psub1_uuu)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t av = (a >> (i * 8)) & 0xff;
        uint8_t bv = (b >> (i * 8)) & 0xff;
        uint8_t rv = ia64_sat_u8((long long)av - (long long)bv);
        res |= (uint64_t)rv << (i * 8);
    }
    return res;
}

uint64_t HELPER(psub1_uus)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t av = (a >> (i * 8)) & 0xff;
        int8_t bv = (int8_t)((b >> (i * 8)) & 0xff);
        uint8_t rv = ia64_sat_u8((long long)av - (long long)bv);
        res |= (uint64_t)rv << (i * 8);
    }
    return res;
}

uint64_t HELPER(psub2)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t av = (a >> (i * 16)) & 0xffff;
        uint16_t bv = (b >> (i * 16)) & 0xffff;
        uint16_t rv = av - bv;
        res |= (uint64_t)rv << (i * 16);
    }
    return res;
}

uint64_t HELPER(psub2_sss)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 4; i++) {
        int16_t av = (int16_t)((a >> (i * 16)) & 0xffff);
        int16_t bv = (int16_t)((b >> (i * 16)) & 0xffff);
        uint16_t rv = ia64_sat_s16((long long)av - (long long)bv);
        res |= (uint64_t)rv << (i * 16);
    }
    return res;
}

uint64_t HELPER(psub2_uuu)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t av = (a >> (i * 16)) & 0xffff;
        uint16_t bv = (b >> (i * 16)) & 0xffff;
        uint16_t rv = ia64_sat_u16((long long)av - (long long)bv);
        res |= (uint64_t)rv << (i * 16);
    }
    return res;
}

uint64_t HELPER(psub2_uus)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint64_t res = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t av = (a >> (i * 16)) & 0xffff;
        int16_t bv = (int16_t)((b >> (i * 16)) & 0xffff);
        uint16_t rv = ia64_sat_u16((long long)av - (long long)bv);
        res |= (uint64_t)rv << (i * 16);
    }
    return res;
}

uint64_t HELPER(psub4)(CPUIA64State *env, uint64_t a, uint64_t b)
{
    (void)env;
    uint32_t lo = (uint32_t)a - (uint32_t)b;
    uint32_t hi = (uint32_t)(a >> 32) - (uint32_t)(b >> 32);
    return ((uint64_t)hi << 32) | lo;
}

void HELPER(setf_sig)(CPUIA64State *env, uint32_t f1, uint64_t val)
{
    uint32_t pf = ia64_fr_phys(env, f1);
    if (pf <= 1) {
        return;
    }
    env->f[pf][0] = val;
    /*
     * Match SKI's dword2freg(): treat the 64-bit payload as an unnormalized
     * significand with an initial exponent of bias+63.
     *
     * fnorm + getf.exp then yields bias+msb_index, which Linux uses for fls().
     */
    env->f[pf][1] = IA64_FP_SEXP(0, IA64_FP_EXP_INTEGER);
}

uint64_t HELPER(getf_sig)(CPUIA64State *env, uint32_t f2)
{
    uint32_t pf = ia64_fr_phys(env, f2);
    return env->f[pf][0];
}

void HELPER(xma_l)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                   uint32_t f4, uint32_t f2)
{
    uint32_t pf1 = ia64_fr_phys(env, f1);
    uint32_t pf2 = ia64_fr_phys(env, f2);
    uint32_t pf3 = ia64_fr_phys(env, f3);
    uint32_t pf4 = ia64_fr_phys(env, f4);
    if (pf1 <= 1) {
        return;
    }
    __uint128_t prod = (__uint128_t)env->f[pf3][0] * (__uint128_t)env->f[pf4][0];
    __uint128_t sum = prod + (__uint128_t)env->f[pf2][0];
    env->f[pf1][0] = (uint64_t)sum;
    env->f[pf1][1] = 0;
}

void HELPER(xma_h)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                   uint32_t f4, uint32_t f2)
{
    uint32_t pf1 = ia64_fr_phys(env, f1);
    uint32_t pf2 = ia64_fr_phys(env, f2);
    uint32_t pf3 = ia64_fr_phys(env, f3);
    uint32_t pf4 = ia64_fr_phys(env, f4);
    if (pf1 <= 1) {
        return;
    }
    __int128 prod = (__int128)(int64_t)env->f[pf3][0] * (__int128)(int64_t)env->f[pf4][0];
    __int128 sum = prod + (__int128)(int64_t)env->f[pf2][0];
    env->f[pf1][0] = (uint64_t)(sum >> 64);
    env->f[pf1][1] = 0;
}

void HELPER(xma_hu)(CPUIA64State *env, uint32_t f1, uint32_t f3,
                    uint32_t f4, uint32_t f2)
{
    uint32_t pf1 = ia64_fr_phys(env, f1);
    uint32_t pf2 = ia64_fr_phys(env, f2);
    uint32_t pf3 = ia64_fr_phys(env, f3);
    uint32_t pf4 = ia64_fr_phys(env, f4);
    if (pf1 <= 1) {
        return;
    }
    __uint128_t prod = (__uint128_t)env->f[pf3][0] * (__uint128_t)env->f[pf4][0];
    __uint128_t sum = prod + (__uint128_t)env->f[pf2][0];
    env->f[pf1][0] = (uint64_t)(sum >> 64);
    env->f[pf1][1] = 0;
}

static long double ia64_fp_to_ld(const CPUIA64State *env, uint32_t f)
{
    uint32_t pf = ia64_fr_phys(env, f);
    uint64_t mant = env->f[pf][0];
    uint64_t expw = env->f[pf][1];

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
    uint32_t pf = ia64_fr_phys(env, f);
    if (pf <= 1) {
        return;
    }

    if (val == 0.0L || isnan(val) || isinf(val)) {
        env->f[pf][0] = 0;
        env->f[pf][1] = 0;
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

    env->f[pf][0] = mant;
    env->f[pf][1] = expw;
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
    uint32_t pf1 = ia64_fr_phys(env, f1);
    uint32_t pf2 = ia64_fr_phys(env, f2);
    if (pf1 <= 1) {
        return;
    }

    uint64_t mant = env->f[pf2][0];
    uint64_t expw = env->f[pf2][1];
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

    env->f[pf1][0] = res;
    env->f[pf1][1] = IA64_FP_SEXP(0, IA64_FP_EXP_INTEGER);
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
    static int log_enabled = -1;
    static int abort_enabled = -1;
    static int dump_enabled = -1;
    static int dump_bundles = -1;
    if (log_enabled == -1) {
        const char *s = getenv("QEMU_IA64_BREAK_LOG");
        log_enabled = (s && *s) ? 1 : 0;
    }
    if (abort_enabled == -1) {
        const char *s = getenv("QEMU_IA64_BREAK_ABORT");
        abort_enabled = (s && *s) ? 1 : 0;
    }
    if (dump_enabled == -1) {
        const char *s = getenv("QEMU_IA64_BREAK_DUMP");
        dump_enabled = (s && *s) ? 1 : 0;
    }
    if (dump_bundles == -1) {
        dump_bundles = 64;
        const char *s = getenv("QEMU_IA64_BREAK_DUMP_BUNDLES");
        if (s && *s) {
            dump_bundles = atoi(s);
        }
        if (dump_bundles < 0) {
            dump_bundles = 0;
        }
        if (dump_bundles > 512) {
            dump_bundles = 512;
        }
    }

    CPUState *cs = env_cpu(env);
    if (log_enabled && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: breaki iim=%016" PRIx64 " ip=%016" PRIx64
                      " psr=%016" PRIx64 " cfm=%016" PRIx64 " pr=%016" PRIx64
                      " iva=%016" PRIx64
                      " r8=%016" PRIx64 " r9=%016" PRIx64 " r10=%016" PRIx64
                      " r11=%016" PRIx64 " r12=%016" PRIx64 " r13=%016" PRIx64
                      " r15=%016" PRIx64 " b0=%016" PRIx64 " b7=%016" PRIx64 "\n",
                      iim, env->ip, env->psr, env->cfm, env->pr, env->cr[2],
                      env->r[8], env->r[9], env->r[10], env->r[11],
                      env->r[12], env->r[13], env->r[15], env->b[0], env->b[7]);
    }
    if (dump_enabled && dump_bundles > 0) {
        uint64_t iva = env->cr[2];
        ia64_fw_dump_code(env, "break_ip", env->ip, dump_bundles);
        ia64_fw_dump_code(env, "break_from", env->last_branch_from, dump_bundles);
        ia64_fw_dump_code(env, "break_b0w", env->last_b0_write_pc, dump_bundles);
        if (iva) {
            ia64_fw_dump_code(env, "break_vec", iva + IA64_VEC_BREAK, dump_bundles);
        }
    }
    if (abort_enabled) {
        cpu_abort(cs, "IA64: breaki iim=%016" PRIx64 " ip=%016" PRIx64,
                  iim, env->ip);
    }
    ia64_fault(cs, env, false, false, IA64_VEC_BREAK, iim, GETPC());
}

/*
 * Minimal EFI status code definitions used for firmware assert decoding.
 * Values match EDK's EfiStatusCode.h.
 */
#define IA64_EFI_STATUS_CODE_TYPE_MASK        0x000000FFu
#define IA64_EFI_STATUS_CODE_SEVERITY_MASK    0xFF000000u
#define IA64_EFI_STATUS_CODE_CLASS_MASK       0xFF000000u
#define IA64_EFI_STATUS_CODE_SUBCLASS_MASK    0x00FF0000u
#define IA64_EFI_STATUS_CODE_OPERATION_MASK   0x0000FFFFu
#define IA64_EFI_PROGRESS_CODE                0x00000001u
#define IA64_EFI_ERROR_CODE                  0x00000002u
#define IA64_EFI_DEBUG_CODE                  0x00000003u
#define IA64_EFI_ERROR_UNRECOVERED           0x90000000u
#define IA64_EFI_SW_EC_ILLEGAL_SOFTWARE_STATE 0x00000007u
#define IA64_EFI_CLASS_COMPUTING             0x00000000u
#define IA64_EFI_CLASS_PERIPHERAL            0x01000000u
#define IA64_EFI_CLASS_IO_BUS                0x02000000u
#define IA64_EFI_CLASS_SOFTWARE              0x03000000u
#define IA64_EFI_IO_BUS_PCI_SUBCLASS         0x00010000u

static bool ia64_fw_status_is_assert(uint32_t code_type, uint32_t value)
{
    return (code_type & IA64_EFI_STATUS_CODE_TYPE_MASK) == IA64_EFI_ERROR_CODE &&
           (code_type & IA64_EFI_STATUS_CODE_SEVERITY_MASK) ==
               IA64_EFI_ERROR_UNRECOVERED &&
           (value & IA64_EFI_STATUS_CODE_OPERATION_MASK) ==
               IA64_EFI_SW_EC_ILLEGAL_SOFTWARE_STATE;
}

static bool ia64_fw_status_code_valid(uint32_t code_type, uint32_t value)
{
    uint32_t type = code_type & IA64_EFI_STATUS_CODE_TYPE_MASK;
    uint32_t class = value & IA64_EFI_STATUS_CODE_CLASS_MASK;

    if (type != IA64_EFI_PROGRESS_CODE &&
        type != IA64_EFI_ERROR_CODE &&
        type != IA64_EFI_DEBUG_CODE) {
        return false;
    }

    return class == IA64_EFI_CLASS_COMPUTING ||
           class == IA64_EFI_CLASS_PERIPHERAL ||
           class == IA64_EFI_CLASS_IO_BUS ||
           class == IA64_EFI_CLASS_SOFTWARE;
}

static const char *ia64_fw_status_class_name(uint32_t class)
{
    switch (class) {
    case IA64_EFI_CLASS_COMPUTING:
        return "COMPUTING";
    case IA64_EFI_CLASS_PERIPHERAL:
        return "PERIPHERAL";
    case IA64_EFI_CLASS_IO_BUS:
        return "IO_BUS";
    case IA64_EFI_CLASS_SOFTWARE:
        return "SOFTWARE";
    default:
        return NULL;
    }
}

static const char *ia64_fw_status_subclass_name(uint32_t value)
{
    if ((value & IA64_EFI_STATUS_CODE_CLASS_MASK) == IA64_EFI_CLASS_IO_BUS &&
        (value & IA64_EFI_STATUS_CODE_SUBCLASS_MASK) ==
            IA64_EFI_IO_BUS_PCI_SUBCLASS) {
        return "PCI";
    }
    return NULL;
}

#define IA64_EFI_STATUS_ERROR_BIT (1ULL << 63)

static const char *ia64_fw_efi_status_name(uint64_t status)
{
    uint64_t code = status & ~IA64_EFI_STATUS_ERROR_BIT;

    if (status == 0) {
        return "EFI_SUCCESS";
    }

    if (status & IA64_EFI_STATUS_ERROR_BIT) {
        switch (code) {
        case 1: return "EFI_LOAD_ERROR";
        case 2: return "EFI_INVALID_PARAMETER";
        case 3: return "EFI_UNSUPPORTED";
        case 4: return "EFI_BAD_BUFFER_SIZE";
        case 5: return "EFI_BUFFER_TOO_SMALL";
        case 6: return "EFI_NOT_READY";
        case 7: return "EFI_DEVICE_ERROR";
        case 8: return "EFI_WRITE_PROTECTED";
        case 9: return "EFI_OUT_OF_RESOURCES";
        case 10: return "EFI_VOLUME_CORRUPTED";
        case 11: return "EFI_VOLUME_FULL";
        case 12: return "EFI_NO_MEDIA";
        case 13: return "EFI_MEDIA_CHANGED";
        case 14: return "EFI_NOT_FOUND";
        case 15: return "EFI_ACCESS_DENIED";
        case 16: return "EFI_NO_RESPONSE";
        case 17: return "EFI_NO_MAPPING";
        case 18: return "EFI_TIMEOUT";
        case 19: return "EFI_NOT_STARTED";
        case 20: return "EFI_ALREADY_STARTED";
        case 21: return "EFI_ABORTED";
        case 22: return "EFI_ICMP_ERROR";
        case 23: return "EFI_TFTP_ERROR";
        case 24: return "EFI_PROTOCOL_ERROR";
        case 25: return "EFI_INCOMPATIBLE_VERSION";
        case 26: return "EFI_SECURITY_VIOLATION";
        case 27: return "EFI_CRC_ERROR";
        case 28: return "EFI_END_OF_MEDIA";
        case 31: return "EFI_END_OF_FILE";
        case 32: return "EFI_INVALID_LANGUAGE";
        case 33: return "EFI_COMPROMISED_DATA";
        case 34: return "EFI_IP_ADDRESS_CONFLICT";
        case 35: return "EFI_HTTP_ERROR";
        default:
            return NULL;
        }
    }

    switch (code) {
    case 1: return "EFI_WARN_UNKNOWN_GLYPH";
    case 2: return "EFI_WARN_DELETE_FAILURE";
    case 3: return "EFI_WARN_WRITE_FAILURE";
    case 4: return "EFI_WARN_BUFFER_TOO_SMALL";
    case 5: return "EFI_WARN_STALE_DATA";
    case 6: return "EFI_WARN_FILE_SYSTEM";
    case 7: return "EFI_WARN_RESET_REQUIRED";
    default:
        return NULL;
    }
}

static bool ia64_fw_efi_status_maybe(uint64_t status)
{
    if (status == 0) {
        return true;
    }
    uint64_t code = status & ~IA64_EFI_STATUS_ERROR_BIT;
    if (code == 0 || code > 0x2000) {
        return false;
    }
    return true;
}

static const char *ia64_fw_decode_sala_post_code(uint16_t code)
{
    switch (code) {
    case 0x8FE0: return "Reset Condition";
    case 0x8FD0: return "Node BSP selection";
    case 0x8FC0: return "Early node init (SNCPEIM)";
    case 0x8FB0: return "Processor health/setup (CVDR PEIM)";
    case 0x8FA0: return "PAL/FW health status";
    case 0x8F70: return "Memory Initialization Entry";
    case 0x8F71: return "RAC Initialization (Mem_DoRacInitialization)";
    case 0x8F72: return "Validate DIMMs (Mem_ValidateInstalledConfiguration)";
    case 0x8F73: return "Program MIRs/MITs (Mem_DoMirMitProgram)";
    case 0x8F74: return "Calculate CAS (Mem_CalcSysCas)";
    case 0xCF74: return "Calculate CAS Error Loop";
    case 0x8F75: return "Program CAS (Mem_SetMrhdCasLatency)";
    case 0x8F76: return "Set Mrhd DIMM Geometry (Mem_SetMrhdDimmGeometry)";
    case 0x8F77: return "SLEW rate calibration (Mem_DoSlewRateCalibration)";
    case 0x8F78: return "Mem_InitDimmAndSetCasLatencyAndBurst";
    case 0x8F79: return "DDR delay Calibration (Mem_DoDdrDelayCalibration)";
    case 0x8F80: return "DIMM path latency Calibration";
    case 0x8F81: return "DIMM Strobe Delay Calibration";
    case 0x8F82: return "Configure SNC timing";
    case 0x8F83: return "Set timings for write pattern";
    case 0x8F90: return "Levelization";
    case 0x8F98: return "Reconfigure memory";
    case 0xCF9F: return "Levelization failed: no memory found";
    case 0x8F60: return "Memory Test";
    case 0x8F50: return "Platform Discovery/Init";
    case 0x8F40: return "SBSP selection";
    case 0x8F20: return "Memory Autoscan entry";
    case 0x8F21: return "Process Auto Scan Input";
    case 0x8F22: return "Process Auto Scan Output";
    case 0x8F10: return "Recovery code entry";
    case 0x8ED0: return "Recovery C-Code Entry";
    case 0x8ED1: return "Recovery Reading media";
    case 0xCEDF: return "Recovery Reading error";
    case 0x8EC0: return "Recovery program start";
    case 0x8EC1: return "Recovery program success";
    case 0xCECF: return "Recovery programming error";
    case 0x8E80: return "PEIM Handoff block entry";
    case 0x8C00: return "SALA to SALB/DXE handoff";
    default:
        return NULL;
    }
}

static bool ia64_fw_is_printable_ascii(uint8_t b)
{
    return b == '\t' || b == '\r' || b == '\n' || (b >= 0x20 && b < 0x7f);
}

static bool ia64_fw_read_bytes_any(CPUState *cs, uint64_t addr,
                                   uint8_t *buf, size_t len)
{
    if (len == 0) {
        return true;
    }
    if (cpu_memory_rw_debug(cs, addr, buf, len, false) == 0) {
        return true;
    }
    MemTxResult r = address_space_read(&address_space_memory, (hwaddr)addr,
                                       MEMTXATTRS_UNSPECIFIED, buf, len);
    if (r == MEMTX_OK) {
        return true;
    }
    uint64_t phys = addr & ((1ULL << 61) - 1);
    if (phys != addr) {
        r = address_space_read(&address_space_memory, (hwaddr)phys,
                               MEMTXATTRS_UNSPECIFIED, buf, len);
        return (r == MEMTX_OK);
    }
    return false;
}

static bool ia64_fw_write_bytes_any(CPUState *cs, uint64_t addr,
                                    const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return true;
    }
    if (cpu_memory_rw_debug(cs, addr, (void *)buf, len, true) == 0) {
        return true;
    }
    MemTxResult r = address_space_write(&address_space_memory, (hwaddr)addr,
                                        MEMTXATTRS_UNSPECIFIED, buf, len);
    if (r == MEMTX_OK) {
        return true;
    }
    uint64_t phys = addr & ((1ULL << 61) - 1);
    if (phys != addr) {
        r = address_space_write(&address_space_memory, (hwaddr)phys,
                                MEMTXATTRS_UNSPECIFIED, buf, len);
        return (r == MEMTX_OK);
    }
    return false;
}

static size_t ia64_fw_read_ascii_string(CPUState *cs, uint64_t addr,
                                        char *out, size_t out_size)
{
    if (!out || out_size < 2) {
        return 0;
    }

    size_t i;
    for (i = 0; i + 1 < out_size; i++) {
        uint8_t b = 0;
        if (!ia64_fw_read_bytes_any(cs, addr + i, &b, 1)) {
            break;
        }
        out[i] = (char)b;
        if (b == '\0') {
            return i;
        }
        if (!ia64_fw_is_printable_ascii(b)) {
            break;
        }
    }
    out[0] = '\0';
    return 0;
}

static size_t ia64_fw_read_ucs2le_string(CPUState *cs, uint64_t addr,
                                        char *out, size_t out_size)
{
    if (!out || out_size < 2) {
        return 0;
    }

    size_t i;
    for (i = 0; i + 1 < out_size; i++) {
        uint8_t bytes[2] = { 0 };
        if (!ia64_fw_read_bytes_any(cs, addr + i * 2, bytes, 2)) {
            break;
        }
        uint16_t ch = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
        if (ch == 0) {
            out[i] = '\0';
            return i;
        }
        if (ch > 0x7e || !ia64_fw_is_printable_ascii((uint8_t)ch)) {
            break;
        }
        out[i] = (char)ch;
    }
    out[0] = '\0';
    return 0;
}

/*
 * Firmware bringup helpers: EFI system table scanning and configuration-table
 * injection.
 *
 * xenipf/EDK firmware's ExtendedSal DXE driver expects an EFI configuration
 * table entry for the SAL System Table GUID. On real platforms, this entry
 * points at an SST_ structure that describes the PAL/SAL procedure entry
 * points. For our TCG bringup, we synthesize a minimal SST_ in firmware
 * work RAM and inject a config table entry that points at it, while the
 * IA-64 backend emulates the PAL/SAL entrypoint procedures.
 *
 * The firmware assert we hit is consistent with `EFI_NOT_FOUND` when looking
 * up `gEfiSalSystemTableGuid` via `EfiLibGetSystemConfigurationTable()`.
 */
#define IA64_EFI_SYSTEM_TABLE_SIGNATURE 0x5453595320494249ULL /* "IBI SYST" */
#define IA64_EFI_BOOT_SERVICES_SIGNATURE 0x56524553544f4f42ULL /* "BOOTSERV" */
#define IA64_EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544e5552ULL /* "RUNTSERV" */
#define IA64_PEI_SERVICES_SIGNATURE     0x5652455320494550ULL /* "P E I S" */

typedef struct QEMU_PACKED {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
} IA64EfiGuid;

typedef struct QEMU_PACKED {
    uint64_t signature;
    uint32_t revision;
    uint32_t headersize;
    uint32_t crc32;
    uint32_t reserved;
} IA64EfiTableHeader;

typedef struct QEMU_PACKED {
    IA64EfiTableHeader hdr;
    uint64_t fw_vendor;
    uint32_t fw_revision;
    uint32_t pad;
    uint64_t con_in_handle;
    uint64_t con_in;
    uint64_t con_out_handle;
    uint64_t con_out;
    uint64_t std_err_handle;
    uint64_t std_err;
    uint64_t runtime;
    uint64_t boot;
    uint64_t nr_tables;
    uint64_t tables;
} IA64EfiSystemTable;

typedef struct QEMU_PACKED {
    IA64EfiGuid guid;
    uint64_t table;
} IA64EfiConfigTableEntry;

typedef struct QEMU_PACKED {
    uint8_t signature[4]; /* "SST_" */
    uint32_t size;
    uint8_t sal_rev_minor;
    uint8_t sal_rev_major;
    uint16_t entry_count;
    uint8_t checksum;
    uint8_t reserved1[7];
    uint8_t sal_a_rev_minor;
    uint8_t sal_a_rev_major;
    uint8_t sal_b_rev_minor;
    uint8_t sal_b_rev_major;
    uint8_t oem_id[32];
    uint8_t product_id[32];
    uint8_t reserved2[8];
} IA64SalSystab;

typedef struct QEMU_PACKED {
    uint8_t type; /* 0 == SAL_DESC_ENTRY_POINT */
    uint8_t reserved1[7];
    uint64_t pal_proc;
    uint64_t sal_proc;
    uint64_t gp;
    uint8_t reserved2[16];
} IA64SalDescEntryPoint;

static const IA64EfiGuid ia64_efi_guid_sal_systab = {
    .data1 = 0xeb9d2d32,
    .data2 = 0x2d88,
    .data3 = 0x11d3,
    .data4 = { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d },
};

static const IA64EfiGuid ia64_efi_guid_esal_pci = {
    .data1 = 0xa46b1a31,
    .data2 = 0xad66,
    .data3 = 0x4905,
    .data4 = { 0x92, 0xf6, 0x2b, 0x46, 0x59, 0xdc, 0x30, 0x63 },
};

static const IA64EfiGuid ia64_efi_guid_esal_fvb = {
    .data1 = 0xa2271df1,
    .data2 = 0xbcbb,
    .data3 = 0x4f1d,
    .data4 = { 0x98, 0xa9, 0x06, 0xbc, 0x17, 0x2f, 0x07, 0x1a },
};

static const IA64EfiGuid ia64_efi_guid_status_assert = {
    .data1 = 0xda571595,
    .data2 = 0x4d99,
    .data3 = 0x487c,
    .data4 = { 0x82, 0x7c, 0x26, 0x22, 0x67, 0x7d, 0x33, 0x07 },
};

static const IA64EfiGuid ia64_efi_guid_status_string = {
    .data1 = 0x92d11080,
    .data2 = 0x496f,
    .data3 = 0x4d95,
    .data4 = { 0xbe, 0x7e, 0x03, 0x74, 0x88, 0x38, 0x2b, 0x0a },
};

static const IA64EfiGuid ia64_efi_guid_status_debug = {
    .data1 = 0x9a4e9246,
    .data2 = 0xd553,
    .data3 = 0x11d5,
    .data4 = { 0x87, 0xe2, 0x00, 0x06, 0x29, 0x45, 0xc3, 0xb9 },
};

static const IA64EfiGuid ia64_efi_guid_status_code_ppi = {
    .data1 = 0x229832d3,
    .data2 = 0x7a30,
    .data3 = 0x4b36,
    .data4 = { 0xb8, 0x27, 0xf4, 0x0c, 0xb7, 0xd4, 0x54, 0x36 },
};

static const IA64EfiGuid ia64_efi_guid_memory_discovered_ppi = {
    .data1 = 0xf894643d,
    .data2 = 0xc449,
    .data3 = 0x42d1,
    .data4 = { 0x8e, 0xa8, 0x85, 0xbd, 0xd8, 0xc6, 0x5b, 0xde },
};

static const IA64EfiGuid ia64_efi_guid_flashmap_hob = {
    .data1 = 0xb091e7d2,
    .data2 = 0x05a0,
    .data3 = 0x4198,
    .data4 = { 0x94, 0xf0, 0x74, 0xb7, 0xb8, 0xc5, 0x54, 0x59 },
};
static const IA64EfiGuid ia64_efi_guid_memtype_info = {
    .data1 = 0x4c19049f,
    .data2 = 0x4137,
    .data3 = 0x4dd3,
    .data4 = { 0x9c, 0x10, 0x8b, 0x97, 0xa8, 0x3f, 0xfd, 0xfa },
};

typedef struct {
    uint32_t type;
    uint32_t pages;
} IA64EfiMemTypeInfo;

static const IA64EfiMemTypeInfo ia64_fw_memtype_info[] = {
    { 0, 0 },  /* EfiReservedMemoryType */
    { 1, 0 },  /* EfiLoaderCode */
    { 2, 0 },  /* EfiLoaderData */
    { 3, 0 },  /* EfiBootServicesCode */
    { 4, 0 },  /* EfiBootServicesData */
    { 5, 0 },  /* EfiRuntimeServicesCode */
    { 6, 0 },  /* EfiRuntimeServicesData */
    { 7, 0 },  /* EfiConventionalMemory */
    { 8, 0 },  /* EfiUnusableMemory */
    { 9, 0 },  /* EfiACPIReclaimMemory */
    { 10, 0 }, /* EfiACPIMemoryNVS */
    { 11, 0 }, /* EfiMemoryMappedIO */
    { 12, 0 }, /* EfiMemoryMappedIOPortSpace */
    { 13, 0 }, /* EfiPalCode */
    { 14, 0 }, /* EfiMaxMemoryType (terminator) */
};

static bool ia64_fw_guid_equal(const IA64EfiGuid *a, const IA64EfiGuid *b)
{
    return a->data1 == b->data1 && a->data2 == b->data2 && a->data3 == b->data3 &&
           memcmp(a->data4, b->data4, sizeof(a->data4)) == 0;
}

static bool ia64_fw_read_phys(hwaddr addr, void *buf, size_t len);
static bool ia64_fw_write_phys(hwaddr addr, const void *buf, size_t len);

static uint8_t ia64_fw_byte_checksum(const uint8_t *buf, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    return (uint8_t)(0 - sum);
}

static hwaddr ia64_fw_systab_inject_pa(void)
{
    hwaddr end = IA64_IPF_FW_WORKRAM_BASE + IA64_IPF_FW_WORKRAM_SIZE;
    return (end - 0x2000) & ~0xFULL;
}

static hwaddr ia64_fw_sal_systab_pa(CPUIA64State *env)
{
    if (env->fw_sal_systab_pa) {
        return (hwaddr)env->fw_sal_systab_pa;
    }
    hwaddr end = IA64_IPF_FW_WORKRAM_BASE + IA64_IPF_FW_WORKRAM_SIZE;
    hwaddr pa = (end - 0x3000) & ~0xFULL;
    env->fw_sal_systab_pa = pa;
    return pa;
}

static bool ia64_fw_write_sal_systab(CPUIA64State *env, hwaddr pa)
{
    IA64SalSystab sst = { 0 };
    IA64SalDescEntryPoint ep = { 0 };
    uint8_t buf[sizeof(sst) + sizeof(ep)] = { 0 };

    memcpy(sst.signature, "SST_", 4);
    sst.size = sizeof(buf);
    sst.sal_rev_major = 2;
    sst.sal_rev_minor = 0;
    sst.entry_count = 1;
    sst.sal_a_rev_major = 0;
    sst.sal_a_rev_minor = 0;
    sst.sal_b_rev_major = 0;
    sst.sal_b_rev_minor = 0;
    memcpy(sst.oem_id, "QEMU", 4);
    memcpy(sst.product_id, "QEMU-IPF", 7);
    sst.checksum = 0;

    ep.type = 0; /* SAL_DESC_ENTRY_POINT */
    ep.pal_proc = IA64_IPF_FW_PAL_PROC_ADDR;
    ep.sal_proc = IA64_IPF_FW_SAL_PROC_ADDR;
    ep.gp = IA64_IPF_FW_SAL_GP_ADDR;

    memcpy(buf, &sst, sizeof(sst));
    memcpy(buf + sizeof(sst), &ep, sizeof(ep));
    buf[offsetof(IA64SalSystab, checksum)] = ia64_fw_byte_checksum(buf, sizeof(buf));

    return ia64_fw_write_phys(pa, buf, sizeof(buf));
}

static void ia64_fw_guid_from_bytes(const uint8_t *buf, IA64EfiGuid *out)
{
    out->data1 = ldl_le_p(buf);
    out->data2 = lduw_le_p(buf + 4);
    out->data3 = lduw_le_p(buf + 6);
    memcpy(out->data4, buf + 8, sizeof(out->data4));
}

static bool ia64_fw_read_guid(CPUIA64State *env, uint64_t va, IA64EfiGuid *out)
{
    CPUState *cs = env_cpu(env);
    return ia64_fw_read_bytes_any(cs, va, (uint8_t *)out, sizeof(*out));
}

static bool ia64_fw_read_u64(CPUState *cs, uint64_t addr, uint64_t *out)
{
    uint8_t buf[8];
    if (!ia64_fw_read_bytes_any(cs, addr, buf, sizeof(buf))) {
        return false;
    }
    *out = ldq_le_p(buf);
    return true;
}

static bool ia64_fw_read_u32(CPUState *cs, uint64_t addr, uint32_t *out)
{
    uint8_t buf[4];
    if (!ia64_fw_read_bytes_any(cs, addr, buf, sizeof(buf))) {
        return false;
    }
    *out = ldl_le_p(buf);
    return true;
}

static bool ia64_fw_read_fdesc(CPUState *cs, uint64_t addr,
                               uint64_t *entry, uint64_t *gp)
{
    return ia64_fw_read_u64(cs, addr, entry) &&
           ia64_fw_read_u64(cs, addr + 8, gp);
}

static bool ia64_fw_pei_is_ps_table(CPUState *cs, uint64_t ptr)
{
    uint64_t sig = 0;
    if (!ptr || (ptr & 7) != 0 || !ia64_fw_read_u64(cs, ptr, &sig)) {
        return false;
    }
    if (sig != IA64_PEI_SERVICES_SIGNATURE) {
        return false;
    }
    uint32_t hdr_size = 0;
    if (!ia64_fw_read_bytes_any(cs, ptr + 0x0c,
                                (uint8_t *)&hdr_size, sizeof(hdr_size))) {
        return false;
    }
    hdr_size = le32_to_cpu(hdr_size);
    if (hdr_size < 0x18 || hdr_size > 0x200) {
        return false;
    }
    return true;
}

static bool ia64_fw_pei_get_ps_ptr(CPUIA64State *env, uint64_t arg0,
                                   uint64_t *ps_out)
{
    CPUState *cs = env_cpu(env);
    uint64_t tmp = 0;
    const uint32_t core_sig = 0x43696550u; /* "PeiC" */
    static bool logged;

    if (!arg0) {
        return false;
    }

    if (ia64_fw_pei_is_ps_table(cs, arg0)) {
        *ps_out = arg0;
        if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
            logged = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ps_ptr ip=%016" PRIx64
                          " arg0=%016" PRIx64 " src=arg0\n",
                          env->ip, arg0);
        }
        return true;
    }

    if (ia64_fw_read_u64(cs, arg0, &tmp) && tmp != 0) {
        if (ia64_fw_pei_is_ps_table(cs, tmp)) {
            *ps_out = tmp;
            if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                logged = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ps_ptr ip=%016" PRIx64
                              " arg0=%016" PRIx64 " src=arg0_ptr ps=%016"
                              PRIx64 "\n",
                              env->ip, arg0, tmp);
            }
            return true;
        }
        uint8_t sig_buf[4];
        uint64_t ps_ptr = 0;
        if (ia64_fw_read_bytes_any(cs, tmp, sig_buf, sizeof(sig_buf)) &&
            ldl_le_p(sig_buf) == core_sig &&
            ia64_fw_read_u64(cs, tmp + 8, &ps_ptr) &&
            ia64_fw_pei_is_ps_table(cs, ps_ptr)) {
            *ps_out = ps_ptr;
            if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                logged = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ps_ptr ip=%016" PRIx64
                              " arg0=%016" PRIx64 " src=arg0_ptr_peicore"
                              " peicore=%016" PRIx64 " ps=%016" PRIx64 "\n",
                              env->ip, arg0, tmp, ps_ptr);
            }
            return true;
        }
    }

    {
        uint8_t sig_buf[4];
        uint64_t ps_ptr = 0;
        if (ia64_fw_read_bytes_any(cs, arg0, sig_buf, sizeof(sig_buf)) &&
            ldl_le_p(sig_buf) == core_sig &&
            ia64_fw_read_u64(cs, arg0 + 8, &ps_ptr) &&
            ia64_fw_pei_is_ps_table(cs, ps_ptr)) {
            *ps_out = ps_ptr;
            if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                logged = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ps_ptr ip=%016" PRIx64
                              " arg0=%016" PRIx64 " src=arg0_peicore"
                              " ps=%016" PRIx64 "\n",
                              env->ip, arg0, ps_ptr);
            }
            return true;
        }
    }

    return false;
}

static bool ia64_fw_pei_find_core_from_ps(CPUIA64State *env, uint64_t ps_ptr,
                                          uint64_t *core_out)
{
    CPUState *cs = env_cpu(env);
    const uint32_t sig = 0x43696550u; /* "PeiC" */
    if (ps_ptr < 8) {
        return false;
    }
    for (uint64_t off = 0; off <= 0x4000; off += 8) {
        uint64_t cand = ps_ptr - 8 - off;
        uint8_t buf[4];
        if (!ia64_fw_read_bytes_any(cs, cand, buf, sizeof(buf))) {
            continue;
        }
        if (ldl_le_p(buf) == sig) {
            uint64_t ps_check = 0;
            if (!ia64_fw_read_u64(cs, cand + 8, &ps_check)) {
                continue;
            }
            if (ps_check != ps_ptr) {
                continue;
            }
            *core_out = cand;
            return true;
        }
    }

    hwaddr sp_phys = ia64_phys_mode_addr(env->r[12]);
    const uint8_t sig_bytes[4] = { 'P', 'e', 'i', 'C' };
    const uint64_t scan_span = 256ULL << 10;
    const uint64_t scan_base =
        (sp_phys > (scan_span / 2)) ? (sp_phys - (scan_span / 2)) : 0;
    const uint64_t scan_len = scan_span;
    const size_t chunk = 64 * 1024;
    g_autofree uint8_t *buf = g_malloc(chunk);

    for (uint64_t off = 0; off < scan_len; off += chunk - 4) {
        uint64_t addr = scan_base + off;
        if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
            continue;
        }
        for (size_t j = 0; j + sizeof(sig_bytes) <= chunk; j++) {
            if (memcmp(&buf[j], sig_bytes, sizeof(sig_bytes)) != 0) {
                continue;
            }
            uint64_t cand = addr + j;
            uint64_t ps_check = 0;
            if (!ia64_fw_read_u64(cs, cand + 8, &ps_check)) {
                continue;
            }
            if (ps_check != ps_ptr) {
                continue;
            }
            *core_out = cand;
            return true;
        }
    }

    return false;
}

static void ia64_fw_pei_log_ps_entry(CPUState *cs, uint64_t ps_ptr,
                                     uint64_t target)
{
    static int enabled = -1;
    static int dump_table = -1;
    static uint64_t last_target;
    static bool dumped;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_CALL_TRACE");
        enabled = (s && *s) ? 1 : 0;
    }
    if (dump_table == -1) {
        const char *s = getenv("QEMU_IA64_PEI_PS_DUMP_TABLE");
        dump_table = (s && *s) ? 1 : 0;
    }
    if (!enabled || !ps_ptr || !target) {
        return;
    }

    if (target != last_target) {
        last_target = target;
        dumped = false;
    }

    size_t scan_len = 0x800;
    const char *scan_env = getenv("QEMU_IA64_PEI_PS_SCAN");
    if (scan_env && *scan_env) {
        long val = strtol(scan_env, NULL, 0);
        if (val > 0) {
            scan_len = (size_t)val;
        }
    }
    if (scan_len < 0x200) {
        scan_len = 0x200;
    }
    if (scan_len > 0x2000) {
        scan_len = 0x2000;
    }

    g_autofree uint8_t *buf = g_malloc(scan_len);
    if (!ia64_fw_read_bytes_any(cs, ps_ptr, buf, scan_len)) {
        return;
    }

    uint64_t target_phys = ia64_phys_mode_addr(target);
    const uint64_t fd_table_lo = 0xffe2ee90ULL;
    const uint64_t fd_table_hi = 0xffe2ef90ULL;
    bool matched = false;
    for (size_t off = 0; off + 8 <= scan_len; off += 8) {
        uint64_t val = ldq_le_p(&buf[off]);
        if (!val || val == UINT64_MAX) {
            continue;
        }
        uint64_t phys = ia64_phys_mode_addr(val);
        uint64_t fd_entry = 0;
        uint64_t fd_gp = 0;
        bool fd_ok = ia64_fw_read_fdesc(cs, val, &fd_entry, &fd_gp);
        uint64_t fd_entry_phys = fd_ok ? ia64_phys_mode_addr(fd_entry) : 0;
        bool val_match = (val == target || phys == target_phys);
        bool entry_match = (fd_ok &&
                            (fd_entry == target || fd_entry_phys == target_phys));
        if (val_match || entry_match) {
            if (fd_ok) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ps_entry ps=%016" PRIx64 " idx=%zu"
                              " off=0x%zx val=%016" PRIx64 " phys=%016" PRIx64
                              " fd.entry=%016" PRIx64 " fd.gp=%016" PRIx64
                              " fd.entry_phys=%016" PRIx64 "%s\n",
                              ps_ptr, off / 8, off, val, phys, fd_entry, fd_gp,
                              fd_entry_phys, entry_match ? " match=entry" : "");
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ps_entry ps=%016" PRIx64 " idx=%zu"
                              " off=0x%zx val=%016" PRIx64 " phys=%016" PRIx64 "\n",
                              ps_ptr, off / 8, off, val, phys);
            }
            if (!dump_table) {
                return;
            }
            matched = true;
            break;
        }
    }
    if (!matched) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_entry ps=%016" PRIx64 " target=%016" PRIx64
                      " not_found\n",
                      ps_ptr, target);
        if (dumped) {
            return;
        }
    } else if (dumped) {
        return;
    }

    dumped = true;

    const uint64_t target_window = 0x2000;
    uint64_t target_lo = (target_phys > target_window) ?
                         (target_phys - target_window) : 0;
    uint64_t target_hi = target_phys + target_window;
    int logged = 0;
    const int log_limit = 32;

    for (size_t off = 0; off + 8 <= scan_len; off += 8) {
        uint64_t val = ldq_le_p(&buf[off]);
        if (!val || val == UINT64_MAX) {
            continue;
        }
        uint64_t phys = ia64_phys_mode_addr(val);
        bool in_table = (phys >= fd_table_lo && phys <= fd_table_hi);
        bool in_flash = ia64_fw_addr_in_flash(phys);
        bool in_workram = (phys >= IA64_IPF_FW_WORKRAM_BASE &&
                           phys < IA64_IPF_FW_WORKRAM_BASE + IA64_IPF_FW_WORKRAM_SIZE);
        bool near_target = (phys >= target_lo && phys <= target_hi);
        if (!in_flash && !in_workram && !near_target && !in_table) {
            continue;
        }
        if (!in_table && logged >= log_limit) {
            continue;
        }
        uint64_t fd_entry = 0;
        uint64_t fd_gp = 0;
        bool fd_ok = ia64_fw_read_fdesc(cs, val, &fd_entry, &fd_gp);
        uint64_t fd_entry_phys = fd_ok ? ia64_phys_mode_addr(fd_entry) : 0;
        if (fd_ok) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ps_entry cand ps=%016" PRIx64 " idx=%zu"
                          " off=0x%zx val=%016" PRIx64 " phys=%016" PRIx64
                          " fd.entry=%016" PRIx64 " fd.gp=%016" PRIx64
                          " fd.entry_phys=%016" PRIx64 "%s\n",
                          ps_ptr, off / 8, off, val, phys, fd_entry, fd_gp,
                          fd_entry_phys, in_table ? " table=1" : "");
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ps_entry cand ps=%016" PRIx64 " idx=%zu"
                          " off=0x%zx val=%016" PRIx64 " phys=%016" PRIx64 "%s\n",
                          ps_ptr, off / 8, off, val, phys,
                          in_table ? " table=1" : "");
        }
        if (!in_table) {
            logged++;
        }
    }
}

static bool ia64_fw_pei_locate_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_LOCATE_TRACE");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static int ia64_fw_pei_locate_trace_limit(void)
{
    static int limit = -1;
    if (limit == -1) {
        limit = 128;
        const char *s = getenv("QEMU_IA64_PEI_LOCATE_TRACE_LIMIT");
        if (s && *s) {
            limit = atoi(s);
        }
        if (limit < 0) {
            limit = 0;
        }
    }
    return limit;
}

static bool ia64_fw_pei_install_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_INSTALL_TRACE");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static int ia64_fw_pei_install_trace_limit(void)
{
    static int limit = -1;
    if (limit == -1) {
        limit = 128;
        const char *s = getenv("QEMU_IA64_PEI_INSTALL_TRACE_LIMIT");
        if (s && *s) {
            limit = atoi(s);
        }
        if (limit < 0) {
            limit = 0;
        }
    }
    return limit;
}

static bool ia64_fw_pei_ppi_list_dump_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_PPI_LIST_DUMP");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static bool ia64_fw_pei_pre_install_probe_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_PRE_INSTALL_PROBE");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static bool ia64_fw_pei_install_pplist_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_INSTALL_PPLIST_TRACE");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static uint64_t ia64_fw_pei_install_pplist_match_addr(void)
{
    static uint64_t addr = UINT64_MAX;
    if (addr == UINT64_MAX) {
        const char *s = getenv("QEMU_IA64_PEI_INSTALL_PPLIST_ADDR");
        if (s && *s) {
            addr = strtoull(s, NULL, 0);
        } else {
            addr = 0;
        }
    }
    return addr;
}

static bool ia64_fw_r33_watch_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_WATCH_R33");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static bool ia64_fw_pei_rse_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_RSE_TRACE");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static bool ia64_fw_bootloop_log_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_BOOTLOOP_LOG");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static void ia64_fw_rse_log(CPUIA64State *env, uint64_t pc, const char *tag)
{
    if (!ia64_fw_pei_rse_trace_enabled()) {
        return;
    }

    uint64_t bsp = ia64_rse_get_bsp(env);
    uint64_t bspstore = env->ar[IA64_AR_BSPSTORE];
    uint64_t pfs = env->ar[IA64_AR_PFS];
    uint64_t rsc = env->ar[IA64_AR_RSC];
    uint64_t rnat = env->ar[IA64_AR_RNAT];
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_rse %s pc=%016" PRIx64
                  " ip=%016" PRIx64 " cfm=%016" PRIx64
                  " pfs=%016" PRIx64 " rsc=%016" PRIx64
                  " bsp=%016" PRIx64 " bspstore=%016" PRIx64
                  " rnat=%016" PRIx64 " depth=%u"
                  " r32=%016" PRIx64 " r33=%016" PRIx64
                  " r34=%016" PRIx64 " r12=%016" PRIx64
                  " r1=%016" PRIx64 "\n",
                  tag, pc, env->ip, env->cfm,
                  pfs, rsc, bsp, bspstore, rnat, env->rse_depth,
                  env->r[32], env->r[33], env->r[34], env->r[12], env->r[1]);

    if (env->rse_depth > 0 && env->rse_frames) {
        const struct IA64RSEFrame *frame = &env->rse_frames[env->rse_depth - 1];
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_rse %s top_frame ret=%016" PRIx64
                      " pfs=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                      tag, frame->ret_addr, frame->ar_pfs, frame->cfm);
    }
}

static void ia64_fw_dump_pei_ppi_list(CPUIA64State *env, uint64_t ppi_list)
{
    CPUState *cs = env_cpu(env);
    if (!ppi_list) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ppi_list: NULL list\n");
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_ppi_list: base=%016" PRIx64 "\n",
                  ppi_list);

    for (int i = 0; i < 64; i++) {
        uint64_t addr = ppi_list + (uint64_t)i * 24;
        uint8_t desc[24];
        if (!ia64_fw_read_bytes_any(cs, addr, desc, sizeof(desc))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ppi_list idx=%d unreadable desc=%016" PRIx64 "\n",
                          i, addr);
            break;
        }
        uint64_t flags = ldq_le_p(&desc[0]);
        uint64_t guid_ptr = ldq_le_p(&desc[8]);
        uint64_t ppi_ptr = ldq_le_p(&desc[16]);
        if (guid_ptr) {
            IA64EfiGuid guid;
            if (ia64_fw_read_guid(env, guid_ptr, &guid)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ppi_list idx=%d desc=%016" PRIx64
                              " flags=%016" PRIx64 " ppi=%016" PRIx64
                              " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                              i, addr, flags, ppi_ptr,
                              guid.data1, guid.data2, guid.data3,
                              guid.data4[0], guid.data4[1],
                              guid.data4[2], guid.data4[3],
                              guid.data4[4], guid.data4[5],
                              guid.data4[6], guid.data4[7]);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ppi_list idx=%d desc=%016" PRIx64
                              " flags=%016" PRIx64 " ppi=%016" PRIx64
                              " guid_ptr=%016" PRIx64 "\n",
                              i, addr, flags, ppi_ptr, guid_ptr);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ppi_list idx=%d desc=%016" PRIx64
                          " flags=%016" PRIx64 " ppi=%016" PRIx64
                          " guid_ptr=0\n",
                          i, addr, flags, ppi_ptr);
        }
        if (flags & 0x80000000ULL) {
            break;
        }
    }
}

static bool ia64_fw_decode_pci_addr(uint64_t addr, uint16_t *seg,
                                    uint8_t *bus, uint8_t *devfn,
                                    uint16_t *reg)
{
    *reg = addr & 0xff;
    *devfn = (((addr >> 11) & 0x1f) << 3) | ((addr >> 8) & 0x7);
    *bus = (addr >> 16) & 0xff;
    *seg = (addr >> 24) & 0xff;
    return true;
}

static bool ia64_fw_pci_width_to_bytes(uint64_t width, uint64_t *bytes)
{
    switch (width) {
    case 0: /* EFI PCI width: byte */
        *bytes = 1;
        return true;
    case 1: /* EFI PCI width: word */
        *bytes = 2;
        return true;
    case 2: /* EFI PCI width: dword */
        *bytes = 4;
        return true;
    default:
        return false;
    }
}

static uint64_t ia64_fw_addr_with_same_region(uint64_t exemplar, hwaddr pa)
{
    uint64_t hi32 = exemplar & 0xffffffff00000000ULL;
    if (hi32 == 0 || hi32 == 0xffffffff00000000ULL) {
        return (uint64_t)pa;
    }
    return (exemplar & ~((1ULL << 61) - 1)) | (uint64_t)pa;
}

static bool ia64_fw_read_phys(hwaddr addr, void *buf, size_t len)
{
    if (len == 0) {
        return true;
    }
    MemTxResult r = address_space_read(&address_space_memory, addr,
                                       MEMTXATTRS_UNSPECIFIED, buf, len);
    return r == MEMTX_OK;
}

static bool ia64_fw_write_phys(hwaddr addr, const void *buf, size_t len)
{
    if (len == 0) {
        return true;
    }
    MemTxResult r = address_space_write(&address_space_memory, addr,
                                        MEMTXATTRS_UNSPECIFIED, buf, len);
    return r == MEMTX_OK;
}

static bool ia64_fw_find_efi_system_table(CPUState *cs,
                                         uint64_t start,
                                         uint64_t end,
                                         uint64_t *out_pa)
{
    if (end <= start || (end - start) > (512ULL << 20)) {
        return false;
    }

    const uint64_t sig = IA64_EFI_SYSTEM_TABLE_SIGNATURE;
    const size_t chunk = 64 * 1024;
    g_autofree uint8_t *buf = g_malloc(chunk + 8);

    for (uint64_t base = start; base + 8 <= end; base += chunk) {
        size_t len = (size_t)MIN((uint64_t)chunk, end - base);
        if (!ia64_fw_read_phys((hwaddr)base, buf, len)) {
            continue;
        }
        for (size_t i = 0; i + 8 <= len; i++) {
            if (ldq_le_p(&buf[i]) != sig) {
                continue;
            }
            uint64_t cand = base + i;
            IA64EfiSystemTable st;
            if (!ia64_fw_read_bytes_any(cs, cand, (uint8_t *)&st, sizeof(st))) {
                continue;
            }
            if (st.hdr.signature != sig) {
                continue;
            }
            if (st.hdr.headersize < sizeof(st) || st.hdr.headersize > 4096) {
                continue;
            }
            /*
             * The system table may temporarily have 0 configuration tables.
             * Accept that case so we can inject the SAL systab entry.
             */
            if (st.nr_tables > 128) {
                continue;
            }
            if (st.nr_tables != 0 && st.tables == 0) {
                continue;
            }

            /*
             * Strong validation: boot/runtime services table headers must be
             * present and have the expected signatures.
             */
            if (st.boot == 0 || st.runtime == 0) {
                continue;
            }
            IA64EfiTableHeader boot_hdr, rt_hdr;
            if (!ia64_fw_read_bytes_any(cs, st.boot, (uint8_t *)&boot_hdr,
                                        sizeof(boot_hdr)) ||
                !ia64_fw_read_bytes_any(cs, st.runtime, (uint8_t *)&rt_hdr,
                                        sizeof(rt_hdr))) {
                continue;
            }
            if (boot_hdr.signature != IA64_EFI_BOOT_SERVICES_SIGNATURE ||
                rt_hdr.signature != IA64_EFI_RUNTIME_SERVICES_SIGNATURE) {
                continue;
            }

            *out_pa = cand;
            return true;
        }
    }
    return false;
}

static void ia64_fw_try_install_sal_systab(CPUIA64State *env)
{
    if (!env->fw_preboot_active) {
        return;
    }
    if (env->fw_sal_systab_installed) {
        return;
    }

    CPUState *cs = env_cpu(env);
    uint64_t systab_pa = env->fw_efi_systab_pa;
    if (systab_pa == 0) {
        uint64_t start = env->fw_phit_mem_bottom;
        uint64_t end = env->fw_phit_mem_top;

        /*
         * Prefer the PHIT-reported EFI memory range (where DXE typically
         * allocates the EFI system table). Fall back to a conservative
         * scan of the xenipf/EDK heap-ish region.
         */
        if (start && end && end > start) {
            /*
             * xenipf/EDK frequently allocates DXE structures above the PHIT
             * EfiMemoryTop (notably within our "slack" RAM). Include a
             * generous headroom to catch those allocations.
             */
            uint64_t scan_start = start > (16ULL << 20) ? start - (16ULL << 20) : 0;
            uint64_t scan_end = end + (128ULL << 20);

            /*
             * Tighten the scan around the current call site hints (stack and
             * firmware status-code buffers) so we find the table even if PHIT
             * values are stale.
             */
            const uint64_t probes[] = { env->r[12], env->r[9], env->r[10], env->r[11] };
            for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
                uint64_t p = probes[i];
                if (p < 0x10000000ULL || p >= 0x30000000ULL) {
                    continue;
                }
                uint64_t lo = p > (32ULL << 20) ? p - (32ULL << 20) : 0;
                uint64_t hi = p + (32ULL << 20);
                scan_start = MIN(scan_start, lo);
                scan_end = MAX(scan_end, hi);
            }

            ia64_fw_find_efi_system_table(cs, scan_start, scan_end, &systab_pa);
        } else {
            ia64_fw_find_efi_system_table(cs, 0x1e000000ULL, 0x24000000ULL, &systab_pa);
        }

        if (systab_pa == 0) {
            return;
        }
        env->fw_efi_systab_pa = systab_pa;
        if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw: located EFI system table at %016" PRIx64 "\n",
                          systab_pa);
        }
    }

    IA64EfiSystemTable st;
    if (!ia64_fw_read_bytes_any(cs, systab_pa, (uint8_t *)&st, sizeof(st))) {
        return;
    }
    if (st.hdr.signature != IA64_EFI_SYSTEM_TABLE_SIGNATURE) {
        env->fw_efi_systab_pa = 0;
        return;
    }
    if (st.boot == 0 || st.runtime == 0) {
        env->fw_efi_systab_pa = 0;
        return;
    }
    IA64EfiTableHeader boot_hdr, rt_hdr;
    if (!ia64_fw_read_bytes_any(cs, st.boot, (uint8_t *)&boot_hdr, sizeof(boot_hdr)) ||
        !ia64_fw_read_bytes_any(cs, st.runtime, (uint8_t *)&rt_hdr, sizeof(rt_hdr))) {
        env->fw_efi_systab_pa = 0;
        return;
    }
    if (boot_hdr.signature != IA64_EFI_BOOT_SERVICES_SIGNATURE ||
        rt_hdr.signature != IA64_EFI_RUNTIME_SERVICES_SIGNATURE) {
        env->fw_efi_systab_pa = 0;
        return;
    }

    uint64_t nr = st.nr_tables;
    if (nr > 128) {
        return;
    }

    uint64_t tables_exemplar = st.tables;
    size_t entries_len = 0;
    g_autofree IA64EfiConfigTableEntry *entries = NULL;
    if (nr != 0) {
        if (st.tables == 0) {
            return;
        }
        entries_len = nr * sizeof(IA64EfiConfigTableEntry);
        entries = g_malloc0(entries_len);
        if (!ia64_fw_read_bytes_any(cs, st.tables, (uint8_t *)entries, entries_len)) {
            return;
        }
    } else {
        /*
         * No configuration tables yet. Use another system-table pointer as
         * the "region exemplar" if available.
         */
        if (tables_exemplar == 0) {
            tables_exemplar = st.boot ? st.boot : st.runtime;
        }
    }

    hwaddr sal_pa = ia64_fw_sal_systab_pa(env);
    if (!ia64_fw_write_sal_systab(env, sal_pa)) {
        return;
    }

    for (uint64_t i = 0; i < nr; i++) {
        if (ia64_fw_guid_equal(&entries[i].guid, &ia64_efi_guid_sal_systab)) {
            env->fw_sal_systab_installed = 1;
            return;
        }
    }

    /* Build a new config table array with one appended entry. */
    uint64_t new_nr = nr + 1;
    size_t new_len = new_nr * sizeof(IA64EfiConfigTableEntry);
    g_autofree IA64EfiConfigTableEntry *new_entries = g_malloc0(new_len);
    if (entries_len) {
        memcpy(new_entries, entries, entries_len);
    }
    new_entries[nr].guid = ia64_efi_guid_sal_systab;
    new_entries[nr].table =
        ia64_fw_addr_with_same_region(tables_exemplar, (uint64_t)sal_pa);

    hwaddr inject_pa = ia64_fw_systab_inject_pa();
    if (!ia64_fw_write_phys(inject_pa, new_entries, new_len)) {
        return;
    }

    /* Update system table fields (nr_tables + tables pointer). */
    uint8_t le8[8];
    stq_le_p(le8, new_nr);
    if (!ia64_fw_write_phys((hwaddr)systab_pa + offsetof(IA64EfiSystemTable, nr_tables),
                            le8, sizeof(le8))) {
        return;
    }
    uint64_t new_tables_ptr = ia64_fw_addr_with_same_region(tables_exemplar, inject_pa);
    stq_le_p(le8, new_tables_ptr);
    if (!ia64_fw_write_phys((hwaddr)systab_pa + offsetof(IA64EfiSystemTable, tables),
                            le8, sizeof(le8))) {
        return;
    }

    /* Recompute the EFI system table CRC32 (best-effort). */
    if (st.hdr.headersize >= sizeof(IA64EfiSystemTable) && st.hdr.headersize <= 4096) {
        g_autofree uint8_t *st_bytes = g_malloc(st.hdr.headersize);
        if (ia64_fw_read_phys((hwaddr)systab_pa, st_bytes, st.hdr.headersize)) {
            stl_le_p(st_bytes + offsetof(IA64EfiTableHeader, crc32), 0);
            uint32_t crc = (uint32_t)crc32(0, st_bytes, st.hdr.headersize);
            uint8_t le4[4];
            stl_le_p(le4, crc);
            ia64_fw_write_phys((hwaddr)systab_pa + offsetof(IA64EfiTableHeader, crc32),
                               le4, sizeof(le4));
        }
    }

    env->fw_sal_systab_installed = 1;
    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw: installed SAL systab config entry (SST_ at %016" PRIx64 ")\n",
                      (uint64_t)sal_pa);
    }
}

static bool ia64_fw_validate_efi_hob_list(CPUState *cs, uint64_t base,
                                         uint64_t *end_out, int *count_out)
{
    enum {
        EFI_HOB_TYPE_HANDOFF = 0x0001,
        EFI_HOB_TYPE_END_OF_HOB_LIST = 0xffff,
    };

    uint64_t cur = base;
    for (int iter = 0; iter < 16384; iter++) {
        uint8_t h[8];
        if (cpu_memory_rw_debug(cs, cur, h, sizeof(h), false) != 0) {
            return false;
        }
        uint16_t type = lduw_le_p(&h[0]);
        uint16_t len = lduw_le_p(&h[2]);
        if (iter == 0 && type != EFI_HOB_TYPE_HANDOFF) {
            return false;
        }
        if (len < sizeof(h)) {
            return false;
        }
        cur += len;
        if (cur - base > (16ULL << 20)) {
            return false;
        }
        if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {
            if (end_out) {
                *end_out = cur;
            }
            if (count_out) {
                *count_out = iter + 1;
            }
            return true;
        }
    }
    return false;
}

static void ia64_fw_pei_log_core_hob_candidates(CPUState *cs, uint64_t core,
                                                uint64_t ps_ptr)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP");
        enabled = (s && *s) ? 1 : 0;
    }
    if (!enabled || !core) {
        return;
    }

    const uint64_t offsets[] = { 0x260, 0x470, 0x478 };
    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        uint64_t raw = 0;
        if (!ia64_fw_read_u64(cs, core + offsets[i], &raw)) {
            continue;
        }
        if (!raw || raw == UINT64_MAX) {
            continue;
        }
        uint64_t phys = ia64_phys_mode_addr(raw);
        uint64_t end = 0;
        int count = 0;
        bool valid = ia64_fw_validate_efi_hob_list(cs, phys, &end, &count);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_core hob_candidate off=0x%zx raw=%016" PRIx64
                      " phys=%016" PRIx64 " valid=%d end=%016" PRIx64
                      " count=%d ps_ptr=%016" PRIx64 "\n",
                      (size_t)offsets[i], raw, phys, valid ? 1 : 0,
                      end, count, ps_ptr);
    }
}

static bool ia64_fw_pei_scan_core(CPUState *cs, uint64_t stack_phys,
                                  uint64_t *core_out, uint64_t *ps_ptr_out)
{
    if (!stack_phys) {
        return false;
    }

    const uint8_t sig[4] = { 'P', 'e', 'i', 'C' };
    const uint64_t scan_span = 256ULL << 10;
    const uint64_t scan_base =
        (stack_phys > (scan_span / 2)) ? (stack_phys - (scan_span / 2)) : 0;
    const uint64_t scan_len = scan_span;
    const size_t chunk = 64 * 1024;
    g_autofree uint8_t *buf = g_malloc(chunk);

    for (uint64_t off = 0; off < scan_len; off += chunk - 4) {
        uint64_t addr = scan_base + off;
        if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
            continue;
        }
        for (size_t j = 0; j + sizeof(sig) <= chunk; j++) {
            if (memcmp(&buf[j], sig, sizeof(sig)) != 0) {
                continue;
            }
            uint64_t base = addr + j;
            uint8_t hdr[16];
            if (cpu_memory_rw_debug(cs, base, hdr, sizeof(hdr), false) != 0) {
                continue;
            }
            uint64_t ps_ptr = ldq_le_p(&hdr[8]);
            uint64_t ps_phys = ia64_phys_mode_addr(ps_ptr);
            bool ps_in_flash = ia64_fw_addr_in_flash(ps_phys);
            bool ps_in_workram = (ps_phys >= IA64_IPF_FW_WORKRAM_BASE &&
                                  ps_phys < IA64_IPF_FW_WORKRAM_BASE + IA64_IPF_FW_WORKRAM_SIZE);
            if (!ps_in_flash && !ps_in_workram) {
                continue;
            }
            if (core_out) {
                *core_out = base;
            }
            if (ps_ptr_out) {
                *ps_ptr_out = ps_ptr;
            }
            return true;
        }
    }

    return false;
}

static bool ia64_fw_find_pei_hob_list(CPUState *cs, uint64_t stack_phys,
                                      uint64_t *hob_base_out, uint64_t *hob_end_out)
{
    static uint64_t cached_hob_base;
    static uint64_t cached_hob_end;
    static uint32_t attempts;

    if (cached_hob_base) {
        uint64_t end = 0;
        int count = 0;
        if (ia64_fw_validate_efi_hob_list(cs, cached_hob_base, &end, &count)) {
            cached_hob_end = end;
            if (hob_base_out) {
                *hob_base_out = cached_hob_base;
            }
            if (hob_end_out) {
                *hob_end_out = cached_hob_end;
            }
            return true;
        }
        cached_hob_base = 0;
        cached_hob_end = 0;
    }
    if (attempts++ > 64) {
        return false;
    }
    if (!stack_phys) {
        return false;
    }

    const uint8_t sig[4] = { 'P', 'e', 'i', 'C' };
    const uint64_t scan_span = 256ULL << 10;
    const uint64_t scan_base =
        (stack_phys > (scan_span / 2)) ? (stack_phys - (scan_span / 2)) : 0;
    const uint64_t scan_len = scan_span;
    const size_t chunk = 64 * 1024;
    g_autofree uint8_t *buf = g_malloc(chunk);

    for (uint64_t off = 0; off < scan_len; off += chunk - 8) {
        uint64_t addr = scan_base + off;
        if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
            continue;
        }
        for (size_t j = 0; j + sizeof(sig) <= chunk; j++) {
            if (memcmp(&buf[j], sig, sizeof(sig)) != 0) {
                continue;
            }

            uint64_t base = addr + j;
            uint8_t hdr[16];
            if (cpu_memory_rw_debug(cs, base, hdr, sizeof(hdr), false) != 0) {
                continue;
            }
            uint64_t sig_val = ldq_le_p(&hdr[0]);
            if ((uint32_t)sig_val != 0x43696550u) {
                continue;
            }
            uint64_t ps_ptr = ldq_le_p(&hdr[8]);
            uint64_t ps_phys = ia64_phys_mode_addr(ps_ptr);
            bool ps_in_flash = ia64_fw_addr_in_flash(ps_phys);
            bool ps_in_workram = (ps_phys >= IA64_IPF_FW_WORKRAM_BASE &&
                                  ps_phys < IA64_IPF_FW_WORKRAM_BASE + IA64_IPF_FW_WORKRAM_SIZE);
            if (!ps_in_flash && !ps_in_workram) {
                continue;
            }

            const size_t scan_bytes = 16 * 1024;
            g_autofree uint8_t *core = g_malloc(scan_bytes);
            if (cpu_memory_rw_debug(cs, base, core, scan_bytes, false) != 0) {
                continue;
            }
            {
                static int peic_trace_enabled = -1;
                if (peic_trace_enabled == -1) {
                    const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP");
                    peic_trace_enabled = (s && *s) ? 1 : 0;
                }
                if (peic_trace_enabled) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: pei_core scan base=%016" PRIx64
                                  " ps_ptr=%016" PRIx64 " ps_phys=%016" PRIx64 "\n",
                                  base, ps_ptr, ps_phys);
                    ia64_fw_pei_log_core_hob_candidates(cs, base, ps_ptr);
                }
            }
            for (size_t core_off = 0; core_off + 8 <= scan_bytes; core_off += 8) {
                if (core_off != 0x260 && core_off != 0x470 &&
                    core_off != 0x478) {
                    continue;
                }
                uint64_t raw = ldq_le_p(core + core_off);
                if (!raw || raw == UINT64_MAX) {
                    continue;
                }
                uint64_t phys = ia64_phys_mode_addr(raw);
                uint64_t end = 0;
                int count = 0;
                if (ia64_fw_validate_efi_hob_list(cs, phys, &end, &count)) {
                    cached_hob_base = phys;
                    cached_hob_end = end;
                    if (hob_base_out) {
                        *hob_base_out = phys;
                    }
                    if (hob_end_out) {
                        *hob_end_out = end;
                    }
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: pei_core hob_list=%016" PRIx64
                                  " end=%016" PRIx64 " base=%016" PRIx64
                                  " off=0x%zx ps_ptr=%016" PRIx64
                                  " stack=%016" PRIx64 "\n",
                                  phys, end, base, core_off, ps_ptr, stack_phys);
                    return true;
                }
            }
            for (size_t k = 0; k + 8 <= scan_bytes; k += 8) {
                uint64_t v = ldq_le_p(core + k);
                if (!v) {
                    continue;
                }
                uint64_t phys = ia64_phys_mode_addr(v);
                uint64_t end = 0;
                int count = 0;
                if (ia64_fw_validate_efi_hob_list(cs, phys, &end, &count)) {
                    cached_hob_base = phys;
                    cached_hob_end = end;
                    if (hob_base_out) {
                        *hob_base_out = phys;
                    }
                    if (hob_end_out) {
                        *hob_end_out = end;
                    }
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: PEI core hob_list=%016" PRIx64
                                  " end=%016" PRIx64 " base=%016" PRIx64
                                  " src_off=0x%zx src_val=%016" PRIx64
                                  " ps_ptr=%016" PRIx64 " stack=%016" PRIx64 "\n",
                                  phys, end, base,
                                  k, v, ps_ptr, stack_phys);
                    return true;
                }
            }
        }
    }
    return false;
}

static bool ia64_fw_clone_hob_list_ram(CPUState *cs,
                                       uint64_t src_base, uint64_t src_end,
                                       uint64_t dst_base,
                                       uint64_t mem_bottom_phys,
                                       uint64_t mem_top_phys,
                                       uint64_t stack_phys)
{
    if (src_end <= src_base || dst_base == 0) {
        return false;
    }
    uint64_t list_len = src_end - src_base;
    if (list_len < 0x40 || list_len > (1ULL << 20)) {
        return false;
    }

    g_autofree uint8_t *buf = g_malloc((size_t)list_len);
    if (cpu_memory_rw_debug(cs, src_base, buf, (size_t)list_len, false) != 0) {
        return false;
    }
    if (lduw_le_p(&buf[0]) != 0x0001 || lduw_le_p(&buf[2]) != 0x0038) {
        return false;
    }

    uint8_t phit[0x38];
    memcpy(phit, buf, sizeof(phit));
    uint64_t mem_top_raw = ldq_le_p(&phit[16]);
    uint64_t mem_bottom_raw = ldq_le_p(&phit[24]);
    uint64_t free_top_raw = ldq_le_p(&phit[32]);
    uint64_t free_bottom_raw = ldq_le_p(&phit[40]);
    uint64_t end_hob_raw = ldq_le_p(&phit[48]);

    uint64_t new_end_hob = dst_base + list_len;
    uint64_t new_free_bottom = (new_end_hob + 0x1fULL) & ~0x1fULL;
    uint64_t new_free_top = mem_top_phys;

    if (stack_phys > mem_bottom_phys && stack_phys < mem_top_phys) {
        const uint64_t stack_guard = 1ULL << 20;
        if (stack_phys > mem_bottom_phys + stack_guard) {
            uint64_t stack_top = (stack_phys - stack_guard) & ~0xfffULL;
            if (stack_top < new_free_top) {
                new_free_top = stack_top;
            }
        }
    }
    if (new_free_bottom >= new_free_top) {
        new_free_bottom = mem_bottom_phys;
        new_free_top = mem_top_phys;
    }

    uint64_t mem_top_enc = ia64_fw_encode_addr(mem_top_raw, mem_top_phys);
    uint64_t mem_bottom_enc = ia64_fw_encode_addr(mem_bottom_raw, mem_bottom_phys);
    uint64_t free_top_tmpl = free_top_raw ? free_top_raw : mem_top_raw;
    uint64_t free_bottom_tmpl = free_bottom_raw ? free_bottom_raw : mem_bottom_raw;
    uint64_t end_hob_tmpl = end_hob_raw ? end_hob_raw : mem_bottom_raw;
    stq_le_p(&phit[16], mem_top_enc);
    stq_le_p(&phit[24], mem_bottom_enc);
    stq_le_p(&phit[32], ia64_fw_encode_addr(free_top_tmpl, new_free_top));
    stq_le_p(&phit[40], ia64_fw_encode_addr(free_bottom_tmpl, new_free_bottom));
    stq_le_p(&phit[48], ia64_fw_encode_addr(end_hob_tmpl, new_end_hob));

    cpu_physical_memory_write(dst_base, phit, sizeof(phit));
    cpu_physical_memory_write(dst_base + sizeof(phit),
                              buf + sizeof(phit),
                              (size_t)(list_len - sizeof(phit)));
    return true;
}

static bool ia64_fw_find_hob_list_in_range(CPUState *cs,
                                           uint64_t base, uint64_t len,
                                           uint64_t *hob_base_out,
                                           uint64_t *hob_end_out)
{
    const uint8_t phit_magic[8] = { 0x01, 0x00, 0x38, 0x00, 0, 0, 0, 0 };
    const size_t chunk = 64 * 1024;
    g_autofree uint8_t *buf = g_malloc(chunk);
    uint64_t best_base = 0;
    uint64_t best_end = 0;
    uint64_t best_span = 0;

    for (uint64_t off = 0; off < len; off += chunk - 8) {
        uint64_t addr = base + off;
        if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
            continue;
        }
        for (size_t j = 0; j + sizeof(phit_magic) <= chunk; j++) {
            if (buf[j] != 0x01) {
                continue;
            }
            if (memcmp(&buf[j], phit_magic, sizeof(phit_magic)) != 0) {
                continue;
            }
            uint64_t cand = addr + j;
            uint64_t end = 0;
            int count = 0;
            if (!ia64_fw_validate_efi_hob_list(cs, cand, &end, &count)) {
                continue;
            }
            uint64_t span = end - cand;
            if (!best_base || span > best_span) {
                best_base = cand;
                best_end = end;
                best_span = span;
            }
        }
    }

    if (!best_base) {
        return false;
    }
    if (hob_base_out) {
        *hob_base_out = best_base;
    }
    if (hob_end_out) {
        *hob_end_out = best_end;
    }
    return true;
}

static bool ia64_fw_dump_efi_hobs_impl(CPUState *cs, uint64_t stack_hint,
                                       bool force)
{
    /*
     * Best-effort EFI HOB list dump to diagnose early DXE ASSERTs.
     * The xenipf firmware typically places the HOB list in low RAM.
     */
    enum {
        EFI_HOB_TYPE_HANDOFF = 0x0001,
        EFI_HOB_TYPE_MEMORY_ALLOCATION = 0x0002,
        EFI_HOB_TYPE_RESOURCE_DESCRIPTOR = 0x0003,
        EFI_HOB_TYPE_GUID_EXTENSION = 0x0004,
        EFI_HOB_TYPE_FV = 0x0005,
        EFI_HOB_TYPE_CPU = 0x0006,
        EFI_HOB_TYPE_END_OF_HOB_LIST = 0xffff,
    };
    enum {
        EFI_RESOURCE_ATTRIBUTE_PRESENT = 0x00000001u,
        EFI_RESOURCE_ATTRIBUTE_INITIALIZED = 0x00000002u,
        EFI_RESOURCE_ATTRIBUTE_TESTED = 0x00000004u,
    };
    static bool dumped;
    static bool dumped_force;
    if (!force && dumped) {
        return true;
    }
    if (force && dumped_force) {
        return true;
    }

    const uint8_t phit_magic[8] = { 0x01, 0x00, 0x38, 0x00, 0, 0, 0, 0 };
    uint64_t flash_base = ia64_fw_flash_base();
    uint64_t flash_size = ia64_fw_flash_size();
    uint64_t stack_phys = ia64_phys_mode_addr(stack_hint);
    uint64_t candidates[] = {
        0x0000000002000000ULL, /* common xenipf PEI workspace */
        0x0000000000100000ULL,
        0x0000000001000000ULL,
        flash_base,
        IA64_IPF_FW_WORKRAM_BASE,
        stack_phys ? (stack_phys & ~0x00ffffffULL) : 0,
    };

    uint64_t hob_base = 0;
    uint64_t hob_end = 0;
    uint64_t hob_best_span = 0;
    bool hob_best_end_ok = false;
    int hob_best_count = 0;
    bool hob_from_pei = false;
    uint8_t hdr[8];
    if (stack_phys &&
        ia64_fw_find_pei_hob_list(cs, stack_phys, &hob_base, &hob_end)) {
        hob_best_span = hob_end - hob_base;
        hob_best_end_ok = true;
        hob_from_pei = true;
        (void)ia64_fw_validate_efi_hob_list(cs, hob_base, NULL, &hob_best_count);
    }
    if (!hob_from_pei) {
        for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
            uint64_t addr = candidates[i];
            if (!addr) {
                continue;
            }
            if (cpu_memory_rw_debug(cs, addr, hdr, sizeof(hdr), false) != 0 ||
                memcmp(hdr, phit_magic, sizeof(phit_magic)) != 0) {
                continue;
            }

            uint8_t phit[0x38];
            if (cpu_memory_rw_debug(cs, addr, phit, sizeof(phit), false) != 0) {
                continue;
            }
            uint64_t mem_top = ldq_le_p(&phit[16]);
            uint64_t mem_bottom = ldq_le_p(&phit[24]);
            if (mem_top <= mem_bottom) {
                continue;
            }

            uint64_t end;
            int count;
            if (!ia64_fw_validate_efi_hob_list(cs, addr, &end, &count)) {
                continue;
            }
            uint64_t span = end - addr;
            uint64_t phit_end = ia64_phys_mode_addr(ldq_le_p(&phit[48]));
            bool end_ok = (phit_end >= addr && phit_end <= end);
            if (!hob_base ||
                (end_ok && !hob_best_end_ok) ||
                (end_ok == hob_best_end_ok && span > hob_best_span)) {
                hob_best_span = span;
                hob_best_count = count;
                hob_base = addr;
                hob_end = end;
                hob_best_end_ok = end_ok;
            }
        }
    }

    if (!hob_base || !hob_best_end_ok) {
        uint64_t scan_ranges[][2] = {
            { 0, 64ULL << 20 },
            { flash_base, flash_size },
            { 0, 0 },
        };
        if (stack_phys > (32ULL << 20)) {
            scan_ranges[1][0] = stack_phys - (32ULL << 20);
            scan_ranges[1][1] = 64ULL << 20;
        }

        const size_t chunk = 64 * 1024;
        g_autofree uint8_t *buf = g_malloc(chunk);
        for (size_t r = 0; r < ARRAY_SIZE(scan_ranges); r++) {
            uint64_t scan_base = scan_ranges[r][0];
            uint64_t scan_len = scan_ranges[r][1];
            if (!scan_len) {
                continue;
            }
            for (uint64_t off = 0; off < scan_len; off += chunk - 8) {
                uint64_t addr = scan_base + off;
                if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
                    continue;
                }
                for (size_t j = 0; j + sizeof(phit_magic) <= chunk; j++) {
                    if (buf[j] != 0x01) {
                        continue;
                    }
                    if (memcmp(&buf[j], phit_magic, sizeof(phit_magic)) != 0) {
                        continue;
                    }
                    uint64_t cand = addr + j;
                    uint8_t phit[0x38];
                    if (cpu_memory_rw_debug(cs, cand, phit, sizeof(phit), false) != 0) {
                        continue;
                    }
                    uint64_t mem_top = ldq_le_p(&phit[16]);
                    uint64_t mem_bottom = ldq_le_p(&phit[24]);
                    if (mem_top <= mem_bottom) {
                        continue;
                    }

                    uint64_t end;
                    int count;
                    if (!ia64_fw_validate_efi_hob_list(cs, cand, &end, &count)) {
                        continue;
                    }
                    uint64_t span = end - cand;
                    uint64_t phit_end = ia64_phys_mode_addr(ldq_le_p(&phit[48]));
                    bool end_ok = (phit_end >= cand && phit_end <= end);
                    if (!hob_base ||
                        (end_ok && !hob_best_end_ok) ||
                        (end_ok == hob_best_end_ok && span > hob_best_span)) {
                        hob_best_span = span;
                        hob_best_count = count;
                        hob_base = cand;
                        hob_end = end;
                        hob_best_end_ok = end_ok;
                    }
                }
            }
        }
    }

    if (!hob_base) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: efi_hob_dump: PHIT HOB not found\n");
        return false;
    }

    uint8_t phit[0x38];
    if (cpu_memory_rw_debug(cs, hob_base, phit, sizeof(phit), false) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: efi_hob_dump: PHIT read failed addr=%016" PRIx64 "\n",
                      hob_base);
        return false;
    }

    uint64_t mem_bottom = ldq_le_p(&phit[24]);
    uint64_t mem_bottom_phys_init = ia64_phys_mode_addr(mem_bottom);
    if (mem_bottom_phys_init && mem_bottom_phys_init != hob_base) {
        uint64_t alt_end;
        int alt_count;
        if (ia64_fw_validate_efi_hob_list(cs, mem_bottom_phys_init,
                                          &alt_end, &alt_count)) {
            uint64_t alt_span = alt_end - mem_bottom_phys_init;
            if (alt_span > hob_best_span) {
                hob_base = mem_bottom_phys_init;
                hob_end = alt_end;
                hob_best_span = alt_span;
                hob_best_count = alt_count;
                if (cpu_memory_rw_debug(cs, hob_base, phit, sizeof(phit), false) != 0) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: efi_hob_dump: PHIT read failed addr=%016" PRIx64 "\n",
                                  hob_base);
                    return false;
                }
            }
        }
    }

    uint32_t version = ldl_le_p(&phit[8]);
    uint32_t boot_mode = ldl_le_p(&phit[12]);
    uint64_t mem_top = ldq_le_p(&phit[16]);
    mem_bottom = ldq_le_p(&phit[24]);
    uint64_t free_top = ldq_le_p(&phit[32]);
    uint64_t free_bottom = ldq_le_p(&phit[40]);
    uint64_t end_hob = ldq_le_p(&phit[48]);
    uint64_t end_hob_phys = ia64_phys_mode_addr(end_hob);
    uint64_t mem_top_phys = ia64_phys_mode_addr(mem_top);
    uint64_t mem_bottom_phys = ia64_phys_mode_addr(mem_bottom);
    uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
    uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: efi_hob_dump: base=%016" PRIx64
                  " version=%u boot_mode=%u"
                  " mem=[%016" PRIx64 "..%016" PRIx64 "]"
                  " free=[%016" PRIx64 "..%016" PRIx64 "]"
                  " end=%016" PRIx64 " list_end=%016" PRIx64
                  " span=0x%" PRIx64 " hobs=%d\n",
                  hob_base, version, boot_mode,
                  mem_bottom, mem_top, free_bottom, free_top,
                  end_hob_phys, hob_end, hob_best_span, hob_best_count);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: efi_hob_dump: phys_mem=[%016" PRIx64 "..%016" PRIx64 "]"
                  " phys_free=[%016" PRIx64 "..%016" PRIx64 "] phys_end=%016" PRIx64 "\n",
                  mem_bottom_phys, mem_top_phys,
                  free_bottom_phys, free_top_phys,
                  end_hob_phys);

    {
        IA64CPU *cpu = IA64_CPU(cs);
        CPUIA64State *env = &cpu->env;
        env->fw_phit_mem_bottom = mem_bottom;
        env->fw_phit_mem_top = mem_top;
        env->fw_phit_free_bottom = free_bottom;
        env->fw_phit_free_top = free_top;
    }

    uint64_t cur = hob_base;
    for (int iter = 0; iter < 4096; iter++) {
        uint8_t h[8];
        if (cpu_memory_rw_debug(cs, cur, h, sizeof(h), false) != 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: efi_hob_dump: header read failed addr=%016" PRIx64 "\n",
                          cur);
            break;
        }
        uint16_t type = lduw_le_p(&h[0]);
        uint16_t len = lduw_le_p(&h[2]);
        if (len < sizeof(h)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: efi_hob_dump: bad hob len=%u type=%u addr=%016" PRIx64 "\n",
                          len, type, cur);
            break;
        }
        if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: efi_hob_dump: end_hob addr=%016" PRIx64 "\n",
                          cur);
            break;
        }

        if (type == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && len >= 0x30) {
            uint8_t rh[0x30];
            if (cpu_memory_rw_debug(cs, cur, rh, sizeof(rh), false) == 0) {
                uint32_t rtype = ldl_le_p(&rh[24]);
                uint32_t rattr = ldl_le_p(&rh[28]);
                uint64_t start = ldq_le_p(&rh[32]);
                uint64_t rlen = ldq_le_p(&rh[40]);
                bool tested = (rattr & (EFI_RESOURCE_ATTRIBUTE_PRESENT |
                                        EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                                        EFI_RESOURCE_ATTRIBUTE_TESTED)) ==
                              (EFI_RESOURCE_ATTRIBUTE_PRESENT |
                               EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                               EFI_RESOURCE_ATTRIBUTE_TESTED);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: efi_hob_dump: RES type=%u attr=0x%08x tested=%d start=%016" PRIx64
                              " len=%016" PRIx64 "\n",
                              rtype, rattr, tested ? 1 : 0, start, rlen);
            }
        }
        if (type == EFI_HOB_TYPE_MEMORY_ALLOCATION && len >= 0x30) {
            uint8_t mh[0x30];
            if (cpu_memory_rw_debug(cs, cur, mh, sizeof(mh), false) == 0) {
                uint32_t guid0 = ldl_le_p(&mh[8]);
                uint16_t guid1 = lduw_le_p(&mh[12]);
                uint16_t guid2 = lduw_le_p(&mh[14]);
                const uint8_t *g = &mh[16];
                uint64_t base = ldq_le_p(&mh[24]);
                uint64_t mlen = ldq_le_p(&mh[32]);
                uint32_t mtype = ldl_le_p(&mh[40]);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: efi_hob_dump: ALLOC memtype=%u base=%016" PRIx64
                              " len=%016" PRIx64
                              " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                              mtype, base, mlen,
                              guid0, guid1, guid2,
                              g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
            }
        }
        if (type == EFI_HOB_TYPE_FV && len >= 0x18) {
            uint8_t fv[0x18];
            if (cpu_memory_rw_debug(cs, cur, fv, sizeof(fv), false) == 0) {
                uint64_t base = ldq_le_p(&fv[8]);
                uint64_t flen = ldq_le_p(&fv[16]);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: efi_hob_dump: FV base=%016" PRIx64 " len=%016" PRIx64 "\n",
                              base, flen);
            }
        }
        if (type == EFI_HOB_TYPE_CPU && len >= 0x10) {
            uint8_t cpu_h[0x10];
            if (cpu_memory_rw_debug(cs, cur, cpu_h, sizeof(cpu_h), false) == 0) {
                uint8_t mem_bits = cpu_h[8];
                uint8_t io_bits = cpu_h[9];
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: efi_hob_dump: CPU mem_bits=%u io_bits=%u\n",
                              mem_bits, io_bits);
            }
        }
        if (type == EFI_HOB_TYPE_GUID_EXTENSION && len >= 0x18) {
            uint8_t gh[0x18];
            if (cpu_memory_rw_debug(cs, cur, gh, sizeof(gh), false) == 0) {
                uint32_t guid0 = ldl_le_p(&gh[8]);
                uint16_t guid1 = lduw_le_p(&gh[12]);
                uint16_t guid2 = lduw_le_p(&gh[14]);
                const uint8_t *g = &gh[16];
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: efi_hob_dump: GUIDEXT len=%u guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                              len,
                              guid0, guid1, guid2,
                              g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
                {
                    IA64EfiGuid guid;
                    ia64_fw_guid_from_bytes(&gh[8], &guid);
                    if (ia64_fw_guid_equal(&guid, &ia64_efi_guid_flashmap_hob) &&
                        len >= 0x18 + 0x40) {
                        uint8_t data[0x40];
                        if (cpu_memory_rw_debug(cs, cur + 0x18, data,
                                                sizeof(data), false) == 0) {
                            uint8_t area_type = data[3];
                            uint64_t base = ldq_le_p(&data[32]);
                            uint64_t hlen = ldq_le_p(&data[40]);
                            uint32_t attrs = ldl_le_p(&data[24]);
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: efi_hob_dump: FLASHMAP area=0x%02x base=%016" PRIx64
                                          " len=%016" PRIx64 " attrs=0x%08x\n",
                                          area_type, base, hlen, attrs);
                        }
                    }
                }
            }
        }

        cur += len;
        if (hob_end && cur >= hob_end) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: efi_hob_dump: reached hob_end cur=%016" PRIx64 "\n",
                          cur);
            break;
        }
        if (cur - hob_base > (16ULL << 20)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: efi_hob_dump: abort, list too long\n");
            break;
        }
    }
    if (hob_best_end_ok) {
        if (force) {
            dumped_force = true;
        } else {
            dumped = true;
        }
        return true;
    }
    return false;
}

static bool ia64_fw_dump_efi_hobs(CPUState *cs, uint64_t stack_hint)
{
    return ia64_fw_dump_efi_hobs_impl(cs, stack_hint, false);
}

static bool ia64_fw_dump_efi_hobs_force(CPUState *cs, uint64_t stack_hint)
{
    return ia64_fw_dump_efi_hobs_impl(cs, stack_hint, true);
}

#ifndef CONFIG_USER_ONLY
static void ia64_fw_dump_gcd_map_candidates(CPUIA64State *env);

void ia64_fw_dump_hobs_and_gcd(CPUIA64State *env)
{
    if (!env) {
        return;
    }
    CPUState *cs = env_cpu(env);
    hwaddr stack_phys = ia64_phys_mode_addr(env->r[12]);
    if (stack_phys) {
        (void)ia64_fw_dump_efi_hobs_force(cs, stack_phys);
    }
    ia64_fw_dump_gcd_map_candidates(env);
}

static void ia64_fw_dump_gcd_map_candidates(CPUIA64State *env)
{
    /*
     * Best-effort post-mortem helper for early DXE ASSERTs in GCD init.
     *
     * The DXE core allocates EFI_GCD_MAP_ENTRY records from the first pool
     * region carved above PHIT->EfiMemoryTop. Scan that region for the
     * EFI_GCD_MAP_SIGNATURE ("gcdm") and print any plausible entries so we can
     * distinguish signature corruption from bad list links/addresses.
     */
    CPUState *cs = env_cpu(env);
    uint64_t mem_bottom = env->fw_phit_mem_bottom;
    uint64_t mem_top = env->fw_phit_mem_top;
    uint64_t free_bottom = env->fw_phit_free_bottom;
    uint64_t free_top = env->fw_phit_free_top;
    if (!mem_top || mem_top < mem_bottom) {
        return;
    }

    /*
     * Scan from the bottom of RAM up through a small window past EfiMemoryTop.
     * While DXE pool allocations are expected to come from above EfiMemoryTop,
     * some builds place early allocations (or temporary copies) below it.
     */
    const uint64_t scan_start = mem_bottom;
    const uint64_t scan_len = 64ULL << 20;
    uint64_t scan_end = mem_top + (16ULL << 20);
    if (scan_end < scan_start) {
        return;
    }
    if (scan_end - scan_start > scan_len) {
        scan_end = scan_start + scan_len;
    }
    const uint32_t sig32 = 0x6d646367U; /* EFI_GCD_MAP_SIGNATURE */
    int found = 0;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: gcd_scan phit mem=[%016" PRIx64 "..%016" PRIx64 "]"
                  " free=[%016" PRIx64 "..%016" PRIx64 "]"
                  " scan=[%016" PRIx64 "..%016" PRIx64 "]\n",
                  mem_bottom, mem_top, free_bottom, free_top,
                  scan_start, scan_end);

    uint8_t buf[4096];
    for (uint64_t base = scan_start;
         base < scan_end && found < 32;
         base += sizeof(buf) - 3) {
        if (cpu_memory_rw_debug(cs, base, buf, sizeof(buf), false) != 0) {
            break;
        }
        for (size_t i = 0; i + 4 <= sizeof(buf) && found < 32; i++) {
            if (ldl_le_p(&buf[i]) != sig32) {
                continue;
            }
            uint64_t addr = base + i;
            uint8_t rec[64];
            if (cpu_memory_rw_debug(cs, addr, rec, sizeof(rec), false) != 0) {
                continue;
            }
            uint64_t sig = ldq_le_p(&rec[0]);
            uint64_t fwd = ldq_le_p(&rec[8]);
            uint64_t back = ldq_le_p(&rec[16]);
            uint64_t range_base = ldq_le_p(&rec[24]);
            uint64_t range_end = ldq_le_p(&rec[32]);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: gcd_map_entry addr=%016" PRIx64
                          " sig=%016" PRIx64
                          " link=[%016" PRIx64 ",%016" PRIx64 "]"
                          " range=[%016" PRIx64 "..%016" PRIx64 "]\n",
                          addr, sig, fwd, back, range_base, range_end);
            found++;
        }
    }

    if (found == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: gcd_scan: no EFI_GCD_MAP_SIGNATURE matches\n");
    }
}
#endif /* !CONFIG_USER_ONLY */

/*
 * EDK1-style status code extended data parsing helper.
 *
 * Some IA64 firmware stacks pass a small indirection record in the break(0)
 * buffer (e.g. {line,u64 ptr,u64 ptr}) where the pointers refer to a larger
 * pool structure that contains one or more EFI_STATUS_CODE_DATA records
 * (ASSERT/DEBUG/STRING/etc). Scan the pointed-to region for embedded
 * EFI_STATUS_CODE_DATA records and decode ASSERT records inline so we can
 * identify the failing source/condition.
 */
static void ia64_fw_break0_scan_status_records_impl(CPUState *cpu,
                                                    uint64_t scan_ptr,
                                                    size_t scan_window,
                                                    int depth,
                                                    bool *out_found_assert,
                                                    uint64_t *seen_ptrs,
                                                    size_t *num_seen_ptrs)
{
    if (*out_found_assert) {
        return;
    }
    if (depth > 2) {
        return;
    }
    if (*num_seen_ptrs >= 64) {
        return;
    }
    for (size_t i = 0; i < *num_seen_ptrs; i++) {
        if (seen_ptrs[i] == scan_ptr) {
            return;
        }
    }
    seen_ptrs[(*num_seen_ptrs)++] = scan_ptr;

    if (scan_ptr < 0x1000 || scan_window < 0x40) {
        return;
    }
    if (scan_window > 65536) {
        scan_window = 65536;
    }

    /* EFI_STATUS_CODE_DATA_TYPE_ASSERT_GUID */
    static const uint8_t assert_guid[16] = {
        0x95, 0x15, 0x57, 0xda, 0x99, 0x4d, 0x7c, 0x48,
        0x82, 0x7c, 0x26, 0x22, 0x67, 0x7d, 0x33, 0x07,
    };
    /* EFI_STATUS_CODE_DATA_TYPE_DEBUG_GUID */
    static const uint8_t debug_guid[16] = {
        0x46, 0x92, 0x4e, 0x9a, 0x53, 0xd5, 0xd5, 0x11,
        0x87, 0xe2, 0x00, 0x06, 0x29, 0x45, 0xc3, 0xb9,
    };
    /* EFI_STATUS_CODE_DATA_TYPE_STRING_GUID */
    static const uint8_t string_guid[16] = {
        0x80, 0x10, 0xd1, 0x92, 0x6f, 0x49, 0x95, 0x4d,
        0xbe, 0x7e, 0x03, 0x74, 0x88, 0x38, 0x2b, 0x0a,
    };
    /* EFI_STATUS_CODE_SPECIFIC_DATA_GUID */
    static const uint8_t specific_guid[16] = {
        0xbd, 0x84, 0x59, 0x33, 0x05, 0xe8, 0x9a, 0x40,
        0xb8, 0xf8, 0xd2, 0x7e, 0xce, 0x5f, 0xf7, 0xa6,
    };

    /* Clamp and align the window; scan around the pointer. */
    uint64_t start = scan_ptr;
    if (start > (scan_window / 4)) {
        start -= (scan_window / 4);
    } else {
        start = 0;
    }
    start &= ~0x7ULL;

    g_autofree uint8_t *buf = g_malloc0(scan_window);
    if (!ia64_fw_read_bytes_any(cpu, start, buf, scan_window)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_break0_scan: read failed ptr=%016" PRIx64
                      " start=%016" PRIx64 " len=%zu\n",
                      scan_ptr, start, scan_window);
        return;
    }

    int printed = 0;
    for (size_t off = 0; off + 20 <= scan_window && printed < 32; off++) {
        uint16_t hdr = lduw_le_p(&buf[off + 0]);
        uint16_t size = lduw_le_p(&buf[off + 2]);
        size_t total = (size_t)hdr + (size_t)size;
        if (hdr < 20 || hdr > 64) {
            continue;
        }
        if (total < hdr || total > (scan_window - off)) {
            continue;
        }

        const uint8_t *guid = &buf[off + 4];
        bool is_assert_rec = (memcmp(guid, assert_guid, 16) == 0);
        bool is_debug_rec = (memcmp(guid, debug_guid, 16) == 0);
        bool is_string_rec = (memcmp(guid, string_guid, 16) == 0);
        bool is_specific_rec = (memcmp(guid, specific_guid, 16) == 0);
        if (!is_assert_rec && !is_debug_rec && !is_string_rec &&
            !is_specific_rec) {
            continue;
        }

        uint64_t rec_addr = start + off;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_break0_scan: rec addr=%016" PRIx64
                      " hdr=%u size=%u type=%s depth=%d\n",
                      rec_addr, hdr, size,
                      is_assert_rec ? "ASSERT" :
                      is_debug_rec ? "DEBUG" :
                      is_string_rec ? "STRING" : "SPECIFIC",
                      depth);

        if (is_assert_rec && total >= hdr + 4 + 2) {
            size_t base = off + hdr;
            uint32_t line = ldl_le_p(&buf[base]);
            const char *file = (const char *)&buf[base + 4];
            size_t file_max = (off + total) - (base + 4);
            size_t file_len = strnlen(file, file_max);
            size_t expr_off = base + 4 + file_len + 1;

            const char *expr = NULL;
            size_t expr_len = 0;
            if (file_len < file_max && expr_off < off + total) {
                expr = (const char *)&buf[expr_off];
                expr_len = strnlen(expr, (off + total) - expr_off);
            }

            if (file_len && expr && expr_len) {
                char fn[256], ex[256];
                size_t nfn = MIN(file_len, sizeof(fn) - 1);
                size_t nex = MIN(expr_len, sizeof(ex) - 1);
                memcpy(fn, file, nfn);
                fn[nfn] = '\0';
                memcpy(ex, expr, nex);
                ex[nex] = '\0';
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_break0_scan: ASSERT line=%u file=\"%s\" expr=\"%s\"\n",
                              line, fn, ex);
                *out_found_assert = true;
                return;
            }
        }

        printed++;
    }

    /* Opportunistically print filename-like ASCII strings in this window. */
    {
        int str_printed = 0;
        for (size_t i = 0; i + 8 < scan_window && str_printed < 6; i++) {
            unsigned char c = buf[i];
            if (!(c >= 0x20 && c < 0x7f)) {
                continue;
            }
            size_t j = i;
            while (j < scan_window) {
                unsigned char cj = buf[j];
                if (!(cj >= 0x20 && cj < 0x7f)) {
                    break;
                }
                j++;
            }
            size_t len = j - i;
            if (len < 12) {
                i = j;
                continue;
            }
            size_t n = MIN(len, 200);
            char tmp[201];
            memcpy(tmp, &buf[i], n);
            tmp[n] = '\0';
            if (strstr(tmp, ".c") || strstr(tmp, ".h") || strstr(tmp, "ASSERT") ||
                strstr(tmp, "Dxe") || strstr(tmp, "DXE") ||
                strstr(tmp, "Sal") || strstr(tmp, "SAL") ||
                strstr(tmp, "Protocol") || strstr(tmp, "EFI_")) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_break0_scan: str addr=%016" PRIx64 " +0x%zx \"%s\"\n",
                              start, i, tmp);
                str_printed++;

                if (strstr(tmp, "ASSERT!Status") || strstr(tmp, "Info")) {
                    /* Print the next adjacent ASCII string (often carries the "Info:" text). */
                    size_t k = i + len;
                    while (k < scan_window && buf[k] != '\0') {
                        k++;
                    }
                    if (k < scan_window) {
                        k++; /* skip NUL */
                    }
                    while (k < scan_window) {
                        unsigned char ck = buf[k];
                        if (ck >= 0x20 && ck < 0x7f) {
                            break;
                        }
                        k++;
                    }
                    size_t ks = k;
                    while (k < scan_window) {
                        unsigned char ck = buf[k];
                        if (!(ck >= 0x20 && ck < 0x7f)) {
                            break;
                        }
                        k++;
                    }
                    size_t klen = k - ks;
                    if (klen >= 8) {
                        size_t kn = MIN(klen, 200);
                        char next[201];
                        memcpy(next, &buf[ks], kn);
                        next[kn] = '\0';
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: fw_break0_scan: str2 addr=%016" PRIx64 " +0x%zx \"%s\"\n",
                                      start, ks, next);
                    }
                }
            }
            i = j;
        }
    }

    if (!*out_found_assert && depth < 2) {
        const size_t max_qwords = MIN(scan_window / 8, 256);
        uint64_t start_phys = ia64_phys_mode_addr(start);
        int follow_limit = (depth == 0) ? 16 : 8;
        int followed = 0;
        for (size_t qi = 0; qi < max_qwords && !*out_found_assert; qi++) {
            uint64_t p = ldq_le_p(&buf[qi * 8]);
            uint64_t phys = ia64_phys_mode_addr(p);
            if (p < 0x1000) {
                continue;
            }
            if (phys < 0x1000 || phys >= (1ULL << 32)) {
                continue;
            }
            /* Skip pointers that fall inside the window we already scanned. */
            if (phys >= start_phys && phys < start_phys + scan_window) {
                continue;
            }
            /* Heuristic: prefer pool-ish RAM and firmware windows. */
            if (!((phys >= 0x1e000000ULL && phys < 0x30000000ULL) ||
                  (phys >= 0xff000000ULL && phys < 0x100000000ULL))) {
                continue;
            }
            if (followed++ >= follow_limit) {
                break;
            }
            if (depth == 0) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_break0_scan: follow ptr=%016" PRIx64
                              " from=%016" PRIx64 " +0x%zx\n",
                              p, start, (size_t)qi * 8);
            }
            ia64_fw_break0_scan_status_records_impl(cpu, p, 8192, depth + 1,
                                                    out_found_assert, seen_ptrs,
                                                    num_seen_ptrs);
        }
    }
}

static void ia64_fw_break0_scan_status_records(CPUState *cs,
                                               uint64_t ptr,
                                               size_t window)
{
    bool found_assert = false;
    uint64_t seen_ptrs[64] = { 0 };
    size_t num_seen_ptrs = 0;
    ia64_fw_break0_scan_status_records_impl(cs, ptr, window, 0, &found_assert,
                                            seen_ptrs, &num_seen_ptrs);
}

static bool ia64_fw_statuscode_try_ptrs(CPUState *cs, uint32_t line,
                                        uint64_t file_ptr, uint64_t desc_ptr)
{
    if (file_ptr < 0x1000 || desc_ptr < 0x1000) {
        return false;
    }

    char fn[256] = { 0 };
    char expr[256] = { 0 };
    const char *fn_enc = NULL;
    const char *expr_enc = NULL;

    if (ia64_fw_read_ascii_string(cs, file_ptr, fn, sizeof(fn)) != 0) {
        fn_enc = "ascii";
    } else if (ia64_fw_read_ucs2le_string(cs, file_ptr, fn, sizeof(fn)) != 0) {
        fn_enc = "ucs2";
    }

    if (ia64_fw_read_ascii_string(cs, desc_ptr, expr, sizeof(expr)) != 0) {
        expr_enc = "ascii";
    } else if (ia64_fw_read_ucs2le_string(cs, desc_ptr, expr, sizeof(expr)) != 0) {
        expr_enc = "ucs2";
    }

    if (fn_enc && expr_enc) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: statuscode assert line=%u file=\"%s\" expr=\"%s\"\n",
                      line, fn, expr);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: statuscode assert enc file=%s expr=%s\n",
                      fn_enc, expr_enc);
        return true;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: statuscode assert_ptr line=%u file_ptr=%016" PRIx64
                  " desc_ptr=%016" PRIx64 "\n",
                  line, file_ptr, desc_ptr);
    return false;
}

static bool ia64_fw_statuscode_try_string_data(CPUState *cs, uint32_t line,
                                               uint64_t data_ptr)
{
    uint8_t hdr[32];
    if (!ia64_fw_read_bytes_any(cs, data_ptr, hdr, sizeof(hdr))) {
        return false;
    }

    uint16_t header_size = lduw_le_p(&hdr[0]);
    uint16_t size = lduw_le_p(&hdr[2]);
    IA64EfiGuid guid;
    ia64_fw_guid_from_bytes(&hdr[4], &guid);
    if (!ia64_fw_guid_equal(&guid, &ia64_efi_guid_status_string)) {
        return false;
    }
    if (header_size < 20 || header_size > 64 ||
        (size_t)header_size + (size_t)size < header_size) {
        return false;
    }

    uint32_t string_type = ldl_le_p(&hdr[20]);
    uint64_t string_ptr = ldq_le_p(&hdr[24]);
    if (!string_ptr) {
        return false;
    }

    char fn[256] = { 0 };
    const char *enc = NULL;
    if (string_type == 0) {
        if (ia64_fw_read_ascii_string(cs, string_ptr, fn, sizeof(fn)) != 0) {
            enc = "ascii";
        }
    } else if (string_type == 1) {
        if (ia64_fw_read_ucs2le_string(cs, string_ptr, fn, sizeof(fn)) != 0) {
            enc = "ucs2";
        }
    }

    if (enc) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: statuscode assert line=%u file=\"%s\" (%s)\n",
                      line, fn, enc);
        return true;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: statuscode assert_str ptr=%016" PRIx64
                  " type=%u\n",
                  data_ptr, string_type);
    return false;
}

static bool ia64_fw_statuscode_try_debug_data(CPUState *cs, const uint8_t *buf,
                                              size_t read_len, size_t base)
{
    if (read_len < base + 16 + 24) {
        return false;
    }

    IA64EfiGuid inner;
    ia64_fw_guid_from_bytes(&buf[base], &inner);
    if (!ia64_fw_guid_equal(&inner, &ia64_efi_guid_status_debug)) {
        return false;
    }

    uint32_t error_level = ldl_le_p(&buf[base + 16]);
    uint64_t arg0 = ldq_le_p(&buf[base + 20]);
    uint64_t arg1 = ldq_le_p(&buf[base + 28]);
    uint32_t line = (uint32_t)arg0;

    if (arg1 >= 0x1000) {
        char fn[256] = { 0 };
        if (ia64_fw_read_ascii_string(cs, arg1, fn, sizeof(fn)) != 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: statuscode assert line=%u file=\"%s\""
                          " (debug level=0x%x)\n",
                          line, fn, error_level);
            return true;
        }
    }

    /* Fall back to scanning for an inline ASCII file path. */
    const uint8_t *scan = &buf[base + 20];
    size_t scan_len = read_len - (base + 20);
    size_t best_len = 0;
    const char *best = NULL;
    size_t cur_len = 0;
    for (size_t i = 0; i < scan_len; i++) {
        uint8_t c = scan[i];
        if (c >= 0x20 && c < 0x7f) {
            cur_len++;
            if (cur_len >= 8 && cur_len > best_len) {
                best_len = cur_len;
                best = (const char *)&scan[i + 1 - cur_len];
            }
        } else {
            cur_len = 0;
        }
    }

    if (best && best_len >= 8) {
        char fn[256] = { 0 };
        size_t n = MIN(best_len, sizeof(fn) - 1);
        memcpy(fn, best, n);
        fn[n] = '\0';
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: statuscode assert line=%u file=\"%s\""
                      " (debug level=0x%x)\n",
                      line, fn, error_level);
        return true;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: statuscode assert_debug line=%u error=0x%x"
                  " arg0=%016" PRIx64 " arg1=%016" PRIx64 "\n",
                  line, error_level, arg0, arg1);
    return false;
}

static void ia64_fw_statuscode_dump(CPUIA64State *env, uint64_t data_ptr,
                                    uint64_t code_type, uint64_t value)
{
    CPUState *cs = env_cpu(env);
    uint8_t hdr[20];
    if (!ia64_fw_read_bytes_any(cs, data_ptr, hdr, sizeof(hdr))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: statuscode read failed data=%016" PRIx64 "\n",
                      data_ptr);
        return;
    }

    uint16_t header_size = lduw_le_p(&hdr[0]);
    uint16_t data_size = lduw_le_p(&hdr[2]);
    IA64EfiGuid guid;
    ia64_fw_guid_from_bytes(&hdr[4], &guid);
    size_t total = (size_t)header_size + (size_t)data_size;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: statuscode data=%016" PRIx64 " type=%08" PRIx64
                  " value=%08" PRIx64 " hdr=%u size=%u guid=%08x-%04x-%04x"
                  "-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                  data_ptr, code_type, value, header_size, data_size,
                  guid.data1, guid.data2, guid.data3,
                  guid.data4[0], guid.data4[1], guid.data4[2], guid.data4[3],
                  guid.data4[4], guid.data4[5], guid.data4[6], guid.data4[7]);

    if (header_size < 20 || header_size > 64 || total < header_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: statuscode header invalid data=%016" PRIx64
                      " hdr=%u size=%u\n",
                      data_ptr, header_size, data_size);
        return;
    }

    size_t read_len = total;
    if (read_len > 4096) {
        read_len = 4096;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: statuscode clamp data=%016" PRIx64
                      " total=%zu -> %zu\n",
                      data_ptr, total, read_len);
    }

    g_autofree uint8_t *buf = g_malloc0(read_len);
    if (!ia64_fw_read_bytes_any(cs, data_ptr, buf, read_len)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: statuscode payload read failed data=%016" PRIx64
                      " len=%zu\n",
                      data_ptr, read_len);
        return;
    }

    static int status_dump_enabled = -1;
    if (status_dump_enabled == -1) {
        status_dump_enabled =
            getenv("QEMU_IA64_TRACE_CALL_STATUS_DUMP") ? 1 : 0;
    }
    if (status_dump_enabled) {
        g_mkdir_with_parents("scratch/ia64_logs", 0755);
        char path[256];
        snprintf(path, sizeof(path),
                 "scratch/ia64_logs/statuscode_%016" PRIx64 ".bin", data_ptr);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            fwrite(buf, 1, read_len, fp);
            fclose(fp);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: statuscode dump data=%016" PRIx64
                          " bytes=%zu file=%s\n",
                          data_ptr, read_len, path);
        }
    }

    if (!ia64_fw_guid_equal(&guid, &ia64_efi_guid_status_assert)) {
        return;
    }

    size_t base = header_size;
    if (read_len < base + 4) {
        return;
    }

    uint32_t line = ldl_le_p(&buf[base]);
    bool logged = false;

    if (!logged) {
        logged = ia64_fw_statuscode_try_debug_data(cs, buf, read_len, base);
    }

    if (!logged && read_len >= base + 4 + 12) {
        uint32_t file_size = ldl_le_p(&buf[base + 4]);
        uint64_t file_ptr = ldq_le_p(&buf[base + 8]);
        if (file_ptr) {
            logged = ia64_fw_statuscode_try_string_data(cs, line, file_ptr);
        }
        if (!logged && file_size > 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: statuscode assert_ptr2 line=%u file_size=%u\n",
                          line, file_size);
        }
    }

    if (!logged && read_len >= base + 4 + 16) {
        uint64_t file_ptr = ldq_le_p(&buf[base + 4]);
        uint64_t desc_ptr = ldq_le_p(&buf[base + 12]);
        if (file_ptr || desc_ptr) {
            logged = ia64_fw_statuscode_try_ptrs(cs, line, file_ptr, desc_ptr);
        }
    }

    if (!logged) {
        const size_t offs[] = { 4, 16 };
        for (size_t oi = 0; oi < ARRAY_SIZE(offs); oi++) {
            size_t off = offs[oi];
            if (read_len < base + off + 1) {
                continue;
            }
            const char *file = (const char *)&buf[base + off];
            size_t file_max = read_len - (base + off);
            size_t file_len = strnlen(file, file_max);
            size_t desc_off = base + off + file_len + 1;

            const char *desc = NULL;
            size_t desc_len = 0;
            if (file_len < file_max && desc_off < read_len) {
                desc = (const char *)&buf[desc_off];
                desc_len = strnlen(desc, read_len - desc_off);
            }

            if (file_len && desc && desc_len) {
                char fn[256] = { 0 };
                char expr[256] = { 0 };
                size_t nfn = MIN(file_len, sizeof(fn) - 1);
                size_t nexpr = MIN(desc_len, sizeof(expr) - 1);
                memcpy(fn, file, nfn);
                fn[nfn] = '\0';
                memcpy(expr, desc, nexpr);
                expr[nexpr] = '\0';
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: statuscode assert line=%u file=\"%s\" expr=\"%s\"\n",
                              line, fn, expr);
                logged = true;
                break;
            }
        }
    }

    if (!logged) {
        ia64_fw_break0_scan_status_records(cs, data_ptr, 8192);
        size_t dump = read_len > base ? read_len - base : 0;
        if (dump > 256) {
            dump = 256;
        }
        for (size_t off = 0; off < dump; off += 16) {
            char ascii[17];
            uint8_t bytes[16] = { 0 };
            size_t chunk = dump - off;
            if (chunk > 16) {
                chunk = 16;
            }
            for (size_t i = 0; i < chunk; i++) {
                uint8_t b = buf[base + off + i];
                bytes[i] = b;
                ascii[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
            }
            for (size_t i = chunk; i < 16; i++) {
                ascii[i] = ' ';
            }
            ascii[16] = '\0';
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: statuscode data+0x%zx "
                          "%02x %02x %02x %02x %02x %02x %02x %02x "
                          "%02x %02x %02x %02x %02x %02x %02x %02x |%s|\n",
                          off,
                          bytes[0], bytes[1], bytes[2], bytes[3],
                          bytes[4], bytes[5], bytes[6], bytes[7],
                          bytes[8], bytes[9], bytes[10], bytes[11],
                          bytes[12], bytes[13], bytes[14], bytes[15],
                          ascii);
        }
    }
}

#ifndef CONFIG_USER_ONLY
static void ia64_fw_try_patch_efi_hobs(CPUIA64State *env);
static bool ia64_fw_pei_ps_dump_enabled(void);
static void ia64_fw_pei_dump_ps(CPUIA64State *env, uint64_t ps_ptr,
                                uint64_t pc);

typedef struct IA64FwFvInfo {
    uint64_t base;
    uint64_t len;
} IA64FwFvInfo;

static bool ia64_fw_fv_list_has_base(const IA64FwFvInfo *list, int count,
                                     uint64_t base)
{
    for (int i = 0; i < count; i++) {
        if (list[i].base == base) {
            return true;
        }
    }
    return false;
}

static bool ia64_fw_fv_list_add(IA64FwFvInfo *list, int *count, int max,
                                uint64_t base, uint64_t len)
{
    if (!len || *count >= max) {
        return false;
    }
    for (int i = 0; i < *count; i++) {
        if (list[i].base == base && list[i].len == len) {
            return false;
        }
    }
    list[*count].base = base;
    list[*count].len = len;
    (*count)++;
    return true;
}

static int ia64_fw_scan_flash_fvs(CPUState *cs, IA64FwFvInfo *out, int max)
{
    const uint8_t sig[4] = { '_', 'F', 'V', 'H' };
    const uint64_t flash_base = ia64_fw_flash_base();
    const uint64_t flash_size = ia64_fw_flash_size();
    const uint64_t flash_end = ia64_fw_flash_end();
    const size_t chunk = 64 * 1024;
    g_autofree uint8_t *buf = g_malloc(chunk);
    int count = 0;

    if (flash_end <= flash_base) {
        return 0;
    }

    for (uint64_t off = 0; off < flash_size; off += chunk - 4) {
        uint64_t addr = flash_base + off;
        if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
            continue;
        }
        for (size_t i = 0; i + sizeof(sig) <= chunk; i++) {
            if (memcmp(&buf[i], sig, sizeof(sig)) != 0) {
                continue;
            }
            uint64_t sig_addr = addr + i;
            if (sig_addr < flash_base + 0x28) {
                continue;
            }
            uint64_t base = sig_addr - 0x28;
            if (base < flash_base || base + 0x38 > flash_end) {
                continue;
            }
            uint8_t hdr[0x38];
            if (cpu_memory_rw_debug(cs, base, hdr, sizeof(hdr), false) != 0) {
                continue;
            }
            if (memcmp(&hdr[0x28], sig, sizeof(sig)) != 0) {
                continue;
            }
            uint64_t fv_len = ldq_le_p(&hdr[0x20]);
            uint16_t hdr_len = lduw_le_p(&hdr[0x30]);
            if (!fv_len || fv_len > flash_size) {
                continue;
            }
            if (base + fv_len > flash_end) {
                continue;
            }
            if (hdr_len < 0x38 || hdr_len > fv_len) {
                continue;
            }
            if (ia64_fw_fv_list_add(out, &count, max, base, fv_len) &&
                count >= max) {
                return count;
            }
        }
    }
    return count;
}
#endif

static bool ia64_fw_find_varstore_region(CPUState *cs, uint64_t *out_base,
                                         uint64_t *out_len)
{
#ifdef CONFIG_USER_ONLY
    (void)cs;
    (void)out_base;
    (void)out_len;
    return false;
#else
    IA64FwFvInfo fv_flash[8] = { 0 };
    int fv_count = ia64_fw_scan_flash_fvs(cs, fv_flash, ARRAY_SIZE(fv_flash));
    for (int i = 0; i < fv_count; i++) {
        uint8_t hdr[0x38];
        if (cpu_memory_rw_debug(cs, fv_flash[i].base, hdr, sizeof(hdr), false) != 0) {
            continue;
        }
        if (ldl_le_p(&hdr[0x28]) != IA64_EFI_FVH_SIGNATURE) {
            continue;
        }
        uint64_t fv_len = ldq_le_p(&hdr[0x20]);
        uint16_t hdr_len = lduw_le_p(&hdr[0x30]);
        if (fv_len < sizeof(hdr) || fv_len > fv_flash[i].len) {
            continue;
        }
        if (hdr_len < sizeof(hdr) || hdr_len > fv_len) {
            continue;
        }
        uint64_t var_base = fv_flash[i].base + hdr_len;
        uint32_t sig = 0;
        if (ia64_fw_read_u32(cs, var_base, &sig) && sig == IA64_EFI_VARSTORE_SIGNATURE) {
            uint32_t vsz = 0;
            if (!ia64_fw_read_u32(cs, var_base + 4, &vsz) || vsz == 0 ||
                vsz > fv_len - hdr_len) {
                vsz = (uint32_t)(fv_len - hdr_len);
            }
            if (out_base) {
                *out_base = var_base;
            }
            if (out_len) {
                *out_len = vsz;
            }
            return true;
        }
    }

    {
        const uint64_t fallback_fv_base = 0x00000000ffe00000ULL;
        uint8_t hdr[0x38];
        if (cpu_memory_rw_debug(cs, fallback_fv_base, hdr, sizeof(hdr), false) == 0 &&
            ldl_le_p(&hdr[0x28]) == IA64_EFI_FVH_SIGNATURE) {
            uint64_t fv_len = ldq_le_p(&hdr[0x20]);
            uint16_t hdr_len = lduw_le_p(&hdr[0x30]);
            if (fv_len >= sizeof(hdr) && hdr_len >= sizeof(hdr) &&
                hdr_len < fv_len) {
                if (out_base) {
                    *out_base = fallback_fv_base + hdr_len;
                }
                if (out_len) {
                    *out_len = fv_len - hdr_len;
                }
                return true;
            }
        }
    }
    return false;
#endif
}

static bool ia64_fw_find_fv_by_index(CPUIA64State *env, uint8_t index,
                                     uint64_t *out_addr)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)index;
    (void)out_addr;
    return false;
#else
    CPUState *cs = env_cpu(env);
    const uint64_t flash_base = ia64_fw_flash_base();
    const uint64_t flash_size = ia64_fw_flash_size();
    uint8_t hdr[0x38];
    uint8_t cur = 0;

    for (uint64_t off = 0; off + sizeof(hdr) <= flash_size; off += 0x10) {
        if (!ia64_fw_read_bytes_any(cs, flash_base + off, hdr, sizeof(hdr))) {
            continue;
        }
        if (ldl_le_p(&hdr[0x28]) != IA64_EFI_FVH_SIGNATURE) {
            continue;
        }
        uint64_t fv_len = ldq_le_p(&hdr[0x20]);
        uint16_t hdr_len = lduw_le_p(&hdr[0x30]);
        if (fv_len < sizeof(hdr) || fv_len > flash_size - off) {
            continue;
        }
        if (hdr_len < sizeof(hdr) || hdr_len > fv_len) {
            continue;
        }
        if (memcmp(&hdr[0x10], ia64_efi_ffs_guid,
                   sizeof(ia64_efi_ffs_guid)) != 0) {
            continue;
        }
        if (cur == index) {
            if (out_addr) {
                *out_addr = flash_base + off;
            }
            return true;
        }
        cur++;
        if (fv_len > 0x10) {
            off += fv_len - 0x10;
        }
    }
    return false;
#endif
}

static uint32_t ia64_fw_section_size(const uint8_t *hdr)
{
    return (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16);
}

static bool ia64_fw_pe32_entry_from_mem(CPUState *cs, hwaddr base, uint64_t len,
                                        uint32_t *entry_out)
{
    uint8_t hdr[0x200];
    size_t need = MIN(len, sizeof(hdr));
    if (need < 0x40) {
        return false;
    }
    if (!ia64_fw_read_bytes_any(cs, base, hdr, need)) {
        return false;
    }

    uint16_t te_sig = lduw_le_p(hdr);
    if (te_sig == IA64_EFI_TE_IMAGE_HEADER_SIGNATURE) {
        if (len < 40) {
            return false;
        }
        uint16_t stripped = lduw_le_p(&hdr[6]);
        uint32_t entry = ldl_le_p(&hdr[8]);
        *entry_out = entry + 40 - stripped;
        return true;
    }

    if (lduw_le_p(hdr) != 0x5a4d) { /* MZ */
        return false;
    }
    uint32_t lfanew = ldl_le_p(&hdr[0x3c]);
    if (lfanew + 0x18 + 0x10 + 4 > len) {
        return false;
    }
    uint8_t pe_sig[4];
    if (!ia64_fw_read_bytes_any(cs, base + lfanew, pe_sig, sizeof(pe_sig))) {
        return false;
    }
    if (ldl_le_p(pe_sig) != 0x00004550) { /* PE\0\0 */
        return false;
    }
    uint8_t opt_hdr[0x1a];
    if (!ia64_fw_read_bytes_any(cs, base + lfanew + 0x18, opt_hdr, sizeof(opt_hdr))) {
        return false;
    }
    uint16_t magic = lduw_le_p(&opt_hdr[0]);
    if (magic != 0x10b && magic != 0x20b) {
        return false;
    }
    uint32_t entry = ldl_le_p(&opt_hdr[0x10]);
    *entry_out = entry;
    return true;
}

static void ia64_fw_handle_security_ppi(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    CPUState *cs = env_cpu(env);
    uint64_t auth_status = env->r[34];
    uint64_t ffs_ptr = env->r[35];
    uint64_t crisis_ptr = env->r[36];

    if (crisis_ptr) {
        uint8_t out = 0;
        if (!ia64_fw_write_bytes_any(cs, crisis_ptr, &out, sizeof(out))) {
            env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
            return;
        }
    }

    env->r[8] = 0; /* EFI_SUCCESS */
    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_security_ppi pc=%016" PRIx64
                      " auth=%016" PRIx64 " ffs=%016" PRIx64
                      " crisis_ptr=%016" PRIx64 "\n",
                      pc, auth_status, ffs_ptr, crisis_ptr);
    }
#endif
}

static void ia64_fw_handle_fv_load_file(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    CPUState *cs = env_cpu(env);
    uint64_t ffs_ptr = env->r[34];
    uint64_t image_addr_ptr = env->r[35];
    uint64_t image_size_ptr = env->r[36];
    uint64_t entry_ptr = env->r[37];
    uint64_t status = 0;

    if (!ffs_ptr || (!image_addr_ptr && !image_size_ptr && !entry_ptr)) {
        env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 2; /* EFI_INVALID_PARAMETER */
        return;
    }

    hwaddr ffs_phys = ia64_phys_mode_addr(ffs_ptr);
    uint8_t fh[IA64_EFI_FFS_FILE_HEADER2_SIZE];
    if (!ia64_fw_read_bytes_any(cs, ffs_phys, fh,
                                IA64_EFI_FFS_FILE_HEADER_SIZE)) {
        env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
        return;
    }

    uint32_t size24 = (uint32_t)fh[20] |
                      ((uint32_t)fh[21] << 8) |
                      ((uint32_t)fh[22] << 16);
    uint64_t fsize = size24;
    size_t hdr_size = IA64_EFI_FFS_FILE_HEADER_SIZE;
    if (size24 == 0xffffff) {
        if (!ia64_fw_read_bytes_any(cs, ffs_phys + IA64_EFI_FFS_FILE_HEADER_SIZE,
                                    fh + IA64_EFI_FFS_FILE_HEADER_SIZE,
                                    IA64_EFI_FFS_FILE_HEADER2_SIZE -
                                    IA64_EFI_FFS_FILE_HEADER_SIZE)) {
            env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
            return;
        }
        fsize = ldq_le_p(&fh[24]);
        hdr_size = IA64_EFI_FFS_FILE_HEADER2_SIZE;
    }

    if (fsize < hdr_size) {
        env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 2; /* EFI_INVALID_PARAMETER */
        return;
    }

    hwaddr file_end = ffs_phys + (hwaddr)fsize;
    hwaddr sec_phys = ffs_phys + hdr_size;
    bool found = false;
    uint64_t img_size = 0;
    uint64_t img_phys = 0;
    uint64_t entry_phys = 0;

    while (sec_phys + sizeof(IA64EfiCommonSectionHeader) <= file_end) {
        uint8_t sh[sizeof(IA64EfiCommonSectionHeader)];
        if (!ia64_fw_read_bytes_any(cs, sec_phys, sh, sizeof(sh))) {
            status = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
            break;
        }
        uint32_t sec_size = ia64_fw_section_size(sh);
        uint8_t sec_type = sh[3];
        if (sec_size < sizeof(IA64EfiCommonSectionHeader) ||
            sec_phys + sec_size > file_end) {
            status = IA64_EFI_STATUS_ERROR_BIT | 3; /* EFI_UNSUPPORTED */
            break;
        }
        if (sec_type == IA64_EFI_SECTION_PE32 ||
            sec_type == IA64_EFI_SECTION_TE) {
            hwaddr img_base = sec_phys + sizeof(IA64EfiCommonSectionHeader);
            uint64_t img_len = sec_size - sizeof(IA64EfiCommonSectionHeader);
            uint32_t entry_off = 0;
            if (!ia64_fw_pe32_entry_from_mem(cs, img_base, img_len, &entry_off) ||
                entry_off >= img_len) {
                status = IA64_EFI_STATUS_ERROR_BIT | 3; /* EFI_UNSUPPORTED */
                break;
            }
            img_phys = img_base;
            img_size = img_len;
            entry_phys = img_base + entry_off;
            found = true;
            status = 0;
            break;
        }
        uint32_t advance = (sec_size + 3) & ~3U;
        if (!advance || sec_phys + advance < sec_phys) {
            status = IA64_EFI_STATUS_ERROR_BIT | 3; /* EFI_UNSUPPORTED */
            break;
        }
        sec_phys += advance;
    }

    if (!found && status == 0) {
        status = IA64_EFI_STATUS_ERROR_BIT | 14; /* EFI_NOT_FOUND */
    }

    if (status == 0) {
        if (image_addr_ptr) {
            uint64_t out = cpu_to_le64(ia64_fw_encode_addr(ffs_ptr, img_phys));
            if (!ia64_fw_write_bytes_any(cs, image_addr_ptr,
                                         (const uint8_t *)&out, sizeof(out))) {
                status = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
            }
        }
        if (status == 0 && image_size_ptr) {
            uint64_t out = cpu_to_le64(img_size);
            if (!ia64_fw_write_bytes_any(cs, image_size_ptr,
                                         (const uint8_t *)&out, sizeof(out))) {
                status = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
            }
        }
        if (status == 0 && entry_ptr) {
            uint64_t out = cpu_to_le64(ia64_fw_encode_addr(ffs_ptr, entry_phys));
            if (!ia64_fw_write_bytes_any(cs, entry_ptr,
                                         (const uint8_t *)&out, sizeof(out))) {
                status = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
            }
        }
    }

    env->r[8] = status;
    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_loadfile pc=%016" PRIx64 " ffs=%016" PRIx64
                      " img=%016" PRIx64 " size=%" PRIu64
                      " entry=%016" PRIx64 " status=%" PRIu64 "\n",
                      pc, ffs_ptr,
                      ia64_fw_encode_addr(ffs_ptr, img_phys), img_size,
                      ia64_fw_encode_addr(ffs_ptr, entry_phys),
                      (uint64_t)(status & ~IA64_EFI_STATUS_ERROR_BIT));
    }
#endif
}

static void ia64_fw_handle_findfv(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    CPUState *cs = env_cpu(env);
    uint64_t fv_num_ptr = env->r[34];
    uint64_t fv_addr_ptr = env->r[35];
    uint8_t fv_num = 0;
    uint64_t fv_addr = 0;
    uint64_t status = 0;

    if (!fv_addr_ptr || !fv_num_ptr) {
        status = IA64_EFI_STATUS_ERROR_BIT | 2; /* EFI_INVALID_PARAMETER */
        env->r[8] = status;
        return;
    }

    if (!ia64_fw_read_bytes_any(cs, fv_num_ptr, &fv_num, sizeof(fv_num))) {
        status = IA64_EFI_STATUS_ERROR_BIT | 2; /* EFI_INVALID_PARAMETER */
        env->r[8] = status;
        return;
    }

    if (!ia64_fw_find_fv_by_index(env, fv_num, &fv_addr)) {
        status = IA64_EFI_STATUS_ERROR_BIT | 9; /* EFI_OUT_OF_RESOURCES */
        fv_num = 0;
        ia64_fw_write_bytes_any(cs, fv_num_ptr, &fv_num, sizeof(fv_num));
        env->r[8] = status;
        return;
    }

    uint64_t le = cpu_to_le64(fv_addr);
    if (!ia64_fw_write_bytes_any(cs, fv_addr_ptr,
                                 (const uint8_t *)&le, sizeof(le))) {
        status = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
        env->r[8] = status;
        return;
    }

    fv_num++;
    ia64_fw_write_bytes_any(cs, fv_num_ptr, &fv_num, sizeof(fv_num));
    env->r[8] = 0; /* EFI_SUCCESS */

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_findfv pc=%016" PRIx64 " fv=%u addr=%016" PRIx64 "\n",
                      pc, fv_num - 1, fv_addr);
    }
#endif
}

static void ia64_fw_handle_sec_platform_info(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    CPUState *cs = env_cpu(env);
    uint64_t size_ptr = env->r[33];
    uint64_t rec_ptr = env->r[34];
    uint64_t size = 0;
    uint64_t status = 0;
    uint32_t health = 0;

    if (!size_ptr) {
        env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 2; /* EFI_INVALID_PARAMETER */
        return;
    }
    if (!ia64_fw_read_bytes_any(cs, size_ptr,
                                (uint8_t *)&size, sizeof(size))) {
        env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
        return;
    }
    size = le64_to_cpu(size);

    const uint64_t need = sizeof(health);
    if (size < need) {
        uint64_t out = cpu_to_le64(need);
        ia64_fw_write_bytes_any(cs, size_ptr,
                                (const uint8_t *)&out, sizeof(out));
        env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 5; /* EFI_BUFFER_TOO_SMALL */
        return;
    }

    if (rec_ptr) {
        uint32_t out = cpu_to_le32(health);
        if (!ia64_fw_write_bytes_any(cs, rec_ptr,
                                     (const uint8_t *)&out, sizeof(out))) {
            env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
            return;
        }
    }

    uint64_t out = cpu_to_le64(need);
    ia64_fw_write_bytes_any(cs, size_ptr,
                            (const uint8_t *)&out, sizeof(out));
    env->r[8] = status; /* EFI_SUCCESS */

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_sec_platform_info pc=%016" PRIx64
                      " size_ptr=%016" PRIx64 " rec_ptr=%016" PRIx64 "\n",
                      pc, size_ptr, rec_ptr);
    }
#endif
}

static bool ia64_fw_memmap_region(CPUIA64State *env, uint64_t index,
                                  uint64_t *base, uint64_t *size)
{
    const uint64_t mem = env->fw_mem_size;
    const uint64_t vga_start = 0x000a0000ULL;
    const uint64_t vga_size = 0x00020000ULL;
    const uint64_t low_limit = 0xC0000000ULL;

    if (!mem) {
        return false;
    }
    if (index == 0) {
        *base = 0;
        if (mem < vga_start) {
            *size = mem;
        } else {
            uint64_t lo = mem + vga_size;
            *size = (lo < low_limit) ? lo : low_limit;
        }
        return *size > 0;
    }
    if (index == 1 && mem > low_limit) {
        *base = 0x100000000ULL;
        *size = mem + vga_size - low_limit;
        return *size > 0;
    }
    return false;
}

static bool ia64_fw_memmap_table_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IPF_FW_MEMMAP_TABLE");
        if (!s || !*s) {
            enabled = 1;
        } else {
            enabled = ia64_env_truthy(s) ? 1 : 0;
        }
    }
    return enabled;
}

void HELPER(fw_autoscan_memtop_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    uint64_t base = 0;
    uint64_t size = 0;
    if (!ia64_fw_memmap_region(env, 0, &base, &size)) {
        return;
    }
    uint64_t mem_top = base + size;
    if (!mem_top) {
        return;
    }
    uint64_t old = env->r[35];
    if (old != mem_top) {
        env->r[35] = mem_top;
        if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_autoscan_memtop_fix pc=%016" PRIx64
                          " r35=%016" PRIx64 " -> %016" PRIx64 "\n",
                          pc, old, mem_top);
        }
    }
#endif
}

void HELPER(fw_pei_install_mem_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    uint64_t ram_base = 0;
    uint64_t ram_size = 0;
    if (!ia64_fw_memmap_region(env, 0, &ram_base, &ram_size)) {
        return;
    }
    uint64_t mem_top = ram_base + ram_size;
    if (mem_top <= ram_base) {
        return;
    }

    uint8_t sof = env->cfm & 0x7f;
    uint8_t sol = (env->cfm >> 7) & 0x7f;
    uint8_t outs = (sof > sol) ? (sof - sol) : 0;
    uint8_t out0 = 32 + sol;
    if (outs < 3 || out0 + 2 >= 128) {
        return;
    }

    uint64_t base_raw = env->r[out0 + 1];
    uint64_t size = env->r[out0 + 2];
    if (!size) {
        size = 0x01000000ULL;
    }

    uint64_t desired_base = ram_base;
    uint64_t desired_size = mem_top - ram_base;
    if (desired_size > (1ULL << 20)) {
        desired_base = ram_base + (1ULL << 20);
        desired_size = mem_top - desired_base;
    }
    if (desired_size == 0) {
        return;
    }

    uint64_t base_phys = ia64_phys_mode_addr(base_raw);
    uint64_t new_phys = base_phys;
    bool fix = false;

    if (size < desired_size) {
        size = desired_size;
        fix = true;
    }

    if (base_phys < desired_base || base_phys >= mem_top ||
        base_phys + size > mem_top) {
        if (mem_top < size) {
            return;
        }
        new_phys = desired_base;
        new_phys &= ~((1ULL << 20) - 1);
        if (new_phys < desired_base) {
            new_phys = desired_base;
        }
        fix = true;
    }

    uint64_t new_base = ia64_fw_encode_addr(base_raw, new_phys);
    if (fix || new_base != base_raw || size != env->r[out0 + 2]) {
        env->r[out0 + 1] = new_base;
        env->r[out0 + 2] = size;
        if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_pei_install_mem_fix pc=%016" PRIx64
                          " base=%016" PRIx64 " -> %016" PRIx64
                          " size=%016" PRIx64 " mem_top=%016" PRIx64 "\n",
                          pc, base_raw, new_base, size, mem_top);
        }
    }

    ia64_fw_try_patch_efi_hobs(env);
#endif
}

static void ia64_fw_try_write_memmap_table(CPUIA64State *env)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    return;
#else
    static bool logged;
    const uint64_t table_base = 0x0000000002000000ULL;
    const uint64_t max_entries = 16;

    if (!env->fw_mem_size) {
        return;
    }

    CPUState *cs = env_cpu(env);
    uint64_t size0 = 0;
    if (ia64_fw_read_u64(cs, table_base + 8, &size0) && size0 != 0) {
        return;
    }

    for (uint64_t idx = 0; idx < max_entries; idx++) {
        uint64_t base = 0;
        uint64_t size = 0;
        uint64_t base_mb = 0;
        uint64_t size_mb = 0;
        if (ia64_fw_memmap_region(env, idx, &base, &size)) {
            if (idx == 0 && base == 0 && size > (1ULL << 20)) {
                base = 1ULL << 20;
                size -= 1ULL << 20;
            }
            base_mb = base >> 20;
            size_mb = size >> 20;
        }

        uint8_t out[16];
        stq_le_p(out, base_mb);
        stq_le_p(out + 8, size_mb);
        if (!ia64_fw_write_bytes_any(cs, table_base + idx * sizeof(out),
                                     out, sizeof(out))) {
            return;
        }
    }

    if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        logged = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_memmap_table base=%016" PRIx64
                      " entries=%" PRIu64 " mem=%" PRIu64 "\n",
                      table_base, max_entries, env->fw_mem_size);
    }
#endif
}

void HELPER(fw_autoscan_memmap_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    static bool logged;
    if (!ia64_fw_memmap_table_enabled()) {
        return;
    }
    ia64_fw_try_write_memmap_table(env);
    if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        CPUState *cs = env_cpu(env);
        uint64_t base0 = 0;
        uint64_t size0 = 0;
        uint64_t base1 = 0;
        uint64_t size1 = 0;
        (void)ia64_fw_read_u64(cs, 0x0000000002000000ULL, &base0);
        (void)ia64_fw_read_u64(cs, 0x0000000002000008ULL, &size0);
        (void)ia64_fw_read_u64(cs, 0x0000000002000010ULL, &base1);
        (void)ia64_fw_read_u64(cs, 0x0000000002000018ULL, &size1);
        logged = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_autoscan_memmap_fix pc=%016" PRIx64
                      " mem=%" PRIu64 " entry0=%" PRIx64 "/%" PRIx64
                      " entry1=%" PRIx64 "/%" PRIx64 "\n",
                      pc, env->fw_mem_size, base0, size0, base1, size1);
    }
#endif
}

static void ia64_fw_handle_memmap_autoscan(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    CPUState *cs = env_cpu(env);
    uint64_t a0 = env->r[32];
    uint64_t a1 = env->r[33];
    uint64_t a2 = env->r[34];
    uint64_t a3 = env->r[35];
    uint64_t a4 = env->r[36];
    uint64_t index = 0;
    uint64_t base_ptr = 0;
    uint64_t size_ptr = 0;
    uint64_t ps_ptr = 0;
    bool has_ps = ia64_fw_pei_get_ps_ptr(env, a0, &ps_ptr);

    /*
     * Autoscan-style PPIs typically use (Index, &Base, &Size). Some variants
     * include PeiServices and/or "This" as leading arguments; detect those
     * and shift arguments if needed.
     */
    if (has_ps && a1 < 0x1000) {
        index = a1;
        base_ptr = a2;
        size_ptr = a3;
    } else if (has_ps && a3 < 0x1000 && a2 && a4) {
        index = a3;
        base_ptr = a2;
        size_ptr = a4;
    } else if (a0 < 0x1000) {
        index = a0;
        base_ptr = a1;
        size_ptr = a2;
    } else {
        index = a0;
        base_ptr = a1;
        size_ptr = a2;
    }
    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_autoscan_args pc=%016" PRIx64
                      " a0=%016" PRIx64 " a1=%016" PRIx64
                      " a2=%016" PRIx64 " a3=%016" PRIx64
                      " a4=%016" PRIx64 " a5=%016" PRIx64
                      " a6=%016" PRIx64 " a7=%016" PRIx64
                      " has_ps=%d ps=%016" PRIx64
                      " idx=%" PRIu64 " base_ptr=%016" PRIx64
                      " size_ptr=%016" PRIx64 "\n",
                      pc, a0, a1, a2, a3, a4,
                      env->r[37], env->r[38], env->r[39],
                      has_ps ? 1 : 0, ps_ptr, index, base_ptr, size_ptr);
    }

    uint64_t base = 0;
    uint64_t size = 0;
    if (!base_ptr || !size_ptr || !ia64_fw_memmap_region(env, index, &base, &size)) {
        env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 3; /* EFI_UNSUPPORTED */
        return;
    }

    ia64_fw_try_write_memmap_table(env);

    uint64_t out_base = cpu_to_le64(base);
    uint64_t out_size = cpu_to_le64(size);
    if (!ia64_fw_write_bytes_any(cs, base_ptr,
                                 (const uint8_t *)&out_base, sizeof(out_base)) ||
        !ia64_fw_write_bytes_any(cs, size_ptr,
                                 (const uint8_t *)&out_size, sizeof(out_size))) {
        env->r[8] = IA64_EFI_STATUS_ERROR_BIT | 7; /* EFI_DEVICE_ERROR */
        return;
    }

    env->r[8] = 0; /* EFI_SUCCESS */
    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        uint64_t rb = 0;
        uint64_t rs = 0;
        (void)ia64_fw_read_u64(cs, base_ptr, &rb);
        (void)ia64_fw_read_u64(cs, size_ptr, &rs);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_autoscan pc=%016" PRIx64
                      " idx=%" PRIu64 " base=%016" PRIx64 " size=%016" PRIx64
                      " base_ptr=%016" PRIx64 " size_ptr=%016" PRIx64 "\n",
                      pc, index, rb, rs, base_ptr, size_ptr);
    }
#endif
}

void HELPER(fw_break0)(CPUIA64State *env, uint64_t pc)
{
    /*
     * Xenipf firmware and some EDK components use break(0) as a last-resort
     * trap/breakpoint. In our bringup environment, a missing handler would
     * otherwise recurse into the empty break vector (0x2c00) and hang.
     *
     * Treat break(0) as a firmware status/debug hook and resume normal
     * execution after recording the status information.
     */
    static int dump_enabled = -1;
    static int dump_len = -1;
    static int abort_assert = -1;
    static int log_limit = -1;
    static int log_count;
    static int gcd_dump_enabled = -1;
    static int scan_always = -1;
    static int scan_limit = -1;
    static int scan_count;

    ia64_fw_try_install_sal_systab(env);
#ifndef CONFIG_USER_ONLY
    ia64_fw_try_patch_efi_hobs(env);
#endif

    if (env->fw_pei_findfv_stub &&
        ((pc ^ env->fw_pei_findfv_stub) & ~0xFULL) == 0) {
        ia64_fw_handle_findfv(env, pc);
        return;
    }
    if (env->fw_pei_secinfo_stub &&
        ((pc ^ env->fw_pei_secinfo_stub) & ~0xFULL) == 0) {
        ia64_fw_handle_sec_platform_info(env, pc);
        return;
    }
    if (env->fw_pei_memmap_stub &&
        ((pc ^ env->fw_pei_memmap_stub) & ~0xFULL) == 0) {
        ia64_fw_handle_memmap_autoscan(env, pc);
        return;
    }
    if (env->fw_pei_security_stub &&
        ((pc ^ env->fw_pei_security_stub) & ~0xFULL) == 0) {
        ia64_fw_handle_security_ppi(env, pc);
        return;
    }
    if (env->fw_pei_loadfile_stub &&
        ((pc ^ env->fw_pei_loadfile_stub) & ~0xFULL) == 0) {
        ia64_fw_handle_fv_load_file(env, pc);
        return;
    }

    uint32_t code_type = (uint32_t)env->r[32];
    uint32_t value = (uint32_t)env->r[33];
    uint32_t alt_code_type = (uint32_t)env->r[8];
    uint32_t alt_value = (uint32_t)env->r[9];
    uint32_t ppi_code_type = (uint32_t)env->r[33];
    uint32_t ppi_value = (uint32_t)env->r[34];
    uint32_t log_code_type = code_type;
    uint32_t log_value = value;
    bool is_assert = ia64_fw_status_is_assert(code_type, value);
    if (!is_assert && ia64_fw_status_is_assert(alt_code_type, alt_value)) {
        is_assert = true;
        code_type = alt_code_type;
        value = alt_value;
    }
    if (!is_assert && ia64_fw_status_is_assert(ppi_code_type, ppi_value)) {
        is_assert = true;
        code_type = ppi_code_type;
        value = ppi_value;
    }
    if (!ia64_fw_status_code_valid(log_code_type, log_value) &&
        ia64_fw_status_code_valid(alt_code_type, alt_value)) {
        log_code_type = alt_code_type;
        log_value = alt_value;
    }
    if (!ia64_fw_status_code_valid(log_code_type, log_value) &&
        ia64_fw_status_code_valid(ppi_code_type, ppi_value)) {
        log_code_type = ppi_code_type;
        log_value = ppi_value;
    }
    if (is_assert && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        uint64_t data_ptrs[3] = { env->r[36], env->r[37], env->r[35] };
        for (size_t i = 0; i < ARRAY_SIZE(data_ptrs); i++) {
            uint64_t data_ptr = data_ptrs[i];
            if (!data_ptr || data_ptr == UINT64_MAX) {
                continue;
            }
            if (i > 0 && data_ptr == data_ptrs[0]) {
                continue;
            }
            ia64_fw_statuscode_dump(env, data_ptr, log_code_type, log_value);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: assert_ctx last_br from=%016" PRIx64
                      " to=%016" PRIx64 " kind=%" PRIu64
                      " last_b0 pc=%016" PRIx64 " val=%016" PRIx64
                      " kind=%" PRIu64 "\n",
                      env->last_branch_from, env->last_branch_to,
                      env->last_branch_kind & 0xff,
                      env->last_b0_write_pc, env->last_b0_write_val,
                      env->last_b0_write_kind & 0xff);
        for (int i = 0; i < 8; i++) {
            int idx = (env->b0_trace_idx + i) & 0xf;
            if (!env->b0_trace_pc[idx]) {
                continue;
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: assert_ctx b0_trace[%02d] pc=%016" PRIx64
                          " val=%016" PRIx64 " kind=%" PRIu64
                          " insn=%011" PRIx64 "\n",
                          i, env->b0_trace_pc[idx], env->b0_trace_val[idx],
                          env->b0_trace_kind[idx] & 0xff,
                          env->b0_trace_insn[idx]);
        }
    }

    if (dump_enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_BREAK0_DUMP");
        if (s && *s) {
            dump_enabled = (strcmp(s, "assert") == 0) ? 2 : 1;
        } else {
            dump_enabled = 0;
        }
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
    if (abort_assert == -1) {
        const char *s = getenv("QEMU_IA64_FW_BREAK0_ABORT_ASSERT");
        abort_assert = (s && *s) ? 1 : 0;
    }
    if (gcd_dump_enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_GCD_DUMP");
        gcd_dump_enabled = (s && *s) ? 1 : 0;
    }
    if (scan_always == -1) {
        const char *s = getenv("QEMU_IA64_FW_BREAK0_SCAN_ALWAYS");
        scan_always = (s && *s) ? 1 : 0;
    }
    if (scan_limit == -1) {
        scan_limit = 0;
        const char *s = getenv("QEMU_IA64_FW_BREAK0_SCAN_LIMIT");
        if (s && *s) {
            scan_limit = atoi(s);
        }
        if (scan_limit < 0) {
            scan_limit = 0;
        }
    }

    if (log_limit == -1) {
        log_limit = 128;
        const char *s = getenv("QEMU_IA64_FW_BREAK0_LOG_LIMIT");
        if (s && *s) {
            log_limit = atoi(s);
        }
        if (log_limit < 0) {
            log_limit = 0;
        }
    }

    uint16_t post_code = (uint16_t)value;
    uint16_t post_code_alt = (uint16_t)alt_value;
    const char *post_desc = ia64_fw_decode_sala_post_code(post_code);
    const char *post_desc_alt = (post_code_alt != post_code) ?
                                ia64_fw_decode_sala_post_code(post_code_alt) :
                                NULL;
    uint8_t post_code_efi = 0;
    const char *class_name = NULL;
    const char *subclass_name = NULL;
    if (ia64_fw_status_code_valid(log_code_type, log_value)) {
        class_name = ia64_fw_status_class_name(
            log_value & IA64_EFI_STATUS_CODE_CLASS_MASK);
        subclass_name = ia64_fw_status_subclass_name(log_value);
        if ((log_code_type & IA64_EFI_STATUS_CODE_TYPE_MASK) ==
                IA64_EFI_PROGRESS_CODE ||
            (log_code_type & IA64_EFI_STATUS_CODE_TYPE_MASK) ==
                IA64_EFI_ERROR_CODE) {
            post_code_efi = (uint8_t)
                ((((log_value & IA64_EFI_STATUS_CODE_CLASS_MASK) >> 24) << 5) |
                 (((log_value & IA64_EFI_STATUS_CODE_SUBCLASS_MASK) >> 16) & 0x1f));
        }
    }

    if (qemu_loglevel_mask(LOG_GUEST_ERROR) &&
        (log_limit == 0 || log_count++ < log_limit)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_break0 ip=%016" PRIx64 " pc=%016" PRIx64
                      " b0=%016" PRIx64 " assert=%d"
                      " r8=%016" PRIx64 " r9=%016" PRIx64 " r10=%016" PRIx64
                      " r11=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                      " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64 "\n",
                      env->ip, pc, env->b[0], is_assert ? 1 : 0,
                      env->r[8], env->r[9], env->r[10], env->r[11],
                      env->r[32], env->r[33], env->r[34], env->r[35],
                      env->r[36], env->r[37]);
        if (ia64_fw_status_code_valid(log_code_type, log_value)) {
            const char *class_open = class_name ? "(" : "";
            const char *class_close = class_name ? ")" : "";
            const char *sub_open = subclass_name ? "(" : "";
            const char *sub_close = subclass_name ? ")" : "";
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_status type=%08x severity=%08x value=%08x"
                          " class=%02x%s%s%s subclass=%02x%s%s%s op=%04x post=0x%02x\n",
                          log_code_type,
                          log_code_type & IA64_EFI_STATUS_CODE_SEVERITY_MASK,
                          log_value,
                          (unsigned)((log_value & IA64_EFI_STATUS_CODE_CLASS_MASK) >> 24),
                          class_open, class_name ? class_name : "", class_close,
                          (unsigned)((log_value & IA64_EFI_STATUS_CODE_SUBCLASS_MASK) >> 16),
                          sub_open, subclass_name ? subclass_name : "", sub_close,
                          (unsigned)(log_value & IA64_EFI_STATUS_CODE_OPERATION_MASK),
                          (unsigned)post_code_efi);
        }
        if (post_desc) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_post r33=0x%04x %s\n",
                          post_code, post_desc);
        }
        if (post_desc_alt) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_post r9=0x%04x %s\n",
                          post_code_alt, post_desc_alt);
        }
    }

    if (is_assert && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        uint64_t status = env->r[8];
        if (ia64_fw_efi_status_maybe(status)) {
            const char *name = ia64_fw_efi_status_name(status);
            if (name) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_assert_status r8=%016" PRIx64 " %s\n",
                              status, name);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_assert_status r8=%016" PRIx64
                              " code=0x%llx\n",
                              status,
                              (unsigned long long)(status & ~IA64_EFI_STATUS_ERROR_BIT));
            }
        }

        status = env->r[9];
        if (status != env->r[8] && ia64_fw_efi_status_maybe(status)) {
            const char *name = ia64_fw_efi_status_name(status);
            if (name) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_assert_status r9=%016" PRIx64 " %s\n",
                              status, name);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_assert_status r9=%016" PRIx64
                              " code=0x%llx\n",
                              status,
                              (unsigned long long)(status & ~IA64_EFI_STATUS_ERROR_BIT));
            }
        }
    }

    bool do_scan = (is_assert || scan_always) &&
                   (scan_limit == 0 || scan_count++ < scan_limit);

    if (do_scan) {
        CPUState *cs = env_cpu(env);
        static int stack_dump_enabled = -1;
        static int stack_dump_len = -1;
        if (stack_dump_enabled == -1) {
            const char *s = getenv("QEMU_IA64_FW_BREAK0_STACK_DUMP");
            stack_dump_enabled = (s && *s) ? 1 : 0;
        }
        if (stack_dump_len == -1) {
            stack_dump_len = 4096;
            const char *s = getenv("QEMU_IA64_FW_BREAK0_STACK_DUMP_LEN");
            if (s && *s) {
                stack_dump_len = atoi(s);
            }
            if (stack_dump_len < 0) {
                stack_dump_len = 0;
            }
            if (stack_dump_len > 65536) {
                stack_dump_len = 65536;
            }
        }
        const struct {
            const char *name;
            uint64_t val;
        } probes[] = {
            { "r9", env->r[9] },
            { "r10", env->r[10] },
            { "r11", env->r[11] },
            { "r32", env->r[32] },
            { "r33", env->r[33] },
            { "r34", env->r[34] },
            { "r35", env->r[35] },
            { "r36", env->r[36] },
            { "r37", env->r[37] },
        };

        for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
            uint64_t a = probes[i].val;
            if (a < 0x1000) {
                continue;
            }
            uint8_t tmp[64];
            if (!ia64_fw_read_bytes_any(cs, a, tmp, sizeof(tmp))) {
                continue;
            }
            uint64_t q0 = ldq_le_p(&tmp[0]);
            uint64_t q1 = ldq_le_p(&tmp[8]);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_break0_probe %s=%016" PRIx64
                          " q0=%016" PRIx64 " q1=%016" PRIx64 "\n",
                          probes[i].name, a, q0, q1);
            /*
             * Also scan the pointed buffer for embedded status-code
             * records/strings. This can be noisy, but is useful to identify
             * the module that triggered ASSERT_EFI_ERROR().
             */
            ia64_fw_break0_scan_status_records(cs, a, 8192);
        }

        /* Try to recover the failing EFI_STATUS from the current stack frame. */
        {
            uint64_t sp = env->r[12];
            uint64_t base = (sp > 0x400) ? (sp - 0x400) : 0;
            uint8_t st[2048];
            if (ia64_fw_read_bytes_any(cs, base, st, sizeof(st))) {
                int hits = 0;
                for (size_t off = 0; off + 8 <= sizeof(st) && hits < 16; off += 8) {
                    uint64_t v = ldq_le_p(&st[off]);
                    if ((v & (1ULL << 63)) == 0) {
                        continue;
                    }
                    /* Heuristic: EFI_STATUS errors have small low codes. */
                    if ((v & 0xffffffffULL) > 0x2000) {
                        continue;
                    }
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_break0_stack_status sp=%016" PRIx64
                                  " addr=%016" PRIx64 " val=%016" PRIx64 "\n",
                                  sp, base + off, v);
                    hits++;
                }
            }
        }

        if (stack_dump_enabled && stack_dump_len > 0) {
            uint64_t sp = env->r[12];
            uint64_t base = (sp > (uint64_t)(stack_dump_len / 2)) ?
                                (sp - (uint64_t)(stack_dump_len / 2)) : 0;
            g_autofree uint8_t *st = g_malloc0((size_t)stack_dump_len);
            if (ia64_fw_read_bytes_any(cs, base, st, (size_t)stack_dump_len)) {
                g_mkdir_with_parents("scratch/ia64_logs", 0755);
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch/ia64_logs/fw_break0_stack_%016" PRIx64 ".bin", sp);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    fwrite(st, 1, (size_t)stack_dump_len, fp);
                    fclose(fp);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_break0_stack_dump sp=%016" PRIx64
                                  " base=%016" PRIx64 " bytes=%d file=%s\n",
                                  sp, base, stack_dump_len, path);
                }
            }
        }
    }

    if (dump_enabled && dump_len > 0 && (dump_enabled == 1 || is_assert)) {
        CPUState *cs = env_cpu(env);
        static int dump_file_enabled = -1;
        static int dump_file_count;
        if (dump_file_enabled == -1) {
            const char *s = getenv("QEMU_IA64_FW_BREAK0_DUMP_FILE");
            dump_file_enabled = (s && *s) ? 1 : 0;
        }

        g_autofree uint8_t *buf = g_malloc0((size_t)dump_len);
        const struct {
            const char *name;
            uint64_t addr;
        } candidates[] = {
            { "r36", env->r[36] },
            { "r37", env->r[37] },
            { "r35", env->r[35] },
            { "r32", env->r[32] },
            { "r33", env->r[33] },
            { "r34", env->r[34] },
        };
        uint64_t seen[ARRAY_SIZE(candidates)];
        size_t seen_count = 0;

        for (size_t ci = 0; ci < ARRAY_SIZE(candidates); ci++) {
            uint64_t addr = candidates[ci].addr;
            if (addr < 0x1000) {
                continue;
            }
            bool dup = false;
            for (size_t si = 0; si < seen_count; si++) {
                if (seen[si] == addr) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            seen[seen_count++] = addr;

            if (!ia64_fw_read_bytes_any(cs, addr, buf, (size_t)dump_len)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_break0_dump_read_failed reg=%s addr=%016" PRIx64
                              " len=%d\n",
                              candidates[ci].name, addr, dump_len);
                continue;
            }

            size_t scan_len = (size_t)dump_len;
            bool has_status_header = false;
            bool status_guid_valid = false;
            bool status_is_assert = false;
            IA64EfiGuid status_guid;
            uint16_t status_header_size = 0;

            /*
             * If the buffer starts with an EFI_STATUS_CODE_DATA header, limit
             * scanning to the declared size to avoid printing unrelated pool
             * data. Some builds appear to use a zero GUID, so treat the header
             * as valid based on size alone.
             */
            if (scan_len >= 20) {
                uint16_t header_size = lduw_le_p(&buf[0]);
                uint16_t data_size = lduw_le_p(&buf[2]);
                size_t total = (size_t)header_size + (size_t)data_size;
                if (header_size >= 20 && header_size <= 64 &&
                    total >= header_size && total <= scan_len) {
                    uint32_t d1 = ldl_le_p(&buf[4]);
                    uint16_t d2 = lduw_le_p(&buf[8]);
                    uint16_t d3 = lduw_le_p(&buf[10]);
                    ia64_fw_guid_from_bytes(&buf[4], &status_guid);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_break0_status_data hdr=%u size=%u guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                                  header_size, data_size,
                                  d1, d2, d3,
                                  buf[12], buf[13], buf[14], buf[15], buf[16], buf[17], buf[18], buf[19]);
                    has_status_header = true;
                    status_header_size = header_size;
                    status_guid_valid = true;
                    status_is_assert =
                        ia64_fw_guid_equal(&status_guid, &ia64_efi_guid_status_assert);
                    scan_len = total;
                }
            }

            if (dump_file_enabled && dump_file_count++ < 8) {
                g_mkdir_with_parents("scratch/ia64_logs", 0755);
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch/ia64_logs/fw_break0_%s_%016" PRIx64 ".bin",
                         candidates[ci].name, addr);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    fwrite(buf, 1, scan_len, fp);
                    fclose(fp);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_break0_dump_file reg=%s addr=%016" PRIx64
                                  " bytes=%zu file=%s\n",
                                  candidates[ci].name, addr, scan_len, path);
                }
            }

            /*
             * Attempt to decode an EDK-style ASSERT payload:
             * - CodeType: EFI_ERROR_UNRECOVERED|EFI_ERROR_CODE (0x90000002)
             * - Value:    *|EFI_SW_EC_ILLEGAL_SOFTWARE_STATE (op=7)
             * - Data:     EFI_STATUS_CODE_DATA + EFI_DEBUG_ASSERT_DATA + strings
             */
            if ((is_assert || status_is_assert) && has_status_header &&
                scan_len >= (size_t)status_header_size + 5) {
                size_t base = status_header_size;
                uint32_t line = ldl_le_p(&buf[base]);
                bool assert_logged = false;

                /*
                 * Some builds use a compact payload that stores pointers to the
                 * file/description strings instead of embedding them.
                 *
                 * Layout: { u32 line; u64 file_ptr; u64 desc_ptr }.
                 */
	                if (scan_len >= base + 4 + 16) {
	                    uint64_t file_ptr = ldq_le_p(&buf[base + 4]);
	                    uint64_t desc_ptr = ldq_le_p(&buf[base + 12]);

	                    qemu_log_mask(LOG_GUEST_ERROR,
	                                  "IA64: fw_break0_assert_ptr line=%u file_ptr=%016" PRIx64
	                                  " desc_ptr=%016" PRIx64 "\n",
	                                  line, file_ptr, desc_ptr);

                            /*
                             * If the payload uses indirection pointers, scan
                             * the pointed-to region for embedded
                             * EFI_STATUS_CODE_DATA records (ASSERT/DEBUG/...).
                             */
                            if (file_ptr >= 0x1000) {
                                ia64_fw_break0_scan_status_records(cs, file_ptr, 16384);
                            }
                            if (desc_ptr >= 0x1000 && desc_ptr != file_ptr) {
                                ia64_fw_break0_scan_status_records(cs, desc_ptr, 16384);
                            }

	                    if (file_ptr >= 0x1000 && desc_ptr >= 0x1000) {
	                        char fn[256] = { 0 };
	                        char expr[256] = { 0 };
	                        const char *fn_enc = NULL;
	                        const char *expr_enc = NULL;

	                        if (ia64_fw_read_ascii_string(cs, file_ptr, fn, sizeof(fn)) != 0) {
	                            fn_enc = "ascii";
	                        } else if (ia64_fw_read_ucs2le_string(cs, file_ptr, fn, sizeof(fn)) != 0) {
	                            fn_enc = "ucs2";
	                        }

	                        if (ia64_fw_read_ascii_string(cs, desc_ptr, expr, sizeof(expr)) != 0) {
	                            expr_enc = "ascii";
	                        } else if (ia64_fw_read_ucs2le_string(cs, desc_ptr, expr, sizeof(expr)) != 0) {
	                            expr_enc = "ucs2";
	                        }

	                        if (fn_enc && expr_enc) {
	                            qemu_log_mask(LOG_GUEST_ERROR,
	                                          "IA64: fw_break0_assert line=%u file=\"%s\" expr=\"%s\"\n",
	                                          line, fn, expr);
	                            qemu_log_mask(LOG_GUEST_ERROR,
	                                          "IA64: fw_break0_assert_enc file=%s expr=%s\n",
	                                          fn_enc, expr_enc);
                                assert_logged = true;
	                        } else {
	                            uint8_t tmp[64] = { 0 };
	                            if (!fn_enc &&
	                                cpu_memory_rw_debug(cs, file_ptr, tmp, sizeof(tmp), false) == 0) {
	                                qemu_log_mask(LOG_GUEST_ERROR,
	                                              "IA64: fw_break0_assert_file_bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	                                              tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7],
	                                              tmp[8], tmp[9], tmp[10], tmp[11], tmp[12], tmp[13], tmp[14], tmp[15]);
	                            }
	                            if (!expr_enc &&
	                                cpu_memory_rw_debug(cs, desc_ptr, tmp, sizeof(tmp), false) == 0) {
	                                qemu_log_mask(LOG_GUEST_ERROR,
	                                              "IA64: fw_break0_assert_expr_bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	                                              tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7],
	                                              tmp[8], tmp[9], tmp[10], tmp[11], tmp[12], tmp[13], tmp[14], tmp[15]);
	                            }

	                            /*
	                             * Heuristic: some builds pass pointers to
	                             * structures (or pointer tables) rather than
	                             * direct strings. If direct decoding failed,
	                             * try treating the first few qwords as
	                             * candidate string pointers.
	                             */
	                            uint64_t ptrs[8];
	                            if (!fn_enc &&
	                                cpu_memory_rw_debug(cs, file_ptr, ptrs, sizeof(ptrs), false) == 0) {
	                                int printed = 0;
	                                for (size_t pi = 0; pi < ARRAY_SIZE(ptrs); pi++) {
	                                    uint64_t p = ptrs[pi];
	                                    if (p < 0x1000) {
	                                        continue;
	                                    }
	                                    if (ia64_fw_read_ascii_string(cs, p, fn, sizeof(fn)) != 0) {
	                                        qemu_log_mask(LOG_GUEST_ERROR,
	                                                      "IA64: fw_break0_assert_file_ptr[%zu]=%016" PRIx64 " \"%s\"\n",
	                                                      pi, p, fn);
                                            assert_logged = true;
	                                        if (++printed >= 4) {
	                                            break;
	                                        }
	                                        continue;
	                                    }
	                                    if (ia64_fw_read_ucs2le_string(cs, p, fn, sizeof(fn)) != 0) {
	                                        qemu_log_mask(LOG_GUEST_ERROR,
	                                                      "IA64: fw_break0_assert_file_ptr[%zu]=%016" PRIx64 " \"%s\" (ucs2)\n",
	                                                      pi, p, fn);
                                            assert_logged = true;
	                                        if (++printed >= 4) {
	                                            break;
	                                        }
	                                        continue;
	                                    }
	                                }
	                            }
	                            if (!expr_enc &&
	                                cpu_memory_rw_debug(cs, desc_ptr, ptrs, sizeof(ptrs), false) == 0) {
	                                int printed = 0;
	                                for (size_t pi = 0; pi < ARRAY_SIZE(ptrs); pi++) {
	                                    uint64_t p = ptrs[pi];
	                                    if (p < 0x1000) {
	                                        continue;
	                                    }
	                                    if (ia64_fw_read_ascii_string(cs, p, expr, sizeof(expr)) != 0) {
	                                        qemu_log_mask(LOG_GUEST_ERROR,
	                                                      "IA64: fw_break0_assert_expr_ptr[%zu]=%016" PRIx64 " \"%s\"\n",
	                                                      pi, p, expr);
                                            assert_logged = true;
	                                        if (++printed >= 4) {
	                                            break;
	                                        }
	                                        continue;
	                                    }
	                                    if (ia64_fw_read_ucs2le_string(cs, p, expr, sizeof(expr)) != 0) {
	                                        qemu_log_mask(LOG_GUEST_ERROR,
	                                                      "IA64: fw_break0_assert_expr_ptr[%zu]=%016" PRIx64 " \"%s\" (ucs2)\n",
	                                                      pi, p, expr);
                                            assert_logged = true;
	                                        if (++printed >= 4) {
	                                            break;
	                                        }
	                                        continue;
	                                    }
	                                }
	                            }
	                        }
	                    }
	                }

	                /*
	                 * Inline-string payload fallback:
                 * { u32 line; char file[]; char expr[] }.
                 */
                if (scan_len >= base + 5) {
                    const char *file = (const char *)&buf[base + 4];
                    size_t file_max = scan_len - (base + 4);
                    size_t file_len = strnlen(file, file_max);
                    size_t desc_off = base + 4 + file_len + 1;

                    const char *desc = NULL;
                    size_t desc_len = 0;
                    if (file_len < file_max && desc_off < scan_len) {
                        desc = (const char *)&buf[desc_off];
                        desc_len = strnlen(desc, scan_len - desc_off);
                    }

                    if (file_len && desc && desc_len) {
                        char fn[256], expr[256];
                        size_t nfn = MIN(file_len, sizeof(fn) - 1);
                        size_t nexpr = MIN(desc_len, sizeof(expr) - 1);
                        memcpy(fn, file, nfn);
                        fn[nfn] = '\0';
                        memcpy(expr, desc, nexpr);
                        expr[nexpr] = '\0';
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: fw_break0_assert line=%u file=\"%s\" expr=\"%s\"\n",
                                      line, fn, expr);
                        assert_logged = true;
                    }
                }

                if (!assert_logged) {
                    size_t file_off = 0;
                    size_t file_len = 0;
                    for (size_t i = base; i < scan_len; i++) {
                        unsigned char c = buf[i];
                        if (c < 0x20 || c >= 0x7f) {
                            continue;
                        }
                        size_t j = i;
                        while (j < scan_len) {
                            unsigned char cj = buf[j];
                            if (cj < 0x20 || cj >= 0x7f) {
                                break;
                            }
                            j++;
                        }
                        size_t len = j - i;
                        if (len >= 8) {
                            const char *s = (const char *)&buf[i];
                            bool has_path = false;
                            bool has_source = false;
                            for (size_t k = 0; k + 1 < len; k++) {
                                if (s[k] == ':' && s[k + 1] == '\\') {
                                    has_path = true;
                                    break;
                                }
                            }
                            for (size_t k = 0; k + 1 < len; k++) {
                                if (s[k] == '.' && (s[k + 1] == 'c' || s[k + 1] == 'h')) {
                                    has_source = true;
                                    break;
                                }
                            }
                            if (has_path || has_source) {
                                file_off = i;
                                file_len = len;
                                break;
                            }
                        }
                        i = j;
                    }

                    if (file_off) {
                        uint32_t line_guess = 0;
                        for (size_t back = 4; back <= 32 && file_off >= back; back += 4) {
                            uint32_t cand = ldl_le_p(&buf[file_off - back]);
                            if (cand && cand < 10000) {
                                line_guess = cand;
                                break;
                            }
                        }
                        size_t desc_off = file_off + file_len + 1;
                        size_t desc_len = 0;
                        if (desc_off < scan_len) {
                            const char *desc = (const char *)&buf[desc_off];
                            desc_len = strnlen(desc, scan_len - desc_off);
                        }
                        char fn[256] = { 0 };
                        char expr[256] = { 0 };
                        size_t nfn = MIN(file_len, sizeof(fn) - 1);
                        memcpy(fn, &buf[file_off], nfn);
                        if (desc_len) {
                            size_t nexpr = MIN(desc_len, sizeof(expr) - 1);
                            memcpy(expr, &buf[desc_off], nexpr);
                        }
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: fw_break0_assert line=%u file=\"%s\" expr=\"%s\"\n",
                                      line_guess, fn, expr);
                    }
                }
            } else if (status_guid_valid && has_status_header) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_break0_status_guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                              status_guid.data1, status_guid.data2, status_guid.data3,
                              status_guid.data4[0], status_guid.data4[1],
                              status_guid.data4[2], status_guid.data4[3],
                              status_guid.data4[4], status_guid.data4[5],
                              status_guid.data4[6], status_guid.data4[7]);
            }

            /* Fallback: print any longer printable runs inside the buffer. */
            size_t i = 0;
            while (i < scan_len) {
                while (i < scan_len) {
                    unsigned char c = buf[i];
                    if (c >= 0x20 && c < 0x7f) {
                        break;
                    }
                    i++;
                }
                size_t start = i;
                while (i < scan_len) {
                    unsigned char c = buf[i];
                    if (!(c >= 0x20 && c < 0x7f)) {
                        break;
                    }
                    i++;
                }
                size_t len = i - start;
                if (len >= 4) {
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

    {
        static int hob_dump_enabled = -1;
        if (hob_dump_enabled == -1) {
            const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP");
            hob_dump_enabled = (s && *s) ? 1 : 0;
        }
        if (hob_dump_enabled) {
            (void)ia64_fw_dump_efi_hobs(env_cpu(env), env->r[12]);
        }
    }

    if (gcd_dump_enabled && is_assert) {
        ia64_fw_dump_gcd_map_candidates(env);
    }

    if (abort_assert && is_assert) {
        CPUState *cs = env_cpu(env);
        cpu_abort(cs, "IA64: firmware ASSERT via break0 pc=%016" PRIx64, pc);
    }

    return;
}

#ifndef CONFIG_USER_ONLY
static void ia64_dbg_probe_dump_mem(CPUIA64State *env, uint64_t pc,
                                    const char *tag, uint64_t addr, int nbytes)
{
    if (nbytes <= 0) {
        return;
    }
    if (nbytes > 16384) {
        nbytes = 16384;
    }
    if (addr == 0) {
        return;
    }

    hwaddr pa;
    if (env->psr & IA64_PSR_DT) {
        pa = helper_tpa(env, addr);
    } else {
        pa = ia64_phys_mode_addr(addr);
    }

    g_autofree uint8_t *mem = g_malloc((size_t)nbytes);
    cpu_physical_memory_read(pa, mem, (size_t)nbytes);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "dbg_probe_%s pc=%016" PRIx64 " va=%016" PRIx64
                  " pa=%016" HWADDR_PRIx " bytes=%d\n",
                  tag, pc, addr, pa, nbytes);
    for (int off = 0; off < nbytes; off += 16) {
        char line[128];
        int pos = 0;
        pos += snprintf(line + pos, sizeof(line) - pos,
                        "  %016" HWADDR_PRIx ":", pa + (hwaddr)off);
        for (int j = 0; j < 16 && off + j < nbytes; j++) {
            pos += snprintf(line + pos, sizeof(line) - pos,
                            " %02x", mem[off + j]);
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
    }
}

static void ia64_dbg_peimage_probe(CPUIA64State *env, uint64_t pc)
{
    static int enabled = -1;
    static uint64_t last_pc;
    static int dump_enabled = -1;
    static uint64_t last_dump_base;

    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_PEIMAGE");
        enabled = (s && *s) ? 1 : 0;
        last_pc = UINT64_MAX;
        dump_enabled = getenv("QEMU_IA64_DBG_PEIMAGE_DUMP") ? 1 : 0;
        last_dump_base = UINT64_MAX;
    }
    if (!enabled || pc == last_pc) {
        return;
    }
    last_pc = pc;

    CPUState *cs = env_cpu(env);
    uint64_t base = pc & ~0xfffULL;
    uint64_t min = (base > (2ULL << 20)) ? (base - (2ULL << 20)) : 0;

    uint8_t hdr[0x200];
    uint8_t page[0x1000];
    for (uint64_t probe = base; probe >= min; probe -= 0x1000) {
        if (!ia64_fw_read_bytes_any(cs, probe, page, sizeof(page))) {
            if (probe == min) {
                break;
            }
            continue;
        }
        uint64_t pe_base = 0;
        uint32_t pe_off = 0;
        for (size_t off = 0; off + 0x40 <= sizeof(page); off++) {
            if (page[off] != 'M' || page[off + 1] != 'Z') {
                continue;
            }
            uint32_t cand_pe = ldl_le_p(&page[off + 0x3c]);
            if (cand_pe + 0x40 > sizeof(page) - off) {
                continue;
            }
            if (ldl_le_p(&page[off + cand_pe]) != 0x00004550) { /* "PE\0\0" */
                continue;
            }
            pe_base = probe + off;
            pe_off = cand_pe;
            break;
        }
        if (!pe_base) {
            if (probe == min) {
                break;
            }
            continue;
        }
        if (!ia64_fw_read_bytes_any(cs, pe_base, hdr, sizeof(hdr))) {
            continue;
        }
        if (hdr[0] != 'M' || hdr[1] != 'Z') {
            continue;
        }
        if (pe_off + 0x40 > sizeof(hdr)) {
            continue;
        }
        if (ldl_le_p(&hdr[pe_off]) != 0x00004550) { /* "PE\0\0" */
            continue;
        }

        uint16_t machine = lduw_le_p(&hdr[pe_off + 4]);
        uint16_t opt_size = lduw_le_p(&hdr[pe_off + 20]);
        uint32_t opt_off = pe_off + 24;
        if (opt_off + opt_size > sizeof(hdr)) {
            continue;
        }
        uint16_t magic = lduw_le_p(&hdr[opt_off]);
        uint64_t image_base = 0;
        uint32_t entry = 0;
        uint32_t size_of_image = 0;
        uint32_t debug_rva = 0;
        uint32_t debug_size = 0;

        if (magic == 0x20b) {
            entry = ldl_le_p(&hdr[opt_off + 0x10]);
            image_base = ldq_le_p(&hdr[opt_off + 0x18]);
            size_of_image = ldl_le_p(&hdr[opt_off + 0x38]);
            debug_rva = ldl_le_p(&hdr[opt_off + 0x70 + 6 * 8]);
            debug_size = ldl_le_p(&hdr[opt_off + 0x70 + 6 * 8 + 4]);
        } else if (magic == 0x10b) {
            entry = ldl_le_p(&hdr[opt_off + 0x10]);
            image_base = ldl_le_p(&hdr[opt_off + 0x1c]);
            size_of_image = ldl_le_p(&hdr[opt_off + 0x38]);
            debug_rva = ldl_le_p(&hdr[opt_off + 0x60 + 6 * 8]);
            debug_size = ldl_le_p(&hdr[opt_off + 0x60 + 6 * 8 + 4]);
        } else {
            continue;
        }

        qemu_log_mask(LOG_GUEST_ERROR,
                      "dbg_peimage pc=%016" PRIx64 " base=%016" PRIx64
                      " image_base=%016" PRIx64 " size=0x%x entry=0x%x machine=0x%x\n",
                      pc, pe_base, image_base, size_of_image, entry, machine);

        if (dump_enabled && size_of_image > 0 && pe_base != last_dump_base) {
            g_mkdir_with_parents("scratch/ia64_logs", 0755);
            g_autofree uint8_t *img = g_malloc(size_of_image);
            if (ia64_fw_read_bytes_any(cs, pe_base, img, size_of_image)) {
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch/ia64_logs/peimage_%016" PRIx64 ".bin",
                         pe_base);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    fwrite(img, 1, size_of_image, fp);
                    fclose(fp);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "dbg_peimage_dump pc=%016" PRIx64
                                  " base=%016" PRIx64 " size=0x%x file=%s\n",
                                  pc, pe_base, size_of_image, path);
                    last_dump_base = pe_base;
                }
            }
        }

        if (debug_rva && debug_size && debug_rva < size_of_image) {
            uint8_t dbghdr[0x1c];
            uint64_t dbg_va = pe_base + debug_rva;
            if (ia64_fw_read_bytes_any(cs, dbg_va, dbghdr, sizeof(dbghdr))) {
                uint32_t dbg_type = ldl_le_p(&dbghdr[12]);
                uint32_t dbg_rva = ldl_le_p(&dbghdr[20]);
                if (dbg_type == 2) { /* IMAGE_DEBUG_TYPE_CODEVIEW */
                    uint8_t cvhdr[256];
                    uint64_t cv_va = pe_base + dbg_rva;
                    if (ia64_fw_read_bytes_any(cs, cv_va, cvhdr, sizeof(cvhdr)) &&
                        memcmp(cvhdr, "RSDS", 4) == 0) {
                        char pdb[128];
                        size_t max = sizeof(pdb) - 1;
                        size_t i;
                        for (i = 24; i < sizeof(cvhdr) && i - 24 < max; i++) {
                            if (cvhdr[i] == '\0') {
                                break;
                            }
                            pdb[i - 24] = (char)cvhdr[i];
                        }
                        pdb[i - 24] = '\0';
                        if (pdb[0]) {
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "dbg_peimage pc=%016" PRIx64 " pdb=%s\n",
                                          pc, pdb);
                        }
                    }
                }
            }
        }
        break;
    }
}
#endif /* !CONFIG_USER_ONLY */

void HELPER(dbg_probe)(CPUIA64State *env, uint64_t pc, uint32_t ri)
{
    typedef struct {
        uint64_t pc;
        uint32_t count;
    } DbgProbeCount;

    static DbgProbeCount probes[64];
    static uint32_t nprobes;
    static int dbg_probe_limit = -1;
    static int dbg_str_enabled = -1;
    static int dbg_str_limit = -1;
    static int dbg_dump_enabled = -1;
    static int dbg_dump_bundles = -1;
    static int dbg_assert_buf_enabled = -1;
    static int dbg_assert_buf_len = -1;
    static int dbg_assert_buf_dump = -1;
    static int dbg_loop_trace = -1;
    static int dbg_loop_limit = -1;
    static uint32_t loop_hits_11ac0;
    static uint32_t loop_hits_12180;
    static uint32_t loop_hits_dxe;

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

    if (dbg_loop_trace == -1) {
        const char *s = getenv("QEMU_IA64_DBG_LOOP_TRACE");
        dbg_loop_trace = (s && *s) ? 1 : 0;
    }
    if (dbg_loop_limit == -1) {
        dbg_loop_limit = 256;
        const char *s = getenv("QEMU_IA64_DBG_LOOP_LIMIT");
        if (s && *s) {
            dbg_loop_limit = atoi(s);
        }
        if (dbg_loop_limit < 0) {
            dbg_loop_limit = 0;
        }
    }

    if (dbg_loop_trace) {
        if (pc == 0x10011ac0ULL || pc == 0x20011ac0ULL) {
            if (loop_hits_11ac0++ < (uint32_t)dbg_loop_limit) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "dbg_loop pc=%016" PRIx64 " hit=%u"
                              " r30=%016" PRIx64 " r31=%016" PRIx64
                              " r32=%016" PRIx64 " r33=%016" PRIx64
                              " r34=%016" PRIx64 " r35=%016" PRIx64
                              " ar.lc=%016" PRIx64 " pr=%016" PRIx64
                              " p14=%u p15=%u\n",
                              pc, loop_hits_11ac0,
                              env->r[30], env->r[31], env->r[32], env->r[33],
                              env->r[34], env->r[35],
                              env->ar[65], env->pr,
                              (unsigned)((env->pr >> 14) & 1),
                              (unsigned)((env->pr >> 15) & 1));
            }
        } else if (pc == 0x10012180ULL || pc == 0x20012180ULL) {
            if (loop_hits_12180++ < (uint32_t)dbg_loop_limit) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "dbg_loop pc=%016" PRIx64 " hit=%u"
                              " r30=%016" PRIx64 " r31=%016" PRIx64
                              " r32=%016" PRIx64 " r33=%016" PRIx64
                              " r34=%016" PRIx64 " r35=%016" PRIx64
                              " ar.lc=%016" PRIx64 " pr=%016" PRIx64
                              " p14=%u p15=%u\n",
                              pc, loop_hits_12180,
                              env->r[30], env->r[31], env->r[32], env->r[33],
                              env->r[34], env->r[35],
                              env->ar[65], env->pr,
                              (unsigned)((env->pr >> 14) & 1),
                              (unsigned)((env->pr >> 15) & 1));
            }
        } else if ((pc >= 0xffe255a0ULL && pc <= 0xffe25620ULL) ||
                   (pc >= 0xffe256a0ULL && pc <= 0xffe2572cULL)) {
            if (loop_hits_dxe++ < (uint32_t)dbg_loop_limit) {
                uint64_t ptr = env->r[12];
                uint8_t val = cpu_ldub_data_ra(env, ptr, GETPC());
                uint64_t next = cpu_ldq_data_ra(env, ptr + 8, GETPC());
                qemu_log_mask(LOG_GUEST_ERROR,
                              "dbg_loop_dxe pc=%016" PRIx64 " hit=%u"
                              " r12=%016" PRIx64 " byte=%02x next=%016" PRIx64
                              " ar.lc=%016" PRIx64 " pr=%016" PRIx64 "\n",
                              pc, loop_hits_dxe, ptr, val, next,
                              env->ar[65], env->pr);
            }
        }
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

    if (dbg_str_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_STR");
        dbg_str_enabled = (s && *s) ? 1 : 0;
    }
    if (dbg_str_limit == -1) {
        dbg_str_limit = 4;
        const char *s = getenv("QEMU_IA64_DBG_STR_LIMIT");
        if (s && *s) {
            dbg_str_limit = atoi(s);
        }
        if (dbg_str_limit < 0) {
            dbg_str_limit = 0;
        }
    }

    if (dbg_dump_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_DUMP_CODE");
        dbg_dump_enabled = (s && *s) ? 1 : 0;
    }
    if (dbg_dump_bundles == -1) {
        dbg_dump_bundles = 64;
        const char *s = getenv("QEMU_IA64_DBG_DUMP_BUNDLES");
        if (s && *s) {
            dbg_dump_bundles = atoi(s);
        }
        if (dbg_dump_bundles < 0) {
            dbg_dump_bundles = 0;
        }
        if (dbg_dump_bundles > 512) {
            dbg_dump_bundles = 512;
        }
    }

    if (dbg_assert_buf_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_ASSERT_BUF");
        dbg_assert_buf_enabled = (s && *s) ? 1 : 0;
    }
    if (dbg_assert_buf_len == -1) {
        dbg_assert_buf_len = 512;
        const char *s = getenv("QEMU_IA64_DBG_ASSERT_BUF_LEN");
        if (s && *s) {
            dbg_assert_buf_len = atoi(s);
        }
        if (dbg_assert_buf_len < 0) {
            dbg_assert_buf_len = 0;
        }
        if (dbg_assert_buf_len > 4096) {
            dbg_assert_buf_len = 4096;
        }
    }
    if (dbg_assert_buf_dump == -1) {
        const char *s = getenv("QEMU_IA64_DBG_ASSERT_BUF_DUMP");
        dbg_assert_buf_dump = (s && *s) ? 1 : 0;
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

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dbg_probe pc=%016" PRIx64 " ri=%u"
                      " psr=%016" PRIx64 " cfm=%016" PRIx64 " pr=%016" PRIx64 " depth=%u"
                      " last_br from=%016" PRIx64 " to=%016" PRIx64
                      " kind=%" PRIu64 " insn=%011" PRIx64
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
                      " r43=%016" PRIx64 " r44=%016" PRIx64
                      " r59=%016" PRIx64
                      " r62=%016" PRIx64
                      " r16=%016" PRIx64 " r17=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                      " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64
                      " r38=%016" PRIx64 " r39=%016" PRIx64 " r40=%016" PRIx64
                      " r41=%016" PRIx64 " r42=%016" PRIx64
                      " r47=%016" PRIx64 " r48=%016" PRIx64 " r49=%016" PRIx64
                      " b0=%016" PRIx64 " b6=%016" PRIx64 " b7=%016" PRIx64
                      " r45=%016" PRIx64 " r46=%016" PRIx64 "\n",
                      pc, ri,
                      env->psr, env->cfm, env->pr, env->rse_depth,
                      env->last_branch_from, env->last_branch_to,
                      env->last_branch_kind, env->last_branch_insn,
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
                      env->r[43], env->r[44],
                      env->r[59],
                      env->r[62],
                      env->r[16], env->r[17],
                      env->r[32], env->r[33], env->r[34], env->r[35],
                      env->r[36], env->r[37], env->r[38], env->r[39], env->r[40],
                      env->r[41], env->r[42],
                      env->r[47], env->r[48], env->r[49],
                      env->b[0], env->b[6], env->b[7], env->r[45], env->r[46]);
    } else {
        fprintf(stderr,
                "dbg_probe pc=%016" PRIx64 " ri=%u"
                " psr=%016" PRIx64 " cfm=%016" PRIx64 " pr=%016" PRIx64 " depth=%u"
                " last_br from=%016" PRIx64 " to=%016" PRIx64
                " kind=%" PRIu64 " insn=%011" PRIx64
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
                " r43=%016" PRIx64 " r44=%016" PRIx64
                " r59=%016" PRIx64
                " r62=%016" PRIx64
                " r16=%016" PRIx64 " r17=%016" PRIx64
                " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64
                " r38=%016" PRIx64 " r39=%016" PRIx64 " r40=%016" PRIx64
                " r41=%016" PRIx64 " r42=%016" PRIx64
                " r47=%016" PRIx64 " r48=%016" PRIx64 " r49=%016" PRIx64
                " b0=%016" PRIx64 " b6=%016" PRIx64 " b7=%016" PRIx64
                " r45=%016" PRIx64 " r46=%016" PRIx64 "\n",
                pc, ri,
                env->psr, env->cfm, env->pr, env->rse_depth,
                env->last_branch_from, env->last_branch_to,
                env->last_branch_kind, env->last_branch_insn,
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
                env->r[43], env->r[44],
                env->r[59],
                env->r[62],
                env->r[16], env->r[17],
                env->r[32], env->r[33], env->r[34], env->r[35],
                env->r[36], env->r[37], env->r[38], env->r[39], env->r[40],
                env->r[41], env->r[42],
                env->r[47], env->r[48], env->r[49],
                env->b[0], env->b[6], env->b[7], env->r[45], env->r[46]);
    }

#ifndef CONFIG_USER_ONLY
    static int dump_r8_len = -1;
    if (dump_r8_len == -1) {
        dump_r8_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R8");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r8_len = 64;
            } else {
                dump_r8_len = atoi(s);
                if (dump_r8_len <= 0) {
                    dump_r8_len = 64;
                }
            }
        }
    }
    if (dump_r8_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r8", env->r[8], dump_r8_len);
    }

    static int dump_r2_len = -1;
    if (dump_r2_len == -1) {
        dump_r2_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R2");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r2_len = 64;
            } else {
                dump_r2_len = atoi(s);
                if (dump_r2_len <= 0) {
                    dump_r2_len = 64;
                }
            }
        }
    }
    if (dump_r2_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r2", env->r[2], dump_r2_len);
    }

    static int dump_r32_len = -1;
    if (dump_r32_len == -1) {
        dump_r32_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R32");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r32_len = 64;
            } else {
                dump_r32_len = atoi(s);
                if (dump_r32_len <= 0) {
                    dump_r32_len = 64;
                }
            }
        }
    }
    if (dump_r32_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r32", env->r[32], dump_r32_len);
    }

    static int dump_r33_len = -1;
    if (dump_r33_len == -1) {
        dump_r33_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R33");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r33_len = 64;
            } else {
                dump_r33_len = atoi(s);
                if (dump_r33_len <= 0) {
                    dump_r33_len = 64;
                }
            }
        }
    }
    if (dump_r33_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r33", env->r[33], dump_r33_len);
    }

    static int dump_r34_len = -1;
    if (dump_r34_len == -1) {
        dump_r34_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R34");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r34_len = 64;
            } else {
                dump_r34_len = atoi(s);
                if (dump_r34_len <= 0) {
                    dump_r34_len = 64;
                }
            }
        }
    }
    if (dump_r34_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r34", env->r[34], dump_r34_len);
    }

    static int dump_r35_len = -1;
    if (dump_r35_len == -1) {
        dump_r35_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R35");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r35_len = 64;
            } else {
                dump_r35_len = atoi(s);
                if (dump_r35_len <= 0) {
                    dump_r35_len = 64;
                }
            }
        }
    }
    if (dump_r35_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r35", env->r[35], dump_r35_len);
    }

    static int dump_r36_len = -1;
    if (dump_r36_len == -1) {
        dump_r36_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R36");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r36_len = 64;
            } else {
                dump_r36_len = atoi(s);
                if (dump_r36_len <= 0) {
                    dump_r36_len = 64;
                }
            }
        }
    }
    if (dump_r36_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r36", env->r[36], dump_r36_len);
    }

    static int dump_r12_len = -1;
    if (dump_r12_len == -1) {
        dump_r12_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R12");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r12_len = 256;
            } else {
                dump_r12_len = atoi(s);
                if (dump_r12_len <= 0) {
                    dump_r12_len = 256;
                }
            }
        }
    }
    if (dump_r12_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r12", env->r[12], dump_r12_len);
    }

    static int dump_r30_len = -1;
    if (dump_r30_len == -1) {
        dump_r30_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R30");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r30_len = 64;
            } else {
                dump_r30_len = atoi(s);
                if (dump_r30_len <= 0) {
                    dump_r30_len = 64;
                }
            }
        }
    }
    if (dump_r30_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r30", env->r[30], dump_r30_len);
    }

    static int dump_r31_len = -1;
    if (dump_r31_len == -1) {
        dump_r31_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_R31");
        if (s && *s) {
            if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
                !strcmp(s, "yes")) {
                dump_r31_len = 64;
            } else {
                dump_r31_len = atoi(s);
                if (dump_r31_len <= 0) {
                    dump_r31_len = 64;
                }
            }
        }
    }
    if (dump_r31_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "r31", env->r[31], dump_r31_len);
    }

    static uint64_t dump_addr = UINT64_MAX;
    static int dump_addr_len = -1;
    if (dump_addr == UINT64_MAX) {
        dump_addr = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_ADDR");
        if (s && *s) {
            dump_addr = strtoull(s, NULL, 0);
        }
    }
    if (dump_addr_len == -1) {
        dump_addr_len = 0;
        const char *s = getenv("QEMU_IA64_DBG_PROBE_DUMP_ADDR_LEN");
        if (s && *s) {
            dump_addr_len = atoi(s);
        }
        if (dump_addr_len < 0) {
            dump_addr_len = 0;
        }
    }
    if (dump_addr_len > 0) {
        ia64_dbg_probe_dump_mem(env, pc, "addr", dump_addr, dump_addr_len);
    }

    static int hob_failfast = -1;
    if (hob_failfast == -1) {
        const char *s = getenv("QEMU_IA64_DBG_PROBE_HOB_FAILFAST");
        hob_failfast = (s && *s) ? 1 : 0;
    }
    if (hob_failfast) {
        CPUState *cs = env_cpu(env);
        uint64_t hob_ptr = env->r[33];
        if (!hob_ptr && env->r[12]) {
            hwaddr pa = (env->psr & IA64_PSR_DT) ?
                helper_tpa(env, env->r[12]) : ia64_phys_mode_addr(env->r[12]);
            uint8_t buf[8];
            if (cpu_memory_rw_debug(cs, pa, buf, sizeof(buf), false) == 0) {
                hob_ptr = ldq_le_p(buf);
            }
        }
        if (hob_ptr) {
            uint64_t cur = hob_ptr;
            for (int iter = 0; iter < 256; iter++) {
                hwaddr pa = (env->psr & IA64_PSR_DT) ?
                    helper_tpa(env, cur) : ia64_phys_mode_addr(cur);
                uint8_t h[8];
                if (cpu_memory_rw_debug(cs, pa, h, sizeof(h), false) != 0) {
                    break;
                }
                uint16_t type = lduw_le_p(&h[0]);
                uint16_t len = lduw_le_p(&h[2]);
                if (len < 8 || (len & 7)) {
                    cpu_abort(cs,
                              "IA64: HOB failfast pc=%016" PRIx64
                              " hob=%016" PRIx64 " type=%04x len=%04x",
                              pc, cur, type, len);
                }
                if (type == 0xffff) {
                    break;
                }
                cur += len;
                if (cur - hob_ptr > (1U << 20)) {
                    break;
                }
            }
        }
    }

    ia64_dbg_peimage_probe(env, pc);
#endif /* !CONFIG_USER_ONLY */

    if (dbg_assert_buf_enabled && dbg_assert_buf_len > 0) {
        /*
         * Heuristic: DebugAssert() implementations typically build an
         * EFI_DEBUG_ASSERT_DATA blob on the stack. Scan a small window
         * above the current stack pointer for printable strings so asserts
         * can be identified even when the status code path is not wired up.
         */
        CPUState *cs = env_cpu(env);
        uint64_t base = env->r[12] + 16;
        uint8_t buf[4096];
        size_t want = (size_t)dbg_assert_buf_len;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        if (want > 0 && cpu_memory_rw_debug(cs, base, buf, want, false) == 0) {
            if (dbg_assert_buf_dump) {
                g_mkdir_with_parents("scratch/ia64_logs", 0755);
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch/ia64_logs/dbg_assert_buf_%016" PRIx64 ".bin", pc);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    fwrite(buf, 1, want, fp);
                    fclose(fp);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "dbg_assert_buf_dump pc=%016" PRIx64 " file=%s bytes=%zu\n",
                                  pc, path, want);
                }
            }

            size_t i = 0;
            while (i < want) {
                while (i < want) {
                    unsigned char c = buf[i];
                    if (c >= 0x20 && c < 0x7f) {
                        break;
                    }
                    i++;
                }
                size_t start = i;
                while (i < want) {
                    unsigned char c = buf[i];
                    if (!(c >= 0x20 && c < 0x7f)) {
                        break;
                    }
                    i++;
                }
                size_t len = i - start;
                if (len >= 6) {
                    char s[256];
                    size_t n = len < (sizeof(s) - 1) ? len : (sizeof(s) - 1);
                    memcpy(s, &buf[start], n);
                    s[n] = '\0';
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "dbg_assert_buf pc=%016" PRIx64
                                  " sp=%016" PRIx64 " addr=%016" PRIx64
                                  " +0x%zx \"%s\"\n",
                                  pc, env->r[12], base, start, s);
                }
            }
        }
    }

    if (dbg_dump_enabled && dbg_dump_bundles > 0 && probes[idx].count == 1) {
        CPUState *cs = env_cpu(env);
        uint64_t base = pc & ~0xFULL;
        uint64_t start = base;
        if (dbg_dump_bundles > 16) {
            uint64_t back = (uint64_t)(dbg_dump_bundles / 4) * 16ULL;
            if (start >= back) {
                start -= back;
            } else {
                start = 0;
            }
        }

        g_mkdir_with_parents("scratch/ia64_logs", 0755);
        char path[256];
        snprintf(path, sizeof(path), "scratch/ia64_logs/dbg_code_%016" PRIx64 ".bin", base);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            for (int i = 0; i < dbg_dump_bundles; i++) {
                uint8_t bundle[16];
                uint64_t bpc = start + (uint64_t)i * 16;
                if (cpu_memory_rw_debug(cs, bpc, bundle, sizeof(bundle), false) != 0) {
                    break;
                }
                fwrite(bundle, 1, sizeof(bundle), fp);
            }
            fclose(fp);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "dbg_dump_code pc=%016" PRIx64 " start=%016" PRIx64
                          " bundles=%d file=%s\n",
                          pc, start, dbg_dump_bundles, path);
        }
    }

    if (dbg_str_enabled && dbg_str_limit > 0) {
        CPUState *cs = env_cpu(env);
        struct {
            const char *name;
            uint64_t val;
        } cand[] = {
            { "r28", env->r[28] }, { "r29", env->r[29] }, { "r30", env->r[30] }, { "r31", env->r[31] },
            { "r32", env->r[32] }, { "r33", env->r[33] }, { "r34", env->r[34] }, { "r35", env->r[35] },
            { "r36", env->r[36] }, { "r37", env->r[37] }, { "r38", env->r[38] }, { "r39", env->r[39] },
            { "r40", env->r[40] }, { "r41", env->r[41] }, { "r42", env->r[42] }, { "r43", env->r[43] },
            { "r8", env->r[8] }, { "r9", env->r[9] }, { "r10", env->r[10] }, { "r11", env->r[11] },
        };

        int printed = 0;
        for (size_t i = 0; i < ARRAY_SIZE(cand) && printed < dbg_str_limit; i++) {
            uint64_t raw = cand[i].val;
            if (raw == 0) {
                continue;
            }

            uint64_t addrs[8];
            size_t naddrs = 0;
            addrs[naddrs++] = raw;
            /*
             * Some firmware build environments pass small offsets rather than
             * full pointers; try common FV bases used by the xenipf firmware.
             */
            if (raw < 0x200000) {
                addrs[naddrs++] = 0x00000000ffe00000ULL + raw; /* variable FV */
                addrs[naddrs++] = 0x00000000ffe20000ULL + raw; /* north FV */
                addrs[naddrs++] = 0x00000000ff600000ULL + raw; /* south FV */
            }

            for (size_t a = 0; a < naddrs && printed < dbg_str_limit; a++) {
                uint64_t addr = addrs[a];
                if (addr < 0x1000) {
                    continue;
                }

                char out[256];
                size_t out_len = 0;
                for (size_t j = 0; j + 1 < sizeof(out); j++) {
                    uint8_t b = 0;
                    if (cpu_memory_rw_debug(cs, addr + j, &b, 1, false) != 0) {
                        break;
                    }
                    out[j] = (char)b;
                    if (b == '\0') {
                        out_len = j;
                        break;
                    }
                }
                if (out_len < 4) {
                    continue;
                }

                size_t printable = 0;
                size_t alnum = 0;
                for (size_t j = 0; j < out_len; j++) {
                    unsigned char c = (unsigned char)out[j];
                    if (c == '\n' || c == '\r' || c == '\t') {
                        printable++;
                        continue;
                    }
                    if (c >= 0x20 && c < 0x7f) {
                        printable++;
                        if ((c >= '0' && c <= '9') ||
                            (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z')) {
                            alnum++;
                        }
                        continue;
                    }
                }
                if (printable < out_len * 9 / 10) {
                    continue;
                }
                if (alnum < 2) {
                    continue;
                }

                out[out_len] = '\0';
                qemu_log_mask(LOG_GUEST_ERROR,
                              "dbg_str pc=%016" PRIx64 " %s=%016" PRIx64
                              " addr=%016" PRIx64 " \"%s\"\n",
                              pc, cand[i].name, raw, addr, out);
                printed++;
            }
        }
    }

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

void HELPER(dbg_ar_lc_store)(CPUIA64State *env, uint64_t pc, uint64_t val)
{
    static int dbg_inited;
    static int dbg_enabled;

    if (!dbg_inited) {
        const char *s = getenv("QEMU_IA64_DBG_AR_LC");
        dbg_enabled = (s && *s && strcmp(s, "0") != 0);
        dbg_inited = 1;
    }
    if (!dbg_enabled) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "dbg_ar_lc_store pc=%016" PRIx64
                  " val=%016" PRIx64
                  " r1=%016" PRIx64 " r2=%016" PRIx64
                  " r3=%016" PRIx64 " r8=%016" PRIx64
                  " ar.lc(old)=%016" PRIx64 "\n",
                  pc, val, env->r[1], env->r[2], env->r[3], env->r[8],
                  env->ar[65]);
}

void HELPER(dbg_cmp)(CPUIA64State *env, uint64_t pc, uint64_t lhs, uint64_t rhs,
                     uint32_t p1, uint32_t p2)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "dbg_cmp pc=%016" PRIx64 " p1=%u p2=%u lhs=%016" PRIx64 " rhs=%016" PRIx64
                  " pr=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                  pc, p1, p2, lhs, rhs, env->pr, env->cfm);
}

void HELPER(dbg_cmp_post)(CPUIA64State *env, uint64_t pc, uint64_t lhs, uint64_t rhs,
                          uint32_t p1, uint32_t p2)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "dbg_cmp_post pc=%016" PRIx64 " p1=%u p2=%u lhs=%016" PRIx64
                  " rhs=%016" PRIx64 " pr=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                  pc, p1, p2, lhs, rhs, env->pr, env->cfm);
}

void HELPER(dbg_bunit_pred)(CPUIA64State *env, uint64_t pc, uint32_t qp)
{
    uint64_t pr = env->pr;
    uint64_t qp_val = (qp == 0) ? 1ULL : ((pr >> qp) & 1ULL);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "dbg_bpred pc=%016" PRIx64 " qp=%u qp_val=%" PRIu64
                  " pr=%016" PRIx64 "\n",
                  pc, qp, qp_val, pr);
}

void HELPER(dbg_bret)(CPUIA64State *env, uint64_t pc, uint32_t b2)
{
    uint64_t tgt = env->b[b2] & ~0xFULL;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "dbg_bret pc=%016" PRIx64 " b%u=%016" PRIx64
                  " kind=%" PRIu64 " depth=%u cfm=%016" PRIx64
                  " pfs=%016" PRIx64 "\n",
                  pc, b2, tgt, env->last_b0_write_kind & 0xff,
                  env->rse_depth, env->cfm, env->ar[64]);
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
     * alloc returns the current ar.pfs value in the destination register and
     * updates CFM. ar.pfs itself is set up by br.call/brl.call and must not
     * be overwritten here (see SKI's allocEx + cfmWrt sequencing).
     */
    uint64_t old_pfs = env->ar[64]; /* ar.pfs */
    uint64_t old_cfm = env->cfm;
    uint8_t old_sof = old_cfm & 0x7f;

    /*
     * Preserve the rotating-base fields (RRB*) and only update SOF/SOL/SOR.
     * (SOF/SOL/SOR live in bits 0..17 in our CFM layout.)
     */
    uint64_t keep = old_cfm & ~((1ULL << 18) - 1);

    uint8_t new_sof = sof & 0x7f;
    env->cfm = keep |
               new_sof |
               ((sol & 0x7f) << 7) |
               ((sor & 0xf) << 14);

    int64_t growth = (int64_t)new_sof - (int64_t)old_sof;
    if (growth != 0) {
        uint64_t bsp = ia64_rse_get_bsp(env);
        bsp = ia64_rse_skip_regs(bsp, growth);
        env->ar[IA64_AR_BSP] = bsp;
        ia64_rse_update_loadrs(env, bsp);
    }

    /*
     * New stacked regs are architecturally undefined. Preserve existing values
     * so firmware entry paths that pre-seed stacked registers behave like real
     * hardware (which does not zero them on alloc).
     */

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
    uint8_t last_nat = env->nat[32 + sor_regs - 1];
    for (uint32_t i = sor_regs - 1; i > 0; i--) {
        env->r[32 + i] = env->r[32 + i - 1];
        env->nat[32 + i] = env->nat[32 + i - 1];
    }
    env->r[32] = last;
    env->nat[32] = last_nat;
}

void HELPER(call)(CPUIA64State *env, uint64_t pc, uint64_t tgt)
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
    uint8_t tmp_nat[96] = { 0 };
    uint64_t bsp = ia64_rse_get_bsp(env);
    uint64_t dbg_pc = ia64_dbg_next_call_pc;
    static uint64_t watch_pc;
    static bool watch_pc_inited;
    if (!watch_pc_inited) {
        const char *s = getenv("QEMU_IA64_CALL_WATCH_PC");
        if (s && *s) {
            watch_pc = strtoull(s, NULL, 0) & ~0xFULL;
        }
        watch_pc_inited = true;
    }
    bool watch_hit = watch_pc && (((pc ^ watch_pc) & ~0xFULL) == 0);
    if (watch_hit) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "call_watch pre pc=%016" PRIx64 " tgt=%016" PRIx64
                      " cfm=%016" PRIx64 " depth=%u r37=%016" PRIx64
                      " r38=%016" PRIx64 "\n",
                      pc, tgt, env->cfm, env->rse_depth,
                      env->r[37], env->r[38]);
    }

    /*
     * Heuristic call tracing to catch mis-mapped argument registers during
     * bringup (e.g. pool allocators being called with a pointer-sized "Size").
     *
     * Enable with QEMU_IA64_TRACE_SUSP_CALLS=1.
     */
    static int susp_enabled = -1;
    static int susp_limit = -1;
    static int susp_count;
    static uint64_t trace_call_pc = UINT64_MAX;
    static int trace_call_abort = -1;
    static uint64_t trace_call_tgt = UINT64_MAX;
    static uint64_t trace_call_tgt_a1_min = UINT64_MAX;
    static int trace_call_tgt_abort = -1;
    static uint64_t trace_call_range_lo = UINT64_MAX;
    static uint64_t trace_call_range_hi = UINT64_MAX;
    static int trace_call_range_limit = -1;
    static int trace_call_range_abort = -1;
    static int trace_call_range_count;
    static uint64_t trace_call_match_a0 = UINT64_MAX;
    static uint64_t trace_call_match_a1 = UINT64_MAX;
    static int trace_call_match_abort = -1;
    static int trace_call_status = -1;
    static int trace_call_dump_peicore = -1;
    static int trace_call_dump_args = -1;
    if (susp_enabled == -1) {
        susp_enabled = getenv("QEMU_IA64_TRACE_SUSP_CALLS") ? 1 : 0;
    }
    if (trace_call_pc == UINT64_MAX) {
        trace_call_pc = 0;
        const char *s = getenv("QEMU_IA64_TRACE_CALL_PC");
        if (s && *s) {
            trace_call_pc = strtoull(s, NULL, 0);
        }
    }
    if (trace_call_tgt == UINT64_MAX) {
        trace_call_tgt = 0;
        const char *s = getenv("QEMU_IA64_TRACE_CALL_TGT");
        if (s && *s) {
            trace_call_tgt = strtoull(s, NULL, 0) & ~0xFULL;
        }
    }
    if (trace_call_tgt_a1_min == UINT64_MAX) {
        trace_call_tgt_a1_min = 0;
        const char *s = getenv("QEMU_IA64_TRACE_CALL_TGT_A1_MIN");
        if (s && *s) {
            trace_call_tgt_a1_min = strtoull(s, NULL, 0);
        }
    }
    if (trace_call_abort == -1) {
        trace_call_abort = getenv("QEMU_IA64_TRACE_CALL_PC_ABORT") ? 1 : 0;
    }
    if (trace_call_tgt_abort == -1) {
        trace_call_tgt_abort = getenv("QEMU_IA64_TRACE_CALL_TGT_ABORT") ? 1 : 0;
    }
    if (trace_call_range_lo == UINT64_MAX) {
        trace_call_range_lo = 0;
        trace_call_range_hi = 0;
        const char *s = getenv("QEMU_IA64_TRACE_CALL_RANGE");
        if (s && *s) {
            char *end = NULL;
            uint64_t lo = strtoull(s, &end, 0);
            if (end && (*end == '-' || *end == ':' || *end == ',')) {
                uint64_t hi = strtoull(end + 1, NULL, 0);
                if (hi < lo) {
                    uint64_t swap_tmp = lo;
                    lo = hi;
                    hi = swap_tmp;
                }
                trace_call_range_lo = lo;
                trace_call_range_hi = hi;
            } else if (lo) {
                trace_call_range_lo = lo;
                trace_call_range_hi = lo;
            }
        }
    }
    if (trace_call_range_limit == -1) {
        trace_call_range_limit = 64;
        const char *s = getenv("QEMU_IA64_TRACE_CALL_RANGE_LIMIT");
        if (s && *s) {
            trace_call_range_limit = atoi(s);
        }
        if (trace_call_range_limit < 0) {
            trace_call_range_limit = 0;
        }
    }
    if (trace_call_range_abort == -1) {
        trace_call_range_abort =
            getenv("QEMU_IA64_TRACE_CALL_RANGE_ABORT") ? 1 : 0;
    }
    if (trace_call_match_a0 == UINT64_MAX) {
        trace_call_match_a0 = 0;
        const char *s = getenv("QEMU_IA64_TRACE_CALL_MATCH_A0");
        if (s && *s) {
            trace_call_match_a0 = strtoull(s, NULL, 0);
        }
    }
    if (trace_call_match_a1 == UINT64_MAX) {
        trace_call_match_a1 = 0;
        const char *s = getenv("QEMU_IA64_TRACE_CALL_MATCH_A1");
        if (s && *s) {
            trace_call_match_a1 = strtoull(s, NULL, 0);
        }
    }
    if (trace_call_match_abort == -1) {
        trace_call_match_abort = getenv("QEMU_IA64_TRACE_CALL_MATCH_ABORT") ? 1 : 0;
    }
    if (trace_call_status == -1) {
        trace_call_status = getenv("QEMU_IA64_TRACE_CALL_STATUS") ? 1 : 0;
    }
    if (trace_call_dump_peicore == -1) {
        trace_call_dump_peicore =
            getenv("QEMU_IA64_TRACE_CALL_DUMP_PEICORE") ? 1 : 0;
    }
    if (trace_call_dump_args == -1) {
        trace_call_dump_args =
            getenv("QEMU_IA64_TRACE_CALL_DUMP_ARGS") ? 1 : 0;
    }
    if (susp_limit == -1) {
        susp_limit = 64;
        const char *s = getenv("QEMU_IA64_TRACE_SUSP_CALLS_LIMIT");
        if (s && *s) {
            susp_limit = atoi(s);
        }
        if (susp_limit < 0) {
            susp_limit = 0;
        }
    }
    static int susp_abort = -1;
    static int susp_dump_bundle = -1;
    static int susp_dump_code_bundles = -1;
    if (susp_abort == -1) {
        susp_abort = getenv("QEMU_IA64_TRACE_SUSP_CALLS_ABORT") ? 1 : 0;
    }
    if (susp_dump_bundle == -1) {
        susp_dump_bundle =
            getenv("QEMU_IA64_TRACE_SUSP_CALLS_DUMP_BUNDLE") ? 1 : 0;
    }
    if (susp_dump_code_bundles == -1) {
        susp_dump_code_bundles = 0;
        const char *s = getenv("QEMU_IA64_TRACE_SUSP_CALLS_DUMP_CODE");
        if (s && *s) {
            susp_dump_code_bundles = atoi(s);
        }
        if (susp_dump_code_bundles < 0) {
            susp_dump_code_bundles = 0;
        }
        if (susp_dump_code_bundles > 2048) {
            susp_dump_code_bundles = 2048;
        }
    }

    uint8_t call_out0 = 32 + sol;
    uint64_t call_a0 = (call_out0 < 128) ? env->r[call_out0] : 0;
    uint64_t call_a1 = (call_out0 + 1 < 128) ? env->r[call_out0 + 1] : 0;
    uint64_t call_a2 = (call_out0 + 2 < 128) ? env->r[call_out0 + 2] : 0;
    uint64_t call_a3 = (call_out0 + 3 < 128) ? env->r[call_out0 + 3] : 0;
    uint64_t call_a4 = (call_out0 + 4 < 128) ? env->r[call_out0 + 4] : 0;
    uint64_t call_a5 = (call_out0 + 5 < 128) ? env->r[call_out0 + 5] : 0;
    uint64_t call_a6 = (call_out0 + 6 < 128) ? env->r[call_out0 + 6] : 0;
    uint64_t call_a7 = (call_out0 + 7 < 128) ? env->r[call_out0 + 7] : 0;
    static uint64_t trace_pc;
    static bool trace_pc_inited;
    static bool trace_pc_logged;
    if (!trace_pc_inited) {
        const char *s = getenv("QEMU_IA64_CALL_TRACE_PC");
        if (s && *s) {
            trace_pc = strtoull(s, NULL, 0) & ~0xFULL;
        }
        trace_pc_inited = true;
    }
    bool trace_hit = trace_pc && (((pc ^ trace_pc) & ~0xFULL) == 0);
    if (trace_hit && !trace_pc_logged) {
        trace_pc_logged = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: call_trace pre pc=%016" PRIx64 " tgt=%016" PRIx64
                      " cfm=%016" PRIx64 " sof=%u sol=%u outs=%u out0=r%u"
                      " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                      " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64
                      " r38=%016" PRIx64 " r39=%016" PRIx64 " r40=%016" PRIx64
                      " a0=%016" PRIx64 " a1=%016" PRIx64 " a2=%016" PRIx64
                      " a3=%016" PRIx64 " a4=%016" PRIx64 " a5=%016" PRIx64
                      " a6=%016" PRIx64 " a7=%016" PRIx64
                      " b0=%016" PRIx64 " r1=%016" PRIx64 " r12=%016" PRIx64
                      " bsp=%016" PRIx64 " ar.pfs=%016" PRIx64 "\n",
                      pc, tgt, caller_cfm, sof, sol, outs, call_out0,
                      env->r[32], env->r[33], env->r[34], env->r[35],
                      env->r[36], env->r[37], env->r[38], env->r[39],
                      env->r[40],
                      call_a0, call_a1, call_a2, call_a3,
                      call_a4, call_a5, call_a6, call_a7,
                      env->b[0], env->r[1], env->r[12],
                      env->ar[IA64_AR_BSP], env->ar[IA64_AR_PFS]);
    }

    if (ia64_fw_pei_locate_trace_enabled()) {
        static int pei_trace_count;
        int limit = ia64_fw_pei_locate_trace_limit();
        if (limit == 0 || pei_trace_count < limit) {
            CPUState *cs = env_cpu(env);
            uint64_t ps_ptr = 0;
            if (ia64_fw_pei_get_ps_ptr(env, call_a0, &ps_ptr)) {
                uint64_t plabel = 0;
                uint64_t entry = 0;
                if (ia64_fw_read_u64(cs, ps_ptr + 0x28, &plabel) &&
                    plabel != 0 &&
                    ia64_fw_read_u64(cs, plabel, &entry) &&
                    entry != 0) {
                    uint64_t entry_phys = ia64_phys_mode_addr(entry);
                    bool entry_match = ((entry ^ tgt) & ~0xFULL) == 0 ||
                                       ((entry_phys ^ tgt) & ~0xFULL) == 0;
                    if (entry_match) {
                        uint8_t guid_bytes[16];
                        IA64EfiGuid guid;
                        bool guid_ok = false;
                        if (call_a1 &&
                            ia64_fw_read_bytes_any(cs, call_a1,
                                                   guid_bytes, sizeof(guid_bytes))) {
                            ia64_fw_guid_from_bytes(guid_bytes, &guid);
                            memcpy(env->fw_pei_locate_guid,
                                   guid_bytes, sizeof(guid_bytes));
                            env->fw_pei_locate_guid_valid = 1;
                            guid_ok = true;
                        } else {
                            env->fw_pei_locate_guid_valid = 0;
                        }
                        env->fw_pei_ps = ps_ptr;
                        env->fw_pei_locate_ret_pc = pc + 16;
                        env->fw_pei_locate_instance = call_a2;
                        env->fw_pei_locate_desc_ptr = call_a3;
                        env->fw_pei_locate_ppi_ptr = call_a4;
                        if (ia64_fw_pei_ps_dump_enabled()) {
                            ia64_fw_pei_dump_ps(env, ps_ptr, pc);
                        }

                        if (guid_ok) {
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: pei_locate_call pc=%016" PRIx64
                                          " tgt=%016" PRIx64 " ps=%016" PRIx64
                                          " plabel=%016" PRIx64 " entry=%016" PRIx64
                                          " inst=%016" PRIx64 " desc_ptr=%016" PRIx64
                                          " ppi_ptr=%016" PRIx64
                                          " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                                          pc, tgt, ps_ptr, plabel, entry,
                                          call_a2, call_a3, call_a4,
                                          guid.data1, guid.data2, guid.data3,
                                          guid.data4[0], guid.data4[1],
                                          guid.data4[2], guid.data4[3],
                                          guid.data4[4], guid.data4[5],
                                          guid.data4[6], guid.data4[7]);
                        } else {
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: pei_locate_call pc=%016" PRIx64
                                          " tgt=%016" PRIx64 " ps=%016" PRIx64
                                          " plabel=%016" PRIx64 " entry=%016" PRIx64
                                          " inst=%016" PRIx64 " desc_ptr=%016" PRIx64
                                          " ppi_ptr=%016" PRIx64
                                          " guid_ptr=%016" PRIx64 "\n",
                                          pc, tgt, ps_ptr, plabel, entry,
                                          call_a2, call_a3, call_a4, call_a1);
                        }
                        pei_trace_count++;
                    }
                }
            }
        }
    }

    if (ia64_fw_pei_install_trace_enabled()) {
        static int pei_install_count;
        int limit = ia64_fw_pei_install_trace_limit();
        if (limit == 0 || pei_install_count < limit) {
            CPUState *cs = env_cpu(env);
            uint64_t ps_ptr = 0;
            if (ia64_fw_pei_get_ps_ptr(env, call_a0, &ps_ptr)) {
                uint64_t plabel = 0;
                uint64_t entry = 0;
                if (ia64_fw_read_u64(cs, ps_ptr + 0x18, &plabel) &&
                    plabel != 0 &&
                    ia64_fw_read_u64(cs, plabel, &entry) &&
                    entry != 0 &&
                    ((entry ^ tgt) & ~0xFULL) == 0) {
                    uint64_t desc_ptr = call_a1;
                    uint64_t flags = 0;
                    uint64_t guid_ptr = 0;
                    uint64_t ppi_ptr = 0;
                    bool desc_ok = false;
                    if (desc_ptr) {
                        uint8_t desc[24];
                        if (ia64_fw_read_bytes_any(cs, desc_ptr,
                                                   desc, sizeof(desc))) {
                            flags = ldq_le_p(&desc[0]);
                            guid_ptr = ldq_le_p(&desc[8]);
                            ppi_ptr = ldq_le_p(&desc[16]);
                            desc_ok = true;
                        }
                    }
                    uint8_t guid_bytes[16];
                    IA64EfiGuid guid;
                    bool guid_ok = false;
                    if (desc_ok && guid_ptr &&
                        ia64_fw_read_bytes_any(cs, guid_ptr,
                                               guid_bytes, sizeof(guid_bytes))) {
                        ia64_fw_guid_from_bytes(guid_bytes, &guid);
                        memcpy(env->fw_pei_install_guid,
                               guid_bytes, sizeof(guid_bytes));
                        env->fw_pei_install_guid_valid = 1;
                        guid_ok = true;
                    } else {
                        env->fw_pei_install_guid_valid = 0;
                    }
                    env->fw_pei_ps = ps_ptr;
                    env->fw_pei_install_ret_pc = pc + 16;
                    env->fw_pei_install_desc_ptr = desc_ptr;
                    if (ia64_fw_pei_ps_dump_enabled()) {
                        ia64_fw_pei_dump_ps(env, ps_ptr, pc);
                    }

                    if (guid_ok) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: pei_install_call pc=%016" PRIx64
                                      " tgt=%016" PRIx64 " ps=%016" PRIx64
                                      " plabel=%016" PRIx64 " entry=%016" PRIx64
                                      " desc=%016" PRIx64 " flags=%016" PRIx64
                                      " ppi=%016" PRIx64
                                      " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                                      pc, tgt, ps_ptr, plabel, entry,
                                      desc_ptr, flags, ppi_ptr,
                                      guid.data1, guid.data2, guid.data3,
                                      guid.data4[0], guid.data4[1],
                                      guid.data4[2], guid.data4[3],
                                      guid.data4[4], guid.data4[5],
                                      guid.data4[6], guid.data4[7]);
                    } else {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: pei_install_call pc=%016" PRIx64
                                      " tgt=%016" PRIx64 " ps=%016" PRIx64
                                      " plabel=%016" PRIx64 " entry=%016" PRIx64
                                      " desc=%016" PRIx64 " flags=%016" PRIx64
                                      " ppi=%016" PRIx64 " guid_ptr=%016" PRIx64
                                      " desc_ok=%d\n",
                                      pc, tgt, ps_ptr, plabel, entry,
                                      desc_ptr, flags, ppi_ptr, guid_ptr,
                                      desc_ok ? 1 : 0);
                    }
                    pei_install_count++;
                }
            }
        }
    }

    if (ia64_fw_pei_install_pplist_trace_enabled()) {
        uint64_t match_addr = ia64_fw_pei_install_pplist_match_addr();
        uint64_t fw_ppi = env->fw_pei_ppi;
        uint64_t fw_ppi_phys = fw_ppi & ((1ULL << 61) - 1);
        uint64_t desc_phys = call_a1 & ((1ULL << 61) - 1);
        bool match = false;
        if (match_addr) {
            uint64_t match_phys = match_addr & ((1ULL << 61) - 1);
            match = call_a1 &&
                    (call_a1 == match_addr || desc_phys == match_phys);
        } else {
            match = call_a1 && fw_ppi &&
                    (call_a1 == fw_ppi || desc_phys == fw_ppi_phys);
        }
        if (match) {
            static bool logged;
            if (!logged) {
                logged = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_install_pplist pc=%016" PRIx64
                              " tgt=%016" PRIx64 " desc=%016" PRIx64
                              " fw_ppi=%016" PRIx64 "\n",
                              pc, tgt, call_a1, fw_ppi);
                ia64_fw_dump_pei_ppi_list(env, call_a1);
            }
        }
    }

    if (trace_call_match_a0 &&
        call_a0 == trace_call_match_a0 &&
        (!trace_call_match_a1 || call_a1 == trace_call_match_a1)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: trace_call_match pc=%016" PRIx64 " tgt=%016" PRIx64
                      " cfm=%016" PRIx64 " sof=%u sol=%u outs=%u out0=r%u"
                      " a0=%016" PRIx64 " a1=%016" PRIx64 " a2=%016" PRIx64
                      " a3=%016" PRIx64 " a4=%016" PRIx64 " a5=%016" PRIx64
                      " a6=%016" PRIx64 " a7=%016" PRIx64
                      " r1=%016" PRIx64 " r12=%016" PRIx64 " b0=%016" PRIx64
                      "\n",
                      pc, tgt, caller_cfm, sof, sol, outs, call_out0,
                      call_a0, call_a1, call_a2, call_a3,
                      call_a4, call_a5, call_a6, call_a7,
                      env->r[1], env->r[12], env->b[0]);
        if (trace_call_match_abort) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: trace_call_match abort pc=%016" PRIx64 "\n", pc);
            abort();
        }
    }

    if (trace_call_range_lo && trace_call_range_hi) {
        uint64_t pc_cmp = pc;
        if (trace_call_range_hi < (1ULL << 61)) {
            pc_cmp = pc & ((1ULL << 61) - 1);
        }
        bool in_range = (pc_cmp >= trace_call_range_lo &&
                         pc_cmp <= trace_call_range_hi);
        if (in_range &&
            (trace_call_range_limit == 0 ||
             trace_call_range_count < trace_call_range_limit)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: trace_call_range pc=%016" PRIx64
                          " tgt=%016" PRIx64 " cfm=%016" PRIx64
                          " sof=%u sol=%u outs=%u out0=r%u"
                          " a0=%016" PRIx64 " a1=%016" PRIx64
                          " a2=%016" PRIx64 " a3=%016" PRIx64
                          " a4=%016" PRIx64 " a5=%016" PRIx64
                          " a6=%016" PRIx64 " a7=%016" PRIx64
                          " r1=%016" PRIx64 " r12=%016" PRIx64
                          " b0=%016" PRIx64 "\n",
                          pc, tgt, caller_cfm, sof, sol, outs, call_out0,
                          call_a0, call_a1, call_a2, call_a3,
                          call_a4, call_a5, call_a6, call_a7,
                          env->r[1], env->r[12], env->b[0]);
            trace_call_range_count++;
            if (trace_call_range_abort) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: trace_call_range abort pc=%016" PRIx64 "\n",
                              pc);
                abort();
            }
        }
    }

    if (trace_call_tgt && (tgt & ~0xFULL) == trace_call_tgt) {
        if (call_a1 >= trace_call_tgt_a1_min) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: trace_call_tgt pc=%016" PRIx64 " tgt=%016" PRIx64
                          " cfm=%016" PRIx64 " sof=%u sol=%u outs=%u out0=r%u"
                          " a0=%016" PRIx64 " a1=%016" PRIx64 " a2=%016" PRIx64
                          " a3=%016" PRIx64 " a4=%016" PRIx64 " a5=%016" PRIx64
                          " a6=%016" PRIx64 " a7=%016" PRIx64
                          " r1=%016" PRIx64 " r12=%016" PRIx64 " b0=%016" PRIx64
                          "\n",
                          pc, tgt, caller_cfm, sof, sol, outs, call_out0,
                          call_a0, call_a1, call_a2, call_a3,
                          call_a4, call_a5, call_a6, call_a7,
                          env->r[1], env->r[12], env->b[0]);

            if (susp_dump_bundle) {
                CPUState *cs = env_cpu(env);
                uint64_t base = pc & ~0xFULL;
                uint8_t bundle[16];
                if (cpu_memory_rw_debug(cs, base, bundle, sizeof(bundle), false) == 0) {
                    uint64_t low = 0, high = 0;
                    memcpy(&low, &bundle[0], sizeof(low));
                    memcpy(&high, &bundle[8], sizeof(high));
                    uint8_t tmpl = low & 0x1f;
                    uint64_t s0 = (low >> 5) & 0x1ffffffffffULL;
                    uint64_t s1 = ((low >> 46) | (high << 18)) & 0x1ffffffffffULL;
                    uint64_t s2 = (high >> 23) & 0x1ffffffffffULL;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: trace_call_tgt_bundle pc=%016" PRIx64
                                  " low=%016" PRIx64 " high=%016" PRIx64
                                  " tmpl=%02x s0=%011" PRIx64 " s1=%011" PRIx64
                                  " s2=%011" PRIx64 "\n",
                                  base, low, high, tmpl, s0, s1, s2);
                }
            }

            if (susp_dump_code_bundles > 0) {
                CPUState *cs = env_cpu(env);
                uint64_t base = pc & ~0xFULL;
                uint64_t start = base;
                if (susp_dump_code_bundles > 16) {
                    uint64_t back =
                        (uint64_t)(susp_dump_code_bundles / 4) * 16ULL;
                    if (start >= back) {
                        start -= back;
                    } else {
                        start = 0;
                    }
                }
                g_mkdir_with_parents("scratch/ia64_logs", 0755);
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch/ia64_logs/trace_tgt_call_code_%016" PRIx64 ".bin",
                         base);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    for (int i = 0; i < susp_dump_code_bundles; i++) {
                        uint8_t bundle[16];
                        uint64_t bpc = start + (uint64_t)i * 16;
                        if (cpu_memory_rw_debug(cs, bpc, bundle, sizeof(bundle),
                                                false) != 0) {
                            break;
                        }
                        fwrite(bundle, 1, sizeof(bundle), fp);
                    }
                    fclose(fp);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: trace_tgt_call_dump_code pc=%016" PRIx64
                                  " start=%016" PRIx64 " bundles=%d file=%s\n",
                                  base, start, susp_dump_code_bundles, path);
                }
            }

            if (susp_dump_code_bundles > 0 && tgt) {
                CPUState *cs = env_cpu(env);
                uint64_t base = tgt & ~0xFULL;
                uint64_t start = base;
                if (susp_dump_code_bundles > 16) {
                    uint64_t back =
                        (uint64_t)(susp_dump_code_bundles / 4) * 16ULL;
                    if (start >= back) {
                        start -= back;
                    } else {
                        start = 0;
                    }
                }
                g_mkdir_with_parents("scratch/ia64_logs", 0755);
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch/ia64_logs/trace_tgt_call_tgt_code_%016" PRIx64 ".bin",
                         base);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    for (int i = 0; i < susp_dump_code_bundles; i++) {
                        uint8_t bundle[16];
                        uint64_t bpc = start + (uint64_t)i * 16;
                        if (cpu_memory_rw_debug(cs, bpc, bundle, sizeof(bundle),
                                                false) != 0) {
                            break;
                        }
                        fwrite(bundle, 1, sizeof(bundle), fp);
                    }
                    fclose(fp);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: trace_tgt_call_dump_tgt_code pc=%016" PRIx64
                                  " tgt=%016" PRIx64 " start=%016" PRIx64
                                  " bundles=%d file=%s\n",
                                  pc, base, start, susp_dump_code_bundles, path);
                }
            }

            if (trace_call_tgt_abort) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: trace_call_tgt abort pc=%016" PRIx64 "\n", pc);
                abort();
            }
        }
    }

    if (trace_call_pc && pc == trace_call_pc) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: trace_call pc=%016" PRIx64 " tgt=%016" PRIx64
                      " cfm=%016" PRIx64 " sof=%u sol=%u outs=%u out0=r%u"
                      " a0=%016" PRIx64 " a1=%016" PRIx64 " a2=%016" PRIx64
                      " a3=%016" PRIx64 " a4=%016" PRIx64 " a5=%016" PRIx64
                      " a6=%016" PRIx64 " a7=%016" PRIx64
                      " r1=%016" PRIx64 " r12=%016" PRIx64 " b0=%016" PRIx64
                      " r30=%016" PRIx64 " r31=%016" PRIx64 " b7=%016" PRIx64
                      "\n",
                      pc, tgt, caller_cfm, sof, sol, outs, call_out0,
                      call_a0, call_a1, call_a2, call_a3,
                      call_a4, call_a5, call_a6, call_a7,
                      env->r[1], env->r[12], env->b[0],
                      env->r[30], env->r[31], env->b[7]);

        if (call_a0) {
            CPUState *cs = env_cpu(env);
            uint8_t raw_obj[32];
            if (cpu_memory_rw_debug(cs, call_a0, raw_obj, sizeof(raw_obj), false) ==
                0) {
                uint64_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
                memcpy(&w0, &raw_obj[0], sizeof(w0));
                memcpy(&w1, &raw_obj[8], sizeof(w1));
                memcpy(&w2, &raw_obj[16], sizeof(w2));
                memcpy(&w3, &raw_obj[24], sizeof(w3));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: trace_call a0_mem %016" PRIx64
                              " %016" PRIx64 " %016" PRIx64 " %016" PRIx64
                              "\n",
                              w0, w1, w2, w3);
            }
        }
        if (trace_call_dump_args) {
            CPUState *cs = env_cpu(env);
            uint64_t args[] = { call_a0, call_a1, call_a2, call_a3, call_a4 };
            const char *names[] = { "a0", "a1", "a2", "a3", "a4" };
            for (size_t i = 0; i < ARRAY_SIZE(args); i++) {
                uint64_t arg = args[i];
                if (!arg) {
                    continue;
                }
                uint8_t raw_obj[32];
                if (cpu_memory_rw_debug(cs, arg, raw_obj, sizeof(raw_obj),
                                        false) == 0) {
                    uint64_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
                    memcpy(&w0, &raw_obj[0], sizeof(w0));
                    memcpy(&w1, &raw_obj[8], sizeof(w1));
                    memcpy(&w2, &raw_obj[16], sizeof(w2));
                    memcpy(&w3, &raw_obj[24], sizeof(w3));
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: trace_call %s_mem %016" PRIx64
                                  " %016" PRIx64 " %016" PRIx64 " %016" PRIx64
                                  "\n",
                                  names[i], w0, w1, w2, w3);
                }
            }
        }

        if (trace_call_dump_peicore && call_a0) {
            CPUState *cs = env_cpu(env);
            uint64_t core = call_a0 - 8;
            hwaddr core_phys = ia64_phys_mode_addr(core);
            uint8_t hdr[16];
            if (cpu_memory_rw_debug(cs, core_phys, hdr, sizeof(hdr), false) == 0) {
                uint32_t sig = ldl_le_p(&hdr[0]);
                uint64_t ps_ptr = ldq_le_p(&hdr[8]);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: trace_call peicore base=%016" PRIx64
                              " sig=0x%08x ps=%016" PRIx64 "\n",
                              core, sig, ps_ptr);
                const uint64_t dispatch_off = 0x438;
                uint8_t disp[0x38];
                if (cpu_memory_rw_debug(cs, core_phys + dispatch_off,
                                        disp, sizeof(disp), false) == 0) {
                    uint8_t cur_peim = disp[0];
                    uint8_t cur_fv = disp[1];
                    uint64_t dispatched = ldq_le_p(&disp[8]);
                    uint64_t prev = ldq_le_p(&disp[16]);
                    uint64_t cur_peim_addr = ldq_le_p(&disp[24]);
                    uint64_t cur_fv_addr = ldq_le_p(&disp[32]);
                    uint64_t boot_fv_addr = ldq_le_p(&disp[40]);
                    uint64_t findfv = ldq_le_p(&disp[48]);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: trace_call peicore_dispatch cur_peim=%u"
                                  " cur_fv=%u dispatched=%016" PRIx64
                                  " prev=%016" PRIx64 "\n",
                                  cur_peim, cur_fv, dispatched, prev);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: trace_call peicore_dispatch cur_peim_addr=%016"
                                  PRIx64 " cur_fv_addr=%016" PRIx64
                                  " boot_fv_addr=%016" PRIx64
                                  " findfv=%016" PRIx64 "\n",
                                  cur_peim_addr, cur_fv_addr,
                                  boot_fv_addr, findfv);
                }
                uint8_t ppi_hdr[32];
                if (cpu_memory_rw_debug(cs, core_phys + 0x10,
                                        ppi_hdr, sizeof(ppi_hdr), false) == 0) {
                    int64_t ppi_end = (int64_t)ldq_le_p(&ppi_hdr[0]);
                    int64_t notify_end = (int64_t)ldq_le_p(&ppi_hdr[8]);
                    int64_t dispatch_end = (int64_t)ldq_le_p(&ppi_hdr[16]);
                    int64_t max_end = ppi_end;
                    if (notify_end > max_end) {
                        max_end = notify_end;
                    }
                    if (dispatch_end > max_end) {
                        max_end = dispatch_end;
                    }
                    if (max_end < 0 || max_end > 256) {
                        max_end = 0;
                    }
                    uint64_t dispatch_off_dyn =
                        16 + (5ULL * 8) + (uint64_t)(max_end + 1) * 8;
                    uint8_t disp_dyn[0x38];
                    if (cpu_memory_rw_debug(cs, core_phys + dispatch_off_dyn,
                                            disp_dyn, sizeof(disp_dyn), false) == 0) {
                        uint8_t cur_peim = disp_dyn[0];
                        uint8_t cur_fv = disp_dyn[1];
                        uint64_t dispatched = ldq_le_p(&disp_dyn[8]);
                        uint64_t prev = ldq_le_p(&disp_dyn[16]);
                        uint64_t cur_peim_addr = ldq_le_p(&disp_dyn[24]);
                        uint64_t cur_fv_addr = ldq_le_p(&disp_dyn[32]);
                        uint64_t boot_fv_addr = ldq_le_p(&disp_dyn[40]);
                        uint64_t findfv = ldq_le_p(&disp_dyn[48]);
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: trace_call peicore_dispatch_dyn"
                                      " off=0x%04" PRIx64 " cur_peim=%u cur_fv=%u"
                                      " dispatched=%016" PRIx64 " prev=%016" PRIx64 "\n",
                                      dispatch_off_dyn, cur_peim, cur_fv,
                                      dispatched, prev);
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: trace_call peicore_dispatch_dyn"
                                      " cur_peim_addr=%016" PRIx64
                                      " cur_fv_addr=%016" PRIx64
                                      " boot_fv_addr=%016" PRIx64
                                      " findfv=%016" PRIx64 "\n",
                                      cur_peim_addr, cur_fv_addr,
                                      boot_fv_addr, findfv);
                    }
                }
                uint8_t raw[0x80];
                if (cpu_memory_rw_debug(cs, core_phys + 0x420,
                                        raw, sizeof(raw), false) == 0) {
                    for (size_t i = 0; i < sizeof(raw); i += 16) {
                        char hex[3 * 16 + 1];
                        for (size_t j = 0; j < 16; j++) {
                            snprintf(hex + j * 3, 4, "%02x ",
                                     raw[i + j]);
                        }
                        hex[3 * 16 - 1] = '\0';
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: trace_call peicore_raw +0x%03zx %s\n",
                                      (size_t)0x420 + i, hex);
                    }
                }
                uint8_t raw_lo[0x80];
                if (cpu_memory_rw_debug(cs, core_phys + 0x200,
                                        raw_lo, sizeof(raw_lo), false) == 0) {
                    for (size_t i = 0; i < sizeof(raw_lo); i += 16) {
                        char hex[3 * 16 + 1];
                        for (size_t j = 0; j < 16; j++) {
                            snprintf(hex + j * 3, 4, "%02x ",
                                     raw_lo[i + j]);
                        }
                        hex[3 * 16 - 1] = '\0';
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: trace_call peicore_raw +0x%03zx %s\n",
                                      (size_t)0x200 + i, hex);
                    }
                }
                uint8_t scan[0x800];
                if (cpu_memory_rw_debug(cs, core_phys,
                                        scan, sizeof(scan), false) == 0) {
                    uint64_t needle = 0x00000000ffe20000ULL;
                    uint64_t needle_r8 = 0x80000000ffe20000ULL;
                    int matches = 0;
                    ssize_t boot_off = -1;
                    for (size_t off = 0; off + 8 <= sizeof(scan); off += 8) {
                        uint64_t val = ldq_le_p(&scan[off]);
                        if (val == needle || val == needle_r8) {
                            if (boot_off < 0) {
                                boot_off = (ssize_t)off;
                            }
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: trace_call peicore_scan +0x%04zx"
                                          " val=%016" PRIx64 "\n",
                                          off, val);
                            if (++matches >= 8) {
                                break;
                            }
                        }
                    }
                    if (boot_off >= 0) {
                        uint64_t dispatch_off2 = (uint64_t)boot_off - 0x28;
                        uint64_t disp_base = core_phys + dispatch_off2;
                        uint8_t disp2[0x38];
                        if (cpu_memory_rw_debug(cs, disp_base,
                                                disp2, sizeof(disp2), false) == 0) {
                            uint8_t cur_peim = disp2[0];
                            uint8_t cur_fv = disp2[1];
                            uint64_t dispatched = ldq_le_p(&disp2[8]);
                            uint64_t prev = ldq_le_p(&disp2[16]);
                            uint64_t cur_peim_addr = ldq_le_p(&disp2[24]);
                            uint64_t cur_fv_addr = ldq_le_p(&disp2[32]);
                            uint64_t boot_fv_addr = ldq_le_p(&disp2[40]);
                            uint64_t findfv = ldq_le_p(&disp2[48]);
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: trace_call peicore_dispatch2"
                                          " off=0x%04" PRIx64 " cur_peim=%u cur_fv=%u"
                                          " dispatched=%016" PRIx64 " prev=%016" PRIx64 "\n",
                                          dispatch_off2, cur_peim, cur_fv,
                                          dispatched, prev);
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: trace_call peicore_dispatch2 cur_peim_addr=%016"
                                          PRIx64 " cur_fv_addr=%016" PRIx64
                                          " boot_fv_addr=%016" PRIx64
                                          " findfv=%016" PRIx64 "\n",
                                          cur_peim_addr, cur_fv_addr,
                                          boot_fv_addr, findfv);
                        }
                    }
                }
            }
        }

        if (trace_call_status) {
            if (call_a4) {
                IA64EfiGuid caller;
                if (ia64_fw_read_guid(env, call_a4, &caller)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: trace_call caller=%08x-%04x-%04x"
                                  "-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                                  caller.data1, caller.data2, caller.data3,
                                  caller.data4[0], caller.data4[1],
                                  caller.data4[2], caller.data4[3],
                                  caller.data4[4], caller.data4[5],
                                  caller.data4[6], caller.data4[7]);
                } else {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: trace_call caller_read_fail ptr=%016" PRIx64 "\n",
                                  call_a4);
                }
            }
            if (call_a5) {
                ia64_fw_statuscode_dump(env, call_a5, call_a1, call_a2);
            }
        }

        /*
         * ABI note: indirect calls typically use IA-64 function descriptors:
         *  - [fd+0] entry point
         *  - [fd+8] GP value (r1)
         *
         * The common sequence is:
         *   ld8 rX=[fd],8; ld8 r1=[fd]; mov bY=rX; br.call b0=bY
         * so at call time r31 often points at (fd+8).
         */
        if (env->r[31] >= 8) {
            CPUState *cs = env_cpu(env);
            uint64_t fd = env->r[31] - 8;
            uint8_t raw[16];
            if (cpu_memory_rw_debug(cs, fd, raw, sizeof(raw), false) == 0) {
                uint64_t entry = 0, gp = 0;
                memcpy(&entry, &raw[0], sizeof(entry));
                memcpy(&gp, &raw[8], sizeof(gp));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: trace_call fd=%016" PRIx64
                              " entry=%016" PRIx64 " gp=%016" PRIx64 "\n",
                              fd, entry, gp);
            }
        }

        if (susp_dump_bundle) {
            CPUState *cs = env_cpu(env);
            uint64_t base = pc & ~0xFULL;
            uint8_t bundle[16];
            if (cpu_memory_rw_debug(cs, base, bundle, sizeof(bundle), false) == 0) {
                uint64_t low = 0, high = 0;
                memcpy(&low, &bundle[0], sizeof(low));
                memcpy(&high, &bundle[8], sizeof(high));
                uint8_t tmpl = low & 0x1f;
                uint64_t s0 = (low >> 5) & 0x1ffffffffffULL;
                uint64_t s1 = ((low >> 46) | (high << 18)) & 0x1ffffffffffULL;
                uint64_t s2 = (high >> 23) & 0x1ffffffffffULL;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: trace_call_bundle pc=%016" PRIx64
                              " low=%016" PRIx64 " high=%016" PRIx64
                              " tmpl=%02x s0=%011" PRIx64 " s1=%011" PRIx64
                              " s2=%011" PRIx64 "\n",
                              base, low, high, tmpl, s0, s1, s2);
            }
        }

        if (susp_dump_code_bundles > 0) {
            CPUState *cs = env_cpu(env);
            uint64_t base = pc & ~0xFULL;
            uint64_t start = base;
            if (susp_dump_code_bundles > 16) {
                uint64_t back =
                    (uint64_t)(susp_dump_code_bundles / 4) * 16ULL;
                if (start >= back) {
                    start -= back;
                } else {
                    start = 0;
                }
            }
            g_mkdir_with_parents("scratch/ia64_logs", 0755);
            char path[256];
            snprintf(path, sizeof(path),
                     "scratch/ia64_logs/trace_call_code_%016" PRIx64 ".bin",
                     base);
            FILE *fp = fopen(path, "wb");
            if (fp) {
                for (int i = 0; i < susp_dump_code_bundles; i++) {
                    uint8_t bundle[16];
                    uint64_t bpc = start + (uint64_t)i * 16;
                    if (cpu_memory_rw_debug(cs, bpc, bundle, sizeof(bundle),
                                            false) != 0) {
                        break;
                    }
                    fwrite(bundle, 1, sizeof(bundle), fp);
                }
                fclose(fp);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: trace_call_dump_code pc=%016" PRIx64
                              " start=%016" PRIx64 " bundles=%d file=%s\n",
                              base, start, susp_dump_code_bundles, path);
            }
        }

        if (susp_dump_code_bundles > 0 && tgt) {
            CPUState *cs = env_cpu(env);
            uint64_t base = tgt & ~0xFULL;
            uint64_t start = base;
            if (susp_dump_code_bundles > 16) {
                uint64_t back =
                    (uint64_t)(susp_dump_code_bundles / 4) * 16ULL;
                if (start >= back) {
                    start -= back;
                } else {
                    start = 0;
                }
            }
            g_mkdir_with_parents("scratch/ia64_logs", 0755);
            char path[256];
            snprintf(path, sizeof(path),
                     "scratch/ia64_logs/trace_call_tgt_code_%016" PRIx64 ".bin",
                     base);
            FILE *fp = fopen(path, "wb");
            if (fp) {
                for (int i = 0; i < susp_dump_code_bundles; i++) {
                    uint8_t bundle[16];
                    uint64_t bpc = start + (uint64_t)i * 16;
                    if (cpu_memory_rw_debug(cs, bpc, bundle, sizeof(bundle),
                                            false) != 0) {
                        break;
                    }
                    fwrite(bundle, 1, sizeof(bundle), fp);
                }
                fclose(fp);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: trace_call_dump_tgt_code pc=%016" PRIx64
                              " tgt=%016" PRIx64 " start=%016" PRIx64
                              " bundles=%d file=%s\n",
                              pc, base, start, susp_dump_code_bundles, path);
            }
        }

        if (trace_call_abort) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: trace_call abort pc=%016" PRIx64 "\n", pc);
            abort();
        }
    }

    if (susp_enabled && susp_count < susp_limit) {
        uint8_t out0 = 32 + sol;
        uint64_t a0 = (out0 < 128) ? env->r[out0] : 0;
        uint64_t a1 = (out0 + 1 < 128) ? env->r[out0 + 1] : 0;
        uint64_t a2 = (out0 + 2 < 128) ? env->r[out0 + 2] : 0;
        bool a1_code = (a1 >= 0x20730000ULL && a1 < 0x20740000ULL);
        bool a2_code = (a2 >= 0x20730000ULL && a2 < 0x20740000ULL);
        uint64_t sp = env->r[12];
        uint64_t spdiff = (a2 > sp) ? (a2 - sp) : (sp - a2);
        bool suspect =
            (a0 <= 0x20 &&
             a1_code &&
             !a2_code &&
             (a2 & 7) == 0 &&
             spdiff < (128ULL << 10));
        if (suspect) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: susp_call pc=%016" PRIx64 " tgt=%016" PRIx64
                          " cfm=%016" PRIx64 " sof=%u sol=%u outs=%u out0=r%u"
                          " a0=%016" PRIx64 " a1=%016" PRIx64 " a2=%016" PRIx64
                          " r1=%016" PRIx64 " r12=%016" PRIx64 " b0=%016" PRIx64 "\n",
                          pc, tgt, caller_cfm, sof, sol, outs, out0,
                          a0, a1, a2, env->r[1], env->r[12], env->b[0]);

            if (susp_dump_bundle) {
                CPUState *cs = env_cpu(env);
                uint64_t base = pc & ~0xFULL;
                uint8_t bundle[16];
                if (cpu_memory_rw_debug(cs, base, bundle, sizeof(bundle),
                                        false) == 0) {
                    uint64_t low = 0, high = 0;
                    memcpy(&low, &bundle[0], sizeof(low));
                    memcpy(&high, &bundle[8], sizeof(high));
                    uint8_t tmpl = low & 0x1f;
                    uint64_t s0 = (low >> 5) & 0x1ffffffffffULL;
                    uint64_t s1 = ((low >> 46) | (high << 18)) & 0x1ffffffffffULL;
                    uint64_t s2 = (high >> 23) & 0x1ffffffffffULL;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: susp_call_bundle pc=%016" PRIx64
                                  " low=%016" PRIx64 " high=%016" PRIx64
                                  " tmpl=%02x s0=%011" PRIx64 " s1=%011" PRIx64
                                  " s2=%011" PRIx64 "\n",
                                  base, low, high, tmpl, s0, s1, s2);
                }
            }

            if (susp_dump_code_bundles > 0) {
                CPUState *cs = env_cpu(env);
                uint64_t base = pc & ~0xFULL;
                uint64_t start = base;
                if (susp_dump_code_bundles > 16) {
                    uint64_t back =
                        (uint64_t)(susp_dump_code_bundles / 4) * 16ULL;
                    if (start >= back) {
                        start -= back;
                    } else {
                        start = 0;
                    }
                }
                g_mkdir_with_parents("scratch/ia64_logs", 0755);
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch/ia64_logs/susp_call_code_%016" PRIx64 ".bin",
                         base);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    for (int i = 0; i < susp_dump_code_bundles; i++) {
                        uint8_t bundle[16];
                        uint64_t bpc = start + (uint64_t)i * 16;
                        if (cpu_memory_rw_debug(cs, bpc, bundle,
                                                sizeof(bundle), false) != 0) {
                            break;
                        }
                        fwrite(bundle, 1, sizeof(bundle), fp);
                    }
                    fclose(fp);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: susp_call_dump_code pc=%016" PRIx64
                                  " start=%016" PRIx64 " bundles=%d file=%s\n",
                                  base, start, susp_dump_code_bundles, path);
                }
            }

            if (susp_dump_code_bundles > 0 && tgt) {
                CPUState *cs = env_cpu(env);
                uint64_t base = tgt & ~0xFULL;
                uint64_t start = base;
                if (susp_dump_code_bundles > 16) {
                    uint64_t back =
                        (uint64_t)(susp_dump_code_bundles / 4) * 16ULL;
                    if (start >= back) {
                        start -= back;
                    } else {
                        start = 0;
                    }
                }
                g_mkdir_with_parents("scratch/ia64_logs", 0755);
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch/ia64_logs/susp_call_tgt_code_%016" PRIx64 ".bin",
                         base);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    for (int i = 0; i < susp_dump_code_bundles; i++) {
                        uint8_t bundle[16];
                        uint64_t bpc = start + (uint64_t)i * 16;
                        if (cpu_memory_rw_debug(cs, bpc, bundle,
                                                sizeof(bundle), false) != 0) {
                            break;
                        }
                        fwrite(bundle, 1, sizeof(bundle), fp);
                    }
                    fclose(fp);
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: susp_call_dump_tgt_code pc=%016" PRIx64
                                  " tgt=%016" PRIx64 " start=%016" PRIx64
                                  " bundles=%d file=%s\n",
                                  pc, base, start, susp_dump_code_bundles, path);
                }
            }

            susp_count++;
            if (susp_abort) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: susp_call abort pc=%016" PRIx64 "\n", pc);
                abort();
            }
        }
    }

    if (dbg_pc) {
        uint8_t out0 = 32 + sol;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "call_map pc=%016" PRIx64 " cfm=%016" PRIx64 " sof=%u sol=%u outs=%u out0=r%u"
                      " out0..4=%016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
                      dbg_pc, env->cfm, sof, sol, outs, out0,
                      env->r[out0 + 0], env->r[out0 + 1], env->r[out0 + 2],
                      env->r[out0 + 3], env->r[out0 + 4]);
    }

    /*
     * Save the caller frame and then install a fresh ar.pfs for the callee.
     *
     * See SKI brCallEx/callWrt:
     *  - ar.pfs carries the caller's CPL and EC count, plus the caller's
     *    current frame marker (SOF/SOL/SOR/RRB*).
     *  - on return, the caller's ar.pfs must be restored, so keep it in our
     *    shadow stack (ia64_rse_push_window()).
     *
     * Note: SKI numbers bitfields from the MSB; our env->cfm layout already
     * matches the extracted CFM (SOF at bits 6:0, ... RRB* at 45:18), so we
     * can reuse caller_cfm directly here.
     */
    if (!ia64_rse_is_lazy(env)) {
        ia64_rse_store_frame(env, bsp, sof);
    }
    bsp = ia64_rse_skip_regs(bsp, sol);
    env->ar[IA64_AR_BSP] = bsp;
    ia64_rse_update_loadrs(env, bsp);

    ia64_rse_push_window(env, pc + 16);
    {
        uint64_t ppl = (env->psr >> 32) & 3; /* PSR.CPL */
        uint64_t pec = (env->ar[66] >> 58) & 0x3f; /* EC_CNT bits 63..58 */
        uint64_t new_pfs = caller_cfm |
                           (ppl << 62) |
                           (pec << 52);
        env->ar[IA64_AR_PFS] = new_pfs;
    }

    outs = MIN(outs, (uint8_t)96);
    if (sol < 96) {
        uint8_t max_copy = MIN(outs, (uint8_t)(96 - sol));
        for (uint8_t i = 0; i < max_copy; i++) {
            tmp[i] = env->r[32 + sol + i];
            tmp_nat[i] = env->nat[32 + sol + i];
        }
        for (uint8_t i = 0; i < max_copy; i++) {
            env->r[32 + i] = tmp[i];
            env->nat[32 + i] = tmp_nat[i];
        }
    }
    if (dbg_pc) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "call_map pc=%016" PRIx64 " mapped in0..4=%016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
                      dbg_pc, env->r[32], env->r[33], env->r[34], env->r[35], env->r[36]);
        ia64_dbg_next_call_pc = 0;
    }
    if (trace_hit) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: call_trace post pc=%016" PRIx64 " tgt=%016" PRIx64
                      " cfm=%016" PRIx64 " r32=%016" PRIx64 " r33=%016" PRIx64
                      " r34=%016" PRIx64 " r35=%016" PRIx64 " r36=%016" PRIx64
                      " r37=%016" PRIx64 " r38=%016" PRIx64 " r39=%016" PRIx64
                      " r40=%016" PRIx64 " b0=%016" PRIx64 " r1=%016" PRIx64
                      " r12=%016" PRIx64 " bsp=%016" PRIx64 " ar.pfs=%016" PRIx64
                      "\n",
                      pc, tgt, env->cfm,
                      env->r[32], env->r[33], env->r[34], env->r[35],
                      env->r[36], env->r[37], env->r[38], env->r[39],
                      env->r[40], env->b[0], env->r[1], env->r[12],
                      env->ar[IA64_AR_BSP], env->ar[IA64_AR_PFS]);
    }

    /*
     * Pre-alloc CFM for callee: on procedure entry the input registers are
     * also the outgoing registers (SOL=0) until the callee executes alloc to
     * create locals/out slots.
     */
    env->cfm = outs & 0x7f;

    /* Restore to the caller's ar.pfs on return (via ia64_rse_pop_window()). */
    if (watch_hit) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "call_watch post pc=%016" PRIx64 " depth=%u"
                      " r37=%016" PRIx64 " r38=%016" PRIx64 "\n",
                      pc, env->rse_depth, env->r[37], env->r[38]);
    }
}

void HELPER(manual_call_enter)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    uint8_t kind = env->last_b0_write_kind & 0xff;
    if (kind != 2) {
        return;
    }

    uint64_t b0 = env->b[0] & ~0xFULL;
    if (!b0) {
        return;
    }

    if (env->last_manual_call_pc == pc && env->last_manual_call_b0 == b0) {
        return;
    }

    uint64_t last_pc = env->last_b0_write_pc & ~0xFULL;
    if (last_pc) {
        uint64_t dist = (pc >= last_pc) ? (pc - last_pc) : (last_pc - pc);
        if (dist > 0x80) {
            return;
        }
    }

    static int log_enabled = -1;
    if (log_enabled == -1) {
        log_enabled = getenv("QEMU_IA64_MANUAL_CALL_LOG") ? 1 : 0;
    }
    if (log_enabled) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "manual_call_enter pc=%016" PRIx64 " b0=%016" PRIx64
                      " last_b0_pc=%016" PRIx64 " kind=%" PRIu64
                      " depth=%u\n",
                      pc, b0, env->last_b0_write_pc,
                      env->last_b0_write_kind & 0xff,
                      env->rse_depth);
    }
    ia64_rse_push_window(env, b0);
    if (log_enabled) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "manual_call_enter post pc=%016" PRIx64 " depth=%u\n",
                      pc, env->rse_depth);
    }
    if (env->rse_depth > 0) {
        env->rse_frames[env->rse_depth - 1].share_outs = 0;
    }
    env->last_manual_call_pc = pc;
    env->last_manual_call_b0 = b0;
#endif
}

void HELPER(ret_restore)(CPUIA64State *env)
{
    static int log_count;
    static int unwind_enabled = -1;
    if (unwind_enabled == -1) {
        unwind_enabled = getenv("QEMU_IA64_RET_UNWIND_PFS") ? 1 : 0;
    }
    static uint64_t watch_b0;
    static bool watch_b0_inited;
    if (!watch_b0_inited) {
        const char *s = getenv("QEMU_IA64_RET_WATCH_B0");
        if (s && *s) {
            watch_b0 = strtoull(s, NULL, 0) & ~0xFULL;
        }
        watch_b0_inited = true;
    }
    bool watch_hit = (watch_b0 != 0) &&
                     (((env->b[0] ^ watch_b0) & ~0xFULL) == 0);
    if (log_count < 64) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ret_restore ip=0x%" PRIx64 " b0=0x%" PRIx64
                      " cfm=0x%" PRIx64 " depth=%u\n",
                      env->ip, env->b[0], env->cfm, env->rse_depth);
        log_count++;
    }
    if (watch_hit) {
        uint64_t top_ret = 0;
        uint64_t top_cfm = 0;
        if (env->rse_depth > 0 && env->rse_frames) {
            const struct IA64RSEFrame *frame = &env->rse_frames[env->rse_depth - 1];
            top_ret = frame->ret_addr;
            top_cfm = frame->cfm;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ret_watch pre ip=%016" PRIx64 " b0=%016" PRIx64
                      " cfm=%016" PRIx64 " pfs=%016" PRIx64
                      " depth=%u top_ret=%016" PRIx64 " top_cfm=%016" PRIx64
                      " r1=%016" PRIx64 " r37=%016" PRIx64
                      " r38=%016" PRIx64 "\n",
                      env->ip, env->b[0], env->cfm, env->ar[64],
                      env->rse_depth, top_ret, top_cfm,
                      env->r[1], env->r[37], env->r[38]);
    }
    if (ia64_fw_pei_locate_trace_enabled() &&
        env->fw_pei_locate_ret_pc &&
        ((env->b[0] ^ env->fw_pei_locate_ret_pc) & ~0xFULL) == 0) {
        CPUState *cs = env_cpu(env);
        uint64_t out_desc = 0;
        bool out_ok = env->fw_pei_locate_desc_ptr &&
                      ia64_fw_read_u64(cs, env->fw_pei_locate_desc_ptr,
                                       &out_desc);
        uint64_t out_ppi = 0;
        bool out_ppi_ok = env->fw_pei_locate_ppi_ptr &&
                          ia64_fw_read_u64(cs, env->fw_pei_locate_ppi_ptr,
                                           &out_ppi);
        if (env->fw_pei_locate_guid_valid) {
            IA64EfiGuid guid;
            ia64_fw_guid_from_bytes(env->fw_pei_locate_guid, &guid);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_locate_ret ip=%016" PRIx64
                          " b0=%016" PRIx64 " status=%016" PRIx64
                          " inst=%016" PRIx64 " desc_ptr=%016" PRIx64
                          " desc=%016" PRIx64
                          " ppi_ptr=%016" PRIx64 " ppi=%016" PRIx64
                          " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                          env->ip, env->b[0], env->r[8],
                          env->fw_pei_locate_instance,
                          env->fw_pei_locate_desc_ptr,
                          out_ok ? out_desc : 0,
                          env->fw_pei_locate_ppi_ptr,
                          out_ppi_ok ? out_ppi : 0,
                          guid.data1, guid.data2, guid.data3,
                          guid.data4[0], guid.data4[1],
                          guid.data4[2], guid.data4[3],
                          guid.data4[4], guid.data4[5],
                          guid.data4[6], guid.data4[7]);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_locate_ret ip=%016" PRIx64
                          " b0=%016" PRIx64 " status=%016" PRIx64
                          " inst=%016" PRIx64 " desc_ptr=%016" PRIx64
                          " desc=%016" PRIx64 " ppi_ptr=%016" PRIx64
                          " ppi=%016" PRIx64 "\n",
                          env->ip, env->b[0], env->r[8],
                          env->fw_pei_locate_instance,
                          env->fw_pei_locate_desc_ptr,
                          out_ok ? out_desc : 0,
                          env->fw_pei_locate_ppi_ptr,
                          out_ppi_ok ? out_ppi : 0);
        }
        env->fw_pei_locate_ret_pc = 0;
        env->fw_pei_locate_guid_valid = 0;
        env->fw_pei_locate_ppi_ptr = 0;
    }
    if (ia64_fw_pei_install_trace_enabled() &&
        env->fw_pei_install_ret_pc &&
        ((env->b[0] ^ env->fw_pei_install_ret_pc) & ~0xFULL) == 0) {
        CPUState *cs = env_cpu(env);
        uint64_t flags = 0;
        uint64_t guid_ptr = 0;
        uint64_t ppi_ptr = 0;
        bool desc_ok = false;
        if (env->fw_pei_install_desc_ptr) {
            uint8_t desc[24];
            if (ia64_fw_read_bytes_any(cs, env->fw_pei_install_desc_ptr,
                                       desc, sizeof(desc))) {
                flags = ldq_le_p(&desc[0]);
                guid_ptr = ldq_le_p(&desc[8]);
                ppi_ptr = ldq_le_p(&desc[16]);
                desc_ok = true;
            }
        }
        if (env->fw_pei_install_guid_valid) {
            IA64EfiGuid guid;
            ia64_fw_guid_from_bytes(env->fw_pei_install_guid, &guid);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_ret ip=%016" PRIx64
                          " b0=%016" PRIx64 " status=%016" PRIx64
                          " desc=%016" PRIx64 " flags=%016" PRIx64
                          " ppi=%016" PRIx64
                          " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                          env->ip, env->b[0], env->r[8],
                          env->fw_pei_install_desc_ptr, flags, ppi_ptr,
                          guid.data1, guid.data2, guid.data3,
                          guid.data4[0], guid.data4[1],
                          guid.data4[2], guid.data4[3],
                          guid.data4[4], guid.data4[5],
                          guid.data4[6], guid.data4[7]);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_ret ip=%016" PRIx64
                          " b0=%016" PRIx64 " status=%016" PRIx64
                          " desc=%016" PRIx64 " flags=%016" PRIx64
                          " ppi=%016" PRIx64 " guid_ptr=%016" PRIx64
                          " desc_ok=%d\n",
                          env->ip, env->b[0], env->r[8],
                          env->fw_pei_install_desc_ptr, flags, ppi_ptr,
                          guid_ptr, desc_ok ? 1 : 0);
        }
        env->fw_pei_install_ret_pc = 0;
        env->fw_pei_install_guid_valid = 0;
    }
    uint64_t bsp = ia64_rse_get_bsp(env);
    uint8_t sof = env->cfm & 0x7f;
    if (!ia64_rse_is_lazy(env)) {
        ia64_rse_store_frame(env, bsp, sof);
    }
    uint64_t pfs_cfm = env->ar[IA64_AR_PFS] & ((1ULL << 46) - 1);
    uint64_t b0 = env->b[0] & ~0xFULL;
    if (unwind_enabled && env->rse_depth > 0) {
        int unwind = 0;
        while (env->rse_depth > 0) {
            const struct IA64RSEFrame *frame =
                &env->rse_frames[env->rse_depth - 1];
            if (frame->ret_addr == b0 || frame->cfm == pfs_cfm) {
                break;
            }
            ia64_rse_pop_window(env);
            unwind++;
        }
        if (unwind && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ret_unwind pfs_cfm=%016" PRIx64 " b0=%016" PRIx64
                          " dropped=%d depth=%u\n",
                          pfs_cfm, b0, unwind, env->rse_depth);
        }
    }
    if (ia64_rse_pop_window(env)) {
        ia64_restore_ec_from_pfs(env);
        uint8_t sol = (env->cfm >> 7) & 0x7f;
        bsp = ia64_rse_skip_regs(bsp, -(int64_t)sol);
        env->ar[IA64_AR_BSP] = bsp;
        ia64_rse_update_loadrs(env, bsp);
    }
    if (watch_hit) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ret_watch post ip=%016" PRIx64 " b0=%016" PRIx64
                      " cfm=%016" PRIx64 " pfs=%016" PRIx64
                      " depth=%u r1=%016" PRIx64 " r37=%016" PRIx64
                      " r38=%016" PRIx64 "\n",
                      env->ip, env->b[0], env->cfm, env->ar[64],
                      env->rse_depth, env->r[1], env->r[37], env->r[38]);
    }
}

void HELPER(ret_restore_b0)(CPUIA64State *env)
{
    /*
     * Some code sequences (notably PAL call paths) use "br.ia b0" as a return
     * control transfer without a matching br.call that created a new stacked
     * register frame.  Only unwind our modeled RSE window if b0 was last
     * written by br.call/brl.call.
     */
    static uint64_t watch_b0;
    static bool watch_b0_inited;
    if (!watch_b0_inited) {
        const char *s = getenv("QEMU_IA64_RET_WATCH_B0");
        if (s && *s) {
            watch_b0 = strtoull(s, NULL, 0) & ~0xFULL;
        }
        watch_b0_inited = true;
    }
    uint8_t kind = env->last_b0_write_kind & 0xff;
    uint64_t tgt = env->b[0] & ~0xFULL;
    bool do_pop = false;
    if (env->rse_depth > 0) {
        const struct IA64RSEFrame *frame = &env->rse_frames[env->rse_depth - 1];
        do_pop = (frame->ret_addr != 0 && frame->ret_addr == tgt);
    }
    if (!do_pop) {
        do_pop = (kind == 1);
    }
    static int log_count;
    bool watch_hit = watch_b0 && (((env->b[0] ^ watch_b0) & ~0xFULL) == 0);

    if (log_count < 64) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ret_restore_b0 ip=0x%" PRIx64 " b0=0x%" PRIx64
                      " last_b0_kind=%u depth=%u pop=%d\n",
                      env->ip, env->b[0], kind, env->rse_depth, do_pop);
        log_count++;
    }
    if (watch_hit) {
        uint64_t top_ret = 0;
        uint64_t top_cfm = 0;
        if (env->rse_depth > 0 && env->rse_frames) {
            const struct IA64RSEFrame *frame = &env->rse_frames[env->rse_depth - 1];
            top_ret = frame->ret_addr;
            top_cfm = frame->cfm;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ret_b0_watch ip=%016" PRIx64 " b0=%016" PRIx64
                      " kind=%u depth=%u do_pop=%d top_ret=%016" PRIx64
                      " top_cfm=%016" PRIx64 " r37=%016" PRIx64 "\n",
                      env->ip, env->b[0], kind, env->rse_depth, do_pop,
                      top_ret, top_cfm, env->r[37]);
    }
    if (do_pop) {
        if (ia64_rse_pop_window(env)) {
            ia64_restore_ec_from_pfs(env);
            uint64_t bsp = ia64_rse_get_bsp(env);
            uint8_t sol = (env->cfm >> 7) & 0x7f;
            bsp = ia64_rse_skip_regs(bsp, -(int64_t)sol);
            env->ar[IA64_AR_BSP] = bsp;
            ia64_rse_update_loadrs(env, bsp);
        }
    }

    if (ia64_fw_pei_locate_trace_enabled() &&
        env->fw_pei_locate_ret_pc &&
        ((env->b[0] ^ env->fw_pei_locate_ret_pc) & ~0xFULL) == 0) {
        CPUState *cs = env_cpu(env);
        uint64_t out_desc = 0;
        bool out_ok = env->fw_pei_locate_desc_ptr &&
                      ia64_fw_read_u64(cs, env->fw_pei_locate_desc_ptr,
                                       &out_desc);
        uint64_t out_ppi = 0;
        bool out_ppi_ok = env->fw_pei_locate_ppi_ptr &&
                          ia64_fw_read_u64(cs, env->fw_pei_locate_ppi_ptr,
                                           &out_ppi);
        if (env->fw_pei_locate_guid_valid) {
            IA64EfiGuid guid;
            ia64_fw_guid_from_bytes(env->fw_pei_locate_guid, &guid);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_locate_ret_b0 ip=%016" PRIx64
                          " b0=%016" PRIx64 " status=%016" PRIx64
                          " inst=%016" PRIx64 " desc_ptr=%016" PRIx64
                          " desc=%016" PRIx64
                          " ppi_ptr=%016" PRIx64 " ppi=%016" PRIx64
                          " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                          env->ip, env->b[0], env->r[8],
                          env->fw_pei_locate_instance,
                          env->fw_pei_locate_desc_ptr,
                          out_ok ? out_desc : 0,
                          env->fw_pei_locate_ppi_ptr,
                          out_ppi_ok ? out_ppi : 0,
                          guid.data1, guid.data2, guid.data3,
                          guid.data4[0], guid.data4[1],
                          guid.data4[2], guid.data4[3],
                          guid.data4[4], guid.data4[5],
                          guid.data4[6], guid.data4[7]);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_locate_ret_b0 ip=%016" PRIx64
                          " b0=%016" PRIx64 " status=%016" PRIx64
                          " inst=%016" PRIx64 " desc_ptr=%016" PRIx64
                          " desc=%016" PRIx64 " ppi_ptr=%016" PRIx64
                          " ppi=%016" PRIx64 "\n",
                          env->ip, env->b[0], env->r[8],
                          env->fw_pei_locate_instance,
                          env->fw_pei_locate_desc_ptr,
                          out_ok ? out_desc : 0,
                          env->fw_pei_locate_ppi_ptr,
                          out_ppi_ok ? out_ppi : 0);
        }
        env->fw_pei_locate_ret_pc = 0;
        env->fw_pei_locate_guid_valid = 0;
        env->fw_pei_locate_ppi_ptr = 0;
    }
    if (ia64_fw_pei_install_trace_enabled() &&
        env->fw_pei_install_ret_pc &&
        ((env->b[0] ^ env->fw_pei_install_ret_pc) & ~0xFULL) == 0) {
        CPUState *cs = env_cpu(env);
        uint64_t flags = 0;
        uint64_t guid_ptr = 0;
        uint64_t ppi_ptr = 0;
        bool desc_ok = false;
        if (env->fw_pei_install_desc_ptr) {
            uint8_t desc[24];
            if (ia64_fw_read_bytes_any(cs, env->fw_pei_install_desc_ptr,
                                       desc, sizeof(desc))) {
                flags = ldq_le_p(&desc[0]);
                guid_ptr = ldq_le_p(&desc[8]);
                ppi_ptr = ldq_le_p(&desc[16]);
                desc_ok = true;
            }
        }
        if (env->fw_pei_install_guid_valid) {
            IA64EfiGuid guid;
            ia64_fw_guid_from_bytes(env->fw_pei_install_guid, &guid);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_ret_b0 ip=%016" PRIx64
                          " b0=%016" PRIx64 " status=%016" PRIx64
                          " desc=%016" PRIx64 " flags=%016" PRIx64
                          " ppi=%016" PRIx64
                          " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                          env->ip, env->b[0], env->r[8],
                          env->fw_pei_install_desc_ptr, flags, ppi_ptr,
                          guid.data1, guid.data2, guid.data3,
                          guid.data4[0], guid.data4[1],
                          guid.data4[2], guid.data4[3],
                          guid.data4[4], guid.data4[5],
                          guid.data4[6], guid.data4[7]);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_ret_b0 ip=%016" PRIx64
                          " b0=%016" PRIx64 " status=%016" PRIx64
                          " desc=%016" PRIx64 " flags=%016" PRIx64
                          " ppi=%016" PRIx64 " guid_ptr=%016" PRIx64
                          " desc_ok=%d\n",
                          env->ip, env->b[0], env->r[8],
                          env->fw_pei_install_desc_ptr, flags, ppi_ptr,
                          guid_ptr, desc_ok ? 1 : 0);
        }
        env->fw_pei_install_ret_pc = 0;
        env->fw_pei_install_guid_valid = 0;
    }

    if (do_pop) {
    if (!do_pop) {
        uint64_t bsp = ia64_rse_get_bsp(env);
        uint8_t sof = env->cfm & 0x7f;
        if (!ia64_rse_is_lazy(env)) {
            ia64_rse_store_frame(env, bsp, sof);
        }
    }
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
static void ia64_fw_try_patch_efi_hobs(CPUIA64State *env)
{
    static int enabled = -1;
    static int dump_enabled = -1;
    static int dump_after_patch = -1;
    static int memtype_enabled = -1;
    static bool fixed_sysmem_rdesc;
    static uint64_t fixed_sysmem_rdesc_base;
    static bool fixed_fv_hobs;
    static uint64_t fixed_fv_hobs_base;
    static bool fixed_flashmap_vars;
    static uint64_t fixed_flashmap_base;
    static bool fixed_memtype_hob;
    static uint64_t fixed_memtype_base;
    static bool fixed_attr;
    static bool fixed_pei_span;
    static bool fixed_free_bottom;
    static bool fixed_free_top;
    static bool fixed_gp_alloc;
    static bool fixed_boot_mode;
    static int attempts;
    static uint32_t throttle;
    static bool logged_phit;
    static uint64_t logged_hob_base;
    static bool logged_end_mismatch;
    static bool logged_fv_scan;
    static bool logged_fv_state;
    static bool dumped_hobs;
    static bool dumped_after_patch;
    static uint32_t dump_throttle;
    static uint64_t reloc_hob_base;
    static uint64_t reloc_hob_end;
    static bool dumped_reloc_hob;
    static uint64_t hob_ptr_stack_page;
    static int hob_patch_trace = -1;
    static int hob_force_ram = -1;

    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_EFI_HOB_PATCH");
        /*
         * Default to disabled so we don't mask firmware/TCG bugs with
         * synthetic HOB edits. Enable explicitly when needed.
         *
         * Enable via QEMU_IA64_EFI_HOB_PATCH=1/on/true/yes.
         */
        if (!s || !*s) {
            enabled = 0;
        } else if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "false") ||
                   !strcmp(s, "no")) {
            enabled = 0;
        } else {
            enabled = 1;
        }
    }

    CPUState *cs = env_cpu(env);
    hwaddr stack_phys = ia64_phys_mode_addr(env->r[12]);
    if (stack_phys == 0) {
        return;
    }
    if (dump_enabled == -1) {
        const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP");
        dump_enabled = (s && *s) ? 1 : 0;
    }
    if (memtype_enabled == -1) {
        const char *s = getenv("QEMU_IA64_EFI_MEMTYPE_HOB");
        if (!s || !*s) {
            memtype_enabled = 1;
        } else if (!strcmp(s, "0") || !strcmp(s, "off") ||
                   !strcmp(s, "false") || !strcmp(s, "no")) {
            memtype_enabled = 0;
        } else {
            memtype_enabled = 1;
        }
    }
    if (!enabled && !memtype_enabled) {
        if (dump_enabled && !dumped_hobs) {
            if ((dump_throttle++ & 0xff) == 0) {
                if (ia64_fw_dump_efi_hobs(cs, stack_phys)) {
                    dumped_hobs = true;
                }
            }
        }
        return;
    }
    if (!env->fw_pei_mem_installed) {
        if (dump_enabled && !dumped_hobs) {
            if ((dump_throttle++ & 0xff) == 0) {
                if (ia64_fw_dump_efi_hobs(cs, stack_phys)) {
                    dumped_hobs = true;
                }
            }
        }
        return;
    }
    if (!enabled) {
        if (dump_enabled && !dumped_hobs) {
            if ((dump_throttle++ & 0xff) == 0) {
                if (ia64_fw_dump_efi_hobs(cs, stack_phys)) {
                    dumped_hobs = true;
                }
            }
        }
    }

    if (enabled && fixed_sysmem_rdesc && fixed_fv_hobs &&
        (fixed_attr && fixed_free_bottom && fixed_free_top)) {
        return;
    }
    if (dump_after_patch == -1) {
        const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP_AFTER_PATCH");
        dump_after_patch = (s && *s) ? 1 : 0;
    }
    if (hob_patch_trace == -1) {
        const char *s = getenv("QEMU_IA64_EFI_HOB_PATCH_TRACE");
        hob_patch_trace = (s && *s) ? 1 : 0;
    }
    if (hob_force_ram == -1) {
        const char *s = getenv("QEMU_IA64_EFI_HOB_FORCE_RAM");
        if (!s || !*s) {
            hob_force_ram = 1;
        } else if (!strcmp(s, "0") || !strcmp(s, "off") ||
                   !strcmp(s, "false") || !strcmp(s, "no")) {
            hob_force_ram = 0;
        } else {
            hob_force_ram = 1;
        }
    }
    if (dump_enabled && !dumped_hobs) {
        if ((dump_throttle++ & 0xff) == 0) {
            if (ia64_fw_dump_efi_hobs(cs, stack_phys)) {
                dumped_hobs = true;
            }
        }
    }
    hwaddr ip_phys = ia64_phys_mode_addr(env->ip);
    bool in_flash = (ip_phys >= 0xff000000ULL);
    bool in_hob_loop = (env->ip >= 0x11b80 && env->ip < 0x11d80);
    if (enabled && !hob_force_ram && (throttle++ & 0x7f) != 0 && !in_hob_loop) {
        return;
    }

    enum {
        EFI_HOB_TYPE_HANDOFF = 0x0001,
        EFI_HOB_TYPE_MEMORY_ALLOCATION = 0x0002,
        EFI_HOB_TYPE_RESOURCE_DESCRIPTOR = 0x0003,
        EFI_HOB_TYPE_GUID_EXTENSION = 0x0004,
        EFI_HOB_TYPE_FV = 0x0005,
        EFI_HOB_TYPE_END_OF_HOB_LIST = 0xffff,
    };
    enum {
        EFI_RESOURCE_ATTRIBUTE_PRESENT = 0x00000001u,
        EFI_RESOURCE_ATTRIBUTE_INITIALIZED = 0x00000002u,
        EFI_RESOURCE_ATTRIBUTE_TESTED = 0x00000004u,
        EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE = 0x00000400u,
        EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE = 0x00002000u,
    };

    /*
     * Scan for the active EFI HOB list near the current firmware stack. Some
     * xenipf builds produce a PEI memory descriptor with both UC and WB set;
     * clear UC to match typical system-memory capabilities and avoid DXE init
     * asserts in the memory/GCD setup.
     */
    const uint8_t phit_magic[8] = { 0x01, 0x00, 0x38, 0x00, 0, 0, 0, 0 };
    uint64_t scan_base = (stack_phys > (8ULL << 20)) ? (stack_phys - (8ULL << 20)) : 0;
    uint64_t scan_len = 16ULL << 20;
    const size_t chunk = 64 * 1024;
    g_autofree uint8_t *buf = g_malloc(chunk);

    uint64_t hob_base = 0;
    uint64_t hob_end = 0;
    uint64_t hob_best_span = 0;
    bool hob_from_pei = false;
    uint8_t hdr[8];
    uint64_t candidates[] = {
        0x0000000002000000ULL,
        0x0000000000100000ULL,
        0x0000000001000000ULL,
        stack_phys ? (stack_phys & ~0x00ffffffULL) : 0,
    };
    if (ia64_fw_find_pei_hob_list(cs, stack_phys, &hob_base, &hob_end)) {
        hob_best_span = hob_end - hob_base;
        hob_from_pei = true;
    }
    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        uint64_t addr = candidates[i];
        if (!addr) {
            continue;
        }
        if (cpu_memory_rw_debug(cs, addr, hdr, sizeof(hdr), false) != 0 ||
            memcmp(hdr, phit_magic, sizeof(phit_magic)) != 0) {
            continue;
        }
        uint8_t phit[0x38];
        if (cpu_memory_rw_debug(cs, addr, phit, sizeof(phit), false) != 0) {
            continue;
        }
        uint64_t mem_top = ldq_le_p(&phit[16]);
        uint64_t mem_bottom = ldq_le_p(&phit[24]);
        if (mem_top <= mem_bottom) {
            continue;
        }
        uint64_t end;
        int count;
        if (!ia64_fw_validate_efi_hob_list(cs, addr, &end, &count)) {
            continue;
        }
        uint64_t span = end - addr;
        if (!hob_base || span > hob_best_span) {
            hob_best_span = span;
            hob_base = addr;
            hob_end = end;
            hob_from_pei = false;
        }
    }
    for (uint64_t off = 0; off < scan_len; off += chunk - 8) {
        uint64_t addr = scan_base + off;
        if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
            continue;
        }
        for (size_t j = 0; j + sizeof(phit_magic) <= chunk; j++) {
            if (buf[j] != 0x01) {
                continue;
            }
            if (memcmp(&buf[j], phit_magic, sizeof(phit_magic)) != 0) {
                continue;
            }
            uint64_t cand = addr + j;
            uint8_t phit[0x38];
            if (cpu_memory_rw_debug(cs, cand, phit, sizeof(phit), false) != 0) {
                continue;
            }
            uint64_t mem_top = ldq_le_p(&phit[16]);
            uint64_t mem_bottom = ldq_le_p(&phit[24]);
            if (mem_top <= mem_bottom) {
                continue;
            }
            uint64_t end;
            int count;
            if (!ia64_fw_validate_efi_hob_list(cs, cand, &end, &count)) {
                continue;
            }
            uint64_t span = end - cand;
            if (!hob_base || span > hob_best_span) {
                hob_best_span = span;
                hob_base = cand;
                hob_end = end;
                hob_from_pei = false;
            }
        }
    }
    {
        uint64_t low_scan_base = 0;
        uint64_t low_scan_len = 64ULL << 20;
        for (uint64_t off = 0; off < low_scan_len; off += chunk - 8) {
            uint64_t addr = low_scan_base + off;
            if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
                continue;
            }
            for (size_t j = 0; j + sizeof(phit_magic) <= chunk; j++) {
                if (buf[j] != 0x01) {
                    continue;
                }
                if (memcmp(&buf[j], phit_magic, sizeof(phit_magic)) != 0) {
                    continue;
                }
                uint64_t cand = addr + j;
                uint8_t phit[0x38];
                if (cpu_memory_rw_debug(cs, cand, phit, sizeof(phit), false) != 0) {
                    continue;
                }
                uint64_t mem_top = ldq_le_p(&phit[16]);
                uint64_t mem_bottom = ldq_le_p(&phit[24]);
                if (mem_top <= mem_bottom) {
                    continue;
                }
                uint64_t end;
                int count;
                if (!ia64_fw_validate_efi_hob_list(cs, cand, &end, &count)) {
                    continue;
                }
                uint64_t span = end - cand;
                if (!hob_base || span > hob_best_span) {
                    hob_best_span = span;
                    hob_base = cand;
                    hob_end = end;
                    hob_from_pei = false;
                }
            }
        }
    }
    {
        uint64_t hi_scan_base = 0xff000000ULL;
        uint64_t hi_scan_len = 16ULL << 20;
        for (uint64_t off = 0; off < hi_scan_len; off += chunk - 8) {
            uint64_t addr = hi_scan_base + off;
            if (cpu_memory_rw_debug(cs, addr, buf, chunk, false) != 0) {
                continue;
            }
            for (size_t j = 0; j + sizeof(phit_magic) <= chunk; j++) {
                if (buf[j] != 0x01) {
                    continue;
                }
                if (memcmp(&buf[j], phit_magic, sizeof(phit_magic)) != 0) {
                    continue;
                }
                uint64_t cand = addr + j;
                uint8_t phit[0x38];
                if (cpu_memory_rw_debug(cs, cand, phit, sizeof(phit), false) != 0) {
                    continue;
                }
                uint64_t mem_top = ldq_le_p(&phit[16]);
                uint64_t mem_bottom = ldq_le_p(&phit[24]);
                if (mem_top <= mem_bottom) {
                    continue;
                }

                uint64_t end;
                int count;
                if (!ia64_fw_validate_efi_hob_list(cs, cand, &end, &count)) {
                    continue;
                }
                uint64_t span = end - cand;
                if (!hob_base || span > hob_best_span) {
                    hob_best_span = span;
                    hob_base = cand;
                    hob_end = end;
                    hob_from_pei = false;
                }
            }
        }
    }
    if (!hob_base) {
        return;
    }

    if (hob_base != logged_hob_base && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        const char *src = hob_from_pei ? "pei" : "scan";
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: hob_patch: using HOB list base=%016" PRIx64
                      " end=%016" PRIx64 " src=%s stack=%016" PRIx64
                      " r33=%016" PRIx64 "\n",
                      hob_base, hob_end, src, stack_phys, env->r[33]);
        logged_hob_base = hob_base;
    }
    if (fixed_sysmem_rdesc_base && fixed_sysmem_rdesc_base != hob_base) {
        fixed_sysmem_rdesc = false;
        fixed_sysmem_rdesc_base = 0;
    }
    uint64_t flash_base = ia64_fw_flash_base();
    bool hob_low = (hob_base < flash_base);
    if (hob_low) {
        if (attempts >= 256) {
            return;
        }
        attempts++;
        if (attempts == 1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: hob_patch: scan ip=%016" PRIx64 " sp=%016" PRIx64 "\n",
                          (uint64_t)ip_phys, (uint64_t)stack_phys);
        }
    }

    uint8_t phit[0x38];
    cpu_physical_memory_read(hob_base, phit, sizeof(phit));
    uint32_t boot_mode = ldl_le_p(&phit[12]);
    uint64_t mem_bottom = ldq_le_p(&phit[24]);
    uint64_t mem_top = ldq_le_p(&phit[16]);
    uint64_t free_top = ldq_le_p(&phit[32]);
    uint64_t free_bottom = ldq_le_p(&phit[40]);
    uint64_t mem_bottom_phys = 0;
    uint64_t mem_top_phys = 0;
    uint64_t orig_mem_top = mem_top;
    uint64_t end_hob_raw = ldq_le_p(&phit[48]);
    uint64_t end_hob = ia64_phys_mode_addr(end_hob_raw);
    if (!end_hob || end_hob < hob_base || end_hob > hob_end) {
        if (!logged_end_mismatch) {
            logged_end_mismatch = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: hob_patch: PHIT end_hob=%016" PRIx64
                          " (phys=%016" PRIx64 ") outside list [%016" PRIx64 "..%016" PRIx64 "],"
                          " using list_end\n",
                          end_hob_raw, end_hob, hob_base, hob_end);
        }
        end_hob = hob_end;
    }
    uint64_t slack_size = 0;

    /*
     * Detect the mapped slack window above the original EfiMemoryTop.
     * This is QEMU-provided RAM used to satisfy xenipf/EDK firmware
     * relocations that stray slightly beyond the RAM size reported to the OS.
     */
    {
        uint8_t probe;
        if (orig_mem_top &&
            cpu_memory_rw_debug(cs, orig_mem_top, &probe, 1, false) == 0) {
            const uint64_t max_slack = 256ULL << 20;
            for (uint64_t try = max_slack; try >= (1ULL << 20); try >>= 1) {
                if (cpu_memory_rw_debug(cs, orig_mem_top + try - 1, &probe, 1, false) == 0) {
                    slack_size = try;
                    break;
                }
            }
        }
    }

    const uint64_t vga_start = 0x000a0000ULL;
    const uint64_t vga_size = 0x00020000ULL;
    uint64_t ram_size = current_machine ? current_machine->ram_size : 0;
    if (!ram_size && env->fw_mem_size) {
        ram_size = env->fw_mem_size;
    }
    uint64_t ram_base = 0;
    uint64_t ram_limit = ram_size;
    uint64_t ram_top = ram_limit;
    if (ram_limit >= vga_start) {
        ram_top += vga_size;
    }
    {
        uint64_t addr_tag = mem_bottom;
        uint64_t mem_bottom_phys_local = ia64_phys_mode_addr(mem_bottom);
        uint64_t mem_top_phys_local = ia64_phys_mode_addr(mem_top);
        uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
        uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
        bool mem_bad = mem_top_phys_local <= mem_bottom_phys_local ||
                       (mem_top_phys_local - mem_bottom_phys_local) < (1ULL << 20);

        if (!ram_limit && (mem_bad || mem_top_phys_local)) {
            uint64_t sysmem_min = UINT64_MAX;
            uint64_t sysmem_max = 0;
            uint64_t cur = hob_base;
            for (int iter = 0; iter < 4096 && cur < hob_end; iter++) {
                uint8_t h[8];
                if (cpu_memory_rw_debug(cs, cur, h, sizeof(h), false) != 0) {
                    break;
                }
                uint16_t type = lduw_le_p(&h[0]);
                uint16_t len = lduw_le_p(&h[2]);
                if (len < sizeof(h)) {
                    break;
                }
                if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {
                    break;
                }
                if (type == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && len >= 0x30) {
                    uint8_t rh[0x30];
                    if (cpu_memory_rw_debug(cs, cur, rh, sizeof(rh), false) != 0) {
                        break;
                    }
                    uint32_t rtype = ldl_le_p(&rh[24]);
                    uint64_t start = ldq_le_p(&rh[32]);
                    uint64_t rlen = ldq_le_p(&rh[40]);
                    if (rtype == 0 && rlen) {
                        uint64_t start_phys = ia64_phys_mode_addr(start);
                        uint64_t end_phys = start_phys + rlen;
                        if (start_phys < sysmem_min) {
                            sysmem_min = start_phys;
                        }
                        if (end_phys > sysmem_max) {
                            sysmem_max = end_phys;
                        }
                    }
                }
                cur += len;
                if (cur - hob_base > (16ULL << 20)) {
                    break;
                }
            }
            if (sysmem_max > sysmem_min) {
                ram_base = sysmem_min;
                ram_limit = sysmem_max;
                ram_top = ram_limit;
                if (ram_limit >= vga_start) {
                    ram_top += vga_size;
                }
            }
        }

        if (hob_patch_trace) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: hob_patch: mem_phys=%016" PRIx64 "-%016" PRIx64
                          " free_phys=%016" PRIx64 "-%016" PRIx64
                          " mem_bad=%d ram_size=%" PRIu64 " ram_limit=%" PRIu64 "\n",
                          mem_bottom_phys_local, mem_top_phys_local,
                          free_bottom_phys, free_top_phys,
                          mem_bad ? 1 : 0, ram_size, ram_limit);
        }

        if (ram_limit && mem_bottom_phys_local != ram_base) {
            uint64_t new_mem_bottom = ram_base;
            uint64_t new_mem_top = ram_top;
            uint64_t new_free_bottom = new_mem_bottom;
            uint64_t new_free_top = new_mem_top;

            if (end_hob >= new_mem_bottom && end_hob < new_mem_top) {
                new_free_bottom = (end_hob + 0x1fULL) & ~0x1fULL;
            }
            if (new_free_bottom >= new_free_top) {
                new_free_bottom = new_mem_bottom;
                new_free_top = new_mem_top;
            }

            mem_bottom = ia64_fw_encode_addr(addr_tag, new_mem_bottom);
            mem_top = ia64_fw_encode_addr(addr_tag, new_mem_top);
            free_bottom = ia64_fw_encode_addr(addr_tag, new_free_bottom);
            free_top = ia64_fw_encode_addr(addr_tag, new_free_top);

            stq_le_p(&phit[16], mem_top);
            stq_le_p(&phit[24], mem_bottom);
            stq_le_p(&phit[32], free_top);
            stq_le_p(&phit[40], free_bottom);
            cpu_physical_memory_write(hob_base, phit, sizeof(phit));

            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: hob_patch: realign PHIT mem/free to mem=%016" PRIx64
                          "-%016" PRIx64 " free=%016" PRIx64 "-%016" PRIx64 "\n",
                          mem_bottom, mem_top, free_bottom, free_top);
        } else if (ram_limit && (mem_bad || mem_top_phys_local > ram_top)) {
            uint64_t new_mem_bottom = ram_base;
            uint64_t new_mem_top = ram_top;
            uint64_t new_free_bottom = new_mem_bottom;
            uint64_t new_free_top = new_mem_top;

            if (end_hob >= new_mem_bottom && end_hob < new_mem_top) {
                new_free_bottom = (end_hob + 0x1fULL) & ~0x1fULL;
            }

            if (stack_phys > new_mem_bottom && stack_phys < new_mem_top) {
                const uint64_t stack_guard = 1ULL << 20;
                if (stack_phys > new_mem_bottom + stack_guard) {
                    uint64_t stack_top = (stack_phys - stack_guard) & ~0xfffULL;
                    if (stack_top < new_free_top) {
                        new_free_top = stack_top;
                    }
                }
            }

            if (new_free_bottom >= new_free_top) {
                new_free_bottom = new_mem_bottom;
                new_free_top = new_mem_top;
            }

            if (new_free_bottom < new_free_top) {
                mem_bottom = ia64_fw_encode_addr(addr_tag, new_mem_bottom);
                mem_top = ia64_fw_encode_addr(addr_tag, new_mem_top);
                free_bottom = ia64_fw_encode_addr(addr_tag, new_free_bottom);
                free_top = ia64_fw_encode_addr(addr_tag, new_free_top);

                stq_le_p(&phit[16], mem_top);
                stq_le_p(&phit[24], mem_bottom);
                stq_le_p(&phit[32], free_top);
                stq_le_p(&phit[40], free_bottom);
                cpu_physical_memory_write(hob_base, phit, sizeof(phit));

                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: reset PHIT mem/free to mem=%016" PRIx64
                              "-%016" PRIx64 " free=%016" PRIx64 "-%016" PRIx64 "\n",
                              mem_bottom, mem_top, free_bottom, free_top);
            }
        } else if (ram_limit && mem_top_phys_local < ram_top) {
            uint64_t new_mem_top = ram_top;
            uint64_t free_bottom_phys_local = ia64_phys_mode_addr(free_bottom);
            uint64_t free_top_phys_local = ia64_phys_mode_addr(free_top);
            uint64_t new_free_bottom = free_bottom_phys_local;
            uint64_t new_free_top = free_top_phys_local;

            if (end_hob && (end_hob + 0x1fULL) > new_free_bottom) {
                new_free_bottom = (end_hob + 0x1fULL) & ~0x1fULL;
            }
            if (new_free_top < new_mem_top) {
                new_free_top = new_mem_top;
            }
            if (new_free_bottom >= new_free_top) {
                new_free_bottom = mem_bottom_phys_local;
                new_free_top = new_mem_top;
            }

            mem_top = ia64_fw_encode_addr(addr_tag, new_mem_top);
            free_bottom = ia64_fw_encode_addr(addr_tag, new_free_bottom);
            free_top = ia64_fw_encode_addr(addr_tag, new_free_top);
            stq_le_p(&phit[16], mem_top);
            stq_le_p(&phit[32], free_top);
            stq_le_p(&phit[40], free_bottom);
            cpu_physical_memory_write(hob_base, phit, sizeof(phit));

            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: hob_patch: expand PHIT mem_top to %016" PRIx64
                          " free=%016" PRIx64 "-%016" PRIx64 "\n",
                          mem_top, free_bottom, free_top);
        } else {
            if (free_bottom_phys < mem_bottom_phys_local ||
                free_bottom_phys > mem_top_phys_local ||
                free_bottom_phys < end_hob) {
                free_bottom_phys = (end_hob + 0x1fULL) & ~0x1fULL;
            }
            if (free_top_phys <= free_bottom_phys || free_top_phys > mem_top_phys_local) {
                free_top_phys = mem_top_phys_local;
            }
            if (free_bottom != free_bottom_phys || free_top != free_top_phys) {
                free_bottom = ia64_fw_encode_addr(addr_tag, free_bottom_phys);
                free_top = ia64_fw_encode_addr(addr_tag, free_top_phys);
                stq_le_p(&phit[32], free_top);
                stq_le_p(&phit[40], free_bottom);
                cpu_physical_memory_write(hob_base, phit, sizeof(phit));
            }
        }
    }

    env->fw_phit_mem_bottom = mem_bottom;
    env->fw_phit_mem_top = mem_top;
    env->fw_phit_free_bottom = free_bottom;
    env->fw_phit_free_top = free_top;

    mem_bottom_phys = ia64_phys_mode_addr(mem_bottom);
    mem_top_phys = ia64_phys_mode_addr(mem_top);

    if (reloc_hob_base && reloc_hob_base != hob_base && hob_end > hob_base) {
        uint64_t src_len = hob_end - hob_base;
        uint64_t dst_len = reloc_hob_end > reloc_hob_base ?
            (reloc_hob_end - reloc_hob_base) : 0;
        if (src_len && src_len <= (1ULL << 20) && dst_len != src_len) {
            if (ia64_fw_clone_hob_list_ram(cs, hob_base, hob_end, reloc_hob_base,
                                           mem_bottom_phys, mem_top_phys,
                                           stack_phys)) {
                reloc_hob_end = reloc_hob_base + src_len;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: synced HOB list %016" PRIx64
                              " -> %016" PRIx64 " len=%" PRIu64 "\n",
                              hob_base, reloc_hob_base, src_len);
            }
        }
    }

    if (enabled) {
        if (!fixed_boot_mode && boot_mode == 0x20) {
            uint8_t out[4];
            stl_le_p(out, 0x00);
            if (ia64_fw_write_bytes_any(cs, hob_base + 12, out, sizeof(out))) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: boot_mode recovery -> full\n");
                fixed_boot_mode = true;
                boot_mode = 0x00;
            }
        }

        if (fixed_fv_hobs && fixed_fv_hobs_base && fixed_fv_hobs_base != hob_base) {
            fixed_fv_hobs = false;
        }
        if (!fixed_fv_hobs) {
        enum { MAX_FV_HOBS = 8 };
        IA64FwFvInfo fv_hobs[MAX_FV_HOBS] = { 0 };
        IA64FwFvInfo fv_flash[MAX_FV_HOBS] = { 0 };
        IA64FwFvInfo fv_add[MAX_FV_HOBS] = { 0 };
        int fv_hob_count = 0;
        int fv_flash_count = 0;
        int fv_add_count = 0;

        uint64_t cur = hob_base;
        for (int iter = 0; iter < 4096 && cur < hob_end; iter++) {
            uint8_t h[8];
            if (cpu_memory_rw_debug(cs, cur, h, sizeof(h), false) != 0) {
                break;
            }
            uint16_t type = lduw_le_p(&h[0]);
            uint16_t len = lduw_le_p(&h[2]);
            if (len < sizeof(h)) {
                break;
            }
            if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {
                break;
            }
            if (type == EFI_HOB_TYPE_FV && len >= 0x18) {
                uint8_t fv[0x18];
                if (cpu_memory_rw_debug(cs, cur, fv, sizeof(fv), false) != 0) {
                    break;
                }
                uint64_t base = ldq_le_p(&fv[8]);
                uint64_t flen = ldq_le_p(&fv[16]);
                ia64_fw_fv_list_add(fv_hobs, &fv_hob_count,
                                    MAX_FV_HOBS, base, flen);
            }
            cur += len;
            if (cur - hob_base > (16ULL << 20)) {
                break;
            }
        }

        fv_flash_count = ia64_fw_scan_flash_fvs(cs, fv_flash, MAX_FV_HOBS);
        if (!logged_fv_scan) {
            logged_fv_scan = true;
            if (fv_flash_count == 0) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: flash scan found no FVH signatures\n");
            } else {
                for (int i = 0; i < fv_flash_count; i++) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: flash FV base=%016" PRIx64
                                  " len=%016" PRIx64 "\n",
                                  fv_flash[i].base, fv_flash[i].len);
                }
            }
        }
        for (int i = 0; i < fv_flash_count; i++) {
            if (!ia64_fw_fv_list_has_base(fv_hobs, fv_hob_count,
                                          fv_flash[i].base)) {
                if (fv_add_count < MAX_FV_HOBS) {
                    fv_add[fv_add_count++] = fv_flash[i];
                }
            }
        }
        if (hob_low && !logged_fv_state) {
            logged_fv_state = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: hob_patch: FV state hob=%016" PRIx64
                          " end_hob=%016" PRIx64 " free_bottom=%016" PRIx64
                          " free_top=%016" PRIx64 " hob_fv=%d flash_fv=%d add=%d\n",
                          hob_base, end_hob, free_bottom, free_top,
                          fv_hob_count, fv_flash_count, fv_add_count);
        }

        if (fv_add_count > 0 && end_hob) {
            uint64_t add_bytes = (uint64_t)fv_add_count * 0x18ULL;
            uint64_t new_end_hob = end_hob + add_bytes;
            uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
            uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
            uint64_t new_free_bottom_phys = free_bottom_phys + add_bytes;

            if (new_free_bottom_phys < free_top_phys &&
                new_free_bottom_phys > free_bottom_phys) {
                for (int i = 0; i < fv_add_count; i++) {
                    uint8_t fh[0x18] = { 0 };
                    stw_le_p(&fh[0], EFI_HOB_TYPE_FV);
                    stw_le_p(&fh[2], 0x18);
                    stq_le_p(&fh[8], fv_add[i].base);
                    stq_le_p(&fh[16], fv_add[i].len);
                    cpu_physical_memory_write(end_hob + (uint64_t)i * 0x18ULL,
                                              fh, sizeof(fh));
                }

                uint8_t endhdr[8] = { 0 };
                stw_le_p(&endhdr[0], EFI_HOB_TYPE_END_OF_HOB_LIST);
                stw_le_p(&endhdr[2], sizeof(endhdr));
                cpu_physical_memory_write(new_end_hob, endhdr, sizeof(endhdr));

                uint64_t end_hob_tmpl = ldq_le_p(&phit[48]);
                uint64_t free_bottom_tmpl = ldq_le_p(&phit[40]);
                if (!end_hob_tmpl) {
                    end_hob_tmpl = mem_bottom;
                }
                if (!free_bottom_tmpl) {
                    free_bottom_tmpl = mem_bottom;
                }
                free_bottom = ia64_fw_encode_addr(free_bottom_tmpl, new_free_bottom_phys);
                stq_le_p(&phit[48], ia64_fw_encode_addr(end_hob_tmpl, new_end_hob));
                stq_le_p(&phit[40], free_bottom);
                cpu_physical_memory_write(hob_base, phit, sizeof(phit));

                end_hob = new_end_hob;
                hob_end = new_end_hob + sizeof(endhdr);
                free_bottom_phys = new_free_bottom_phys;
                fixed_fv_hobs = true;
                fixed_fv_hobs_base = hob_base;

                env->fw_phit_free_bottom = free_bottom;
                env->fw_phit_free_top = free_top;

                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: inserted %d FV HOB(s)"
                              " end_hob %016" PRIx64 " free_bottom %016" PRIx64 "\n",
                              fv_add_count, end_hob, free_bottom);
            }
        } else if (fv_hob_count > 0 || fv_flash_count == 0) {
            fixed_fv_hobs = true;
            fixed_fv_hobs_base = hob_base;
        }
        }
    }

    if (enabled) {
        if (fixed_flashmap_vars && fixed_flashmap_base && fixed_flashmap_base != hob_base) {
            fixed_flashmap_vars = false;
        }
        if (!fixed_flashmap_vars && !in_flash) {
        bool have_vars = false;
        uint64_t cur = hob_base;
        for (int iter = 0; iter < 4096 && cur < hob_end; iter++) {
            uint8_t h[8];
            if (cpu_memory_rw_debug(cs, cur, h, sizeof(h), false) != 0) {
                break;
            }
            uint16_t type = lduw_le_p(&h[0]);
            uint16_t len = lduw_le_p(&h[2]);
            if (len < sizeof(h)) {
                break;
            }
            if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {
                break;
            }
            if (type == EFI_HOB_TYPE_GUID_EXTENSION && len >= 0x18) {
                uint8_t gh[0x20];
                if (cpu_memory_rw_debug(cs, cur, gh, sizeof(gh), false) == 0) {
                    IA64EfiGuid guid;
                    ia64_fw_guid_from_bytes(&gh[8], &guid);
                    if (ia64_fw_guid_equal(&guid, &ia64_efi_guid_flashmap_hob)) {
                        uint8_t data[0x20];
                        if (cpu_memory_rw_debug(cs, cur + 0x18, data,
                                                sizeof(data), false) == 0) {
                            uint8_t area_type = data[3];
                            if (area_type == IA64_EFI_FLASH_EFI_VARIABLES) {
                                have_vars = true;
                                break;
                            }
                        }
                    }
                }
            }
            cur += len;
            if (cur - hob_base > (16ULL << 20)) {
                break;
            }
        }

        if (have_vars) {
            fixed_flashmap_vars = true;
            fixed_flashmap_base = hob_base;
        } else {
            uint64_t var_base = 0;
            uint64_t var_len = 0;
            if (ia64_fw_find_varstore_region(cs, &var_base, &var_len) && end_hob) {
                uint8_t gh[0x58];
                const uint16_t hob_len = sizeof(gh);
                uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
                uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
                uint64_t new_end_hob = end_hob + hob_len;
                uint64_t new_free_bottom_phys = free_bottom_phys + hob_len;

                if (new_free_bottom_phys < free_top_phys &&
                    new_free_bottom_phys > free_bottom_phys) {
                    memset(gh, 0, sizeof(gh));
                    stw_le_p(&gh[0], EFI_HOB_TYPE_GUID_EXTENSION);
                    stw_le_p(&gh[2], hob_len);
                    stl_le_p(&gh[4], 0);
                    stl_le_p(&gh[8], ia64_efi_guid_flashmap_hob.data1);
                    stw_le_p(&gh[12], ia64_efi_guid_flashmap_hob.data2);
                    stw_le_p(&gh[14], ia64_efi_guid_flashmap_hob.data3);
                    memcpy(&gh[16], ia64_efi_guid_flashmap_hob.data4, 8);

                    uint8_t *data = &gh[0x18];
                    data[3] = IA64_EFI_FLASH_EFI_VARIABLES;
                    stl_le_p(&data[20], 1);
                    stl_le_p(&data[24], IA64_EFI_FLASH_SUBFV |
                                         IA64_EFI_FLASH_MEMMAPPED_FV);
                    stl_le_p(&data[28], 0);
                    stq_le_p(&data[32], var_base);
                    stq_le_p(&data[40], var_len);

                    cpu_physical_memory_write(end_hob, gh, sizeof(gh));

                    uint8_t endhdr[8] = { 0 };
                    stw_le_p(&endhdr[0], EFI_HOB_TYPE_END_OF_HOB_LIST);
                    stw_le_p(&endhdr[2], sizeof(endhdr));
                    cpu_physical_memory_write(new_end_hob, endhdr, sizeof(endhdr));

                    uint64_t end_hob_tmpl = ldq_le_p(&phit[48]);
                    uint64_t free_bottom_tmpl = ldq_le_p(&phit[40]);
                    if (!end_hob_tmpl) {
                        end_hob_tmpl = mem_bottom;
                    }
                    if (!free_bottom_tmpl) {
                        free_bottom_tmpl = mem_bottom;
                    }
                    free_bottom = ia64_fw_encode_addr(free_bottom_tmpl,
                                                      new_free_bottom_phys);
                    stq_le_p(&phit[48], ia64_fw_encode_addr(end_hob_tmpl, new_end_hob));
                    stq_le_p(&phit[40], free_bottom);
                    cpu_physical_memory_write(hob_base, phit, sizeof(phit));

                    end_hob = new_end_hob;
                    hob_end = new_end_hob + sizeof(endhdr);
                    env->fw_phit_free_bottom = free_bottom;
                    env->fw_phit_free_top = free_top;
                    fixed_flashmap_vars = true;
                    fixed_flashmap_base = hob_base;

                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: inserted flashmap vars"
                                  " base=%016" PRIx64 " len=%016" PRIx64 "\n",
                                  var_base, var_len);
                }
            }
        }
    }

    if (memtype_enabled) {
        if (fixed_memtype_hob && fixed_memtype_base && fixed_memtype_base != hob_base) {
            fixed_memtype_hob = false;
        }
        if (!fixed_memtype_hob && hob_low) {
            bool have_memtype = false;
            uint64_t cur = hob_base;
            for (int iter = 0; iter < 4096 && cur < hob_end; iter++) {
                uint8_t h[8];
                if (cpu_memory_rw_debug(cs, cur, h, sizeof(h), false) != 0) {
                    break;
                }
                uint16_t type = lduw_le_p(&h[0]);
                uint16_t len = lduw_le_p(&h[2]);
                if (len < sizeof(h)) {
                    break;
                }
                if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {
                    break;
                }
                if (type == EFI_HOB_TYPE_GUID_EXTENSION && len >= 0x18) {
                    uint8_t gh[0x20];
                    if (cpu_memory_rw_debug(cs, cur, gh, sizeof(gh), false) == 0) {
                        IA64EfiGuid guid;
                        ia64_fw_guid_from_bytes(&gh[8], &guid);
                        if (ia64_fw_guid_equal(&guid, &ia64_efi_guid_memtype_info)) {
                            have_memtype = true;
                            break;
                        }
                    }
                }
                cur += len;
                if (cur - hob_base > (16ULL << 20)) {
                    break;
                }
            }

            if (hob_patch_trace) {
                uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
                uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: memtype scan base=%016" PRIx64
                              " end=%016" PRIx64 " free=%016" PRIx64 "-%016" PRIx64
                              " have=%d\n",
                              hob_base, hob_end,
                              free_bottom_phys, free_top_phys,
                              have_memtype ? 1 : 0);
            }

            if (have_memtype) {
                fixed_memtype_hob = true;
                fixed_memtype_base = hob_base;
            } else if (end_hob) {
                const uint16_t hob_len = 0x18 + sizeof(ia64_fw_memtype_info);
                uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
                uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
                uint64_t new_end_hob = end_hob + hob_len;
                uint64_t new_free_bottom_phys = free_bottom_phys + hob_len;

                if (new_free_bottom_phys < free_top_phys &&
                    new_free_bottom_phys > free_bottom_phys) {
                    uint8_t gh[0x18 + sizeof(ia64_fw_memtype_info)] = { 0 };

                    stw_le_p(&gh[0], EFI_HOB_TYPE_GUID_EXTENSION);
                    stw_le_p(&gh[2], hob_len);
                    stl_le_p(&gh[4], 0);
                    stl_le_p(&gh[8], ia64_efi_guid_memtype_info.data1);
                    stw_le_p(&gh[12], ia64_efi_guid_memtype_info.data2);
                    stw_le_p(&gh[14], ia64_efi_guid_memtype_info.data3);
                    memcpy(&gh[16], ia64_efi_guid_memtype_info.data4, 8);
                    memcpy(&gh[0x18], ia64_fw_memtype_info, sizeof(ia64_fw_memtype_info));

                    cpu_physical_memory_write(end_hob, gh, sizeof(gh));

                    uint8_t endhdr[8] = { 0 };
                    stw_le_p(&endhdr[0], EFI_HOB_TYPE_END_OF_HOB_LIST);
                    stw_le_p(&endhdr[2], sizeof(endhdr));
                    cpu_physical_memory_write(new_end_hob, endhdr, sizeof(endhdr));

                    uint64_t end_hob_tmpl = ldq_le_p(&phit[48]);
                    uint64_t free_bottom_tmpl = ldq_le_p(&phit[40]);
                    if (!end_hob_tmpl) {
                        end_hob_tmpl = mem_bottom;
                    }
                    if (!free_bottom_tmpl) {
                        free_bottom_tmpl = mem_bottom;
                    }
                    free_bottom = ia64_fw_encode_addr(free_bottom_tmpl, new_free_bottom_phys);
                    stq_le_p(&phit[48], ia64_fw_encode_addr(end_hob_tmpl, new_end_hob));
                    stq_le_p(&phit[40], free_bottom);
                    cpu_physical_memory_write(hob_base, phit, sizeof(phit));

                    end_hob = new_end_hob;
                    hob_end = new_end_hob + sizeof(endhdr);
                    env->fw_phit_free_bottom = free_bottom;
                    env->fw_phit_free_top = free_top;
                    fixed_memtype_hob = true;
                    fixed_memtype_base = hob_base;

                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: inserted MemoryTypeInformation HOB\n");
                }
            }
        }
    }

    if (!enabled) {
        return;
    }

    if (in_flash) {
        /* Defer other HOB fixes until firmware relocates into RAM. */
        if (dump_after_patch && !dumped_after_patch) {
            if (ia64_fw_dump_efi_hobs_force(cs, stack_phys)) {
                dumped_after_patch = true;
            }
        }
    }

    if (!logged_phit) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: hob_patch: phit hob=%016" PRIx64 "-%016" PRIx64
                      " mem=%016" PRIx64 "-%016" PRIx64
                      " free=%016" PRIx64 "-%016" PRIx64 "\n",
                      hob_base, hob_end, mem_bottom, mem_top,
                      free_bottom, free_top);
        logged_phit = true;
    }

    /*
     * Some xenipf/EDK builds relocate PEI/DXE images so that the current
     * module's small-data/GOT (GP-relative) window extends into the slack RAM
     * mapped immediately above PHIT->EfiMemoryTop. DXE memory services then
     * carve the initial pool starting at EfiMemoryTop and may clear it,
     * corrupting the executing module and leading to early ASSERTs.
     *
     * If GP sits above EfiMemoryTop, advance EfiMemoryTop past GP (plus a small
     * safety margin) so the initial pool begins above the active GP window.
     */
    {
        uint64_t gp_phys = ia64_phys_mode_addr(env->r[1]);
        uint64_t mem_top_phys_local = ia64_phys_mode_addr(mem_top);
        uint64_t mem_bottom_phys_local = ia64_phys_mode_addr(mem_bottom);
        uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
        uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
        uint64_t orig_mem_top_phys = ia64_phys_mode_addr(orig_mem_top);
        /*
         * IA-64 ABI GP-relative addressing can span a fairly large window.
         * Keep a conservative margin above GP before allowing DXE pool
         * allocations to start.
         */
        const uint64_t gp_safety = 2ULL << 20;
        if (slack_size &&
            gp_phys > mem_top_phys_local &&
            gp_phys < mem_top_phys_local + slack_size &&
            gp_phys + gp_safety < mem_top_phys_local + slack_size) {
            uint64_t new_mem_top_phys = (gp_phys + gp_safety + 0xfffULL) & ~0xfffULL;
            if (new_mem_top_phys > mem_top_phys_local &&
                new_mem_top_phys < mem_bottom_phys_local + (256ULL << 20) &&
                new_mem_top_phys <= mem_top_phys_local + slack_size) {
                /*
                 * Reserve the GP window region in the HOB list so DXE doesn't
                 * later treat it as free conventional memory.
                 */
                if (!fixed_gp_alloc && end_hob) {
                    uint64_t reserve_base = orig_mem_top_phys;
                    uint64_t reserve_len = new_mem_top_phys - orig_mem_top_phys;
                    uint64_t add_bytes = 0x30ULL;
                    uint64_t new_end_hob = end_hob + add_bytes;
                    uint64_t new_free_bottom_phys = free_bottom_phys + add_bytes;

                    if (reserve_len &&
                        reserve_base >= mem_bottom_phys_local &&
                        reserve_base + reserve_len <= mem_top_phys_local + slack_size &&
                        new_free_bottom_phys < free_top_phys &&
                        new_free_bottom_phys > free_bottom_phys) {
                        uint8_t mh[0x30] = { 0 };
                        stw_le_p(&mh[0], EFI_HOB_TYPE_MEMORY_ALLOCATION);
                        stw_le_p(&mh[2], 0x30);
                        /* Name GUID left zero. */
                        stq_le_p(&mh[24], reserve_base);
                        stq_le_p(&mh[32], reserve_len);
                        stl_le_p(&mh[40], 6 /* EfiRuntimeServicesData */);
                        cpu_physical_memory_write(end_hob, mh, sizeof(mh));

                        uint8_t endhdr[8] = { 0 };
                        stw_le_p(&endhdr[0], EFI_HOB_TYPE_END_OF_HOB_LIST);
                        stw_le_p(&endhdr[2], sizeof(endhdr));
                        cpu_physical_memory_write(new_end_hob, endhdr, sizeof(endhdr));

                        end_hob = new_end_hob;
                        free_bottom_phys = new_free_bottom_phys;
                        hob_end = new_end_hob + sizeof(endhdr);
                        {
                            uint64_t end_hob_tmpl = ldq_le_p(&phit[48]);
                            uint64_t free_bottom_tmpl = ldq_le_p(&phit[40]);
                            if (!end_hob_tmpl) {
                                end_hob_tmpl = mem_bottom;
                            }
                            if (!free_bottom_tmpl) {
                                free_bottom_tmpl = mem_bottom;
                            }
                            free_bottom = ia64_fw_encode_addr(free_bottom_tmpl, free_bottom_phys);
                            stq_le_p(&phit[48], ia64_fw_encode_addr(end_hob_tmpl, end_hob));
                            stq_le_p(&phit[40], free_bottom);
                        }
                        fixed_gp_alloc = true;

                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: hob_patch: reserved gp window base=%016" PRIx64
                                      " len=%016" PRIx64 " end_hob=%016" PRIx64
                                      " free_bottom=%016" PRIx64 "\n",
                                      reserve_base, reserve_len, end_hob, free_bottom);
                    }
                }

                {
                    uint64_t mem_top_enc = ia64_fw_encode_addr(mem_top, new_mem_top_phys);
                    stq_le_p(&phit[16], mem_top_enc);
                    mem_top = mem_top_enc;
                }
                cpu_physical_memory_write(hob_base, phit, sizeof(phit));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: mem_top %016" PRIx64
                              " -> %016" PRIx64 " (gp=%016" PRIx64 " slack=%" PRIu64 ")\n",
                              mem_top, new_mem_top_phys, gp_phys, slack_size);
                env->fw_phit_mem_top = mem_top;
                mem_top_phys_local = new_mem_top_phys;
                mem_top_phys = new_mem_top_phys;
            }
        }
    }

    if (!fixed_sysmem_rdesc) {
        /*
         * Some xenipf firmware builds forget to publish EFI resource
         * descriptors for system memory and only report firmware device
         * ranges. DXE GCD initialization requires a tested system-memory
         * resource descriptor that covers EfiFreeMemoryBottom..Top.
         *
         * Materialize minimal EFI_RESOURCE_SYSTEM_MEMORY HOB(s) that describe
         * the guest RAM layout around the legacy VGA hole.
         */
        bool has_sysmem = false;
        bool inserted = false;
        uint64_t cur = hob_base;
        for (int iter = 0; iter < 4096 && cur < hob_end; iter++) {
            uint8_t h[8];
            if (cpu_memory_rw_debug(cs, cur, h, sizeof(h), false) != 0) {
                break;
            }
            uint16_t type = lduw_le_p(&h[0]);
            uint16_t len = lduw_le_p(&h[2]);
            if (len < sizeof(h)) {
                break;
            }
            if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {
                break;
            }
            if (type == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && len >= 0x30) {
                uint8_t rh[0x30];
                if (cpu_memory_rw_debug(cs, cur, rh, sizeof(rh), false) != 0) {
                    break;
                }
                uint32_t rtype = ldl_le_p(&rh[24]);
                if (rtype == 0 /* EFI_RESOURCE_SYSTEM_MEMORY */) {
                    has_sysmem = true;
                    break;
                }
            }
            cur += len;
        }

        if (!has_sysmem && mem_top_phys > mem_bottom_phys) {
            /*
             * DXE memory services bootstrap expects the tested system-memory
             * resource descriptor to extend above PHIT's EfiMemoryTop so it can
             * carve out an initial pool from the "headroom" region.
             *
             * The IPF machine maps a small slack RAM window immediately above
             * EfiMemoryTop (see IPF_FW_SLACK_SIZE). If present, include it in
             * the synthetic system-memory resource descriptor so DXE can
             * allocate without trampling the PEI workspace.
             */
            uint64_t sysmem_len = mem_top_phys - mem_bottom_phys;
            uint8_t probe;
            uint64_t orig_mem_top_phys = ia64_phys_mode_addr(orig_mem_top);
            if (slack_size &&
                cpu_memory_rw_debug(cs, orig_mem_top_phys, &probe, 1, false) == 0 &&
                cpu_memory_rw_debug(cs, orig_mem_top_phys + slack_size - 1, &probe, 1, false) == 0) {
                sysmem_len += slack_size;
            }

            struct {
                uint64_t start;
                uint64_t len;
            } segs[2];
            int nsegs = 0;
            segs[nsegs++] = (typeof(segs[0])){ .start = mem_bottom_phys, .len = sysmem_len };

            uint64_t add_bytes = (uint64_t)nsegs * 0x30ULL;
            uint64_t new_end_hob = end_hob + add_bytes;
            uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
            uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
            uint64_t new_free_bottom_phys = free_bottom_phys + add_bytes;
            if (end_hob != (hob_end - 8)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: unexpected end_hob %016" PRIx64 " hob_end %016" PRIx64 "\n",
                              end_hob, hob_end);
            }

            if (new_free_bottom_phys < free_top_phys &&
                new_free_bottom_phys > free_bottom_phys) {
                uint64_t attr = EFI_RESOURCE_ATTRIBUTE_PRESENT |
                                EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                                EFI_RESOURCE_ATTRIBUTE_TESTED |
                                EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE;

                for (int i = 0; i < nsegs; i++) {
                    uint8_t rh[0x30] = { 0 };
                    stw_le_p(&rh[0], EFI_HOB_TYPE_RESOURCE_DESCRIPTOR);
                    stw_le_p(&rh[2], 0x30);
                    stl_le_p(&rh[24], 0 /* EFI_RESOURCE_SYSTEM_MEMORY */);
                    stl_le_p(&rh[28], (uint32_t)attr);
                    stq_le_p(&rh[32], ia64_fw_encode_addr(mem_bottom, segs[i].start));
                    stq_le_p(&rh[40], segs[i].len);
                    cpu_physical_memory_write(end_hob + (uint64_t)i * 0x30ULL, rh, sizeof(rh));
                }

                uint8_t endhdr[8] = { 0 };
                stw_le_p(&endhdr[0], EFI_HOB_TYPE_END_OF_HOB_LIST);
                stw_le_p(&endhdr[2], sizeof(endhdr));
                cpu_physical_memory_write(new_end_hob, endhdr, sizeof(endhdr));

                {
                    uint64_t end_hob_tmpl = ldq_le_p(&phit[48]);
                    uint64_t free_bottom_tmpl = ldq_le_p(&phit[40]);
                    if (!end_hob_tmpl) {
                        end_hob_tmpl = mem_bottom;
                    }
                    if (!free_bottom_tmpl) {
                        free_bottom_tmpl = mem_bottom;
                    }
                    free_bottom = ia64_fw_encode_addr(free_bottom_tmpl, new_free_bottom_phys);
                    stq_le_p(&phit[48], ia64_fw_encode_addr(end_hob_tmpl, new_end_hob));
                    stq_le_p(&phit[40], free_bottom);
                }
                cpu_physical_memory_write(hob_base, phit, sizeof(phit));

                end_hob = new_end_hob;
                hob_end = new_end_hob + sizeof(endhdr);
                free_bottom_phys = new_free_bottom_phys;
                inserted = true;

                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: inserted %d sysmem resource HOB(s)"
                              " end_hob %016" PRIx64 " free_bottom %016" PRIx64 "\n",
                              nsegs, end_hob, free_bottom);
            }
        }
        fixed_sysmem_rdesc = has_sysmem || inserted;
        if (fixed_sysmem_rdesc) {
            fixed_sysmem_rdesc_base = hob_base;
        }
        env->fw_phit_mem_bottom = mem_bottom;
        env->fw_phit_mem_top = mem_top;
        env->fw_phit_free_bottom = free_bottom;
        env->fw_phit_free_top = free_top;
    }

    if (enabled && !fixed_free_bottom) {
        /*
         * Some xenipf firmware builds leave EfiFreeMemoryBottom unset (0),
         * which later causes DXE pool sizing code to treat EfiFreeMemoryTop
         * (an address) as a byte size and attempt multi-hundred-megabyte
         * allocations. Default the free bottom to the end of the active HOB
         * list when it is missing.
         */
        if (free_bottom == 0) {
            uint64_t mem_bottom_phys_local = ia64_phys_mode_addr(mem_bottom);
            uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
            uint64_t mem_top_phys_local = ia64_phys_mode_addr(mem_top);
            uint64_t new_free_bottom_phys = (hob_end + 7) & ~0x7ULL;
            if (new_free_bottom_phys > mem_bottom_phys_local &&
                new_free_bottom_phys < free_top_phys &&
                new_free_bottom_phys <= mem_top_phys_local) {
                uint64_t free_bottom_tmpl = mem_bottom ? mem_bottom : free_top;
                free_bottom = ia64_fw_encode_addr(free_bottom_tmpl, new_free_bottom_phys);
                stq_le_p(&phit[40], free_bottom);
                cpu_physical_memory_write(hob_base, phit, sizeof(phit));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: free_bottom %016" PRIx64
                              " -> %016" PRIx64 "\n",
                              free_bottom, new_free_bottom_phys);
            }
        }
        fixed_free_bottom = (free_bottom != 0);
    }

    if (attempts == 1) {
        (void)ia64_fw_dump_efi_hobs(cs, stack_phys);
    }

    uint64_t mem_bottom_phys_local = ia64_phys_mode_addr(mem_bottom);
    uint64_t orig_mem_top_phys = ia64_phys_mode_addr(orig_mem_top);
    uint64_t cur = hob_base;
    for (int iter = 0; iter < 4096 && cur < hob_end; iter++) {
        uint8_t h[8];
        if (cpu_memory_rw_debug(cs, cur, h, sizeof(h), false) != 0) {
            break;
        }
        uint16_t type = lduw_le_p(&h[0]);
        uint16_t len = lduw_le_p(&h[2]);
        if (len < sizeof(h)) {
            break;
        }
        if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {
            break;
        }
        if (type == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && len >= 0x30) {
            uint8_t rh[0x30];
            if (cpu_memory_rw_debug(cs, cur, rh, sizeof(rh), false) != 0) {
                break;
            }
            uint32_t rtype = ldl_le_p(&rh[24]);
            uint64_t start = ldq_le_p(&rh[32]);
            uint64_t rlen = ldq_le_p(&rh[40]);
            uint32_t rattr = ldl_le_p(&rh[28]);
            uint64_t start_phys = ia64_phys_mode_addr(start);

            if (rtype == 0 && start_phys == mem_bottom_phys_local) {
                uint64_t start_fix = ia64_fw_encode_addr(mem_bottom, start_phys);
                if (start != start_fix) {
                    stq_le_p(&rh[32], start_fix);
                    cpu_physical_memory_write(cur, rh, sizeof(rh));
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: sysmem start %016" PRIx64
                                  " -> %016" PRIx64 " at hob=%016" PRIx64 "\n",
                                  start, start_fix, cur);
                    start = start_fix;
                }
            }

        if (!fixed_pei_span &&
            start_phys == mem_bottom_phys_local &&
            (orig_mem_top_phys > mem_bottom_phys_local) &&
            rlen == (orig_mem_top_phys - mem_bottom_phys_local)) {
            uint64_t extra = slack_size ? slack_size : (8ULL << 20);
            uint64_t new_len = rlen + extra;
                stq_le_p(&rh[40], new_len);
                cpu_physical_memory_write(cur, rh, sizeof(rh));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: PEI mem len %016" PRIx64 " -> %016" PRIx64
                              " at hob=%016" PRIx64 "\n",
                              rlen, new_len, cur);
            fixed_pei_span = true;
            /* Refresh rlen for subsequent matches in this iteration. */
            rlen = new_len;
        }
        if (ram_top &&
            start_phys == mem_bottom_phys_local &&
            ram_top > mem_bottom_phys_local &&
            rlen < (ram_top - mem_bottom_phys_local)) {
            uint64_t new_len = ram_top - mem_bottom_phys_local;
            stq_le_p(&rh[40], new_len);
            cpu_physical_memory_write(cur, rh, sizeof(rh));
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: hob_patch: expand sysmem len %016" PRIx64 " -> %016" PRIx64
                          " at hob=%016" PRIx64 "\n",
                          rlen, new_len, cur);
            rlen = new_len;
        }

            if (start_phys == mem_bottom_phys_local &&
                (rattr & (EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |
                          EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE)) ==
                    (EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |
                     EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE)) {
                uint32_t new_attr = rattr & ~EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE;
                stl_le_p(&rh[28], new_attr);
                cpu_physical_memory_write(cur, rh, sizeof(rh));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: PEI mem attr 0x%08x -> 0x%08x at hob=%016" PRIx64 "\n",
                              rattr, new_attr, cur);
                fixed_attr = true;
            }
        }
        cur += len;
        if (cur - hob_base > (16ULL << 20)) {
            break;
        }
    }

    if (!fixed_free_top) {
        /*
         * The xenipf EDK firmware relocates some modules very close to the top
         * of PEI memory. With incorrect/absent memory allocation HOBs, DXE can
         * later treat parts of the relocated image's small-data/GOT area as
         * free pool space and zero it, leading to early ASSERTs.
         *
         * Clamp EfiFreeMemoryTop below the beginning of the current module's
         * GP-relative window (GP-2MiB, 4KiB aligned) when that window sits
         * near the reported free_top.
         */
        uint64_t gp_phys = ia64_phys_mode_addr(env->r[1]);
        uint64_t gp_low = 0;
        if (gp_phys >= (2ULL << 20)) {
            gp_low = (gp_phys - (2ULL << 20)) & ~0xfffULL;
        }

        uint64_t free_top_phys = ia64_phys_mode_addr(free_top);
        uint64_t free_bottom_phys = ia64_phys_mode_addr(free_bottom);
        uint64_t mem_top_phys_local = ia64_phys_mode_addr(mem_top);
        if (gp_low &&
            gp_phys > free_top_phys &&
            gp_low < free_top_phys &&
            gp_low < mem_top_phys_local &&
            gp_low > free_bottom_phys &&
            (free_top_phys - gp_low) <= (256ULL << 10)) {
            uint64_t free_top_tmpl = free_top ? free_top : mem_top;
            free_top = ia64_fw_encode_addr(free_top_tmpl, gp_low);
            stq_le_p(&phit[32], free_top);
            cpu_physical_memory_write(hob_base, phit, sizeof(phit));
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: hob_patch: free_top %016" PRIx64 " -> %016" PRIx64
                          " (gp=%016" PRIx64 ")\n",
                          free_top, gp_low, gp_phys);
            fixed_free_top = true;
        }
    }

    if (stack_phys && hob_base && hob_end > hob_base &&
        mem_top_phys > mem_bottom_phys) {
        uint8_t scan[0x400];
        uint64_t stack_page = stack_phys & ~0xfffULL;
        uint64_t temp_base = ia64_fw_pei_cached_temp_base;
        uint64_t temp_size = ia64_fw_pei_cached_temp_size;
        uint64_t best_cand = 0;
        uint64_t best_cand_raw = 0;
        uint64_t best_cand_loc = 0;
        bool best_cand_from_reg = false;
        if (in_hob_loop) {
            uint64_t cand_raw = env->r[33];
            uint64_t cand = ia64_phys_mode_addr(cand_raw);
            bool cand_in_temp = temp_base && temp_size &&
                cand >= temp_base && cand < (temp_base + temp_size);
            if (cand && cand < flash_base &&
                (cand & 0xfffULL) == 0 &&
                cand != reloc_hob_base &&
                !cand_in_temp) {
                uint64_t cand_end = 0;
                int cand_count = 0;
                if (!ia64_fw_validate_efi_hob_list(cs, cand, &cand_end, &cand_count)) {
                    best_cand = cand;
                    best_cand_raw = cand_raw;
                    best_cand_loc = 0;
                    best_cand_from_reg = true;
                    hob_ptr_stack_page = stack_page;
                }
            }
        }
        if (!best_cand &&
            cpu_memory_rw_debug(cs, stack_page, scan, sizeof(scan), false) == 0) {
            for (size_t off = 0; off + 8 <= sizeof(scan); off += 8) {
                uint64_t cand_raw = ldq_le_p(&scan[off]);
                uint64_t cand = ia64_phys_mode_addr(cand_raw);
                bool cand_in_temp = temp_base && temp_size &&
                    cand >= temp_base && cand < (temp_base + temp_size);
                if (!cand || cand >= flash_base) {
                    continue;
                }
                if ((cand & 0xfffULL) != 0) {
                    continue;
                }
                if (cand == reloc_hob_base) {
                    continue;
                }
                if (cand_in_temp) {
                    continue;
                }

                uint64_t cand_end = 0;
                int cand_count = 0;
                if (ia64_fw_validate_efi_hob_list(cs, cand, &cand_end, &cand_count)) {
                    continue;
                }
                if (!best_cand || cand < best_cand) {
                    best_cand = cand;
                    best_cand_raw = cand_raw;
                    best_cand_loc = stack_page + off;
                    best_cand_from_reg = false;
                    hob_ptr_stack_page = stack_page;
                }
            }
        }

        if (best_cand) {
            bool reloc_valid = false;
            if (reloc_hob_base) {
                uint64_t reloc_end = 0;
                int reloc_count = 0;
                reloc_valid = ia64_fw_validate_efi_hob_list(cs, reloc_hob_base,
                                                           &reloc_end,
                                                           &reloc_count);
                if (!reloc_valid) {
                    reloc_hob_base = 0;
                    env->fw_hob_reloc_base = 0;
                    reloc_hob_end = 0;
                } else {
                    reloc_hob_end = reloc_end;
                }
            }

            if (!dumped_reloc_hob) {
                uint8_t raw[64];
                if (cpu_memory_rw_debug(cs, best_cand, raw, sizeof(raw), false) == 0) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: candidate HOB list %016" PRIx64
                                  " (raw=%016" PRIx64 ") invalid, hexdump:\n",
                                  best_cand, best_cand_raw);
                    for (int i = 0; i < (int)sizeof(raw); i += 16) {
                        char line[128];
                        int pos = 0;
                        pos += snprintf(line + pos, sizeof(line) - pos,
                                        "  %016" PRIx64 ":", best_cand + i);
                        for (int j = 0; j < 16; j++) {
                            pos += snprintf(line + pos, sizeof(line) - pos,
                                            " %02x", raw[i + j]);
                        }
                        qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
                    }
                }
                dumped_reloc_hob = true;
            }

            if (reloc_hob_base && reloc_valid) {
                uint64_t enc = ia64_fw_encode_addr(best_cand_raw, reloc_hob_base);
                if (best_cand_from_reg) {
                    env->r[33] = enc;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: redirect hob_ptr r33 -> %016" PRIx64 "\n",
                                  reloc_hob_base);
                } else if (best_cand_loc) {
                    uint8_t val[8];
                    stq_le_p(val, enc);
                    cpu_physical_memory_write(best_cand_loc, val, sizeof(val));
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: redirect hob_ptr @%016" PRIx64
                                  " -> %016" PRIx64 "\n",
                                  best_cand_loc, reloc_hob_base);
                }
                if (stack_phys) {
                    uint8_t val[8];
                    if (cpu_memory_rw_debug(cs, stack_phys, val, sizeof(val), false) == 0) {
                        uint64_t sp_raw = ldq_le_p(val);
                        uint64_t sp_phys = ia64_phys_mode_addr(sp_raw);
                        bool sp_in_reloc = reloc_hob_end &&
                            sp_phys >= reloc_hob_base && sp_phys < reloc_hob_end;
                        bool sp_valid = sp_in_reloc;
                        if (!sp_valid && sp_raw) {
                            sp_valid = ia64_fw_validate_efi_hob_list(cs, sp_phys, NULL, NULL);
                        }
                        if (!sp_valid ||
                            sp_phys == hob_base ||
                            sp_phys == best_cand ||
                            (sp_phys < flash_base &&
                             (sp_phys & 0xfffULL) == 0)) {
                            uint64_t sp_tmpl = sp_raw ? sp_raw : best_cand_raw;
                            stq_le_p(val, ia64_fw_encode_addr(sp_tmpl, reloc_hob_base));
                            cpu_physical_memory_write(stack_phys, val, sizeof(val));
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: hob_patch: redirect hob_ptr sp @%016" PRIx64
                                          " -> %016" PRIx64 "\n",
                                          (uint64_t)stack_phys, reloc_hob_base);
                        }
                    }
                }
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: clone list_len=%" PRIu64
                              " hob_base=%016" PRIx64 " hob_end=%016" PRIx64 "\n",
                              hob_end - hob_base, hob_base, hob_end);
                if (!ia64_fw_clone_hob_list_ram(cs, hob_base, hob_end, best_cand,
                                               mem_bottom_phys, mem_top_phys,
                                               stack_phys)) {
                    return;
                }
                reloc_hob_base = best_cand;
                env->fw_hob_reloc_base = reloc_hob_base;
                reloc_hob_end = best_cand + (hob_end - hob_base);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: cloned HOB list %016" PRIx64
                              " -> %016" PRIx64 "\n",
                              hob_base, best_cand);
                uint64_t enc = ia64_fw_encode_addr(best_cand_raw, best_cand);
                if (best_cand_from_reg) {
                    env->r[33] = enc;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: reset hob_ptr r33 -> %016" PRIx64 "\n",
                                  best_cand);
                } else if (best_cand_loc) {
                    uint8_t val[8];
                    stq_le_p(val, enc);
                    cpu_physical_memory_write(best_cand_loc, val, sizeof(val));
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: reset hob_ptr @%016" PRIx64
                                  " -> %016" PRIx64 "\n",
                                  best_cand_loc, best_cand);
                }
                if (stack_phys) {
                    uint8_t val[8];
                    if (cpu_memory_rw_debug(cs, stack_phys, val, sizeof(val), false) == 0) {
                        uint64_t sp_raw = ldq_le_p(val);
                        uint64_t sp_phys = ia64_phys_mode_addr(sp_raw);
                        if (!sp_raw ||
                            sp_phys == hob_base ||
                            sp_phys == best_cand ||
                            (sp_phys < flash_base &&
                             (sp_phys & 0xfffULL) == 0)) {
                            uint64_t sp_tmpl = sp_raw ? sp_raw : best_cand_raw;
                            stq_le_p(val, ia64_fw_encode_addr(sp_tmpl, best_cand));
                            cpu_physical_memory_write(stack_phys, val, sizeof(val));
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: hob_patch: reset hob_ptr sp @%016" PRIx64
                                          " -> %016" PRIx64 "\n",
                                          (uint64_t)stack_phys, best_cand);
                        }
                    }
                }
            }
        }
    }

    if (reloc_hob_base && stack_phys && hob_ptr_stack_page &&
        ((stack_phys & ~0xfffULL) == hob_ptr_stack_page)) {
        uint8_t val[8];
        if (cpu_memory_rw_debug(cs, stack_phys, val, sizeof(val), false) == 0) {
            uint64_t sp_raw = ldq_le_p(val);
            uint64_t sp_phys = ia64_phys_mode_addr(sp_raw);
            if (sp_phys >= hob_base && sp_phys < hob_end) {
                uint64_t sp_enc = ia64_fw_encode_addr(sp_raw, reloc_hob_base);
                if (sp_enc != sp_raw) {
                    stq_le_p(val, sp_enc);
                    cpu_physical_memory_write(stack_phys, val, sizeof(val));
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: hob_patch: updated sp hob_ptr @%016" PRIx64
                                  " %016" PRIx64 " -> %016" PRIx64 "\n",
                                  (uint64_t)stack_phys, sp_raw, sp_enc);
                }
            }
        }
    }
    if (reloc_hob_base && stack_phys && in_hob_loop) {
        uint8_t val[8];
        if (cpu_memory_rw_debug(cs, stack_phys, val, sizeof(val), false) == 0) {
            uint64_t sp_raw = ldq_le_p(val);
            uint64_t sp_phys = ia64_phys_mode_addr(sp_raw);
            bool sp_in_reloc = reloc_hob_end &&
                sp_phys >= reloc_hob_base && sp_phys < reloc_hob_end;
            bool sp_valid = sp_in_reloc;
            if (!sp_valid && sp_raw) {
                sp_valid = ia64_fw_validate_efi_hob_list(cs, sp_phys, NULL, NULL);
            }
            if (!sp_valid && sp_phys != reloc_hob_base) {
                uint64_t sp_tmpl = sp_raw ? sp_raw : reloc_hob_base;
                stq_le_p(val, ia64_fw_encode_addr(sp_tmpl, reloc_hob_base));
                cpu_physical_memory_write(stack_phys, val, sizeof(val));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hob_patch: in-loop redirect hob_ptr sp @%016" PRIx64
                              " %016" PRIx64 " -> %016" PRIx64 "\n",
                              (uint64_t)stack_phys, sp_raw, reloc_hob_base);
            }
        }
    }

    if (dump_after_patch && !dumped_after_patch) {
        if (ia64_fw_dump_efi_hobs_force(cs, stack_phys)) {
            dumped_after_patch = true;
        }
    }
}

}

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
    /*
     * Use this frequently-invoked helper as a hook to install missing EFI
     * configuration table entries before DXE drivers assert. Throttle probing
     * to avoid scanning memory repeatedly during early PEI.
     */
    static uint64_t fp_calls;
    if (!env->fw_sal_systab_installed && ((++fp_calls & 0xfff) == 0)) {
        ia64_fw_try_install_sal_systab(env);
    }

    static int trace_enabled = -1;
    static int trace_count;
    static int trace_limit = -1;
    static uint64_t trace_match_len = UINT64_MAX;
    static uint64_t trace_match_src = UINT64_MAX;
    static uint64_t trace_match_dst_lo = UINT64_MAX;
    static uint64_t trace_match_dst_hi = UINT64_MAX;
    if (trace_enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_FASTPATH_TRACE");
        trace_enabled = (s && *s) ? 1 : 0;
    }
    if (trace_limit == -1) {
        trace_limit = 64;
        const char *s = getenv("QEMU_IA64_FW_FASTPATH_TRACE_LIMIT");
        if (s && *s) {
            trace_limit = atoi(s);
        }
        if (trace_limit < 0) {
            trace_limit = 0;
        }
        if (trace_limit > 1000000) {
            trace_limit = 1000000;
        }
    }
    if (trace_match_len == UINT64_MAX) {
        trace_match_len = 0;
        const char *s = getenv("QEMU_IA64_FW_FASTPATH_TRACE_MATCH_LEN");
        if (s && *s) {
            trace_match_len = strtoull(s, NULL, 0);
        }
    }
    if (trace_match_src == UINT64_MAX) {
        trace_match_src = 0;
        const char *s = getenv("QEMU_IA64_FW_FASTPATH_TRACE_MATCH_SRC");
        if (s && *s) {
            trace_match_src = ia64_phys_mode_addr(strtoull(s, NULL, 0));
        }
    }
    if (trace_match_dst_lo == UINT64_MAX) {
        trace_match_dst_lo = 0;
        trace_match_dst_hi = 0;
        const char *s = getenv("QEMU_IA64_FW_FASTPATH_TRACE_MATCH_DST_RANGE");
        if (s && *s) {
            char *endp = NULL;
            uint64_t lo = strtoull(s, &endp, 0);
            if (endp && endp != s) {
                while (*endp && (isspace((unsigned char)*endp) || *endp == ',')) {
                    endp++;
                }
                if (*endp == ':' || *endp == '-') {
                    endp++;
                } else if (*endp == '.' && endp[1] == '.') {
                    endp += 2;
                }
                while (*endp && isspace((unsigned char)*endp)) {
                    endp++;
                }
                if (*endp) {
                    uint64_t hi = strtoull(endp, NULL, 0);
                    if (hi > lo) {
                        trace_match_dst_lo = ia64_phys_mode_addr(lo);
                        trace_match_dst_hi = ia64_phys_mode_addr(hi);
                    }
                }
            }
        }
    }
    if (ri != 0) {
        return 0;
    }
    if (env->psr & IA64_PSR_DT) {
        /* Only handle physical-mode loops. */
        return 0;
    }

    ia64_fw_try_patch_efi_hobs(env);

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

        bool match = (trace_match_len && count == trace_match_len) ||
                     (trace_match_src && src_phys == trace_match_src) ||
                     (trace_match_dst_hi > trace_match_dst_lo &&
                      dst_phys >= trace_match_dst_lo &&
                      dst_phys < trace_match_dst_hi);
        if (trace_enabled && (match || trace_count++ < trace_limit)) {
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

    /*
     * Bytewise memcpy loop (forward), variant used by PEI memory services:
     *   struct { dst, src, ..., count } at r12 offsets 0,8,32.
     *   exit at pc + 0xc0.
     *
     * This loop decrements count before the copy and stores -1 on exit, just
     * like the main memcpy loop above.
     */
    if (low0 == 0x01e021001880f811ULL &&
        high0 == 0x2000000000420031ULL &&
        low1 == 0x00f010183e00f80bULL &&
        high1 == 0x84006203e070007cULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t tmp[8];
        cpu_physical_memory_read(frame + 0, tmp, sizeof(tmp));
        uint64_t dst_raw = ldq_le_p(tmp);
        cpu_physical_memory_read(frame + 8, tmp, sizeof(tmp));
        uint64_t src_raw = ldq_le_p(tmp);
        cpu_physical_memory_read(frame + 32, tmp, sizeof(tmp));
        uint64_t count = ldq_le_p(tmp);

        hwaddr dst_phys = ia64_phys_mode_addr(dst_raw);
        hwaddr src_phys = ia64_phys_mode_addr(src_raw);

        /*
         * This loop head is also used by EDK's CopyMem() backward-copy path.
         * In that case the frame holds end pointers (dst_end/src_end) and the
         * original (start) pointers remain in the incoming argument registers.
         *
         * Detect that case by checking:
         *   dst_end == dst_arg + (count - 1)
         *   src_end == src_arg + (count - 1)
         *
         * When true, accelerate the remaining bytes by copying from the start
         * pointers and then update the end pointers the way the loop would
         * leave them (dst/src decremented by count, count stored as -1).
         */
        bool backward = false;
        hwaddr copy_dst = dst_phys;
        hwaddr copy_src = src_phys;
        if (count) {
            hwaddr arg_dst = ia64_phys_mode_addr(env->r[32]);
            hwaddr arg_src = ia64_phys_mode_addr(env->r[33]);
            if (dst_phys == arg_dst + (count - 1) &&
                src_phys == arg_src + (count - 1)) {
                backward = true;
                copy_dst = arg_dst;
                copy_src = arg_src;
            }
        }

        if (count && !ia64_fw_fastpath_copy(copy_dst, copy_src, count)) {
            return 0;
        }

        bool match = (trace_match_len && count == trace_match_len) ||
                     (trace_match_src && (backward ? copy_src : src_phys) == trace_match_src) ||
                     (trace_match_dst_hi > trace_match_dst_lo &&
                      (backward ? copy_dst : dst_phys) >= trace_match_dst_lo &&
                      (backward ? copy_dst : dst_phys) < trace_match_dst_hi);
        if (trace_enabled && (match || trace_count++ < trace_limit)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath memcpy(v2%s) pc=%016" PRIx64
                          " dst=%016" PRIx64 " src=%016" PRIx64
                          " len=%" PRIu64 "\n",
                          backward ? ",back" : "", pc, dst_raw, src_raw, count);
        }

        if (backward) {
            dst_raw -= count;
            src_raw -= count;
        } else {
            dst_raw += count;
            src_raw += count;
        }
        count = UINT64_MAX;

        stq_le_p(tmp, dst_raw);
        cpu_physical_memory_write(frame + 0, tmp, sizeof(tmp));
        stq_le_p(tmp, src_raw);
        cpu_physical_memory_write(frame + 8, tmp, sizeof(tmp));
        stq_le_p(tmp, count);
        cpu_physical_memory_write(frame + 32, tmp, sizeof(tmp));

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

            bool match = (trace_match_len && count == trace_match_len) ||
                         (trace_match_dst_hi > trace_match_dst_lo &&
                          dst_phys >= trace_match_dst_lo &&
                          dst_phys < trace_match_dst_hi);
            if (trace_enabled && (match || trace_count++ < trace_limit)) {
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

    /*
     * Zero-byte loop seen in PEI:
     *   counter32 at [r12+16], base ptr at [r12+56], limit = 0x3440.
     *   increments counter and stores zero byte to base+counter.
     * Exit target is pc + 0xa0.
     */
    if (low0 == 0x81e021001840f811ULL &&
        high0 == 0x2000000000420030ULL &&
        low1 == 0x09f010103e00f80bULL &&
        high1 == 0x000400000042007cULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t tmp[8];
        cpu_physical_memory_read(frame + 16, tmp, 4);
        uint32_t count = ldl_le_p(tmp);
        const uint32_t limit = 0x3440;
        if (count < limit) {
            cpu_physical_memory_read(frame + 56, tmp, sizeof(tmp));
            hwaddr base_raw = ldq_le_p(tmp);
            hwaddr base = ia64_phys_mode_addr(base_raw);
            uint64_t start = base + (uint64_t)count + 1;
            uint64_t len = (uint64_t)limit - count - 1;

            if (len && !ia64_fw_fastpath_fill(start, len, 0)) {
                return 0;
            }

            stl_le_p(tmp, limit);
            cpu_physical_memory_write(frame + 16, tmp, 4);

            if (trace_enabled && trace_count++ < trace_limit) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "fw_fastpath zero-loop pc=%016" PRIx64
                              " base=%016" PRIx64 " count=%u len=%" PRIu64 "\n",
                              pc, base_raw, count, len);
            }

            env->ip = pc + 0xa0;
            env->psr &= ~PSR_RI_MASK;
            return 1;
        }
    }

    /*
     * Table copy loop seen in DXE:
     *   counter64 at [r12+160], src base at [r12+152], dst base at [r12+176].
     *   limit entries at [r12+432] (already scaled to 8-byte entries).
     *   increments counter, copies src[count] -> dst[count] until count >= limit.
     * Exit target is pc + 0xd0.
     */
    if (low0 == 0x01e021011880f811ULL &&
        high0 == 0x2000000000420231ULL &&
        low1 == 0x09f010183e00f80bULL &&
        high1 == 0x000400000042007cULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t tmp[8];
        cpu_physical_memory_read(frame + 160, tmp, sizeof(tmp));
        uint64_t count = ldq_le_p(tmp);
        cpu_physical_memory_read(frame + 432, tmp, sizeof(tmp));
        uint64_t limit = ldq_le_p(tmp);

        uint64_t next = count + 1;
        if (next >= limit) {
            stq_le_p(tmp, next);
            cpu_physical_memory_write(frame + 160, tmp, sizeof(tmp));
            env->ip = pc + 0xd0;
            env->psr &= ~PSR_RI_MASK;
            return 1;
        }

        cpu_physical_memory_read(frame + 152, tmp, sizeof(tmp));
        uint64_t src_raw = ldq_le_p(tmp);
        cpu_physical_memory_read(frame + 176, tmp, sizeof(tmp));
        uint64_t dst_raw = ldq_le_p(tmp);
        hwaddr src_phys = ia64_phys_mode_addr(src_raw);
        hwaddr dst_phys = ia64_phys_mode_addr(dst_raw);

        uint64_t entries = limit - next;
        uint64_t offset = next << 3;
        uint64_t bytes = entries << 3;
        if (bytes && !ia64_fw_fastpath_copy(dst_phys + offset,
                                            src_phys + offset,
                                            bytes)) {
            return 0;
        }

        if (trace_enabled && trace_count++ < trace_limit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath table-copy pc=%016" PRIx64
                          " src=%016" PRIx64 " dst=%016" PRIx64
                          " next=%" PRIu64 " limit=%" PRIu64 "\n",
                          pc, src_raw, dst_raw, next, limit);
        }

        stq_le_p(tmp, limit);
        cpu_physical_memory_write(frame + 160, tmp, sizeof(tmp));

        env->ip = pc + 0xd0;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    /*
     * Cache flush loop used after code relocation:
     *   fc r32; r32 += 32; cmp.ltu p14,p15=r32,r31; (p14) br back.
     * Exit target is pc + 0x20.
     */
    if (low0 == 0x0200043040000002ULL &&
        high0 == 0xd03cfa01c0420081ULL &&
        low1 == 0x000000010000001dULL &&
        high1 == 0x4afffff007000200ULL) {
        uint64_t start_raw = env->r[32];
        uint64_t end_raw = env->r[31];

        uint64_t final_raw;
        if (start_raw >= end_raw) {
            final_raw = start_raw + 32;
        } else {
            uint64_t diff = end_raw - start_raw;
            uint64_t iters = (diff + 31) >> 5;
            final_raw = start_raw + (iters << 5);
        }

        hwaddr start_phys = ia64_phys_mode_addr(start_raw);
        hwaddr end_phys = ia64_phys_mode_addr(end_raw);
        hwaddr last_phys = (start_phys >= end_phys) ? start_phys : end_phys - 1;
        hwaddr start_page = start_phys & TARGET_PAGE_MASK;
        hwaddr end_page = last_phys & TARGET_PAGE_MASK;
        if (end_page >= start_page) {
            CPUState *cs = env_cpu(env);
            tb_invalidate_phys_range(cs, start_page,
                                     end_page + TARGET_PAGE_SIZE - 1);
            env->fc_last_page = end_page;
        }

        if (trace_enabled && trace_count++ < trace_limit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath fc-loop pc=%016" PRIx64
                          " start=%016" PRIx64 " end=%016" PRIx64
                          " final=%016" PRIx64 "\n",
                          pc, start_raw, end_raw, final_raw);
        }

        env->r[32] = final_raw;
        env->ip = pc + 0x20;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    /*
     * Byte histogram loop used during PEI:
     *   count16 at [r12+46], limit16 at [r12+152], src base at [r12+160].
     *   table base at [r12+48] (512 bytes of 16-bit counters).
     *   increments count, then for each src byte increments table[val].
     * Exit target is pc + 0x100.
     */
    if (low0 == 0x71e0210018b8f811ULL &&
        high0 == 0x2000000000420031ULL &&
        low1 == 0x09f010083e00f80bULL &&
        high1 == 0x000400000042007cULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t tmp[8];

        cpu_physical_memory_read(frame + 46, tmp, 2);
        uint32_t count = lduw_le_p(tmp);
        cpu_physical_memory_read(frame + 152, tmp, 2);
        uint32_t limit = lduw_le_p(tmp);

        uint32_t start = count + 1;
        if (start >= limit) {
            uint16_t new_count = (uint16_t)start;
            stw_le_p(tmp, new_count);
            cpu_physical_memory_write(frame + 46, tmp, 2);
            env->ip = pc + 0x100;
            env->psr &= ~PSR_RI_MASK;
            return 1;
        }

        cpu_physical_memory_read(frame + 160, tmp, sizeof(tmp));
        uint64_t src_raw = ldq_le_p(tmp);
        hwaddr src_phys = ia64_phys_mode_addr(src_raw);
        hwaddr table_base = frame + 48;

        uint32_t len = limit - start;
        g_autofree uint8_t *src_buf = g_malloc(len);
        cpu_physical_memory_read(src_phys + start, src_buf, len);

        uint8_t table[512];
        cpu_physical_memory_read(table_base, table, sizeof(table));
        for (uint32_t i = 0; i < len; i++) {
            uint8_t val = src_buf[i];
            uint16_t entry = lduw_le_p(table + (val << 1));
            entry++;
            stw_le_p(table + (val << 1), entry);
        }
        cpu_physical_memory_write(table_base, table, sizeof(table));

        if (trace_enabled && trace_count++ < trace_limit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath hist-loop pc=%016" PRIx64
                          " src=%016" PRIx64 " start=%u limit=%u\n",
                          pc, src_raw, start, limit);
        }

        stw_le_p(tmp, (uint16_t)limit);
        cpu_physical_memory_write(frame + 46, tmp, 2);

        env->ip = pc + 0x100;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    /*
     * Byte-indexed table clear loop:
     *   counter8 at [r12+0], table base ptr at [r12+8].
     *   increments counter, then zeros table[(counter) * 8] in the block
     *   starting at base+56 until counter > 63.
     * Exit target is pc + 0x90.
     */
    if (low0 == 0x01e021001800f811ULL &&
        high0 == 0x2000000000420030ULL &&
        low1 == 0x09f010003e00f80bULL &&
        high1 == 0x000400000042007cULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t count = 0;
        cpu_physical_memory_read(frame + 0, &count, 1);

        uint32_t start = (uint32_t)count + 1;
        uint8_t final_count = (uint8_t)start;
        if (start <= 63) {
            uint8_t tmp[8];
            cpu_physical_memory_read(frame + 8, tmp, sizeof(tmp));
            uint64_t base_raw = ldq_le_p(tmp);
            hwaddr base = ia64_phys_mode_addr(base_raw) + 56;

            uint32_t entries = 64 - start;
            uint64_t len = (uint64_t)entries << 3;
            if (len && !ia64_fw_fastpath_fill(base + ((uint64_t)start << 3),
                                              len, 0)) {
                return 0;
            }
            final_count = 64;
        }

        cpu_physical_memory_write(frame + 0, &final_count, 1);

        if (trace_enabled && trace_count++ < trace_limit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath tblclr pc=%016" PRIx64
                          " count=%u final=%u\n",
                          pc, count, final_count);
        }

        env->ip = pc + 0x90;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    /*
     * ASCII to UTF-16 copy loop:
     *   src ptr at [r12+2616], dest base at [r12+24],
     *   count at [r12+536], copies bytes until NUL or count >= 254.
     * Exit target is pc + 0xF0.
     */
    if (low0 == 0x01f0211418e0f80bULL &&
        high0 == 0x000400000020307cULL &&
        low1 == 0x00f010003e00f80aULL &&
        high1 == 0x000400000071007cULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t tmp[8];

        cpu_physical_memory_read(frame + 536, tmp, sizeof(tmp));
        uint64_t count = ldq_le_p(tmp);

        cpu_physical_memory_read(frame + 2616, tmp, sizeof(tmp));
        uint64_t src_raw = ldq_le_p(tmp);
        hwaddr src_phys = ia64_phys_mode_addr(src_raw);

        hwaddr dst_phys = frame + 24 + (count << 1);
        uint64_t max = (count < 254) ? (254 - count) : 0;
        uint64_t len = 0;

        if (max) {
            g_autofree uint8_t *src_buf = g_malloc(max);
            cpu_physical_memory_read(src_phys, src_buf, max);
            while (len < max && src_buf[len] != 0) {
                len++;
            }

            if (len) {
                g_autofree uint8_t *dst_buf = g_malloc(len * 2);
                for (uint64_t i = 0; i < len; i++) {
                    stw_le_p(dst_buf + (i << 1), (uint16_t)src_buf[i]);
                }
                cpu_physical_memory_write(dst_phys, dst_buf, len * 2);
            }

            count += len;
            src_raw += len;
        }

        stq_le_p(tmp, count);
        cpu_physical_memory_write(frame + 536, tmp, sizeof(tmp));
        stq_le_p(tmp, src_raw);
        cpu_physical_memory_write(frame + 2616, tmp, sizeof(tmp));

        if (trace_enabled && trace_count++ < trace_limit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath ascii2utf16 pc=%016" PRIx64
                          " count=%" PRIu64 " len=%" PRIu64 "\n",
                          pc, count, len);
        }

        env->ip = pc + 0xF0;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    /*
     * Table adjust loop:
     *   counter8 at [r12+0], table base ptr at [r12+8],
     *   base ptr at [r12+24], adjust ptr at [r12+32].
     *   For each index 1..63:
     *     if entry < *(base+40), entry -= (base - adjust).
     * Exit target is pc + 0x130.
     */
    if (low0 == 0x41e010183e00f80bULL &&
        high0 == 0x0004000000420079ULL &&
        low1 == 0xf80010183c00f00aULL &&
        high1 == 0x84006083e0681c78ULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t count = 0;
        cpu_physical_memory_read(frame + 0, &count, 1);

        uint32_t start = (uint32_t)count + 1;
        uint8_t final_count = (uint8_t)start;
        if (start <= 63) {
            uint8_t tmp[8];
            cpu_physical_memory_read(frame + 8, tmp, sizeof(tmp));
            uint64_t table_raw = ldq_le_p(tmp);
            hwaddr table_phys = ia64_phys_mode_addr(table_raw);

            cpu_physical_memory_read(frame + 24, tmp, sizeof(tmp));
            uint64_t base_raw = ldq_le_p(tmp);
            hwaddr base_phys = ia64_phys_mode_addr(base_raw);

            cpu_physical_memory_read(frame + 32, tmp, sizeof(tmp));
            uint64_t adjust_raw = ldq_le_p(tmp);

            uint8_t limit_buf[8];
            cpu_physical_memory_read(base_phys + 40, limit_buf, sizeof(limit_buf));
            uint64_t limit = ldq_le_p(limit_buf);
            uint64_t delta = base_raw - adjust_raw;

            for (uint32_t idx = start; idx <= 63; idx++) {
                hwaddr entry_addr = table_phys + 56 + ((hwaddr)idx << 3);
                uint8_t entry_buf[8];
                cpu_physical_memory_read(entry_addr, entry_buf, sizeof(entry_buf));
                uint64_t entry = ldq_le_p(entry_buf);
                if (entry < limit) {
                    entry -= delta;
                    stq_le_p(entry_buf, entry);
                    cpu_physical_memory_write(entry_addr, entry_buf, sizeof(entry_buf));
                }
            }
            final_count = 64;
        }

        cpu_physical_memory_write(frame + 0, &final_count, 1);

        if (trace_enabled && trace_count++ < trace_limit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath tbladj pc=%016" PRIx64
                          " count=%u final=%u\n",
                          pc, count, final_count);
        }

        env->ip = pc + 0x130;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    /*
     * Table copy loop:
     *   counter8 at [r12+0], src base ptr at [r12+24],
     *   dst base ptr at [r12+8], copies entries 1..63 from src+56 to dst+56.
     * Exit target is pc + 0x30.
     */
    if (low0 == 0x01f010183c00f019ULL &&
        high0 == 0x200000000020307cULL &&
        low1 == 0xf1e021003ce0f00bULL &&
        high1 == 0x0004000000400074ULL) {
        hwaddr frame = ia64_phys_mode_addr(env->r[12]);
        uint8_t count = 0;
        cpu_physical_memory_read(frame + 0, &count, 1);

        uint32_t start = (uint32_t)count + 1;
        uint8_t final_count = (uint8_t)start;
        if (start <= 63) {
            uint8_t tmp[8];
            cpu_physical_memory_read(frame + 24, tmp, sizeof(tmp));
            uint64_t src_raw = ldq_le_p(tmp);
            hwaddr src = ia64_phys_mode_addr(src_raw) + 56 + ((uint64_t)start << 3);

            cpu_physical_memory_read(frame + 8, tmp, sizeof(tmp));
            uint64_t dst_raw = ldq_le_p(tmp);
            hwaddr dst = ia64_phys_mode_addr(dst_raw) + 56 + ((uint64_t)start << 3);

            uint64_t entries = 64 - start;
            uint64_t len = entries << 3;
            if (len && !ia64_fw_fastpath_copy(dst, src, len)) {
                return 0;
            }
            final_count = 64;
        }

        cpu_physical_memory_write(frame + 0, &final_count, 1);

        if (trace_enabled && trace_count++ < trace_limit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath tblcpy pc=%016" PRIx64
                          " count=%u final=%u\n",
                          pc, count, final_count);
        }

        env->ip = pc + 0x30;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    /*
     * Spin loop with constant compare:
     *   mov r31=3328; cmp4.eq p15,p0=r0,r31; (p15) br exit; br back.
     * Exit target is pc + 0x20.
     */
    if (low0 == 0x00f0241a0000f80aULL &&
        high0 == 0x000400000071007cULL &&
        low1 == 0x0c03c00100000013ULL &&
        high1 == 0x48fffff800210000ULL) {
        env->r[31] = 3328;
        env->pr &= ~(1ULL << 15);
        if (trace_enabled && trace_count++ < trace_limit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "fw_fastpath spin-skip pc=%016" PRIx64 "\n", pc);
        }
        env->ip = pc + 0x20;
        env->psr &= ~PSR_RI_MASK;
        return 1;
    }

    return 0;
#endif
}

void HELPER(fw_xenipf_mpbuffer_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    /*
     * xenipf/EDK firmware MP buffer bringup.
     *
     * IpfEarlyMpInit elects the BSP by comparing a per-CPU signature against
     * the literal " __BSP__". If the MP buffer base is NULL, the firmware
     * mis-identifies CPU0 as an AP and calls through a NULL rendezvous
     * function pointer, crashing early in PEI.
     *
     * Provide a minimal MP buffer in the ia64 firmware work RAM and point the
     * scratch stack slots at it so CPU0 takes the BSP path.
     */
    static bool logged;
    if (env->psr & IA64_PSR_DT) {
        return;
    }

    hwaddr sp = ia64_phys_mode_addr(env->r[12]);
    uint8_t tmp[8];
    cpu_physical_memory_read(sp + 336, tmp, sizeof(tmp));
    uint64_t cur = ldq_le_p(tmp);
    if (cur != 0) {
        return;
    }

    const hwaddr mp_base = 0x0000000100ffe000ULL; /* within ipf.fw-workram */
    const uint64_t bsp_sig = 0x5f5f5053425f5f20ULL; /* " __BSP__" */

    stq_le_p(tmp, bsp_sig);
    cpu_physical_memory_write(mp_base + 0x188, tmp, sizeof(tmp));

    stq_le_p(tmp, (uint64_t)mp_base);
    cpu_physical_memory_write(sp + 336, tmp, sizeof(tmp));
    cpu_physical_memory_write(sp + 344, tmp, sizeof(tmp));

    if (!logged) {
        logged = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: xenipf mpbuffer: pc=%016" PRIx64
                      " sp=%016" HWADDR_PRIx " mp_base=%016" HWADDR_PRIx "\n",
                      pc, sp, mp_base);
    }
#endif
}

void HELPER(fw_pei_ppi_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    uint64_t ppi = env->fw_pei_ppi;
    uint64_t handoff = env->fw_pei_handoff;
    static bool logged;

    env->r[8] = 0;
    env->r[9] = handoff;
    env->r[10] = ppi;
    env->r[11] = 0;

    if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        logged = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_ppi_fix pc=%016" PRIx64
                      " ppi=%016" PRIx64 " handoff=%016" PRIx64 "\n",
                      pc, ppi, handoff);
    }
#endif
}

void HELPER(fw_pei_startup_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    uint64_t handoff = env->fw_pei_handoff;
    uint64_t ppi = env->fw_pei_ppi;
    uint64_t stack_count = env->fw_pei_stack_count;
    static bool logged;

    if (!handoff && !ppi && !stack_count) {
        return;
    }

    bool sec_handoff = false;
    if (handoff) {
        uint8_t handoff_buf[0x48];
        hwaddr handoff_phys = ia64_phys_mode_addr(handoff);
        if (cpu_memory_rw_debug(env_cpu(env), handoff_phys,
                                handoff_buf, sizeof(handoff_buf), false) == 0) {
            uint16_t data_size = lduw_le_p(&handoff_buf[0]);
            uint64_t bfv_base = ldq_le_p(&handoff_buf[8]);
            hwaddr bfv_phys = ia64_phys_mode_addr(bfv_base);
            sec_handoff = (data_size >= sizeof(handoff_buf) &&
                           data_size <= 0x80 &&
                           ia64_fw_addr_in_flash(bfv_phys));
        }
    }

    if (handoff) {
        env->r[9] = handoff;
    }
    /*
     * The PEI boot block copies r9/r10 into r32/r33 before calling the PEI
     * core. Some xenipf/EDK firmware still expects r10 to hold the stack
     * count for ar.k4-based stack sizing; prefer that when available and
     * provide the PPI list later via fw_pei_entry_fix()/fw_pei_ppi_fix().
     */
    if (stack_count) {
        env->r[10] = stack_count;
        env->r[33] = stack_count;
    } else if (ppi) {
        env->r[10] = ppi;
        if (sec_handoff) {
            env->r[33] = ppi;
        }
    }
    /* OldCoreData must be NULL on the first PEI core entry. */
    env->r[34] = 0;
    env->nat[34] = 0;

    if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        logged = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_startup_fix pc=%016" PRIx64
                      " r9=%016" PRIx64 " r10=%016" PRIx64 "\n",
                      pc, env->r[9], env->r[10]);
    }
#endif
}

void HELPER(fw_bootloop_log)(CPUIA64State *env, uint64_t pc, uint32_t stage)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)stage;
    return;
#else
    if (!ia64_fw_bootloop_log_enabled() ||
        !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static uint32_t logged_mask;
    if (stage < 8 && (logged_mask & (1u << stage))) {
        return;
    }
    if (stage < 8) {
        logged_mask |= (1u << stage);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_bootloop stage=%u pc=%016" PRIx64
                  " r2=%016" PRIx64 " r3=%016" PRIx64
                  " r8=%016" PRIx64 " r9=%016" PRIx64
                  " r10=%016" PRIx64 " r12=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64
                  " r34=%016" PRIx64 " ar.k3=%016" PRIx64
                  " ar.k4=%016" PRIx64 " ar.k5=%016" PRIx64
                  " ar.lc=%016" PRIx64
                  " ar.bsp=%016" PRIx64 " ar.bspstore=%016" PRIx64
                  " psr=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                  stage, pc, env->r[2], env->r[3], env->r[8], env->r[9],
                  env->r[10], env->r[12], env->r[32], env->r[33], env->r[34],
                  env->ar[3], env->ar[4], env->ar[5], env->ar[65],
                  env->ar[IA64_AR_BSP], env->ar[IA64_AR_BSPSTORE],
                  env->psr, env->cfm);
#endif
}

void HELPER(fw_boot_k4_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    uint64_t count = env->fw_pei_stack_count;
    if (!count) {
        return;
    }
    if (env->ar[4] == count) {
        return;
    }
    if (env->fw_pei_handoff && env->ar[4] == env->fw_pei_handoff) {
        env->ar[4] = count;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_boot_k4_fix pc=%016" PRIx64
                      " ar.k4=%016" PRIx64 " -> %016" PRIx64 "\n",
                      pc, env->fw_pei_handoff, count);
    }
#endif
}

void HELPER(fw_pei_entry_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    uint64_t handoff = env->fw_pei_handoff;
    uint64_t ppi = env->fw_pei_ppi;
    uint64_t stack_count = env->fw_pei_stack_count;
    static bool logged;
    bool sec_handoff = false;
    bool have_handoff_buf = false;
    uint8_t handoff_buf[0x48];

    if (!handoff && !ppi && !stack_count) {
        return;
    }
    uint64_t orig_r32 = env->r[32];
    uint64_t orig_r33 = env->r[33];
    uint64_t orig_r34 = env->r[34];
    uint8_t sof = env->cfm & 0x7f;
    uint8_t sol = (env->cfm >> 7) & 0x7f;
    uint8_t sor = (env->cfm >> 14) & 0x0f;
    if (handoff) {
        hwaddr handoff_phys = ia64_phys_mode_addr(handoff);
        if (cpu_memory_rw_debug(env_cpu(env), handoff_phys,
                                handoff_buf, sizeof(handoff_buf), false) == 0) {
            uint16_t data_size = lduw_le_p(&handoff_buf[0]);
            uint64_t bfv_base = ldq_le_p(&handoff_buf[8]);
            hwaddr bfv_phys = ia64_phys_mode_addr(bfv_base);
            sec_handoff = (data_size >= sizeof(handoff_buf) &&
                           data_size <= 0x80 &&
                           ia64_fw_addr_in_flash(bfv_phys));
            have_handoff_buf = true;
        }
        env->r[32] = handoff;
    }
    if (stack_count) {
        env->r[10] = stack_count;
        env->r[33] = stack_count;
    } else if (ppi) {
        env->r[33] = ppi;
    }
    if (ia64_fw_r33_watch_enabled()) {
        env->fw_pei_r33_watch_active = 1;
        env->fw_pei_r33_last = env->r[33];
        env->fw_pei_r33_last_pc = pc;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: r33_watch start pc=%016" PRIx64
                      " r33=%016" PRIx64 "\n",
                      pc, env->r[33]);
    }
    if (ia64_fw_pei_ppi_list_dump_enabled()) {
        static bool dumped;
        if (!dumped) {
            dumped = true;
            ia64_fw_dump_pei_ppi_list(env, ppi);
        }
    }
    ia64_fw_rse_log(env, pc, "pei_entry_fix");

    if (!logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        logged = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_entry_fix pc=%016" PRIx64
                      " handoff=%016" PRIx64 " ppi=%016" PRIx64
                      " stack_count=%016" PRIx64 " old=%016" PRIx64 "\n",
                      pc, handoff, ppi, stack_count, env->r[34]);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_entry_fix orig r32=%016" PRIx64
                      " r33=%016" PRIx64 " r34=%016" PRIx64
                      " cfm=%016" PRIx64 " sof=%u sol=%u sor=%u\n",
                      orig_r32, orig_r33, orig_r34, env->cfm,
                      sof, sol, sor);
        if (handoff && have_handoff_buf) {
            if (sec_handoff) {
                uint16_t data_size = lduw_le_p(&handoff_buf[0]);
                uint64_t bfv_base = ldq_le_p(&handoff_buf[8]);
                uint64_t bfv_size = ldq_le_p(&handoff_buf[16]);
                uint64_t temp_base = ldq_le_p(&handoff_buf[24]);
                uint64_t temp_size = ldq_le_p(&handoff_buf[32]);
                uint64_t pei_base = ldq_le_p(&handoff_buf[40]);
                uint64_t pei_size = ldq_le_p(&handoff_buf[48]);
                uint64_t stack_base = ldq_le_p(&handoff_buf[56]);
                uint64_t stack_size = ldq_le_p(&handoff_buf[64]);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_pei_entry_fix handoff(sec) size=%u"
                              " bfv=%016" PRIx64 "/%016" PRIx64
                              " temp=%016" PRIx64 "/%016" PRIx64
                              " pei=%016" PRIx64 "/%016" PRIx64
                              " stack=%016" PRIx64 "/%016" PRIx64 "\n",
                              data_size, bfv_base, bfv_size,
                              temp_base, temp_size,
                              pei_base, pei_size,
                              stack_base, stack_size);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_pei_entry_fix handoff(sec) phys"
                              " bfv=%016" PRIx64 " temp=%016" PRIx64
                              " pei=%016" PRIx64 " stack=%016" PRIx64 "\n",
                              (uint64_t)ia64_phys_mode_addr(bfv_base),
                              (uint64_t)ia64_phys_mode_addr(temp_base),
                              (uint64_t)ia64_phys_mode_addr(pei_base),
                              (uint64_t)ia64_phys_mode_addr(stack_base));
            } else {
                uint64_t boot_fv = ldq_le_p(&handoff_buf[0]);
                uint64_t car_size = ldq_le_p(&handoff_buf[8]);
                uint64_t dispatch16 = ldq_le_p(&handoff_buf[16]);
                uint64_t dispatch24 = ldq_le_p(&handoff_buf[24]);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_pei_entry_fix handoff(startup)"
                              " boot_fv=%016" PRIx64
                              " car_size=%016" PRIx64
                              " dispatch16=%016" PRIx64
                              " dispatch24=%016" PRIx64 "\n",
                              boot_fv, car_size, dispatch16, dispatch24);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_pei_entry_fix handoff(startup)"
                              " boot_fv_phys=%016" PRIx64
                              " dispatch_phys=%016" PRIx64 "\n",
                              (uint64_t)ia64_phys_mode_addr(boot_fv),
                              (uint64_t)ia64_phys_mode_addr(dispatch24));
            }
        }
    }
#endif
}

void HELPER(fw_pei_hob_init_fix)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    uint64_t handoff = env->fw_pei_handoff;
    uint64_t hob_size = env->r[40] ? env->r[40] : env->r[34];
    uint64_t temp_base = 0;
    uint64_t temp_size = 0;

    if (!handoff) {
        return;
    }

    uint8_t buf[0x48];
    hwaddr handoff_phys = ia64_phys_mode_addr(handoff);
    if (cpu_memory_rw_debug(env_cpu(env), handoff_phys, buf, sizeof(buf), false) == 0) {
        uint16_t data_size = lduw_le_p(&buf[0]);
        uint64_t bfv_base = ldq_le_p(&buf[8]);
        hwaddr bfv_phys = ia64_phys_mode_addr(bfv_base);
        bool sec_handoff = (data_size >= sizeof(buf) &&
                            data_size <= 0x80 &&
                            ia64_fw_addr_in_flash(bfv_phys));
        if (sec_handoff) {
            temp_base = ldq_le_p(&buf[24]);
            temp_size = ldq_le_p(&buf[32]);
        } else {
            return;
        }
    }

    if (!temp_base || !temp_size) {
        return;
    }
    if (!hob_size || hob_size > temp_size) {
        hob_size = temp_size;
    }

    uint64_t hob_base = temp_base + (temp_size - hob_size);
    uint64_t active_hob_base = hob_base;
    bool redirect_hob = false;
    ia64_fw_pei_cached_temp_base = temp_base;
    ia64_fw_pei_cached_temp_size = temp_size;

    /*
     * The PEI core loads the HOB list base from a pointer in its stack
     * frame; if that pointer still targets the Xen GFW HOB list (0xff200000),
     * the firmware builds the PHIT inside the GFW window and reports only
     * that 1MB region as usable memory. Redirect the pointer into temp RAM
     * so the PEI HOB list lives in the CAR window instead.
     */
    CPUState *cs = env_cpu(env);
    uint64_t hob_ptr = env->r[31];
    if (hob_ptr) {
        uint64_t cur = 0;
        if (ia64_fw_read_u64(cs, hob_ptr, &cur)) {
            uint64_t cur_phys = ia64_phys_mode_addr(cur);
            uint64_t cur_end = 0;
            int count = 0;
            bool have_src = false;
            bool cur_in_temp = false;
            bool cur_in_gfw = false;
            bool cur_in_flash = false;

            if (cur_phys) {
                cur_in_temp = (cur_phys >= temp_base &&
                               cur_phys < (temp_base + temp_size));
                cur_in_gfw = (cur_phys >= IA64_IPF_GFW_HOB_BASE &&
                              cur_phys < IA64_IPF_GFW_HOB_BASE + IA64_IPF_GFW_HOB_SIZE);
                cur_in_flash = ia64_fw_addr_in_flash(cur_phys);
                have_src = ia64_fw_validate_efi_hob_list(cs, cur_phys,
                                                         &cur_end, &count);
            }

            bool cur_in_ram = (!cur_in_temp && !cur_in_gfw && !cur_in_flash);
            if (cur_in_ram) {
                active_hob_base = cur_phys;
            } else if (!cur_in_temp && (cur_in_gfw || cur_in_flash || !cur_phys)) {
                if (!have_src) {
                    uint64_t alt_base = 0;
                    uint64_t alt_end = 0;
                    if (ia64_fw_validate_efi_hob_list(cs,
                                                      IA64_IPF_GFW_HOB_BASE,
                                                      &alt_end, &count)) {
                        alt_base = IA64_IPF_GFW_HOB_BASE;
                    } else if (ia64_fw_find_pei_hob_list(cs,
                                                         ia64_phys_mode_addr(env->r[12]),
                                                         &alt_base, &alt_end) &&
                               alt_base && alt_base != cur_phys) {
                        /* Found an alternative HOB list elsewhere. */
                    } else if (ia64_fw_find_hob_list_in_range(cs,
                                                              ia64_fw_flash_base(),
                                                              ia64_fw_flash_size(),
                                                              &alt_base, &alt_end) &&
                               alt_base && alt_base != cur_phys) {
                        /* Found a HOB list in the GFW/flash window. */
                    } else {
                        alt_base = 0;
                    }
                    if (alt_base && alt_base != cur_phys) {
                        cur_phys = alt_base;
                        cur_end = alt_end;
                        have_src = true;
                        if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                                          " fallback_hob %016" PRIx64 " len=%" PRIu64 "\n",
                                          pc, alt_base, (alt_end - alt_base));
                        }
                    }
                }
                if (have_src) {
                    uint8_t phit[0x38];
                    if (cpu_memory_rw_debug(cs, cur_phys, phit,
                                            sizeof(phit), false) == 0) {
                        uint64_t mem_top_raw = ldq_le_p(&phit[16]);
                        uint64_t mem_bottom_raw = ldq_le_p(&phit[24]);
                        uint64_t mem_top_phys = ia64_phys_mode_addr(mem_top_raw);
                        uint64_t mem_bottom_phys =
                            ia64_phys_mode_addr(mem_bottom_raw);
                        uint64_t stack_phys = ia64_phys_mode_addr(env->r[12]);
                        if (ia64_fw_clone_hob_list_ram(cs, cur_phys, cur_end,
                                                       hob_base,
                                                       mem_bottom_phys,
                                                       mem_top_phys,
                                                       stack_phys) &&
                            qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                                          " cloned_hob %016" PRIx64
                                          " -> %016" PRIx64 " len=%" PRIu64 "\n",
                                          pc, cur_phys, hob_base,
                                          (cur_end - cur_phys));
                        }
                    }
                    redirect_hob = true;
                }
            }
            if (redirect_hob && cur != hob_base) {
                uint8_t out[8];
                stq_le_p(out, hob_base);
                if (ia64_fw_write_bytes_any(cs, hob_ptr, out, sizeof(out)) &&
                    qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                                  " hob_ptr=%016" PRIx64
                                  " old=%016" PRIx64 " new=%016" PRIx64 "\n",
                                  pc, hob_ptr, cur, hob_base);
                }
            }
        }
    }

    if (redirect_hob) {
        env->r[39] = hob_base;
        env->r[30] = hob_base;
        ia64_fw_pei_cached_hob_base = hob_base;
    } else if (active_hob_base) {
        ia64_fw_pei_cached_hob_base = active_hob_base;
    }
    hob_base = active_hob_base;

    /*
     * The PEI core also caches the HOB list base in its private core
     * structure (offset 0x260). If it still points at a low/flash list,
     * update it to the CAR list so PEIMs see the same list as the stack
     * hob_ptr.
     */
    {
        uint64_t core = 0;
        uint64_t ps_ptr = 0;
        static bool ps_logged;
        hwaddr sp_phys = ia64_phys_mode_addr(env->r[12]);
        if (ia64_fw_pei_scan_core(cs, sp_phys, &core, &ps_ptr)) {
            if (ps_ptr) {
                env->fw_pei_ps = ps_ptr;
                if (!ps_logged && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                    uint8_t hdr[16] = { 0 };
                    if (cpu_memory_rw_debug(cs, ps_ptr, hdr, sizeof(hdr), false) == 0) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                                      " ps_hdr=%02x%02x%02x%02x%02x%02x%02x%02x"
                                      " %02x%02x%02x%02x%02x%02x%02x%02x\n",
                                      pc,
                                      hdr[0], hdr[1], hdr[2], hdr[3],
                                      hdr[4], hdr[5], hdr[6], hdr[7],
                                      hdr[8], hdr[9], hdr[10], hdr[11],
                                      hdr[12], hdr[13], hdr[14], hdr[15]);
                        ps_logged = true;
                    }
                }
            }
            uint64_t hob_raw = 0;
            if (ia64_fw_read_u64(cs, core + 0x260, &hob_raw)) {
                uint64_t hob_phys = ia64_phys_mode_addr(hob_raw);
                bool hob_in_temp = (hob_phys >= temp_base &&
                                    hob_phys < (temp_base + temp_size));
                if (!hob_in_temp && hob_phys != hob_base) {
                    uint64_t enc = ia64_fw_encode_addr(hob_raw, hob_base);
                    uint8_t out[8];
                    stq_le_p(out, enc);
                    if (ia64_fw_write_bytes_any(cs, core + 0x260, out, sizeof(out)) &&
                        qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                                      " core=%016" PRIx64
                                      " hob_raw=%016" PRIx64 " new=%016" PRIx64
                                      " ps_ptr=%016" PRIx64 "\n",
                                      pc, core, hob_raw, enc, ps_ptr);
                    }
                }
            }
        }
    }

    /*
     * Some PEI core paths cache the PHIT base in gp-relative globals
     * (gp+0x20/gp+0x28). Seed those with the CAR HOB base so later free
     * memory calculations read the correct list.
     */
    uint64_t gp = env->r[1];
    if (gp && redirect_hob) {
        uint64_t free_top = hob_base + hob_size;
        uint64_t free_bottom = hob_base + 0x40;
        uint64_t vars[] = { gp + 0x20, gp + 0x28 };
        uint64_t vals[] = { free_top, free_bottom };
        for (size_t i = 0; i < ARRAY_SIZE(vars); i++) {
            uint64_t addr = vars[i];
            uint64_t cur = 0;
            if (!ia64_fw_read_u64(cs, addr, &cur)) {
                continue;
            }
            bool cur_in_temp = (cur >= temp_base &&
                                cur < (temp_base + temp_size));
            if (!cur_in_temp && cur != vals[i]) {
                uint8_t out[8];
                stq_le_p(out, vals[i]);
                if (ia64_fw_write_bytes_any(cs, addr, out, sizeof(out)) &&
                    qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                                  " gp_var=%016" PRIx64
                                  " old=%016" PRIx64 " new=%016" PRIx64 "\n",
                                  pc, addr, cur, vals[i]);
                }
            }
        }
    }

    {
        const uint64_t vga_start = 0x000a0000ULL;
        const uint64_t vga_size = 0x00020000ULL;
        uint64_t ram_size = env->fw_mem_size;
        if (!ram_size && current_machine) {
            ram_size = current_machine->ram_size;
        }
        if (!ram_size && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                          " PHIT patch skipped (ram_size=0)\n",
                          pc);
        }
        if (ram_size) {
            uint8_t phit[0x38];
            if (cpu_memory_rw_debug(cs, hob_base, phit, sizeof(phit), false) == 0) {
                uint16_t htype = lduw_le_p(&phit[0]);
                uint16_t hlen = lduw_le_p(&phit[2]);
                if (htype == 0x0001 && hlen >= sizeof(phit)) {
                    uint64_t mem_top_raw = ldq_le_p(&phit[16]);
                    uint64_t mem_bottom_raw = ldq_le_p(&phit[24]);
                    uint64_t end_hob_raw = ldq_le_p(&phit[48]);
                    uint64_t end_hob_phys = ia64_phys_mode_addr(end_hob_raw);
                    uint64_t ram_top = ram_size;
                    if (ram_size >= vga_start) {
                        ram_top += vga_size;
                    }
                    uint64_t free_bottom_phys = (end_hob_phys + 0x1fULL) & ~0x1fULL;
                    if (free_bottom_phys > ram_top) {
                        free_bottom_phys = 0;
                    }
                    stq_le_p(&phit[16], ia64_fw_encode_addr(mem_top_raw, ram_top));
                    stq_le_p(&phit[24], ia64_fw_encode_addr(mem_bottom_raw, 0));
                    stq_le_p(&phit[32], ia64_fw_encode_addr(mem_top_raw, ram_top));
                    stq_le_p(&phit[40], ia64_fw_encode_addr(mem_bottom_raw, free_bottom_phys));
                    cpu_physical_memory_write(hob_base, phit, sizeof(phit));
                    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                                      " PHIT mem_top=%016" PRIx64 " free_bottom=%016" PRIx64 "\n",
                                      pc, ram_top, free_bottom_phys);
                    }
                } else if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                                  " PHIT header invalid (type=%u len=%u)\n",
                                  pc, htype, hlen);
                }
            }
        }
    }

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_hob_init_fix pc=%016" PRIx64
                      " temp=%016" PRIx64 "/%016" PRIx64
                      " hob_size=%016" PRIx64 " hob_base=%016" PRIx64
                      " r30=%016" PRIx64 " r39=%016" PRIx64 "\n",
                      pc, temp_base, temp_size, hob_size, hob_base,
                      env->r[30], env->r[39]);
    }
#endif
}

void HELPER(fw_pei_oldcore_clear)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    static int clear_oldcore = -1;
    if (clear_oldcore == -1) {
        clear_oldcore = getenv("QEMU_IA64_PEI_CLEAR_OLDCORE") ? 1 : 0;
    }

    uint64_t oldcore = env->r[34];
    uint64_t oldcore_phys = ia64_phys_mode_addr(oldcore);
    if (!clear_oldcore &&
        oldcore_phys != IA64_IPF_FW_PAL_PROC_ADDR &&
        oldcore_phys != IA64_IPF_FW_PAL_LEGACY_ADDR) {
        return;
    }

    env->r[34] = 0;
    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_oldcore_clear pc=%016" PRIx64
                      " old=%016" PRIx64 " ar.k5=%016" PRIx64 "\n",
                      pc, oldcore, env->ar[5]);
    }
#endif
}

void HELPER(fw_ar_k5_store)(CPUIA64State *env, uint64_t pc, uint32_t src_reg,
                            uint64_t value)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)src_reg;
    (void)value;
    return;
#else
    static int log_limit = -1;
    static int log_count;
    if (log_limit == -1) {
        if (getenv("QEMU_IA64_LOG_K5")) {
            log_limit = 64;
            const char *s = getenv("QEMU_IA64_LOG_K5_LIMIT");
            if (s && *s) {
                log_limit = atoi(s);
            }
            if (log_limit < 0) {
                log_limit = 0;
            }
        } else {
            log_limit = 0;
        }
    }
    if (log_limit && log_count < log_limit &&
        qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        const char *src_label = "imm";
        char src_buf[16];
        if (src_reg != UINT32_MAX) {
            snprintf(src_buf, sizeof(src_buf), "r%u", src_reg);
            src_label = src_buf;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: ar.k5 store pc=%016" PRIx64
                      " src=%s val=%016" PRIx64
                      " old=%016" PRIx64 " r34=%016" PRIx64
                      " bsp=%016" PRIx64 " bspstore=%016" PRIx64
                      " cfm=%016" PRIx64 "\n",
                      pc, src_label, value, env->ar[5], env->r[34],
                      env->ar[IA64_AR_BSP], env->ar[IA64_AR_BSPSTORE],
                      env->cfm);
        log_count++;
    }
    env->ar[5] = value;
#endif
}

void HELPER(fw_ar_k3_store)(CPUIA64State *env, uint64_t pc, uint32_t src_reg,
                            uint64_t value)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)src_reg;
    (void)value;
    return;
#else
    static int log_limit = -1;
    static int log_count;
    static int force_k3 = -2;
    if (force_k3 == -2) {
        const char *s = getenv("QEMU_IA64_FORCE_K3");
        if (s && *s) {
            force_k3 = (int)strtol(s, NULL, 0);
        } else {
            force_k3 = -1;
        }
    }
    if (log_limit == -1) {
        if (getenv("QEMU_IA64_LOG_K3")) {
            log_limit = 64;
            const char *s = getenv("QEMU_IA64_LOG_K3_LIMIT");
            if (s && *s) {
                log_limit = atoi(s);
            }
            if (log_limit < 0) {
                log_limit = 0;
            }
        } else {
            log_limit = 0;
        }
    }
    if (log_limit && log_count < log_limit &&
        qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        const char *src_label = "imm";
        char src_buf[16];
        if (src_reg != UINT32_MAX) {
            snprintf(src_buf, sizeof(src_buf), "r%u", src_reg);
            src_label = src_buf;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: ar.k3 store pc=%016" PRIx64
                      " src=%s val=%016" PRIx64
                      " old=%016" PRIx64
                      " r12=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                      pc, src_label, value, env->ar[3], env->r[12], env->cfm);
        log_count++;
    }
    if (force_k3 >= 0) {
        value = (uint64_t)force_k3;
    }
    env->ar[3] = value;
#endif
}

void HELPER(fw_pei_core_entry_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    static bool logged;
    bool log_enabled = qemu_loglevel_mask(LOG_GUEST_ERROR);

    uint64_t r32 = env->r[32];
    uint64_t r33 = env->r[33];
    uint64_t r34 = env->r[34];
    uint8_t sof = env->cfm & 0x7f;
    uint8_t sol = (env->cfm >> 7) & 0x7f;
    uint8_t sor = (env->cfm >> 14) & 0x0f;
    if (log_enabled) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_core_entry pc=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                      " cfm=%016" PRIx64 " sof=%u sol=%u sor=%u\n",
                      pc, r32, r33, r34, env->cfm, sof, sol, sor);
    }

    if (r32) {
        uint8_t buf[0x48];
        hwaddr handoff_phys = ia64_phys_mode_addr(r32);
        if (cpu_memory_rw_debug(env_cpu(env), handoff_phys, buf, sizeof(buf), false) == 0) {
            uint16_t data_size = lduw_le_p(&buf[0]);
            uint64_t bfv_base = ldq_le_p(&buf[8]);
            hwaddr bfv_phys = ia64_phys_mode_addr(bfv_base);
            bool sec_handoff = (data_size >= sizeof(buf) &&
                                data_size <= 0x80 &&
                                ia64_fw_addr_in_flash(bfv_phys));
            if (sec_handoff) {
                uint64_t bfv_size = ldq_le_p(&buf[16]);
                uint64_t temp_base = ldq_le_p(&buf[24]);
                uint64_t temp_size = ldq_le_p(&buf[32]);
                uint64_t pei_base = ldq_le_p(&buf[40]);
                uint64_t pei_size = ldq_le_p(&buf[48]);
                uint64_t stack_base = ldq_le_p(&buf[56]);
                uint64_t stack_size = ldq_le_p(&buf[64]);
                if (env->fw_pei_ppi && env->r[33] != env->fw_pei_ppi) {
                    env->r[33] = env->fw_pei_ppi;
                    if (log_enabled) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "IA64: fw_pei_core_entry fix r33 -> %016" PRIx64 "\n",
                                      env->fw_pei_ppi);
                    }
                }
                if (log_enabled) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_pei_core_entry handoff(sec) size=%u"
                                  " bfv=%016" PRIx64 "/%016" PRIx64
                                  " temp=%016" PRIx64 "/%016" PRIx64
                                  " pei=%016" PRIx64 "/%016" PRIx64
                                  " stack=%016" PRIx64 "/%016" PRIx64 "\n",
                                  data_size, bfv_base, bfv_size,
                                  temp_base, temp_size,
                                  pei_base, pei_size,
                                  stack_base, stack_size);
                }
            } else {
                uint64_t boot_fv = ldq_le_p(&buf[0]);
                uint64_t car_size = ldq_le_p(&buf[8]);
                uint64_t dispatch16 = ldq_le_p(&buf[16]);
                uint64_t dispatch24 = ldq_le_p(&buf[24]);
                if (log_enabled) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: fw_pei_core_entry handoff(startup)"
                                  " boot_fv=%016" PRIx64
                                  " car_size=%016" PRIx64
                                  " dispatch16=%016" PRIx64
                                  " dispatch24=%016" PRIx64 "\n",
                                  boot_fv, car_size, dispatch16, dispatch24);
                }
            }
        }
    }

    if (logged || !log_enabled) {
        return;
    }
    logged = true;
#endif
}

void HELPER(fw_pei_err_watch)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static int limit = -1;
    static int count;
    static int abort_on_err = -1;
    static uint64_t abort_skip_pc = UINT64_MAX;
    if (limit == -1) {
        limit = 64;
        const char *s = getenv("QEMU_IA64_PEI_ERR_LIMIT");
        if (s && *s) {
            limit = atoi(s);
        }
        if (limit < 0) {
            limit = 0;
        }
    }
    if (abort_on_err == -1) {
        abort_on_err = getenv("QEMU_IA64_PEI_ERR_ABORT") ? 1 : 0;
    }
    if (abort_skip_pc == UINT64_MAX) {
        abort_skip_pc = 0;
        const char *s = getenv("QEMU_IA64_PEI_ERR_ABORT_PC");
        if (s && *s) {
            abort_skip_pc = strtoull(s, NULL, 0) & ~0xFULL;
        }
    }
    if (limit == 0 || count >= limit) {
        return;
    }
    uint64_t r8 = env->r[8];
    if (!(r8 & (1ULL << 63))) {
        return;
    }
    static uint64_t last_pc;
    static uint64_t last_r8;
    static bool dumped_ppi;
    if (pc == last_pc && r8 == last_r8) {
        return;
    }
    last_pc = pc;
    last_r8 = r8;
    count++;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_pei_err pc=%016" PRIx64
                  " r8=%016" PRIx64 " r32=%016" PRIx64
                  " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64 "\n",
                  pc, r8, env->r[32], env->r[33], env->r[34],
                  env->r[1], env->r[12]);
    if ((pc & ~0xFULL) == 0x00000000ffe268b0ULL ||
        (pc & ~0xFULL) == 0x80000000ffe268b0ULL) {
        HELPER(fw_pei_err268_probe)(env, pc);
    }
    {
        CPUState *cs = env_cpu(env);
        uint64_t arg0 = env->r[32];
        hwaddr arg0_phys = ia64_phys_mode_addr(arg0);
        uint8_t arg0_buf[64];
        if (arg0 &&
            cpu_memory_rw_debug(cs, arg0_phys, arg0_buf, sizeof(arg0_buf),
                                false) == 0) {
            char hex[3 * sizeof(arg0_buf) + 1];
            size_t pos = 0;
            for (size_t i = 0; i < sizeof(arg0_buf); i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ",
                                arg0_buf[i]);
            }
            if (pos > 0) {
                hex[pos - 1] = '\0';
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_pei_err arg0_mem [%016" PRIx64
                          "]: %s\n",
                          (uint64_t)arg0_phys, hex);
        } else if (arg0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_pei_err arg0_mem [%016" PRIx64
                          "] unreadable\n",
                          (uint64_t)arg0_phys);
        }
        uint64_t arg0_ptr = 0;
        if (arg0 &&
            ia64_fw_read_u64(cs, arg0, &arg0_ptr) && arg0_ptr) {
            hwaddr arg0_ptr_phys = ia64_phys_mode_addr(arg0_ptr);
            uint8_t ptr_buf[64];
            if (cpu_memory_rw_debug(cs, arg0_ptr_phys, ptr_buf, sizeof(ptr_buf),
                                    false) == 0) {
                char hex[3 * sizeof(ptr_buf) + 1];
                size_t pos = 0;
                for (size_t i = 0; i < sizeof(ptr_buf); i++) {
                    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ",
                                    ptr_buf[i]);
                }
                if (pos > 0) {
                    hex[pos - 1] = '\0';
                }
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_pei_err arg0_ptr [%016" PRIx64
                              " -> %016" PRIx64 "]: %s\n",
                              (uint64_t)arg0_phys, (uint64_t)arg0_ptr, hex);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_pei_err arg0_ptr [%016" PRIx64
                              " -> %016" PRIx64 "] unreadable\n",
                              (uint64_t)arg0_phys, (uint64_t)arg0_ptr);
            }
        }
    }
    if (!dumped_ppi) {
        dumped_ppi = true;
        uint64_t ps_ptr = env->fw_pei_ps;
        bool ps_ok = false;
        if (!ps_ptr) {
            ps_ok = ia64_fw_pei_get_ps_ptr(env, env->r[32], &ps_ptr);
        } else {
            ps_ok = true;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_err pc=%016" PRIx64
                      " ppi_dump ps=%016" PRIx64 " ps_ok=%d\n",
                      pc, ps_ptr, ps_ok ? 1 : 0);
        if (ps_ptr) {
            ia64_fw_pei_dump_ps(env, ps_ptr, pc);
        }
        helper_fw_pei_ppi_dump(env, pc);
    }
    if (abort_on_err && (!abort_skip_pc || ((pc ^ abort_skip_pc) & ~0xFULL))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_err abort pc=%016" PRIx64 "\n",
                      pc);
        abort();
    }
#endif
}

void HELPER(fw_pei_callsite_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    bool abort_on_a1 = getenv("QEMU_IA64_PEI_ABORT_A1_7E") != NULL;
    bool should_abort = abort_on_a1 && env->r[33] == 0x7e;

    if (dumped) {
        if (should_abort) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_callsite_probe abort pc=%016" PRIx64
                          " r33=%016" PRIx64 "\n",
                          pc, env->r[33]);
            abort();
        }
        return;
    }
    dumped = true;

    CPUState *cs = env_cpu(env);
    const uint64_t probe_base = 0x100000000ULL - 0x80;
    uint8_t buf[0x80];
    if (cpu_memory_rw_debug(cs, probe_base, buf, sizeof(buf), false) == 0) {
        char hex[3 * sizeof(buf) + 1];
        size_t pos = 0;
        for (size_t i = 0; i < sizeof(buf); i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[i]);
        }
        if (pos > 0) {
            hex[pos - 1] = '\0';
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_callsite_probe pc=%016" PRIx64
                      " mem[%016" PRIx64 "..%016" PRIx64 "]: %s\n",
                      pc, probe_base, probe_base + sizeof(buf) - 1, hex);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_callsite_probe pc=%016" PRIx64
                      " mem[%016" PRIx64 "..%016" PRIx64 "] unreadable\n",
                      pc, probe_base, probe_base + sizeof(buf) - 1);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callsite_probe regs r33=%016" PRIx64
                  " r38=%016" PRIx64 " r39=%016" PRIx64 " r40=%016" PRIx64
                  " r12=%016" PRIx64 " r1=%016" PRIx64 "\n",
                  env->r[33], env->r[38], env->r[39], env->r[40],
                  env->r[12], env->r[1]);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callsite_probe regs2 r32=%016" PRIx64
                  " r34=%016" PRIx64 " r35=%016" PRIx64 " r36=%016" PRIx64
                  " r37=%016" PRIx64 "\n",
                  env->r[32], env->r[34], env->r[35], env->r[36],
                  env->r[37]);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callsite_probe state b0=%016" PRIx64
                  " cfm=%016" PRIx64 " ar.pfs=%016" PRIx64 "\n",
                  env->b[0], env->cfm, env->ar[IA64_AR_PFS]);
    if (should_abort) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_callsite_probe abort pc=%016" PRIx64
                      " r33=%016" PRIx64 "\n",
                      pc, env->r[33]);
        abort();
    }
#endif
}

void HELPER(fw_pei_callsite_pre_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callsite_pre pc=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64
                  " r38=%016" PRIx64 " r39=%016" PRIx64 " r40=%016" PRIx64
                  "\n",
                  pc, env->r[32], env->r[33], env->r[34], env->r[35],
                  env->r[36], env->r[37], env->r[38], env->r[39],
                  env->r[40]);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callsite_pre state b0=%016" PRIx64
                  " cfm=%016" PRIx64 " ar.pfs=%016" PRIx64
                  " r12=%016" PRIx64 " r1=%016" PRIx64 "\n",
                  env->b[0], env->cfm, env->ar[IA64_AR_PFS],
                  env->r[12], env->r[1]);
#endif
}

void HELPER(fw_pei_callsite_post_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callsite_post pc=%016" PRIx64
                  " r8=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r35=%016" PRIx64 " r39=%016" PRIx64 " r40=%016" PRIx64
                  " p6=%u p7=%u\n",
                  pc, env->r[8], env->r[33], env->r[34], env->r[35],
                  env->r[39], env->r[40],
                  (uint32_t)((env->pr >> 6) & 1),
                  (uint32_t)((env->pr >> 7) & 1));
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callsite_post state b0=%016" PRIx64
                  " cfm=%016" PRIx64 " ar.pfs=%016" PRIx64
                  " r12=%016" PRIx64 " r1=%016" PRIx64 "\n",
                  env->b[0], env->cfm, env->ar[IA64_AR_PFS],
                  env->r[12], env->r[1]);
#endif
}

void HELPER(fw_pei_callsite_use_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callsite_use pc=%016" PRIx64
                  " r33=%016" PRIx64 " r34=%016" PRIx64 " r35=%016" PRIx64
                  " r37=%016" PRIx64 " r39=%016" PRIx64
                  " r8=%016" PRIx64 " p6=%u p7=%u\n",
                  pc, env->r[33], env->r[34], env->r[35],
                  env->r[37], env->r[39], env->r[8],
                  (uint32_t)((env->pr >> 6) & 1),
                  (uint32_t)((env->pr >> 7) & 1));
#endif
}

void HELPER(fw_pei_ptr_chain_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    CPUState *cs = env_cpu(env);
    uint64_t r8 = env->r[8];
    uint64_t r1 = env->r[1];
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_ptr_chain pc=%016" PRIx64
                  " r8=%016" PRIx64 " r1=%016" PRIx64 "\n",
                  pc, r8, r1);

    uint8_t buf[32];
    uint64_t addr = 0xffe2ee30ULL;
    if (ia64_fw_read_bytes_any(cs, addr, buf, sizeof(buf))) {
        char hex[3 * sizeof(buf) + 1];
        size_t pos = 0;
        for (size_t i = 0; i < sizeof(buf); i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[i]);
        }
        if (pos > 0) {
            hex[pos - 1] = '\0';
        }
        uint64_t entry = ldq_le_p(&buf[0]);
        uint64_t gp = ldq_le_p(&buf[8]);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ptr_chain mem[%016" PRIx64 "]: %s\n",
                      addr, hex);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ptr_chain fdesc entry=%016" PRIx64
                      " gp=%016" PRIx64 "\n",
                      entry, gp);
        if (entry) {
            uint8_t code[32];
            hwaddr entry_phys = ia64_phys_mode_addr(entry);
            if (cpu_memory_rw_debug(cs, entry_phys, code, sizeof(code),
                                    false) == 0) {
                char chex[3 * sizeof(code) + 1];
                size_t cpos = 0;
                for (size_t i = 0; i < sizeof(code); i++) {
                    cpos += snprintf(chex + cpos, sizeof(chex) - cpos, "%02x ",
                                     code[i]);
                }
                if (cpos > 0) {
                    chex[cpos - 1] = '\0';
                }
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ptr_chain fdesc_code[%016" PRIx64
                              "]: %s\n",
                              (uint64_t)entry_phys, chex);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ptr_chain fdesc_code[%016" PRIx64
                              "] unreadable\n",
                              (uint64_t)entry_phys);
            }
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ptr_chain mem[%016" PRIx64 "] unreadable\n",
                      addr);
    }

    if (r8) {
        uint8_t r8buf[32];
        hwaddr r8_phys = ia64_phys_mode_addr(r8);
        if (cpu_memory_rw_debug(cs, r8_phys, r8buf, sizeof(r8buf), false) == 0) {
            char hex[3 * sizeof(r8buf) + 1];
            size_t pos = 0;
            for (size_t i = 0; i < sizeof(r8buf); i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", r8buf[i]);
            }
            if (pos > 0) {
                hex[pos - 1] = '\0';
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain r8_mem[%016" PRIx64 "]: %s\n",
                          (uint64_t)r8_phys, hex);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain r8_mem[%016" PRIx64
                          "] unreadable\n",
                          (uint64_t)r8_phys);
        }
    }

    if (r1) {
        uint8_t r1buf[32];
        hwaddr r1_phys = ia64_phys_mode_addr(r1);
        if (cpu_memory_rw_debug(cs, r1_phys, r1buf, sizeof(r1buf), false) == 0) {
            char hex[3 * sizeof(r1buf) + 1];
            size_t pos = 0;
            for (size_t i = 0; i < sizeof(r1buf); i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", r1buf[i]);
            }
            if (pos > 0) {
                hex[pos - 1] = '\0';
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain r1_mem[%016" PRIx64 "]: %s\n",
                          (uint64_t)r1_phys, hex);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain r1_mem[%016" PRIx64
                          "] unreadable\n",
                          (uint64_t)r1_phys);
        }
    }
#endif
}

void HELPER(fw_pei_ptr_chain_post_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    CPUState *cs = env_cpu(env);
    uint64_t r8 = env->r[8];
    uint64_t r1 = env->r[1];
    uint64_t target = r1 ? (r1 - 2096704ULL) : 0;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_ptr_chain_post pc=%016" PRIx64
                  " r1=%016" PRIx64 " r8=%016" PRIx64
                  " target=%016" PRIx64 "\n",
                  pc, r1, r8, target);

    if (target) {
        uint8_t buf[32];
        hwaddr phys = ia64_phys_mode_addr(target);
        if (cpu_memory_rw_debug(cs, phys, buf, sizeof(buf), false) == 0) {
            char hex[3 * sizeof(buf) + 1];
            size_t pos = 0;
            for (size_t i = 0; i < sizeof(buf); i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[i]);
            }
            if (pos > 0) {
                hex[pos - 1] = '\0';
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain_post mem[%016" PRIx64 "]: %s\n",
                          (uint64_t)phys, hex);
            char ascii[sizeof(buf) + 1];
            for (size_t i = 0; i < sizeof(buf); i++) {
                uint8_t c = buf[i];
                ascii[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            ascii[sizeof(buf)] = '\0';
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain_post ascii: %s\n", ascii);
            char utf16[sizeof(buf) / 2 + 1];
            size_t ulen = 0;
            for (size_t i = 0; i + 1 < sizeof(buf); i += 2) {
                uint8_t lo = buf[i];
                uint8_t hi = buf[i + 1];
                if (hi != 0) {
                    utf16[ulen++] = '.';
                } else {
                    utf16[ulen++] = (lo >= 0x20 && lo < 0x7f) ? (char)lo : '.';
                }
            }
            utf16[ulen] = '\0';
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain_post utf16: %s\n", utf16);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain_post qwords=%016" PRIx64
                          " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
                          ldq_le_p(&buf[0]), ldq_le_p(&buf[8]),
                          ldq_le_p(&buf[16]), ldq_le_p(&buf[24]));
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ptr_chain_post mem[%016" PRIx64
                          "] unreadable\n",
                          (uint64_t)phys);
        }
    }
    if (getenv("QEMU_IA64_PEI_PTR_CHAIN_POST_ABORT")) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ptr_chain_post abort pc=%016" PRIx64 "\n",
                      pc);
        abort();
    }
#endif
}

void HELPER(fw_pei_fit_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    CPUState *cs = env_cpu(env);
    const uint64_t fit_ptr_addr = 0x100000000ULL - 0x20;
    uint64_t fit_ptr = 0;
    if (!ia64_fw_read_u64(cs, fit_ptr_addr, &fit_ptr) || !fit_ptr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_fit_probe pc=%016" PRIx64
                      " fit_ptr_addr=%016" PRIx64 " unreadable\n",
                      pc, fit_ptr_addr);
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_fit_probe pc=%016" PRIx64
                  " fit_ptr_addr=%016" PRIx64 " fit_ptr=%016" PRIx64 "\n",
                  pc, fit_ptr_addr, fit_ptr);

    uint8_t fit_dump[64];
    bool fit_dump_ok =
        cpu_memory_rw_debug(cs, fit_ptr, fit_dump, sizeof(fit_dump), false) == 0;
    if (fit_dump_ok) {
        char hex[3 * sizeof(fit_dump) + 1];
        size_t pos = 0;
        for (size_t i = 0; i < sizeof(fit_dump); i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", fit_dump[i]);
        }
        if (pos > 0) {
            hex[pos - 1] = '\0';
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_fit_probe fit_dump[%016" PRIx64 "]: %s\n",
                      fit_ptr, hex);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_fit_probe fit_dump[%016" PRIx64
                      "] unreadable\n",
                      fit_ptr);
    }

    uint32_t fit_entries = 0;
    if (fit_dump_ok) {
        fit_entries = fit_dump[8] | (fit_dump[9] << 8) | (fit_dump[10] << 16);
    }
    const int max_entries = 64;
    uint32_t entries = fit_entries ? fit_entries : max_entries;
    if (entries > max_entries) {
        entries = max_entries;
    }
    for (uint32_t i = 0; i < entries; i++) {
        uint8_t entry[16];
        uint64_t ent_addr = fit_ptr + (uint64_t)i * sizeof(entry);
        if (cpu_memory_rw_debug(cs, ent_addr, entry, sizeof(entry), false) != 0) {
            break;
        }
        uint64_t addr = ldq_le_p(&entry[0]);
        uint32_t size = entry[8] | (entry[9] << 8) | (entry[10] << 16);
        uint8_t rev = entry[12];
        uint8_t type = entry[14] & 0x7f;
        uint8_t csum_valid = (entry[14] >> 7) & 1;
        uint8_t csum = entry[15];
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_fit_probe entry%d addr=%016" PRIx64
                      " size=0x%06x rev=%u type=0x%02x csum_valid=%u csum=0x%02x\n",
                      i, addr, size, rev, type, csum_valid, csum);
    }
#endif
}

void HELPER(fw_pei_fit_compare_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_fit_compare pc=%016" PRIx64
                  " r33=%016" PRIx64 " r40=%016" PRIx64 " r39=%016" PRIx64 "\n",
                  pc, env->r[33], env->r[40], env->r[39]);
#endif
}

void HELPER(fw_pei_err268_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_err268 pc=%016" PRIx64
                  " r8=%016" PRIx64 " r32=%016" PRIx64
                  " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r35=%016" PRIx64 " r36=%016" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64
                  " b0=%016" PRIx64 "\n",
                  pc, env->r[8], env->r[32], env->r[33], env->r[34],
                  env->r[35], env->r[36], env->r[1], env->r[12], env->b[0]);

    CPUState *cs = env_cpu(env);
    uint8_t stack[128];
    hwaddr sp = ia64_phys_mode_addr(env->r[12]);
    if (cpu_memory_rw_debug(cs, sp, stack, sizeof(stack), false) == 0) {
        char hex[3 * sizeof(stack) + 1];
        size_t pos = 0;
        for (size_t i = 0; i < sizeof(stack); i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", stack[i]);
        }
        if (pos > 0) {
            hex[pos - 1] = '\0';
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_err268 stack[%016" PRIx64 "]: %s\n",
                      (uint64_t)sp, hex);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_err268 stack_qw +0x30=%016" PRIx64
                      " +0x38=%016" PRIx64 " +0x40=%016" PRIx64
                      " +0x48=%016" PRIx64 " +0x50=%016" PRIx64 "\n",
                      ldq_le_p(&stack[0x30]), ldq_le_p(&stack[0x38]),
                      ldq_le_p(&stack[0x40]), ldq_le_p(&stack[0x48]),
                      ldq_le_p(&stack[0x50]));
    }
    if (env->r[33]) {
        uint8_t buf[64];
        hwaddr p = ia64_phys_mode_addr(env->r[33]);
        if (cpu_memory_rw_debug(cs, p, buf, sizeof(buf), false) == 0) {
            char hex[3 * sizeof(buf) + 1];
            size_t pos = 0;
            for (size_t i = 0; i < sizeof(buf); i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ",
                                buf[i]);
            }
            if (pos > 0) {
                hex[pos - 1] = '\0';
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_err268 r33_mem[%016" PRIx64 "]: %s\n",
                          (uint64_t)p, hex);
            for (size_t i = 0; i < 4; i++) {
                IA64EfiGuid guid;
                ia64_fw_guid_from_bytes(&buf[i * 16], &guid);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_err268 r33_guid%zu="
                              "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                              i,
                              guid.data1, guid.data2, guid.data3,
                              guid.data4[0], guid.data4[1], guid.data4[2],
                              guid.data4[3], guid.data4[4], guid.data4[5],
                              guid.data4[6], guid.data4[7]);
            }
        }
    }

    uint64_t ps_ptr = 0;
    if (ia64_fw_pei_get_ps_ptr(env, env->r[32], &ps_ptr) ||
        ia64_fw_pei_get_ps_ptr(env, env->r[33], &ps_ptr) ||
        ia64_fw_pei_get_ps_ptr(env, env->r[34], &ps_ptr)) {
        ia64_fw_pei_dump_ps(env, ps_ptr, pc);
    }
#endif
}

void HELPER(fw_pei_callfd_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    CPUState *cs = env_cpu(env);
    uint64_t fd = env->r[31];
    uint64_t entry = 0;
    uint64_t gp = 0;
    if (fd) {
        ia64_fw_read_fdesc(cs, fd, &entry, &gp);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_callfd pc=%016" PRIx64
                  " fd=%016" PRIx64 " entry=%016" PRIx64
                  " gp=%016" PRIx64 " r1=%016" PRIx64 "\n",
                  pc, fd, entry, gp, env->r[1]);
#endif
}

static bool ia64_fw_pei_startup_dump_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_STARTUP_DUMP");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static bool ia64_fw_pei_ps_dump_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_PS_DUMP");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static void ia64_fw_pei_dump_ps(CPUIA64State *env, uint64_t ps_ptr, uint64_t pc)
{
    if (!ps_ptr || !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool dumped;
    if (dumped) {
        return;
    }
    dumped = true;

    CPUState *cs = env_cpu(env);
    uint64_t install_ppi = 0, locate_ppi = 0;
    uint64_t ffs_find_vol = 0, ffs_find_file = 0, ffs_find_sec = 0;
    uint64_t install_pei_mem = 0, report_status = 0;
    uint64_t get_hob_list = 0, create_hob = 0;
    uint64_t entry = 0, gp = 0;

    /* Offsets based on EFI_PEI_SERVICES (Framework, PI < 1.0). */
    ia64_fw_read_u64(cs, ps_ptr + 0x18, &install_ppi);
    ia64_fw_read_u64(cs, ps_ptr + 0x28, &locate_ppi);
    ia64_fw_read_u64(cs, ps_ptr + 0x48, &get_hob_list);
    ia64_fw_read_u64(cs, ps_ptr + 0x50, &create_hob);
    ia64_fw_read_u64(cs, ps_ptr + 0x58, &ffs_find_vol);
    ia64_fw_read_u64(cs, ps_ptr + 0x60, &ffs_find_file);
    ia64_fw_read_u64(cs, ps_ptr + 0x68, &ffs_find_sec);
    ia64_fw_read_u64(cs, ps_ptr + 0x70, &install_pei_mem);
    ia64_fw_read_u64(cs, ps_ptr + 0x98, &report_status);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_ps_dump pc=%016" PRIx64 " ps=%016" PRIx64 "\n",
                  pc, ps_ptr);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_ps_dump install_ppi=%016" PRIx64
                  " locate_ppi=%016" PRIx64 " get_hob_list=%016" PRIx64
                  " create_hob=%016" PRIx64 "\n",
                  install_ppi, locate_ppi, get_hob_list, create_hob);
    if (install_ppi && ia64_fw_read_fdesc(cs, install_ppi, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump install_ppi fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
    if (locate_ppi && ia64_fw_read_fdesc(cs, locate_ppi, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump locate_ppi fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
    if (get_hob_list && ia64_fw_read_fdesc(cs, get_hob_list, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump get_hob_list fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
    if (create_hob && ia64_fw_read_fdesc(cs, create_hob, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump create_hob fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_ps_dump ffs_find_vol=%016" PRIx64
                  " ffs_find_file=%016" PRIx64 " ffs_find_sec=%016" PRIx64 "\n",
                  ffs_find_vol, ffs_find_file, ffs_find_sec);
    if (ffs_find_vol && ia64_fw_read_fdesc(cs, ffs_find_vol, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump ffs_find_vol fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
    if (ffs_find_file && ia64_fw_read_fdesc(cs, ffs_find_file, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump ffs_find_file fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
    if (ffs_find_sec && ia64_fw_read_fdesc(cs, ffs_find_sec, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump ffs_find_sec fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_ps_dump install_pei_mem=%016" PRIx64
                  " report_status=%016" PRIx64 "\n",
                  install_pei_mem, report_status);
    if (install_pei_mem && ia64_fw_read_fdesc(cs, install_pei_mem, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump install_pei_mem fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
    if (report_status && ia64_fw_read_fdesc(cs, report_status, &entry, &gp)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ps_dump report_status fd.entry=%016" PRIx64
                      " fd.gp=%016" PRIx64 "\n",
                      entry, gp);
    }
}

static void ia64_fw_pei_dump_startup_desc(CPUIA64State *env, uint64_t ptr,
                                          const char *tag, uint64_t pc)
{
    CPUState *cs = env_cpu(env);
    if (!ptr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_startup %s pc=%016" PRIx64 " ptr=0\n",
                      tag, pc);
        return;
    }

    hwaddr phys = ia64_phys_mode_addr(ptr);
    uint8_t buf[32];
    if (cpu_memory_rw_debug(cs, phys, buf, sizeof(buf), false) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_startup %s pc=%016" PRIx64
                      " ptr=%016" PRIx64 " phys=%016" HWADDR_PRIx
                      " unreadable\n",
                      tag, pc, ptr, phys);
        return;
    }

    uint64_t boot_fv = ldq_le_p(&buf[0]);
    uint64_t car_size = ldq_le_p(&buf[8]);
    uint64_t dispatch16 = ldq_le_p(&buf[16]);
    uint64_t dispatch24 = ldq_le_p(&buf[24]);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_startup %s pc=%016" PRIx64
                  " ptr=%016" PRIx64 " phys=%016" HWADDR_PRIx
                  " boot_fv=%016" PRIx64 " car_size=%016" PRIx64
                  " dispatch16=%016" PRIx64 " dispatch24=%016" PRIx64 "\n",
                  tag, pc, ptr, phys, boot_fv, car_size,
                  dispatch16, dispatch24);
}

void HELPER(fw_pei_startup_dump)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR) || !ia64_fw_log_enabled()) {
        return;
    }
    if (!ia64_fw_pei_startup_dump_enabled()) {
        return;
    }
    static int count;
    const int limit = 8;
    if (count++ >= limit) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_startup_dump pc=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64
                  " r34=%016" PRIx64 " r1=%016" PRIx64
                  " r12=%016" PRIx64 "\n",
                  pc, env->r[32], env->r[33], env->r[34],
                  env->r[1], env->r[12]);
    ia64_fw_pei_dump_startup_desc(env, env->r[32], "r32", pc);
    ia64_fw_pei_dump_startup_desc(env, env->r[33], "r33", pc);

    if (env->r[32]) {
        uint64_t ps_ptr = 0;
        if (ia64_fw_pei_get_ps_ptr(env, env->r[32], &ps_ptr) && ps_ptr >= 8) {
            CPUState *cs = env_cpu(env);
            uint64_t core = ps_ptr - 8;
            uint64_t slot_a = core + 0x2a8;
            uint64_t slot_b = core + 0x2b0;
            uint8_t tmp[8];
            uint64_t val_a = UINT64_MAX;
            uint64_t val_b = UINT64_MAX;
            if (cpu_memory_rw_debug(cs, ia64_phys_mode_addr(slot_a),
                                    tmp, sizeof(tmp), false) == 0) {
                val_a = ldq_le_p(tmp);
            }
            if (cpu_memory_rw_debug(cs, ia64_phys_mode_addr(slot_b),
                                    tmp, sizeof(tmp), false) == 0) {
                val_b = ldq_le_p(tmp);
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_startup_dump core=%016" PRIx64
                          " ppi_slot_a=%016" PRIx64 " val=%016" PRIx64
                          " ppi_slot_b=%016" PRIx64 " val=%016" PRIx64 "\n",
                          core, slot_a, val_a, slot_b, val_b);
        }
    }
#endif
}

void HELPER(fw_pei_dispatch_dump)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    static int enabled = -1;
    static int count;
    const int limit = 4;
    if (enabled == -1) {
        enabled = getenv("QEMU_IA64_PEI_DISPATCH_DUMP") ? 1 : 0;
    }
    if (!enabled || !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    if (count++ >= limit) {
        return;
    }

    CPUState *cs = env_cpu(env);
    uint64_t slot = env->r[12] + 1456;
    uint64_t startup = 0;
    uint64_t dispatch16 = 0;
    uint64_t dispatch24 = 0;
    if (slot >= env->r[12] &&
        cpu_memory_rw_debug(cs, ia64_phys_mode_addr(slot),
                            (uint8_t *)&startup, sizeof(startup), false) == 0 &&
        startup) {
        (void)ia64_fw_read_u64(cs, startup + 16, &dispatch16);
        (void)ia64_fw_read_u64(cs, startup + 24, &dispatch24);
    }

    uint64_t p15 = (env->pr >> 15) & 1;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_dispatch_dump pc=%016" PRIx64
                  " r12=%016" PRIx64 " slot=%016" PRIx64
                  " startup=%016" PRIx64
                  " dispatch16=%016" PRIx64
                  " dispatch24=%016" PRIx64
                  " pr=%016" PRIx64 " p15=%" PRIu64 "\n",
                  pc, env->r[12], slot, startup,
                  dispatch16, dispatch24, env->pr, p15);
#endif
}

void HELPER(fw_pei_ppi_dump)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    static int enabled = -1;
    static bool dumped;
    static int dump_always = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_PPI_DUMP");
        enabled = (s && *s) ? 1 : 0;
    }
    if (dump_always == -1) {
        const char *s = getenv("QEMU_IA64_PEI_PPI_DUMP_ALWAYS");
        dump_always = (s && *s) ? 1 : 0;
    }
    if (!enabled || (!dump_always && dumped)) {
        return;
    }
    dumped = true;

    CPUState *cs = env_cpu(env);
    uint64_t ps_ptr = env->fw_pei_ps;
    if (ps_ptr && !ia64_fw_pei_is_ps_table(cs, ps_ptr)) {
        ps_ptr = 0;
        env->fw_pei_ps = 0;
    }
    if (!ps_ptr) {
        if (!ia64_fw_pei_get_ps_ptr(env, env->r[32], &ps_ptr)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ppi_db pc=%016" PRIx64
                          " missing PeiServices\n",
                          pc);
            return;
        }
    }
    env->fw_pei_ps = ps_ptr;

    uint64_t core = 0;
    if (!ia64_fw_pei_find_core_from_ps(env, ps_ptr, &core)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ppi_db pc=%016" PRIx64
                      " ps=%016" PRIx64 " missing core\n",
                      pc, ps_ptr);
        return;
    }

    const uint64_t ppi_base = core + 16; /* Signature + PS pointer. */
    uint8_t hdr[40];
    if (!ia64_fw_read_bytes_any(cs, ppi_base, hdr, sizeof(hdr))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ppi_db pc=%016" PRIx64
                      " ps=%016" PRIx64 " core=%016" PRIx64
                      " ppi_data unreadable\n",
                      pc, ps_ptr, core);
        return;
    }

    int64_t ppi_end = (int64_t)ldq_le_p(&hdr[0]);
    int64_t notify_end = (int64_t)ldq_le_p(&hdr[8]);
    int64_t dispatch_end = (int64_t)ldq_le_p(&hdr[16]);
    int64_t last_install = (int64_t)ldq_le_p(&hdr[24]);
    int64_t last_notify = (int64_t)ldq_le_p(&hdr[32]);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_ppi_db pc=%016" PRIx64
                  " ps=%016" PRIx64 " core=%016" PRIx64
                  " ppi_end=%" PRIi64 " notify_end=%" PRIi64
                  " dispatch_end=%" PRIi64 " last_install=%" PRIi64
                  " last_notify=%" PRIi64 "\n",
                  pc, ps_ptr, core, ppi_end, notify_end,
                  dispatch_end, last_install, last_notify);

    if (ppi_end < 0) {
        ppi_end = 0;
    }
    const int64_t max_ppi = 128;
    if (ppi_end > max_ppi) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_ppi_db clamp ppi_end=%" PRIi64 " -> %d\n",
                      ppi_end, (int)max_ppi);
        ppi_end = max_ppi;
    }

    const uint64_t list_base = ppi_base + 40;
    for (int64_t i = 0; i < ppi_end; i++) {
        uint64_t desc_ptr = 0;
        if (!ia64_fw_read_u64(cs, list_base + (uint64_t)i * 8, &desc_ptr)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ppi_db idx=%" PRIi64
                          " desc_ptr unreadable\n",
                          i);
            break;
        }
        if (!desc_ptr) {
            continue;
        }

        uint8_t desc[24];
        if (!ia64_fw_read_bytes_any(cs, desc_ptr, desc, sizeof(desc))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ppi_db idx=%" PRIi64
                          " desc=%016" PRIx64 " unreadable\n",
                          i, desc_ptr);
            continue;
        }
        uint64_t flags = ldq_le_p(&desc[0]);
        uint64_t guid_ptr = ldq_le_p(&desc[8]);
        uint64_t ppi_ptr = ldq_le_p(&desc[16]);
        if (guid_ptr) {
            IA64EfiGuid guid;
            if (ia64_fw_read_guid(env, guid_ptr, &guid)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ppi_db idx=%" PRIi64
                              " desc=%016" PRIx64 " flags=%016" PRIx64
                              " ppi=%016" PRIx64
                              " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                              i, desc_ptr, flags, ppi_ptr,
                              guid.data1, guid.data2, guid.data3,
                              guid.data4[0], guid.data4[1],
                              guid.data4[2], guid.data4[3],
                              guid.data4[4], guid.data4[5],
                              guid.data4[6], guid.data4[7]);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_ppi_db idx=%" PRIi64
                              " desc=%016" PRIx64 " flags=%016" PRIx64
                              " ppi=%016" PRIx64 " guid_ptr=%016" PRIx64 "\n",
                              i, desc_ptr, flags, ppi_ptr, guid_ptr);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_ppi_db idx=%" PRIi64
                          " desc=%016" PRIx64 " flags=%016" PRIx64
                          " ppi=%016" PRIx64 " guid_ptr=0\n",
                          i, desc_ptr, flags, ppi_ptr);
        }
    }
#endif
}

void HELPER(fw_pei_status_log)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    static bool logged;
    if (logged || !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    logged = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_pei_status pc=%016" PRIx64
                  " r8=%016" PRIx64 " r9=%016" PRIx64 " r10=%016" PRIx64
                  " r11=%016" PRIx64 " r32=%016" PRIx64 " r33=%016" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64 "\n",
                  pc,
                  env->r[8], env->r[9], env->r[10], env->r[11],
                  env->r[32], env->r[33], env->r[1], env->r[12]);
#endif
}

static bool ia64_fw_pei_addr_in_cached_temp(uint64_t phys)
{
    return ia64_fw_pei_cached_temp_base &&
           ia64_fw_pei_cached_temp_size &&
           phys >= ia64_fw_pei_cached_temp_base &&
           phys < (ia64_fw_pei_cached_temp_base +
                   ia64_fw_pei_cached_temp_size);
}

static bool ia64_fw_try_translate_addr(CPUIA64State *env, uint64_t va,
                                       hwaddr *pa)
{
    if (!(env->psr & IA64_PSR_DT)) {
        *pa = ia64_phys_mode_addr(va);
        return true;
    }
    return ia64_try_translate(env, va, pa);
}

static bool ia64_fw_pei_store_watch_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_STORE_WATCH");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled != 0;
}

static bool ia64_fw_pei_store_watch_active;

static bool ia64_fw_pei_load_watch_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_LOAD_WATCH");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled != 0;
}

static bool ia64_fw_pei_memdump_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PEI_MEMDUMP");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled != 0;
}

void HELPER(fw_pei_store_watch_toggle)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!ia64_fw_pei_store_watch_enabled()) {
        return;
    }
    bool enable = (pc == 0x00000000ffe66020ULL ||
                   pc == 0x80000000ffe66020ULL);
    ia64_fw_pei_store_watch_active = enable;
    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_store_watch %s pc=%016" PRIx64 "\n",
                      enable ? "on" : "off", pc);
    }
#endif
}

void HELPER(fw_pei_store_watch)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                                uint64_t addr, uint32_t size, uint64_t val)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)ri;
    (void)addr;
    (void)size;
    (void)val;
    return;
#else
    if (!ia64_fw_pei_store_watch_enabled() ||
        !ia64_fw_pei_store_watch_active) {
        return;
    }

    static int log_limit = -1;
    static int log_count;
    if (log_limit == -1) {
        log_limit = 128;
        const char *s = getenv("QEMU_IA64_PEI_STORE_WATCH_LIMIT");
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

    hwaddr addr_pa = 0;
    bool pa_ok = ia64_fw_try_translate_addr(env, addr, &addr_pa);
    uint64_t addr_phys = ia64_phys_mode_addr(addr);
    const hwaddr lo = 0x000000001ef10000ULL;
    const hwaddr hi = 0x000000001ef10100ULL;
    bool hit = (pa_ok && addr_pa >= lo && addr_pa < hi) ||
               (addr_phys >= lo && addr_phys < hi);
    if (!hit) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "pei_store_watch pc=%016" PRIx64 " ri=%u"
                  " ip=%016" PRIx64
                  " addr=%016" PRIx64 " addr_pa=%016" HWADDR_PRIx
                  " addr_phys=%016" PRIx64 " size=%u val=%016" PRIx64 "\n",
                  pc, ri, env->ip, addr, addr_pa, addr_phys, size, val);
#endif
}

void HELPER(fw_pei_load_watch)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                               uint64_t addr, uint32_t size, uint64_t val)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)ri;
    (void)addr;
    (void)size;
    (void)val;
    return;
#else
    if (!ia64_fw_pei_load_watch_enabled() ||
        !ia64_fw_pei_store_watch_active) {
        return;
    }

    static int log_limit = -1;
    static int log_count;
    if (log_limit == -1) {
        log_limit = 128;
        const char *s = getenv("QEMU_IA64_PEI_LOAD_WATCH_LIMIT");
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

    hwaddr addr_pa = 0;
    bool pa_ok = ia64_fw_try_translate_addr(env, addr, &addr_pa);
    uint64_t addr_phys = ia64_phys_mode_addr(addr);
    const hwaddr lo = 0x000000001ef10000ULL;
    const hwaddr hi = 0x000000001ef10100ULL;
    bool hit = (pa_ok && addr_pa >= lo && addr_pa < hi) ||
               (addr_phys >= lo && addr_phys < hi);
    if (!hit) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "pei_load_watch pc=%016" PRIx64 " ri=%u"
                  " ip=%016" PRIx64
                  " addr=%016" PRIx64 " addr_pa=%016" HWADDR_PRIx
                  " addr_phys=%016" PRIx64 " size=%u val=%016" PRIx64 "\n",
                  pc, ri, env->ip, addr, addr_pa, addr_phys, size, val);
#endif
}

void HELPER(fw_pei_memdump)(CPUIA64State *env, uint64_t pc, uint32_t stage)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)stage;
    return;
#else
    if (!ia64_fw_pei_memdump_enabled() ||
        !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }

    static bool dumped[2];
    if (stage < 2 && dumped[stage]) {
        return;
    }
    if (stage < 2) {
        dumped[stage] = true;
    }

    const char *tag = (stage == 0) ? "pre" : (stage == 1) ? "post" : "stage";
    const hwaddr base = 0x000000001ef10000ULL;
    enum { IA64_PEI_MEMDUMP_SIZE = 0x100 };
    uint8_t buf[IA64_PEI_MEMDUMP_SIZE];
    CPUState *cs = env_cpu(env);
    if (cpu_memory_rw_debug(cs, base, buf, sizeof(buf), false) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_memdump %s pc=%016" PRIx64
                      " base=%016" HWADDR_PRIx " unreadable\n",
                      tag, pc, base);
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_memdump %s pc=%016" PRIx64
                  " base=%016" HWADDR_PRIx " size=%u\n",
                  tag, pc, base, (unsigned)IA64_PEI_MEMDUMP_SIZE);
    for (size_t off = 0; off < IA64_PEI_MEMDUMP_SIZE; off += 16) {
        char line[128];
        int pos = snprintf(line, sizeof(line), "  %016" HWADDR_PRIx ":",
                           base + off);
        for (size_t i = 0; i < 16; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos,
                            " %02x", buf[off + i]);
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
    }
#endif
}

void HELPER(fw_pei_install_mem_call_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    static bool logged;
    if (logged) {
        return;
    }
    logged = true;

    CPUState *cs = env_cpu(env);
    uint64_t b7 = env->b[7];
    uint64_t entry = 0;
    uint64_t gp = 0;
    uint8_t fdesc_raw[16];
    bool fdesc_ok = false;
    if (b7) {
        hwaddr b7_phys = 0;
        (void)ia64_fw_try_translate_addr(env, b7, &b7_phys);
        if (cpu_memory_rw_debug(cs, b7_phys, fdesc_raw,
                                sizeof(fdesc_raw), false) == 0) {
            fdesc_ok = true;
            entry = ldq_le_p(&fdesc_raw[0]);
            gp = ldq_le_p(&fdesc_raw[8]);
        } else {
            ia64_fw_read_fdesc(cs, b7, &entry, &gp);
        }
    }
    uint64_t entry_region = entry >> 61;
    uint64_t entry_low = entry & ((1ULL << 61) - 1);
    hwaddr entry_phys = 0;
    if (entry) {
        if (!ia64_fw_try_translate_addr(env, entry, &entry_phys)) {
            entry_phys = 0;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_install_mem_call pc=%016" PRIx64
                  " b7=%016" PRIx64 " entry=%016" PRIx64
                  " entry_region=%" PRIu64 " entry_low=%016" PRIx64
                  " entry_phys=%016" HWADDR_PRIx " gp=%016" PRIx64
                  " r1=%016" PRIx64 " r30=%016" PRIx64
                  " r31=%016" PRIx64 " psr=%016" PRIx64 "\n",
                  pc, b7, entry, entry_region, entry_low, entry_phys,
                  gp, env->r[1], env->r[30], env->r[31], env->psr);
    {
        hwaddr r31_phys = 0;
        bool r31_ok = ia64_fw_try_translate_addr(env, env->r[31], &r31_phys);
        uint8_t r31_buf[32];
        if (r31_ok &&
            cpu_memory_rw_debug(cs, r31_phys, r31_buf, sizeof(r31_buf), false) == 0) {
            char line[160];
            int pos = snprintf(line, sizeof(line),
                               "IA64: pei_install_mem_call r31_mem %016" HWADDR_PRIx ":",
                               r31_phys);
            for (size_t i = 0; i < sizeof(r31_buf); i++) {
                pos += snprintf(line + pos, sizeof(line) - pos,
                                " %02x", r31_buf[i]);
            }
            qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_call r31_phys=%016" HWADDR_PRIx
                          " ok=%d\n",
                          r31_phys, r31_ok ? 1 : 0);
        }
    }
    if (entry) {
        uint8_t rr_idx = extract64(entry, 61, 3);
        uint64_t rr = env->rr[rr_idx];
        uint32_t rid = RR_RID(rr);
        uint8_t ps = RR_PS(rr);
        if (!ps) {
            ps = 12;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_call entry_rr idx=%u rr=%016" PRIx64
                      " rid=%u ps=%u\n",
                      rr_idx, rr, rid, ps);
        if ((env->psr & IA64_PSR_DT) && PTA_VE(env->cr[8]) && RR_VE(rr)) {
            uint64_t vhpt_addr = helper_thash(env);
            uint64_t pte = cpu_ldq_data(env, vhpt_addr);
            uint64_t tar = PTA_VF(env->cr[8]) ?
                cpu_ldq_data(env, vhpt_addr + 8) :
                ((uint64_t)rid << 8) | ((uint64_t)RR_PS(rr) << 2);
            uint64_t tag = PTA_VF(env->cr[8]) ?
                cpu_ldq_data(env, vhpt_addr + 16) : 0;
            uint64_t expected = helper_ttag(env);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_call vhpt addr=%016" PRIx64
                          " pte=%016" PRIx64 " tar=%016" PRIx64
                          " tag=%016" PRIx64 " exp=%016" PRIx64 "\n",
                          vhpt_addr, pte, tar, tag, expected);
        }
        for (int i = 0; i < ARRAY_SIZE(env->dtrs); i++) {
            const typeof(env->dtrs[0]) *e = &env->dtrs[i];
            if (!e->valid || e->rid != rid || !PTE_P(e->pte)) {
                continue;
            }
            uint64_t mask = ~((1ULL << e->ps) - 1);
            if ((entry & mask) == e->tag) {
                uint64_t pa = e->pa + (entry & ~mask);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_install_mem_call DTR[%d] tag=%016" PRIx64
                              " ps=%u pa_base=%016" PRIx64 " -> pa=%016" PRIx64 "\n",
                              i, e->tag, e->ps, e->pa, pa);
            }
        }
        for (int i = 0; i < ARRAY_SIZE(env->itrs); i++) {
            const typeof(env->itrs[0]) *e = &env->itrs[i];
            if (!e->valid || e->rid != rid || !PTE_P(e->pte)) {
                continue;
            }
            uint64_t mask = ~((1ULL << e->ps) - 1);
            if ((entry & mask) == e->tag) {
                uint64_t pa = e->pa + (entry & ~mask);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_install_mem_call ITR[%d] tag=%016" PRIx64
                              " ps=%u pa_base=%016" PRIx64 " -> pa=%016" PRIx64 "\n",
                              i, e->tag, e->ps, e->pa, pa);
            }
        }
    }
    if (fdesc_ok) {
        hwaddr b7_phys = 0;
        bool b7_ok = ia64_fw_try_translate_addr(env, b7, &b7_phys);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_call fdesc bytes=%02x %02x %02x %02x"
                      " %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                      fdesc_raw[0], fdesc_raw[1], fdesc_raw[2], fdesc_raw[3],
                      fdesc_raw[4], fdesc_raw[5], fdesc_raw[6], fdesc_raw[7],
                      fdesc_raw[8], fdesc_raw[9], fdesc_raw[10], fdesc_raw[11],
                      fdesc_raw[12], fdesc_raw[13], fdesc_raw[14], fdesc_raw[15]);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_call b7_phys=%016" HWADDR_PRIx
                      " ok=%d\n",
                      b7_phys, b7_ok ? 1 : 0);
        uint8_t around[48];
        hwaddr start = (b7_phys >= 16) ? (b7_phys - 16) : 0;
        if (cpu_memory_rw_debug(cs, start, around, sizeof(around), false) == 0) {
            char line[192];
            int pos = snprintf(line, sizeof(line),
                               "  b7_mem %016" HWADDR_PRIx ":", start);
            for (size_t i = 0; i < sizeof(around); i++) {
                pos += snprintf(line + pos, sizeof(line) - pos,
                                " %02x", around[i]);
            }
            qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
        }
    }
    if (entry_phys) {
        uint8_t probe[16];
        int rc = cpu_memory_rw_debug(cs, entry_phys, probe, sizeof(probe), false);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_call probe entry_phys=%016" HWADDR_PRIx
                      " rc=%d\n",
                      entry_phys, rc);
        if (rc == 0) {
            ia64_fw_dump_code(env, "pei_install_mem_call", entry_phys, 16);
        }
    }
    if (entry) {
        hwaddr entry32 = (hwaddr)(entry & 0xffffffffULL);
        uint8_t probe[16];
        int rc = cpu_memory_rw_debug(cs, entry32, probe, sizeof(probe), false);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_call probe entry32=%08" HWADDR_PRIx
                      " rc=%d\n",
                      entry32, rc);
        if (rc == 0) {
            ia64_fw_dump_code(env, "pei_install_mem_call_32", entry32, 16);
        }
    }
#endif
}

void HELPER(fw_psr_update_log)(CPUIA64State *env, uint64_t pc,
                               uint64_t old_psr, uint64_t new_psr,
                               uint64_t mask)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)old_psr;
    (void)new_psr;
    (void)mask;
    return;
#else
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_PSR_LOG");
        enabled = (s && *s) ? 1 : 0;
    }
    if (!enabled || !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    if (!(mask & (IA64_PSR_DT | IA64_PSR_IT))) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: psr_update pc=%016" PRIx64 " mask=%016" PRIx64
                  " old=%016" PRIx64 " new=%016" PRIx64 "\n",
                  pc, mask, old_psr, new_psr);
#endif
}

void HELPER(fw_pei_install_mem_trace)(CPUIA64State *env, uint64_t pc, uint32_t stage)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)stage;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }

    static bool logged[2];
    if (stage < 2 && logged[stage]) {
        return;
    }
    if (stage < 2) {
        logged[stage] = true;
    }

    const char *tag = (stage == 0) ? "pre" : (stage == 1) ? "post" : "stage";
    uint64_t ps_ptr = env->fw_pei_ps;
    CPUState *cs = env_cpu(env);
    if (ps_ptr && !ia64_fw_pei_is_ps_table(cs, ps_ptr)) {
        ps_ptr = 0;
        env->fw_pei_ps = 0;
    }
    if (!ps_ptr) {
        if (!ia64_fw_pei_get_ps_ptr(env, env->r[32], &ps_ptr) &&
            !ia64_fw_pei_get_ps_ptr(env, env->r[33], &ps_ptr) &&
            !ia64_fw_pei_get_ps_ptr(env, env->r[34], &ps_ptr)) {
            ps_ptr = 0;
        }
    }
    if (ps_ptr) {
        env->fw_pei_ps = ps_ptr;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_install_mem_trace %s pc=%016" PRIx64
                  " ps=%016" PRIx64 " r8=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64
                  " r34=%016" PRIx64 " r12=%016" PRIx64
                  " r1=%016" PRIx64 "\n",
                  tag, pc, ps_ptr, env->r[8], env->r[32], env->r[33],
                  env->r[34], env->r[12], env->r[1]);

    if (!ps_ptr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_trace %s missing PeiServices\n",
                      tag);
        return;
    }

    uint64_t core = 0;
    if (!ia64_fw_pei_find_core_from_ps(env, ps_ptr, &core)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_trace %s ps=%016" PRIx64
                      " missing core\n",
                      tag, ps_ptr);
        return;
    }

    const uint64_t ppi_base = core + 16;
    uint8_t hdr[40];
    if (!ia64_fw_read_bytes_any(cs, ppi_base, hdr, sizeof(hdr))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_trace %s ps=%016" PRIx64
                      " core=%016" PRIx64 " ppi_hdr unreadable\n",
                      tag, ps_ptr, core);
        return;
    }

    int64_t ppi_end = (int64_t)ldq_le_p(&hdr[0]);
    int64_t notify_end = (int64_t)ldq_le_p(&hdr[8]);
    int64_t dispatch_end = (int64_t)ldq_le_p(&hdr[16]);

    uint64_t core_phys = ia64_phys_mode_addr(core);
    uint64_t ppi_phys = ia64_phys_mode_addr(ppi_base);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_install_mem_trace %s core=%016" PRIx64
                  " core_phys=%016" PRIx64 " ppi_base=%016" PRIx64
                  " ppi_phys=%016" PRIx64 " ppi_end=%" PRIi64
                  " notify_end=%" PRIi64 " dispatch_end=%" PRIi64
                  " temp=%016" PRIx64 "+%016" PRIx64 "\n",
                  tag, core, core_phys, ppi_base, ppi_phys,
                  ppi_end, notify_end, dispatch_end,
                  ia64_fw_pei_cached_temp_base,
                  ia64_fw_pei_cached_temp_size);

    if (ppi_end < 0) {
        ppi_end = 0;
    }
    const int64_t max_ppi = 256;
    if (ppi_end > max_ppi) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: pei_install_mem_trace %s clamp ppi_end=%" PRIi64
                      " -> %d\n",
                      tag, ppi_end, (int)max_ppi);
        ppi_end = max_ppi;
    }

    const uint64_t list_base = ppi_base + 40;
    bool dump_all = (stage == 1);
    bool found_status = false;
    bool found_mem = false;
    for (int64_t i = 0; i < ppi_end; i++) {
        uint64_t desc_ptr = 0;
        if (!ia64_fw_read_u64(cs, list_base + (uint64_t)i * 8, &desc_ptr)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_trace %s idx=%" PRIi64
                          " desc_ptr unreadable\n",
                          tag, i);
            break;
        }
        if (!desc_ptr) {
            if (dump_all) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_install_mem_trace %s entry idx=%" PRIi64
                              " desc=0\n",
                              tag, i);
            }
            continue;
        }
        uint8_t desc[24];
        if (!ia64_fw_read_bytes_any(cs, desc_ptr, desc, sizeof(desc))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_trace %s idx=%" PRIi64
                          " desc=%016" PRIx64 " unreadable\n",
                          tag, i, desc_ptr);
            continue;
        }
        uint64_t flags = ldq_le_p(&desc[0]);
        uint64_t guid_ptr = ldq_le_p(&desc[8]);
        uint64_t ppi_ptr = ldq_le_p(&desc[16]);
        if (!guid_ptr) {
            if (dump_all) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_install_mem_trace %s entry idx=%" PRIi64
                              " desc=%016" PRIx64 " flags=%016" PRIx64
                              " guid_ptr=0 ppi=%016" PRIx64 "\n",
                              tag, i, desc_ptr, flags, ppi_ptr);
            }
            continue;
        }
        uint64_t desc_phys = ia64_phys_mode_addr(desc_ptr);
        uint64_t guid_phys = ia64_phys_mode_addr(guid_ptr);
        bool desc_temp = ia64_fw_pei_addr_in_cached_temp(desc_phys);
        bool guid_temp = ia64_fw_pei_addr_in_cached_temp(guid_phys);
        if (desc_temp || guid_temp) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_trace %s temp_ppi idx=%" PRIi64
                          " desc=%016" PRIx64 " guid_ptr=%016" PRIx64
                          " ppi=%016" PRIx64 " desc_temp=%d guid_temp=%d\n",
                          tag, i, desc_ptr, guid_ptr, ppi_ptr,
                          desc_temp ? 1 : 0, guid_temp ? 1 : 0);
        }
        IA64EfiGuid guid;
        bool guid_ok = ia64_fw_read_guid(env, guid_ptr, &guid);
        if (!guid_ok) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_trace %s idx=%" PRIi64
                          " guid_ptr=%016" PRIx64 " unreadable\n",
                          tag, i, guid_ptr);
            if (dump_all) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_install_mem_trace %s entry idx=%" PRIi64
                              " desc=%016" PRIx64 " guid_ptr=%016" PRIx64
                              " ppi=%016" PRIx64 "\n",
                              tag, i, desc_ptr, guid_ptr, ppi_ptr);
            }
            continue;
        }
        if (dump_all) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_trace %s entry idx=%" PRIi64
                          " desc=%016" PRIx64 " guid_ptr=%016" PRIx64
                          " ppi=%016" PRIx64
                          " guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                          tag, i, desc_ptr, guid_ptr, ppi_ptr,
                          guid.data1, guid.data2, guid.data3,
                          guid.data4[0], guid.data4[1], guid.data4[2],
                          guid.data4[3], guid.data4[4], guid.data4[5],
                          guid.data4[6], guid.data4[7]);
        }
        if (ia64_fw_guid_equal(&guid, &ia64_efi_guid_status_code_ppi)) {
            uint64_t ppi_ptr_phys = ia64_phys_mode_addr(ppi_ptr);
            bool in_temp = ia64_fw_pei_addr_in_cached_temp(desc_phys) ||
                           ia64_fw_pei_addr_in_cached_temp(ppi_ptr_phys);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_trace %s status_ppi"
                          " idx=%" PRIi64 " desc=%016" PRIx64
                          " ppi=%016" PRIx64 " guid_ptr=%016" PRIx64
                          " desc_phys=%016" PRIx64 " ppi_phys=%016" PRIx64
                          " temp=%d\n",
                          tag, i, desc_ptr, ppi_ptr, guid_ptr,
                          desc_phys, ppi_ptr_phys, in_temp ? 1 : 0);
            found_status = true;
        } else if (ia64_fw_guid_equal(&guid, &ia64_efi_guid_memory_discovered_ppi)) {
            uint64_t ppi_ptr_phys = ia64_phys_mode_addr(ppi_ptr);
            bool in_temp = ia64_fw_pei_addr_in_cached_temp(desc_phys) ||
                           ia64_fw_pei_addr_in_cached_temp(ppi_ptr_phys);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_install_mem_trace %s memdisc_ppi"
                          " idx=%" PRIi64 " desc=%016" PRIx64
                          " ppi=%016" PRIx64 " guid_ptr=%016" PRIx64
                          " desc_phys=%016" PRIx64 " ppi_phys=%016" PRIx64
                          " temp=%d\n",
                          tag, i, desc_ptr, ppi_ptr, guid_ptr,
                          desc_phys, ppi_ptr_phys, in_temp ? 1 : 0);
            found_mem = true;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_install_mem_trace %s found_status=%d"
                  " found_mem=%d\n",
                  tag, found_status ? 1 : 0, found_mem ? 1 : 0);

    if (stage == 1 && env->r[8] == 0) {
        env->fw_pei_mem_installed = 1;
    }
#endif
}

void HELPER(fw_pei_aftermem_trace)(CPUIA64State *env, uint64_t pc, uint32_t stage)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)stage;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }

    static int counts[8];
    const int limit = 16;
    uint32_t idx = stage;
    if (idx >= ARRAY_SIZE(counts)) {
        idx = ARRAY_SIZE(counts) - 1;
    }
    if (counts[idx]++ >= limit) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_pei_aftermem stage=%u pc=%016" PRIx64
                  " r8=%016" PRIx64 " r9=%016" PRIx64 " r10=%016" PRIx64
                  " r11=%016" PRIx64 " r30=%016" PRIx64 " r31=%016" PRIx64
                  " r39=%016" PRIx64 " r40=%016" PRIx64 " r41=%016" PRIx64
                  " r42=%016" PRIx64 " r43=%016" PRIx64 " r44=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64 "\n",
                  stage, pc,
                  env->r[8], env->r[9], env->r[10], env->r[11],
                  env->r[30], env->r[31],
                  env->r[39], env->r[40], env->r[41],
                  env->r[42], env->r[43], env->r[44],
                  env->r[32], env->r[33],
                  env->r[1], env->r[12]);
#endif
}

static void ia64_fw_pei_log_phit(CPUIA64State *env, uint64_t pc, uint64_t hob_ptr)
{
    CPUState *cs = env_cpu(env);
    uint64_t hob_list = 0;
    if (hob_ptr) {
        uint8_t tmp[8];
        hwaddr hob_ptr_phys = ia64_phys_mode_addr(hob_ptr);
        if (cpu_memory_rw_debug(cs, hob_ptr_phys, tmp, sizeof(tmp), false) == 0) {
            hob_list = ldq_le_p(tmp);
        }
    }

    if (!hob_list) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_oor phit pc=%016" PRIx64
                      " hob_ptr=%016" PRIx64 " hob_list=0\n",
                      pc, hob_ptr);
        return;
    }

    hwaddr hob_phys = ia64_phys_mode_addr(hob_list);
    uint8_t phit[0x38];
    if (cpu_memory_rw_debug(cs, hob_phys, phit, sizeof(phit), false) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_oor phit pc=%016" PRIx64
                      " hob_ptr=%016" PRIx64 " hob_list=%016" PRIx64
                      " read_failed\n",
                      pc, hob_ptr, hob_list);
        return;
    }

    uint16_t hob_type = lduw_le_p(&phit[0]);
    uint16_t hob_len = lduw_le_p(&phit[2]);
    uint32_t version = ldl_le_p(&phit[8]);
    uint32_t boot_mode = ldl_le_p(&phit[12]);
    uint64_t mem_top = ldq_le_p(&phit[16]);
    uint64_t mem_bottom = ldq_le_p(&phit[24]);
    uint64_t free_top = ldq_le_p(&phit[32]);
    uint64_t free_bottom = ldq_le_p(&phit[40]);
    uint64_t end_hob = ldq_le_p(&phit[48]);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_pei_oor phit pc=%016" PRIx64
                  " hob_ptr=%016" PRIx64 " hob_list=%016" PRIx64
                  " type=%04x len=%04x ver=%u boot=%u\n",
                  pc, hob_ptr, hob_list, hob_type, hob_len,
                  version, boot_mode);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_pei_oor phit mem=[%016" PRIx64 "..%016" PRIx64 "]"
                  " free=[%016" PRIx64 "..%016" PRIx64 "] end=%016" PRIx64
                  " phys_mem=[%016" PRIx64 "..%016" PRIx64 "]"
                  " phys_free=[%016" PRIx64 "..%016" PRIx64 "] phys_end=%016" PRIx64 "\n",
                  mem_bottom, mem_top, free_bottom, free_top, end_hob,
                  (uint64_t)ia64_phys_mode_addr(mem_bottom),
                  (uint64_t)ia64_phys_mode_addr(mem_top),
                  (uint64_t)ia64_phys_mode_addr(free_bottom),
                  (uint64_t)ia64_phys_mode_addr(free_top),
                  (uint64_t)ia64_phys_mode_addr(end_hob));
}

void HELPER(fw_pei_oor_probe)(CPUIA64State *env, uint64_t pc, uint32_t stage)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)stage;
    return;
#else
    if (!qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }

    CPUState *cs = env_cpu(env);
    static int counts[2];
    const int limit = 16;
    uint32_t idx = (stage < ARRAY_SIZE(counts)) ? stage : 1;
    if (counts[idx]++ >= limit) {
        return;
    }

    if (stage == 0) {
        static int oor_dump_enabled = -1;
        static int oor_dump_bundles = -1;
        static bool oor_dumped;
        if (oor_dump_enabled == -1) {
            const char *s = getenv("QEMU_IA64_PEI_OOR_DUMP");
            oor_dump_enabled = (s && *s) ? 1 : 0;
            oor_dump_bundles = 64;
            const char *b = getenv("QEMU_IA64_PEI_OOR_DUMP_BUNDLES");
            if (b && *b) {
                oor_dump_bundles = atoi(b);
                if (oor_dump_bundles <= 0) {
                    oor_dump_bundles = 64;
                }
            }
        }
        env->dbg_fw_pei_oor_ptr30 = env->r[30];
        env->dbg_fw_pei_oor_ptr31 = env->r[31];
        env->dbg_fw_pei_oor_hob_ptr = env->r[40] ? env->r[40] : env->r[35];
        env->dbg_fw_pei_oor_active = 1;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_oor pre pc=%016" PRIx64
                      " r30_ptr=%016" PRIx64 " r31_ptr=%016" PRIx64
                      " r12=%016" PRIx64 " r32=%016" PRIx64 " r33=%016" PRIx64
                      " r34=%016" PRIx64 " r35=%016" PRIx64
                      " r39=%016" PRIx64 " r40=%016" PRIx64 "\n",
                      pc, env->r[30], env->r[31], env->r[12],
                      env->r[32], env->r[33], env->r[34], env->r[35],
                      env->r[39], env->r[40]);
        if (oor_dump_enabled && !oor_dumped) {
            ia64_fw_dump_code(env, "pei_oor_pc", pc, oor_dump_bundles);
            if (env->b[7]) {
                ia64_fw_dump_code(env, "pei_oor_b7", env->b[7], oor_dump_bundles);
            }
            if (env->last_branch_from) {
                ia64_fw_dump_code(env, "pei_oor_from",
                                  env->last_branch_from, oor_dump_bundles);
            }
            if (env->last_branch_to) {
                ia64_fw_dump_code(env, "pei_oor_to",
                                  env->last_branch_to, oor_dump_bundles);
            }
            oor_dumped = true;
        }
        return;
    }

    uint64_t ptr30 = env->dbg_fw_pei_oor_ptr30;
    uint64_t ptr31 = env->dbg_fw_pei_oor_ptr31;
    bool mem30_ok = false;
    bool mem31_ok = false;
    uint64_t mem30 = 0;
    uint16_t mem31 = 0;
    if (ptr30) {
        uint8_t tmp[8];
        hwaddr ptr30_phys = ia64_phys_mode_addr(ptr30);
        if (cpu_memory_rw_debug(cs, ptr30_phys, tmp, sizeof(tmp), false) == 0) {
            mem30 = ldq_le_p(tmp);
            mem30_ok = true;
        }
    }
    if (ptr31) {
        uint8_t tmp[2];
        hwaddr ptr31_phys = ia64_phys_mode_addr(ptr31);
        if (cpu_memory_rw_debug(cs, ptr31_phys, tmp, sizeof(tmp), false) == 0) {
            mem31 = lduw_le_p(tmp);
            mem31_ok = true;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_pei_oor post pc=%016" PRIx64
                  " r30=%016" PRIx64 " r31=%016" PRIx64
                  " ptr30=%016" PRIx64 " ptr31=%016" PRIx64
                  " mem30=%016" PRIx64 "%s mem31=%04x%s\n",
                  pc, env->r[30], env->r[31], ptr30, ptr31,
                  mem30, mem30_ok ? "" : "(!)",
                  mem31, mem31_ok ? "" : "(!)");

    if (env->dbg_fw_pei_oor_active) {
        uint64_t hob_ptr = env->dbg_fw_pei_oor_hob_ptr;
        if (!hob_ptr) {
            hwaddr frame = ia64_phys_mode_addr(env->r[12]);
            if (frame) {
                uint8_t tmp[8];
                if (cpu_memory_rw_debug(cs, frame + 88, tmp, sizeof(tmp), false) == 0) {
                    hob_ptr = ldq_le_p(tmp);
                }
            }
        }
        ia64_fw_pei_log_phit(env, pc, hob_ptr);
        ia64_fw_r8_log_pei_hob(env, pc, env->r[32]);
        {
            uint64_t ps_ptr = env->fw_pei_ps;
            uint64_t core = 0;
            if (!ps_ptr) {
                uint64_t cand_ps = 0;
                if (ia64_fw_pei_get_ps_ptr(env, env->r[39], &cand_ps) ||
                    ia64_fw_pei_get_ps_ptr(env, env->r[32], &cand_ps) ||
                    ia64_fw_pei_get_ps_ptr(env, env->r[33], &cand_ps)) {
                    ps_ptr = cand_ps;
                }
            }
            if (ps_ptr && ia64_fw_pei_find_core_from_ps(env, ps_ptr, &core)) {
                ia64_fw_pei_log_core_hob_candidates(cs, core, ps_ptr);
            }
        }
        {
            static int hob_dump_enabled = -1;
            static bool dumped;
            if (hob_dump_enabled == -1) {
                const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP");
                hob_dump_enabled = (s && *s) ? 1 : 0;
            }
            if (hob_dump_enabled && !dumped) {
                dumped = ia64_fw_dump_efi_hobs_force(cs, env->r[12]);
            }
        }
    }
#endif
}

void HELPER(fw_pei_indcall_probe)(CPUIA64State *env, uint64_t pc, uint32_t stage)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)stage;
    return;
#else
    static int pei_log_enabled = -1;
    if (pei_log_enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_PEI_LOG");
        pei_log_enabled = ia64_env_truthy(s) ? 1 : 0;
    }

    if (!qemu_loglevel_mask(LOG_GUEST_ERROR) || !pei_log_enabled) {
        return;
    }

    CPUState *cs = env_cpu(env);
    static int counts[2];
    const int limit = 16;
    uint32_t idx = (stage < ARRAY_SIZE(counts)) ? stage : 1;
    if (counts[idx]++ >= limit) {
        return;
    }

    if (stage == 0) {
        uint8_t sof = env->cfm & 0x7f;
        uint8_t sol = (env->cfm >> 7) & 0x7f;
        uint8_t sor = (env->cfm >> 14) & 0x0f;
        uint8_t outs = (sof > sol) ? (sof - sol) : 0;
        uint8_t out0 = 32 + sol;
        uint64_t call_a0 = (out0 < 128) ? env->r[out0] : 0;
        uint64_t call_a1 = (out0 + 1 < 128) ? env->r[out0 + 1] : 0;
        uint64_t call_a2 = (out0 + 2 < 128) ? env->r[out0 + 2] : 0;
        uint64_t call_a3 = (out0 + 3 < 128) ? env->r[out0 + 3] : 0;
        uint64_t call_a4 = (out0 + 4 < 128) ? env->r[out0 + 4] : 0;
        uint64_t call_a5 = (out0 + 5 < 128) ? env->r[out0 + 5] : 0;
        uint64_t call_a6 = (out0 + 6 < 128) ? env->r[out0 + 6] : 0;
        uint64_t call_a7 = (out0 + 7 < 128) ? env->r[out0 + 7] : 0;
        uint64_t ps_ptr = 0;
        uint64_t ps_arg = call_a0 ? call_a0 : env->r[32];
        (void)ia64_fw_pei_get_ps_ptr(env, ps_arg, &ps_ptr);
        if (ps_ptr) {
            uint64_t ps_phys = ia64_phys_mode_addr(ps_ptr);
            bool ps_in_flash = ia64_fw_addr_in_flash(ps_phys);
            bool ps_in_workram = (ps_phys >= IA64_IPF_FW_WORKRAM_BASE &&
                                  ps_phys < IA64_IPF_FW_WORKRAM_BASE + IA64_IPF_FW_WORKRAM_SIZE);
            if (!ps_in_flash && !ps_in_workram) {
                ps_ptr = 0;
            }
        }
        if (!ps_ptr) {
            uint64_t core = 0;
            uint64_t ps_scan = 0;
            hwaddr sp_phys = ia64_phys_mode_addr(env->r[12]);
            if (ia64_fw_pei_scan_core(cs, sp_phys, &core, &ps_scan)) {
                ps_ptr = ps_scan;
            }
        }
        if (ps_ptr && ia64_fw_pei_cached_hob_base &&
            ia64_fw_pei_cached_temp_base && ia64_fw_pei_cached_temp_size) {
            uint64_t core = 0;
            if (ia64_fw_pei_find_core_from_ps(env, ps_ptr, &core)) {
                uint64_t hob_raw = 0;
                if (ia64_fw_read_u64(cs, core + 0x260, &hob_raw)) {
                    uint64_t hob_phys = ia64_phys_mode_addr(hob_raw);
                    bool hob_in_temp =
                        (hob_phys >= ia64_fw_pei_cached_temp_base &&
                         hob_phys < (ia64_fw_pei_cached_temp_base +
                                     ia64_fw_pei_cached_temp_size));
                    if (!hob_in_temp &&
                        hob_phys != ia64_fw_pei_cached_hob_base) {
                        uint64_t enc =
                            ia64_fw_encode_addr(hob_raw,
                                                ia64_fw_pei_cached_hob_base);
                        uint8_t out[8];
                        stq_le_p(out, enc);
                        if (ia64_fw_write_bytes_any(cs, core + 0x260, out,
                                                    sizeof(out)) &&
                            qemu_loglevel_mask(LOG_GUEST_ERROR)) {
                            qemu_log_mask(LOG_GUEST_ERROR,
                                          "IA64: fw_pei_call patch core=%016" PRIx64
                                          " hob_raw=%016" PRIx64 " new=%016" PRIx64
                                          " ps_ptr=%016" PRIx64 "\n",
                                          core, hob_raw, enc, ps_ptr);
                        }
                    }
                }
            }
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_call pre pc=%016" PRIx64
                      " b7=%016" PRIx64 " b0=%016" PRIx64
                      " r1=%016" PRIx64 " r12=%016" PRIx64
                      " cfm=%016" PRIx64 " sof=%u sol=%u sor=%u"
                      " outs=%u out0=r%u"
                      " r30=%016" PRIx64 " r31=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64
                      " r34=%016" PRIx64 " r35=%016" PRIx64
                      " r39=%016" PRIx64 " r40=%016" PRIx64
                      " r41=%016" PRIx64 " r42=%016" PRIx64
                      " r43=%016" PRIx64 " r44=%016" PRIx64
                      " r45=%016" PRIx64 " r46=%016" PRIx64
                      " r47=%016" PRIx64 "\n",
                      pc, env->b[7], env->b[0], env->r[1], env->r[12],
                      env->cfm, sof, sol, sor, outs, out0,
                      env->r[30], env->r[31], env->r[32], env->r[33],
                      env->r[34], env->r[35], env->r[39], env->r[40],
                      env->r[41], env->r[42], env->r[43], env->r[44],
                      env->r[45], env->r[46], env->r[47]);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_call args out0=r%u outs=%u"
                      " a0=%016" PRIx64 " a1=%016" PRIx64
                      " a2=%016" PRIx64 " a3=%016" PRIx64
                      " a4=%016" PRIx64 " a5=%016" PRIx64
                      " a6=%016" PRIx64 " a7=%016" PRIx64 "\n",
                      out0, outs,
                      call_a0, call_a1, call_a2, call_a3,
                      call_a4, call_a5, call_a6, call_a7);
        if (ps_ptr) {
            ia64_fw_pei_log_ps_entry(cs, ps_ptr, env->b[7]);
        }
        if (call_a1 >= 0x100000 && call_a2 >= 0x100000 &&
            (call_a2 & 0xfffULL) == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_mem_call base=%016" PRIx64
                          " size=%016" PRIx64 " ps=%016" PRIx64
                          " pc=%016" PRIx64 "\n",
                          call_a1, call_a2, ps_ptr, pc);
        }
        const uint64_t get_hob_list_entry = 0x00000000ffe24b90ULL;
        const uint64_t get_hob_list_entry_hi = 0x80000000ffe24b90ULL;
        const uint64_t create_hob_entry = 0x00000000ffe24c70ULL;
        const uint64_t create_hob_entry_hi = 0x80000000ffe24c70ULL;
        if (env->b[7] == get_hob_list_entry ||
            env->b[7] == get_hob_list_entry_hi) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_get_hob_list hob_ptr=%016" PRIx64
                          " ps=%016" PRIx64 "\n",
                          call_a1, ps_ptr);
            if (call_a1) {
                uint8_t tmp[8];
                uint64_t hob_val = 0;
                hwaddr hob_ptr_phys = ia64_phys_mode_addr(call_a1);
                if (cpu_memory_rw_debug(cs, hob_ptr_phys, tmp, sizeof(tmp), false) == 0) {
                    hob_val = ldq_le_p(tmp);
                }
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_get_hob_list hob_val=%016" PRIx64 "\n",
                              hob_val);
            }
        }
        if (env->b[7] == create_hob_entry ||
            env->b[7] == create_hob_entry_hi) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: pei_create_hob type=%" PRIu64 " len=%" PRIu64
                          " hob_ptr=%016" PRIx64 " ps=%016" PRIx64 "\n",
                          call_a1, call_a2, call_a3, ps_ptr);
            if (call_a3) {
                uint8_t tmp[8];
                uint64_t hob_val = 0;
                hwaddr hob_ptr_phys = ia64_phys_mode_addr(call_a3);
                if (cpu_memory_rw_debug(cs, hob_ptr_phys, tmp, sizeof(tmp), false) == 0) {
                    hob_val = ldq_le_p(tmp);
                }
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_create_hob hob_val=%016" PRIx64 "\n",
                              hob_val);
            }
            if (ps_ptr) {
                uint64_t core = 0;
                if (ia64_fw_pei_find_core_from_ps(env, ps_ptr, &core)) {
                    ia64_fw_pei_log_core_hob_candidates(cs, core, ps_ptr);
                }
            }
        }
        if (env->r[40]) {
            uint8_t tmp[8];
            uint64_t hob_val = 0;
            hwaddr hob_ptr_phys = ia64_phys_mode_addr(env->r[40]);
            if (cpu_memory_rw_debug(cs, hob_ptr_phys, tmp, sizeof(tmp), false) == 0) {
                hob_val = ldq_le_p(tmp);
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_pei_call pre hob_ptr=%016" PRIx64
                          " hob_val=%016" PRIx64 "\n",
                          env->r[40], hob_val);
        }
        if (env->r[39]) {
            uint64_t ps_ptr_reg = env->r[39];
            uint64_t core = ps_ptr_reg - 8;
            uint64_t hob_raw = 0;
            uint64_t hob_phys_val = 0;
            uint8_t hob_tmp[8];
            hwaddr hob_va = core + 0x260;
            hwaddr hob_phys = ia64_phys_mode_addr(hob_va);
            hwaddr hob_tpa = 0;
            bool hob_tpa_ok = ia64_try_translate(env, hob_va, &hob_tpa);
            if (cpu_memory_rw_debug(cs, hob_phys, hob_tmp, sizeof(hob_tmp), false) == 0) {
                hob_raw = ldq_le_p(hob_tmp);
            }
            if (address_space_read(&address_space_memory,
                                   hob_tpa_ok ? hob_tpa : hob_phys,
                                   MEMTXATTRS_UNSPECIFIED,
                                   hob_tmp, sizeof(hob_tmp)) == MEMTX_OK) {
                hob_phys_val = ldq_le_p(hob_tmp);
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: fw_pei_call pre ps_ptr=%016" PRIx64
                          " core=%016" PRIx64 " hob_raw=%016" PRIx64
                          " hob_va=%016" PRIx64
                          " hob_tpa=%016" PRIx64 " tpa_ok=%d"
                          " hob_phys=%016" PRIx64 " phys_val=%016" PRIx64 "\n",
                          ps_ptr_reg, core, hob_raw, hob_va,
                          hob_tpa, hob_tpa_ok ? 1 : 0,
                          (uint64_t)hob_phys, hob_phys_val);
        }
        if (env->b[7]) {
            ia64_fw_dump_code(env, "pei_b7", env->b[7], 64);
        }
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_pei_call post pc=%016" PRIx64
                  " r8=%016" PRIx64 " b0=%016" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64 "\n",
                  pc, env->r[8], env->b[0], env->r[1], env->r[12]);
    if (env->r[40]) {
        uint8_t tmp[8];
        uint64_t hob_val = 0;
        hwaddr hob_ptr_phys = ia64_phys_mode_addr(env->r[40]);
        if (cpu_memory_rw_debug(cs, hob_ptr_phys, tmp, sizeof(tmp), false) == 0) {
            hob_val = ldq_le_p(tmp);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_pei_call post hob_ptr=%016" PRIx64
                      " hob_val=%016" PRIx64 "\n",
                      env->r[40], hob_val);
    }
#endif
}

void HELPER(fw_pei_pre_install_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    if (!ia64_fw_pei_pre_install_probe_enabled()) {
        return;
    }

    static bool logged;
    if (logged) {
        return;
    }
    logged = true;

    uint64_t fw_ppi = env->fw_pei_ppi;
    uint64_t r33 = env->r[33];
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: pei_pre_install pc=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r35=%016" PRIx64 " r12=%016" PRIx64 " r13=%016" PRIx64
                  " r1=%016" PRIx64 " fw_ppi=%016" PRIx64 "\n",
                  pc, env->r[32], r33, env->r[34], env->r[35],
                  env->r[12], env->r[13], env->r[1], fw_ppi);
    if (ia64_fw_r33_watch_enabled() && env->fw_pei_r33_watch_active) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: r33_watch stop pc=%016" PRIx64
                      " last=%016" PRIx64 " r33=%016" PRIx64
                      " last_pc=%016" PRIx64 "\n",
                      pc, env->fw_pei_r33_last, env->r[33],
                      env->fw_pei_r33_last_pc);
        env->fw_pei_r33_watch_active = 0;
    }
    ia64_fw_rse_log(env, pc, "pei_pre_install");

    if (fw_ppi) {
        ia64_fw_dump_pei_ppi_list(env, fw_ppi);
    }
    if (r33 && r33 != fw_ppi) {
        ia64_fw_dump_pei_ppi_list(env, r33);
    }

    CPUState *cs = env_cpu(env);
    hwaddr sp = ia64_phys_mode_addr(env->r[12]);
    uint8_t buf[256];
    if (cpu_memory_rw_debug(cs, sp, buf, sizeof(buf), false) == 0) {
        uint64_t fw_ppi_phys = fw_ppi & ((1ULL << 61) - 1);
        for (size_t off = 0; off + 8 <= sizeof(buf); off += 8) {
            uint64_t v = ldq_le_p(&buf[off]);
            if (v == fw_ppi || (fw_ppi && (v & ((1ULL << 61) - 1)) == fw_ppi_phys)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: pei_pre_install stack_hit sp=%016" HWADDR_PRIx
                              " off=0x%zx val=%016" PRIx64 "\n",
                              sp, off, v);
            }
        }
    }
#endif
}

void HELPER(fw_pei_rse_probe)(CPUIA64State *env, uint64_t pc, uint32_t stage)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    (void)stage;
    return;
#else
    if (!ia64_fw_pei_rse_trace_enabled()) {
        return;
    }
    const char *tag = "pei_rse_probe";
    switch (stage) {
    case 0:
        tag = "pei_rse_entry";
        break;
    case 1:
        tag = "pei_rse_mid";
        break;
    case 2:
        tag = "pei_rse_post";
        break;
    default:
        break;
    }
    ia64_fw_rse_log(env, pc, tag);
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

#ifndef CONFIG_USER_ONLY
    ia64_fw_try_patch_efi_hobs(env);
    /*
     * Keep attempting to install the SAL systab entry while firmware is
     * running, even before the first SAL call. Throttle to avoid heavy scans.
     */
    static uint64_t tlb_calls;
    if (!env->fw_sal_systab_installed && ((++tlb_calls & 0xfff) == 0)) {
        ia64_fw_try_install_sal_systab(env);
    }
#endif

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
    static uint64_t abort_to = UINT64_MAX;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_ABORT_NULL_BRANCH");
        enabled = (s && *s) ? 1 : 0;
    }
    if (abort_to == UINT64_MAX) {
        abort_to = 0;
        const char *s = getenv("QEMU_IA64_ABORT_BRANCH_TO");
        if (s && *s) {
            abort_to = strtoull(s, NULL, 0) & ~0xFULL;
        }
    }
    if (!enabled) {
        if (!abort_to) {
            return;
        }
    }
    if (to == 0) {
        CPUState *cs = env_cpu(env);
        uint64_t r30 = env->r[30];
        uint64_t r31 = env->r[31];
        uint64_t b7 = env->b[7];
        uint64_t desc_va = 0;
        uint64_t desc_entry = 0;
        uint64_t desc_gp = 0;
        bool desc_ok = false;
        if (r31 >= 8) {
            desc_va = r31 - 8;
            hwaddr desc_phys = ia64_phys_mode_addr(desc_va);
            if (desc_phys) {
                MemTxResult r1 = address_space_read(&address_space_memory,
                                                    desc_phys, MEMTXATTRS_UNSPECIFIED,
                                                    (uint8_t *)&desc_entry,
                                                    sizeof(desc_entry));
                MemTxResult r2 = address_space_read(&address_space_memory,
                                                    desc_phys + 8, MEMTXATTRS_UNSPECIFIED,
                                                    (uint8_t *)&desc_gp,
                                                    sizeof(desc_gp));
                desc_ok = (r1 == MEMTX_OK && r2 == MEMTX_OK);
            }
        }
        cpu_restore_state(cs, GETPC());
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: null branch pc=%016" PRIx64 " ri=%u insn=%011" PRIx64
                      " ip=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                      " b0=%016" PRIx64 " b6=%016" PRIx64 " b7=%016" PRIx64
                      " r12=%016" PRIx64 " r14=%016" PRIx64
                      " r30=%016" PRIx64 " r31=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                      " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64
                      " desc=%016" PRIx64 " entry=%016" PRIx64 " gp=%016" PRIx64
                      " desc_ok=%d\n",
                      pc, ri, insn,
                      env->ip, env->psr, env->cfm,
                      env->b[0], env->b[6], b7,
                      env->r[12], env->r[14],
                      r30, r31,
                      env->r[32], env->r[33], env->r[34], env->r[35],
                      env->r[36], env->r[37],
                      desc_va, desc_entry, desc_gp, desc_ok ? 1 : 0);
        cpu_abort(cs,
                  "IA64: null branch target pc=%016" PRIx64 " ri=%u insn=%011" PRIx64,
                  pc, ri, insn);
    }

    if (abort_to && (to & ~0xFULL) == abort_to) {
        CPUState *cs = env_cpu(env);
        cpu_restore_state(cs, GETPC());
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: branch_to pc=%016" PRIx64 " ri=%u insn=%011" PRIx64
                      " ip=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                      " to=%016" PRIx64
                      " b0=%016" PRIx64 " b6=%016" PRIx64
                      " r1=%016" PRIx64 " r12=%016" PRIx64 " r13=%016" PRIx64
                      " r14=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                      " r35=%016" PRIx64 " r36=%016" PRIx64 " r37=%016" PRIx64 "\n",
                      pc, ri, insn,
                      env->ip, env->psr, env->cfm, to,
                      env->b[0], env->b[6],
                      env->r[1], env->r[12], env->r[13], env->r[14],
                      env->r[32], env->r[33], env->r[34], env->r[35],
                      env->r[36], env->r[37]);
        cpu_abort(cs,
                  "IA64: branch_to hit pc=%016" PRIx64 " ri=%u to=%016" PRIx64,
                  pc, ri, to);
    }
}

void HELPER(null_pc_abort)(CPUIA64State *env, uint64_t pc, uint32_t ri)
{
    CPUState *cs = env_cpu(env);
    uint64_t gp = env->r[1];
    uint64_t r37 = env->r[37];
    uint64_t gp_slot = 0;
    uint64_t gp_slot_val = 0;
    bool gp_slot_ok = false;

    if (gp) {
        gp_slot = gp - 0x1fee50ULL;
    } else if (r37) {
        gp_slot = r37 - 0x1fee50ULL;
    }
    if (gp_slot) {
        hwaddr gp_slot_phys = ia64_phys_mode_addr(gp_slot);
        if (gp_slot_phys) {
            MemTxResult r = address_space_read(&address_space_memory,
                                               gp_slot_phys,
                                               MEMTXATTRS_UNSPECIFIED,
                                               (uint8_t *)&gp_slot_val,
                                               sizeof(gp_slot_val));
            gp_slot_ok = (r == MEMTX_OK);
        }
    }
    cpu_restore_state(cs, GETPC());
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: null pc pc=%016" PRIx64 " ri=%u"
                  " ip=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                  " last_branch from=%016" PRIx64 " to=%016" PRIx64
                  " kind=%" PRIu64 " insn=%011" PRIx64
                  " b0=%016" PRIx64 " b6=%016" PRIx64 " b7=%016" PRIx64
                  " r12=%016" PRIx64 " r14=%016" PRIx64
                  " r30=%016" PRIx64 " r31=%016" PRIx64
                  " r1=%016" PRIx64 " r37=%016" PRIx64
                  " gp_slot=%016" PRIx64 " gp_slot_val=%016" PRIx64
                  " gp_slot_ok=%d\n",
                  pc, ri, env->ip, env->psr, env->cfm,
                  env->last_branch_from, env->last_branch_to,
                  env->last_branch_kind, env->last_branch_insn,
                  env->b[0], env->b[6], env->b[7],
                  env->r[12], env->r[14], env->r[30], env->r[31],
                  gp, r37, gp_slot, gp_slot_val, gp_slot_ok ? 1 : 0);
    cpu_abort(cs, "IA64: null pc pc=%016" PRIx64 " ri=%u", pc, ri);
}

void HELPER(dbg_mem_watch)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                           uint64_t addr, uint32_t size, uint64_t val)
{
    static int log_limit = -1;
    static int log_count;
    static int src_dump_count;

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

    if (pc == 0xffe226d0ULL || pc == 0xffe22830ULL) {
        uint64_t sp = env->r[12];
        hwaddr sp_pa = (env->psr & IA64_PSR_DT) ?
            helper_tpa(env, sp) : ia64_phys_mode_addr(sp);
        hwaddr addr_pa = (env->psr & IA64_PSR_DT) ?
            helper_tpa(env, addr) : ia64_phys_mode_addr(addr);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "store_watch_hob pc=%016" PRIx64
                      " addr_pa=%016" HWADDR_PRIx
                      " r1=%016" PRIx64 " r12=%016" PRIx64
                      " r30=%016" PRIx64 " r31=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64
                      " r34=%016" PRIx64 " r35=%016" PRIx64
                      " ar.k3=%016" PRIx64 " ar.k4=%016" PRIx64
                      " ar.k5=%016" PRIx64
                      " ar.bsp=%016" PRIx64 " ar.bspstore=%016" PRIx64
                      " b0=%016" PRIx64 " b7=%016" PRIx64 "\n",
                      pc, addr_pa, env->r[1], env->r[12],
                      env->r[30], env->r[31], env->r[32], env->r[33],
                      env->r[34], env->r[35],
                      env->ar[3], env->ar[4], env->ar[5],
                      env->ar[IA64_AR_BSP], env->ar[IA64_AR_BSPSTORE],
                      env->b[0], env->b[7]);
        if (sp_pa) {
            uint8_t mem[32];
            if (cpu_memory_rw_debug(env_cpu(env), sp_pa, mem, sizeof(mem), false) == 0) {
                char line[128];
                int pos = 0;
                pos += snprintf(line + pos, sizeof(line) - pos,
                                "  sp  %016" HWADDR_PRIx ":", sp_pa);
                for (size_t i = 0; i < sizeof(mem); i++) {
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    " %02x", mem[i]);
                }
                qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "  sp  %016" HWADDR_PRIx ": <read failed>\n",
                              sp_pa);
            }
        }
    }

    if (pc == 0xffe2cc20ULL &&
        addr >= 0x1f020000ULL && addr < 0x1f030000ULL &&
        src_dump_count++ < 4) {
        uint64_t src = env->r[31];
        hwaddr src_pa = (env->psr & IA64_PSR_DT) ?
            helper_tpa(env, src) : ia64_phys_mode_addr(src);
        uint64_t sp = env->r[12];
        hwaddr sp_pa = (env->psr & IA64_PSR_DT) ?
            helper_tpa(env, sp) : ia64_phys_mode_addr(sp);
        uint8_t buf[32];
        uint64_t sp_slot1 = 0;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "store_watch_src pc=%016" PRIx64
                      " r30=%016" PRIx64 " r31=%016" PRIx64
                      " r12=%016" PRIx64
                      " src_pa=%016" HWADDR_PRIx " sp_pa=%016" HWADDR_PRIx
                      " b0=%016" PRIx64 " b7=%016" PRIx64
                      " last_b0_pc=%016" PRIx64 " last_b0_val=%016" PRIx64
                      " last_br_from=%016" PRIx64 " last_br_to=%016" PRIx64
                      " r32=%016" PRIx64 " r33=%016" PRIx64
                      " r34=%016" PRIx64 " r35=%016" PRIx64 "\n",
                      pc, env->r[30], src, env->r[12],
                      src_pa, sp_pa,
                      env->b[0], env->b[7],
                      env->last_b0_write_pc, env->last_b0_write_val,
                      env->last_branch_from, env->last_branch_to,
                      env->r[32], env->r[33], env->r[34], env->r[35]);
        if (cpu_memory_rw_debug(env_cpu(env), src_pa, buf, sizeof(buf), false) == 0) {
            char line[128];
            int pos = 0;
            pos += snprintf(line + pos, sizeof(line) - pos,
                            "  src %016" HWADDR_PRIx ":", src_pa);
            for (size_t i = 0; i < sizeof(buf); i++) {
                pos += snprintf(line + pos, sizeof(line) - pos,
                                " %02x", buf[i]);
            }
            qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "  src %016" HWADDR_PRIx ": <read failed>\n",
                          src_pa);
        }
        if (cpu_memory_rw_debug(env_cpu(env), sp_pa, buf, sizeof(buf), false) == 0) {
            char line[128];
            int pos = 0;
            pos += snprintf(line + pos, sizeof(line) - pos,
                            "  sp  %016" HWADDR_PRIx ":", sp_pa);
            for (size_t i = 0; i < sizeof(buf); i++) {
                pos += snprintf(line + pos, sizeof(line) - pos,
                                " %02x", buf[i]);
            }
            qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
            sp_slot1 = ldq_le_p(&buf[8]);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "  sp  %016" HWADDR_PRIx ": <read failed>\n",
                          sp_pa);
        }
        if (sp_slot1) {
            hwaddr sp_slot1_pa = (env->psr & IA64_PSR_DT) ?
                helper_tpa(env, sp_slot1) : ia64_phys_mode_addr(sp_slot1);
            if (cpu_memory_rw_debug(env_cpu(env), sp_slot1_pa, buf, sizeof(buf),
                                    false) == 0) {
                char line[128];
                int pos = 0;
                pos += snprintf(line + pos, sizeof(line) - pos,
                                "  sp+8 %016" HWADDR_PRIx ":", sp_slot1_pa);
                for (size_t i = 0; i < sizeof(buf); i++) {
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    " %02x", buf[i]);
                }
                qemu_log_mask(LOG_GUEST_ERROR, "%s\n", line);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "  sp+8 %016" HWADDR_PRIx ": <read failed>\n",
                              sp_slot1_pa);
            }
        }
    }
}

void HELPER(dbg_load_watch)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                            uint64_t addr, uint32_t size, uint64_t val)
{
    static int log_limit = -1;
    static int log_count;

    if (log_limit == -1) {
        log_limit = 64;
        const char *s = getenv("QEMU_IA64_WATCH_LOAD_LIMIT");
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
                  "load_watch pc=%016" PRIx64 " ri=%u"
                  " ip=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                  " addr=%016" PRIx64 " size=%u val=%016" PRIx64 "\n",
                  pc, ri, env->ip, env->psr, env->cfm, addr, size, val);
}

void HELPER(dbg_r33_watch)(CPUIA64State *env, uint64_t pc, uint32_t ri)
{
    if (!env->fw_pei_r33_watch_active) {
        return;
    }
    uint64_t cur = env->r[33];
    if (cur == env->fw_pei_r33_last) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: r33_watch pc=%016" PRIx64 " ri=%u"
                  " last=%016" PRIx64 " new=%016" PRIx64
                  " last_pc=%016" PRIx64 "\n",
                  pc, ri, env->fw_pei_r33_last, cur, env->fw_pei_r33_last_pc);
    env->fw_pei_r33_last = cur;
    env->fw_pei_r33_last_pc = pc;
}

void HELPER(hang_abort)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                        uint64_t threshold)
{
    static int log_every_inited;
    static uint64_t log_every;

    if (!log_every_inited) {
        log_every_inited = 1;
        const char *s = getenv("QEMU_IA64_HANG_LOG_EVERY");
        if (s && *s) {
            log_every = (uint64_t)strtoull(s, NULL, 0);
        }
    }

    if (threshold == 0) {
        return;
    }

    env->dbg_tb_total++;

    if (log_every && (env->dbg_tb_total % log_every) == 0 &&
        qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64 hang_heartbeat total=%" PRIu64 " tbpc=%016" PRIx64
                      " ri=%u ip=%016" PRIx64 " psr=%016" PRIx64 " cfm=%016" PRIx64
                      " last_branch from=%016" PRIx64 " to=%016" PRIx64
                      " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                      env->dbg_tb_total, pc, ri, env->ip, env->psr, env->cfm,
                      env->last_branch_from, env->last_branch_to,
                      env->last_branch_kind, env->last_branch_insn);
    }

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

    {
        static int dump_enabled = -1;
        static bool dumped;
        if (dump_enabled == -1) {
            const char *s = getenv("QEMU_IA64_HANG_DUMP_GCD");
            dump_enabled = (s && *s) ? 1 : 0;
        }
        if (dump_enabled && !dumped) {
            dumped = true;
            ia64_fw_dump_hobs_and_gcd(env);
        }
    }

    ia64_fw_trace_dump();
    ia64_fw_dump_code(env, "hang_pc", pc, 64);
    ia64_fw_dump_code(env, "hang_from", env->last_branch_from, 64);
    ia64_fw_dump_code(env, "hang_to", env->last_branch_to, 64);
    ia64_fw_dump_code(env, "hang_b7", env->b[7], 64);
    {
        static int extra_dump_inited;
        static uint64_t extra_dump_pc;
        if (!extra_dump_inited) {
            extra_dump_inited = 1;
            const char *s = getenv("QEMU_IA64_HANG_DUMP_PC");
            if (s && *s) {
                char *endp = NULL;
                extra_dump_pc = strtoull(s, &endp, 0);
                if (endp == s) {
                    extra_dump_pc = 0;
                }
            }
        }
        if (extra_dump_pc) {
            ia64_fw_dump_code(env, "hang_extra", extra_dump_pc, 64);
        }
    }

    if (env->fw_hob_reloc_base &&
        pc >= 0x0000000000011c20ULL &&
        pc <= 0x0000000000011d10ULL) {
        hwaddr sp = ia64_phys_mode_addr(env->r[12]);
        uint8_t val[8];
        if (cpu_memory_rw_debug(cs, sp, val, sizeof(val), false) == 0) {
            uint64_t sp_raw = ldq_le_p(val);
            uint64_t sp_tmpl = sp_raw ? sp_raw : env->fw_phit_mem_bottom;
            uint64_t sp_enc = ia64_fw_encode_addr(sp_tmpl, env->fw_hob_reloc_base);
            if (sp_enc != sp_raw) {
                stq_le_p(val, sp_enc);
                cpu_physical_memory_write(sp, val, sizeof(val));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: hang_abort: patched hob_ptr @%016" PRIx64
                              " %016" PRIx64 " -> %016" PRIx64 "\n",
                              (uint64_t)sp, sp_raw, sp_enc);
                env->dbg_tb_same1 = 0;
                env->dbg_tb_same2 = 0;
                return;
            }
        }
    }

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
                  " r8=%016" PRIx64 " r9=%016" PRIx64
                  " r10=%016" PRIx64 " r11=%016" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64 " r13=%016" PRIx64
                  " r24=%016" PRIx64 " r27=%016" PRIx64
                  " r28=%016" PRIx64 " r29=%016" PRIx64
                  " r30=%016" PRIx64 " r31=%016" PRIx64
                  " r35=%016" PRIx64 " r36=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                  " r37=%016" PRIx64 " r38=%016" PRIx64 " r39=%016" PRIx64
                  " r52=%016" PRIx64 " r53=%016" PRIx64
                  " kbias=%016" PRIx64
                  " con_waiter_va=%016" PRIx64 " con_waiter=%02x"
                  " con_owner_va=%016" PRIx64 " con_owner=%016" PRIx64
                  " con_ok=%u"
                  " b0=%016" PRIx64 " b6=%016" PRIx64 " b7=%016" PRIx64 "\n",
                  threshold, env->dbg_tb_total, pc, ri,
                  env->dbg_tb_same1, env->dbg_tb_same2,
                  env->ip, env->psr, env->cfm, env->pr,
                  env->ar[65], env->ar[66],
                  env->last_branch_from, env->last_branch_to,
                  env->last_branch_kind, env->last_branch_insn,
                  env->r[8], env->r[9], env->r[10], env->r[11],
                  env->r[1], env->r[12], env->r[13],
                  env->r[24], env->r[27], env->r[28], env->r[29],
                  env->r[30], env->r[31], env->r[35], env->r[36],
                  env->r[32], env->r[33], env->r[34],
                  env->r[37], env->r[38], env->r[39],
                  env->r[52], env->r[53],
                  env->kernel_bias,
                  con_waiter_va, (unsigned)con_waiter,
                  con_owner_va, con_owner,
                  con_ok,
                  env->b[0], env->b[6], env->b[7]);
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
        enabled = ia64_env_truthy(s) ? 1 : 0;
    }
    return enabled;
}

static bool ia64_env_truthy(const char *s)
{
    if (!s || !*s) {
        return false;
    }
    if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "false") ||
        !strcmp(s, "no")) {
        return false;
    }
    return true;
}

static bool ia64_fw_r8_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_R8_TRACE");
        const char *c = getenv("QEMU_IA64_FW_CALL_TRACE");
        enabled = ia64_env_truthy(s) || ia64_env_truthy(c);
    }
    return enabled;
}

static bool ia64_fw_r8_target_enabled(uint64_t *target)
{
    static int enabled = -1;
    static uint64_t value;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_R8_TARGET");
        if (!s || !*s) {
            enabled = 0;
        } else {
            char *endp = NULL;
            uint64_t parsed = strtoull(s, &endp, 0);
            if (endp && endp != s) {
                enabled = 1;
                value = parsed;
            } else {
                enabled = 0;
            }
        }
    }
    if (enabled > 0 && target) {
        *target = value;
    }
    return enabled > 0;
}

static bool ia64_fw_fvb_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_FVB_TRACE");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

#define IA64_FW_CALL_TRACE_MAX 32

static uint64_t ia64_fw_call_trace_pcs[IA64_FW_CALL_TRACE_MAX];
static uint32_t ia64_fw_call_trace_count;
static int ia64_fw_call_trace_inited;

static bool ia64_fw_call_trace_match(uint64_t pc)
{
    if (!ia64_fw_call_trace_inited) {
        ia64_fw_call_trace_inited = 1;
        const char *s = getenv("QEMU_IA64_FW_CALL_TRACE");
        if (ia64_env_truthy(s)) {
            while (*s && ia64_fw_call_trace_count <
                         ARRAY_SIZE(ia64_fw_call_trace_pcs)) {
                while (*s && (isspace((unsigned char)*s) || *s == ',')) {
                    s++;
                }
                if (!*s) {
                    break;
                }
                char *endp = NULL;
                uint64_t raw_pc = strtoull(s, &endp, 0);
                if (!endp || endp == s) {
                    break;
                }
                ia64_fw_call_trace_pcs[ia64_fw_call_trace_count++] =
                    raw_pc & ~0xFULL;
                s = endp;
            }
        }
    }
    for (uint32_t i = 0; i < ia64_fw_call_trace_count; i++) {
        if (ia64_fw_call_trace_pcs[i] == pc) {
            return true;
        }
    }
    return false;
}

static void ia64_fw_call_trace_step(CPUIA64State *env, uint64_t pc)
{
    static int limit = -1;
    static uint32_t count;
    if (limit == -1) {
        limit = 256;
        const char *s = getenv("QEMU_IA64_FW_CALL_TRACE_LIMIT");
        if (s && *s) {
            limit = atoi(s);
        }
        if (limit < 0) {
            limit = 0;
        }
    }

    if (env->dbg_fw_call_depth) {
        for (int i = (int)env->dbg_fw_call_depth - 1; i >= 0; i--) {
            if (env->dbg_fw_call_ret_pc[i] == pc) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: fw_call ret pc=%016" PRIx64
                              " entry=%016" PRIx64
                              " r8=%016" PRIx64 "\n",
                              pc, env->dbg_fw_call_entry_pc[i], env->r[8]);
                env->dbg_fw_call_depth = (uint32_t)i;
                break;
            }
        }
    }

    if (!ia64_fw_call_trace_match(pc)) {
        return;
    }
    if (count++ >= (uint32_t)limit) {
        return;
    }

    if (env->dbg_fw_call_depth >=
        ARRAY_SIZE(env->dbg_fw_call_ret_pc)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_call overflow pc=%016" PRIx64 "\n", pc);
        return;
    }

    env->dbg_fw_call_ret_pc[env->dbg_fw_call_depth] = env->b[0];
    env->dbg_fw_call_entry_pc[env->dbg_fw_call_depth] = pc;
    env->dbg_fw_call_depth++;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_call enter pc=%016" PRIx64
                  " ret=%016" PRIx64
                  " r9=%016" PRIx64 " r10=%016" PRIx64
                  " r20=%016" PRIx64 " r21=%016" PRIx64
                  " r22=%016" PRIx64 " r23=%016" PRIx64
                  " r28=%016" PRIx64 " r29=%016" PRIx64
                  " r30=%016" PRIx64 " r31=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64
                  " r34=%016" PRIx64 " r35=%016" PRIx64
                  " r36=%016" PRIx64 " r37=%016" PRIx64
                  " r38=%016" PRIx64 "\n",
                  pc, env->b[0],
                  env->r[9], env->r[10],
                  env->r[20], env->r[21], env->r[22], env->r[23],
                  env->r[28], env->r[29], env->r[30], env->r[31],
                  env->r[32], env->r[33], env->r[34], env->r[35],
                  env->r[36], env->r[37], env->r[38]);

    uint8_t sol = (env->cfm >> 7) & 0x7f;
    uint8_t out0 = 32 + sol;
    uint64_t a0 = (out0 < 128) ? env->r[out0] : 0;
    uint64_t a1 = (out0 + 1 < 128) ? env->r[out0 + 1] : 0;
    uint64_t a2 = (out0 + 2 < 128) ? env->r[out0 + 2] : 0;
    uint64_t a3 = (out0 + 3 < 128) ? env->r[out0 + 3] : 0;
    uint64_t a4 = (out0 + 4 < 128) ? env->r[out0 + 4] : 0;
    uint64_t a5 = (out0 + 5 < 128) ? env->r[out0 + 5] : 0;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_call args out0=r%u"
                  " a0=%016" PRIx64 " a1=%016" PRIx64
                  " a2=%016" PRIx64 " a3=%016" PRIx64
                  " a4=%016" PRIx64 " a5=%016" PRIx64 "\n",
                  out0, a0, a1, a2, a3, a4, a5);
}

#define IA64_FW_TRACE_RING_SIZE 64

typedef enum IA64FwTraceKind {
    IA64_FW_TRACE_SAL,
    IA64_FW_TRACE_PAL,
} IA64FwTraceKind;

typedef struct IA64FwTraceEntry {
    IA64FwTraceKind kind;
    uint64_t pc;
    uint64_t func_raw;
    uint64_t func;
    uint64_t args[4];
    int64_t status;
    uint64_t v0;
    uint64_t v1;
    uint64_t v2;
    bool break_abi;
} IA64FwTraceEntry;

static IA64FwTraceEntry ia64_fw_trace_ring[IA64_FW_TRACE_RING_SIZE];
static uint32_t ia64_fw_trace_pos;
static uint32_t ia64_fw_trace_count;

static void ia64_fw_trace_record(IA64FwTraceKind kind, uint64_t pc,
                                 uint64_t func_raw, uint64_t func,
                                 const uint64_t *args, int64_t status,
                                 uint64_t v0, uint64_t v1, uint64_t v2,
                                 bool break_abi)
{
    if (!ia64_fw_log_enabled()) {
        return;
    }
    IA64FwTraceEntry *e = &ia64_fw_trace_ring[ia64_fw_trace_pos];
    *e = (IA64FwTraceEntry){
        .kind = kind,
        .pc = pc,
        .func_raw = func_raw,
        .func = func,
        .status = status,
        .v0 = v0,
        .v1 = v1,
        .v2 = v2,
        .break_abi = break_abi,
    };
    for (int i = 0; i < 4; i++) {
        e->args[i] = args ? args[i] : 0;
    }
    ia64_fw_trace_pos = (ia64_fw_trace_pos + 1) % IA64_FW_TRACE_RING_SIZE;
    if (ia64_fw_trace_count < IA64_FW_TRACE_RING_SIZE) {
        ia64_fw_trace_count++;
    }
}

static void ia64_fw_trace_dump(void)
{
    if (!ia64_fw_log_enabled() || ia64_fw_trace_count == 0) {
        return;
    }
    uint32_t start = (ia64_fw_trace_pos + IA64_FW_TRACE_RING_SIZE -
                      ia64_fw_trace_count) % IA64_FW_TRACE_RING_SIZE;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_trace last=%u\n", ia64_fw_trace_count);
    for (uint32_t i = 0; i < ia64_fw_trace_count; i++) {
        const IA64FwTraceEntry *e =
            &ia64_fw_trace_ring[(start + i) % IA64_FW_TRACE_RING_SIZE];
        const char *kind = (e->kind == IA64_FW_TRACE_PAL) ? "PAL" : "SAL";
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_trace[%u] %s pc=%016" PRIx64
                      " func=%016" PRIx64 " raw=%016" PRIx64
                      " a0=%016" PRIx64 " a1=%016" PRIx64
                      " a2=%016" PRIx64 " a3=%016" PRIx64
                      " status=%" PRId64 " v0=%016" PRIx64
                      " v1=%016" PRIx64 " v2=%016" PRIx64
                      " break_abi=%d\n",
                      i, kind, e->pc, e->func, e->func_raw,
                      e->args[0], e->args[1], e->args[2], e->args[3],
                      e->status, e->v0, e->v1, e->v2,
                      e->break_abi ? 1 : 0);
    }
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
#define IA64_PAL_BUS_GET_FEATURES 9
#define IA64_PAL_DEBUG_INFO      11
#define IA64_PAL_FIXED_ADDR      12
#define IA64_PAL_FREQ_BASE       13
#define IA64_PAL_FREQ_RATIOS     14
#define IA64_PAL_PERF_MON_INFO   15
#define IA64_PAL_PLATFORM_ADDR   16
#define IA64_PAL_PROC_GET_FEATURES 17
#define IA64_PAL_VERSION         20
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
    case IA64_PAL_BUS_GET_FEATURES:
        /* Return all-zero feature sets (matches SKI). */
        v0 = 0;
        v1 = 0;
        v2 = 0;
        break;
    case IA64_PAL_DEBUG_INFO:
        /* SKI returns 4/4 for PAL_DEBUG_INFO. */
        v0 = 4;
        v1 = 4;
        break;
    case IA64_PAL_FIXED_ADDR:
        /* Unique address on the processor bus (not modeled). */
        v0 = 0;
        break;
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
    case IA64_PAL_PLATFORM_ADDR: {
        /*
         * Set platform addresses (interrupt block or I/O port space).
         *
         * PAL spec: a1 = type, a2 = physical address.
         */
        uint64_t type = a1;
        uint64_t phys = ia64_phys_mode_addr(a2);
        if (type < ARRAY_SIZE(env->pal_platform_addr)) {
            env->pal_platform_addr[type] = phys;
            status = IA64_PAL_STATUS_SUCCESS;
        } else {
            status = IA64_PAL_STATUS_UNIMPLEMENTED;
        }
        break;
    }
    case IA64_PAL_PERF_MON_INFO:
        v0 = 4;
        break;
    case IA64_PAL_PROC_GET_FEATURES:
        v0 = 0;
        v1 = 0;
        v2 = 0;
        break;
    case IA64_PAL_VERSION:
        v0 = 0;
        v1 = 0;
        break;
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

    uint64_t args_trace[4] = { a1, a2, a3, 0 };
    ia64_fw_trace_record(IA64_FW_TRACE_PAL, env->ip, idx, idx,
                         args_trace, status, v0, v1, v2, from_call);

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

/* Firmware volume access via ESAL. */

#define IA64_ESAL_FVB_READ                 0
#define IA64_ESAL_FVB_WRITE                1
#define IA64_ESAL_FVB_ERASE_BLOCK          2
#define IA64_ESAL_FVB_GET_VOLUME_ATTRS     3
#define IA64_ESAL_FVB_SET_VOLUME_ATTRS     4
#define IA64_ESAL_FVB_GET_PHYS_ADDR        5
#define IA64_ESAL_FVB_GET_BLOCK_SIZE       6
#define IA64_ESAL_FVB_ERASE_CUSTOM_RANGE   7

static uint8_t ia64_fw_out_base(CPUIA64State *env)
{
    uint8_t sol = (env->cfm >> 7) & 0x7f;
    uint8_t out0 = 32 + sol;
    if (out0 >= 128) {
        out0 = 32;
    }
    return out0;
}

static uint64_t ia64_fw_arg(CPUIA64State *env, uint8_t out0, uint8_t idx)
{
    uint8_t reg = out0 + idx;
    return (reg < 128) ? env->r[reg] : 0;
}

/* Xenipf firmware SAL break ABI passes arguments in static r28..r31. */
static uint64_t ia64_fw_arg_break(CPUIA64State *env, uint8_t idx)
{
    uint8_t reg = 28 + idx;
    return (reg < 128) ? env->r[reg] : 0;
}

static bool ia64_fw_fvb_translate(uint64_t instance, uint64_t lba,
                                  uint64_t offset, uint64_t *out_pa,
                                  uint64_t *out_avail)
{
    uint64_t block_size = IA64_IPF_FW_FLASH_BLOCK_SIZE;
    uint64_t blocks = ia64_fw_flash_blocks();
    uint64_t flash_size = ia64_fw_flash_size();
    uint64_t flash_base = ia64_fw_flash_base();

    if (lba >= blocks) {
        return false;
    }

    uint64_t block_off = lba * block_size;
    if (block_off + offset >= flash_size) {
        return false;
    }

    if (instance != 0 && ia64_fw_log_enabled()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: ESAL_FVB instance=%" PRIu64
                      " using base=%016" PRIx64 "\n",
                      instance, flash_base);
    }

    *out_pa = flash_base + block_off + offset;
    *out_avail = flash_size - (block_off + offset);
    return true;
}

static void ia64_fw_r8_log_pei_hob(CPUIA64State *env, uint64_t pc,
                                   uint64_t peis_ptr)
{
#ifndef CONFIG_USER_ONLY
    CPUState *cs = env_cpu(env);
    uint64_t ps_ptr = 0;
    uint64_t core = 0;
    if (!peis_ptr) {
        return;
    }
    if (!ia64_fw_pei_get_ps_ptr(env, peis_ptr, &ps_ptr)) {
        return;
    }
    if (!ia64_fw_pei_find_core_from_ps(env, ps_ptr, &core)) {
        return;
    }

    uint64_t hob_field_raw = 0;
    uint64_t hob_field_phys = 0;
    if (ia64_fw_read_u64(cs, core + 0x470, &hob_field_raw)) {
        hob_field_phys = ia64_phys_mode_addr(hob_field_raw);
    }

    uint64_t hob_end = 0;
    uint64_t hob_raw = 0;
    uint64_t hob_phys = 0;
    uint64_t hob_off = 0;
    int hob_count = 0;
    for (uint64_t off = 0; off < 0x2000; off += 8) {
        uint64_t cand = 0;
        if (!ia64_fw_read_u64(cs, core + off, &cand) || !cand) {
            continue;
        }
        uint64_t phys = ia64_phys_mode_addr(cand);
        if (ia64_fw_validate_efi_hob_list(cs, phys, &hob_end, &hob_count)) {
            hob_raw = cand;
            hob_phys = phys;
            hob_off = off;
            break;
        }
    }

    if (!hob_raw) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_r8_pei_hob pc=%016" PRIx64
                      " peis_ptr=%016" PRIx64 " ps_ptr=%016" PRIx64
                      " core=%016" PRIx64 " hob=not_found"
                      " hob_field=%016" PRIx64 " hob_field_phys=%016" PRIx64 "\n",
                      pc, peis_ptr, ps_ptr, core, hob_field_raw, hob_field_phys);
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_r8_pei_hob pc=%016" PRIx64
                  " peis_ptr=%016" PRIx64 " ps_ptr=%016" PRIx64
                  " core=%016" PRIx64 " hob_raw=%016" PRIx64
                  " hob_phys=%016" PRIx64 " hob_end=%016" PRIx64
                  " hob_off=0x%" PRIx64 " hob_cnt=%d"
                  " hob_field=%016" PRIx64 " hob_field_phys=%016" PRIx64 "\n",
                  pc, peis_ptr, ps_ptr, core, hob_raw, hob_phys, hob_end,
                  hob_off, hob_count, hob_field_raw, hob_field_phys);
#endif
}

static void ia64_fw_dump_code(CPUIA64State *env, const char *tag,
                              uint64_t pc, int bundles)
{
    if (!pc || bundles <= 0) {
        return;
    }

    CPUState *cs = env_cpu(env);
    uint64_t base = pc & ~0xFULL;
    uint64_t start = base;
    if (bundles > 16) {
        uint64_t back = (uint64_t)(bundles / 4) * 16ULL;
        start = (start >= back) ? (start - back) : 0;
    }

    g_mkdir_with_parents("scratch/ia64_logs", 0755);
    char path[256];
    snprintf(path, sizeof(path),
             "scratch/ia64_logs/sal_pci_%s_%016" PRIx64 ".bin",
             tag ? tag : "caller", base);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return;
    }

    for (int i = 0; i < bundles; i++) {
        uint8_t bundle[16];
        uint64_t bpc = start + (uint64_t)i * 16;
        if (cpu_memory_rw_debug(cs, bpc, bundle, sizeof(bundle), false) != 0) {
            break;
        }
        fwrite(bundle, 1, sizeof(bundle), fp);
    }
    fclose(fp);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: SAL_PCI dump pc=%016" PRIx64 " start=%016" PRIx64
                  " bundles=%d file=%s\n",
                  base, start, bundles, path);
}

void HELPER(fw_dump_pc)(CPUIA64State *env, uint64_t pc, uint32_t bundles)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    ia64_fw_dump_code(env, "pc", pc, bundles ? (int)bundles : 64);

    CPUState *cs = env_cpu(env);
    uint64_t r12 = env->r[12];
    uint64_t ptr_raw = 0;
    uint8_t tmp[8];
    hwaddr r12_phys = ia64_phys_mode_addr(r12);
    if (cpu_memory_rw_debug(cs, r12_phys, tmp, sizeof(tmp), false) == 0) {
        ptr_raw = ldq_le_p(tmp);
    }
    hwaddr ptr_phys = ia64_phys_mode_addr(ptr_raw);
    uint16_t entries[16];
    bool entries_ok = false;
    if (ptr_raw &&
        cpu_memory_rw_debug(cs, ptr_phys, entries, sizeof(entries), false) == 0) {
        entries_ok = true;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_dump_pc pc=%016" PRIx64 " r12=%016" PRIx64
                  " ptr=%016" PRIx64 " ptr_phys=%016" PRIx64
                  " entries_ok=%d\n",
                  pc, r12, ptr_raw, (uint64_t)ptr_phys, entries_ok ? 1 : 0);
    if (entries_ok) {
        char line[128];
        char *p = line;
        size_t left = sizeof(line);
        for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
            uint16_t val = lduw_le_p((uint8_t *)&entries[i]);
            int n = snprintf(p, left, "%s%04x",
                             i ? " " : "", val);
            if (n < 0 || (size_t)n >= left) {
                break;
            }
            p += n;
            left -= (size_t)n;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_dump_pc table16=%s\n", line);
    }
}

void HELPER(fw_sal_call_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    static int abort_enabled = -1;
    static uint64_t abort_pc;
    static bool abort_pc_set;
    if (abort_enabled == -1) {
        const char *s = getenv("QEMU_IA64_SAL_CALL_ABORT");
        abort_enabled = (s && *s) ? 1 : 0;
        const char *p = getenv("QEMU_IA64_SAL_CALL_ABORT_PC");
        if (p && *p) {
            char *endp = NULL;
            abort_pc = strtoull(p, &endp, 0);
            if (endp && endp != p) {
                abort_pc_set = true;
            }
        }
    }

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: sal_call_probe pc=%016" PRIx64
                      " r30=%016" PRIx64 " b7=%016" PRIx64
                      " r31=%016" PRIx64 " r12=%016" PRIx64
                      " r1=%016" PRIx64 " r37=%016" PRIx64 " r38=%016" PRIx64
                      " from=%016" PRIx64 " to=%016" PRIx64
                      " rse_depth=%u"
                      " psr=%016" PRIx64 " pr=%016" PRIx64 "\n",
                      pc, env->r[30], env->b[7],
                      env->r[31], env->r[12],
                      env->r[1], env->r[37], env->r[38],
                      env->last_branch_from, env->last_branch_to,
                      env->rse_depth,
                      env->psr, env->pr);
    }

    ia64_fw_dump_code(env, "sal_call_pc", pc, 64);
    ia64_fw_dump_code(env, "sal_call_b7", env->b[7], 64);

    {
        static int dump_enabled = -1;
        if (dump_enabled == -1) {
            const char *s = getenv("QEMU_IA64_SAL_CALL_DUMP");
            dump_enabled = (s && *s) ? 1 : 0;
        }
        if (dump_enabled) {
            ia64_dbg_probe_dump_mem(env, pc, "sal_r31", env->r[31], 64);
            ia64_dbg_probe_dump_mem(env, pc, "sal_r30", env->r[30], 64);
            ia64_dbg_probe_dump_mem(env, pc, "sal_r12", env->r[12], 128);
            ia64_dbg_probe_dump_mem(env, pc, "sal_r1", env->r[1], 64);
            ia64_fw_dump_code(env, "sal_call_from",
                              env->last_branch_from, 64);
        }
    }

    if (abort_enabled && (!abort_pc_set || pc == abort_pc)) {
        CPUState *cs = env_cpu(env);
        cpu_abort(cs, "IA64: sal_call_probe pc=%016" PRIx64 " b7=%016" PRIx64,
                  pc, env->b[7]);
    }
#endif
}

void HELPER(fw_sal_ret_probe)(CPUIA64State *env, uint64_t pc)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)pc;
    return;
#else
    static int abort_enabled = -1;
    if (abort_enabled == -1) {
        const char *s = getenv("QEMU_IA64_SAL_RET_ABORT");
        abort_enabled = (s && *s) ? 1 : 0;
    }

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: sal_ret_probe pc=%016" PRIx64
                      " b0=%016" PRIx64 " b7=%016" PRIx64
                      " ar.pfs=%016" PRIx64 " ar.rsc=%016" PRIx64
                      " r12=%016" PRIx64 " r1=%016" PRIx64
                      " psr=%016" PRIx64 " pr=%016" PRIx64 "\n",
                      pc, env->b[0], env->b[7],
                      env->ar[IA64_AR_PFS], env->ar[IA64_AR_RSC],
                      env->r[12], env->r[1],
                      env->psr, env->pr);
    }

    ia64_fw_dump_code(env, "sal_ret_pc", pc, 64);
    ia64_fw_dump_code(env, "sal_ret_b0", env->b[0], 64);

    if (abort_enabled) {
        CPUState *cs = env_cpu(env);
        cpu_abort(cs, "IA64: sal_ret_probe pc=%016" PRIx64 " b0=%016" PRIx64,
                  pc, env->b[0]);
    }
#endif
}

void HELPER(fw_r8_watch)(CPUIA64State *env, uint64_t pc, uint32_t ri,
                         uint64_t insn)
{
    if (!ia64_fw_r8_trace_enabled()) {
        return;
    }
    if (ia64_fw_call_trace_match(pc)) {
        ia64_fw_call_trace_step(env, pc);
    } else if (env->dbg_fw_call_depth) {
        ia64_fw_call_trace_step(env, pc);
    }
    uint64_t r8 = env->r[8];
    if (r8 == env->dbg_fw_r8_last) {
        return;
    }
    env->dbg_fw_r8_last = r8;
    uint64_t target = 0;
    bool target_enabled = ia64_fw_r8_target_enabled(&target);
    if (target_enabled) {
        if (r8 != target) {
            return;
        }
        if (env->dbg_fw_r8_logged) {
            return;
        }
    } else if ((r8 & (1ULL << 63)) == 0) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_r8_error pc=%016" PRIx64 " ri=%u insn=%011" PRIx64
                  " r8=%016" PRIx64 "\n",
                  pc, ri, insn, r8);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_r8_ctx lb_from=%016" PRIx64 " lb_to=%016" PRIx64
                  " lb_kind=%" PRIu64 " b0=%016" PRIx64 " b7=%016" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64
                  " r28=%016" PRIx64 " r29=%016" PRIx64
                  " r30=%016" PRIx64 " r31=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64
                  " r34=%016" PRIx64 " r35=%016" PRIx64
                  " r39=%016" PRIx64 " r40=%016" PRIx64
                  " r41=%016" PRIx64 " r42=%016" PRIx64
                  " r43=%016" PRIx64 " r44=%016" PRIx64 "\n",
                  env->last_branch_from, env->last_branch_to,
                  env->last_branch_kind,
                  env->b[0], env->b[7],
                  env->r[1], env->r[12],
                  env->r[28], env->r[29], env->r[30], env->r[31],
                  env->r[32], env->r[33], env->r[34], env->r[35],
                  env->r[39], env->r[40], env->r[41], env->r[42],
                  env->r[43], env->r[44]);
    uint8_t out0 = ia64_fw_out_base(env);
    uint64_t out_args[4] = {
        ia64_fw_arg(env, out0, 0),
        ia64_fw_arg(env, out0, 1),
        ia64_fw_arg(env, out0, 2),
        ia64_fw_arg(env, out0, 3),
    };
    {
        uint8_t sol = (env->cfm >> 7) & 0x7f;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: fw_r8_args cfm=%016" PRIx64 " sol=%u out0=r%u"
                      " out0..3=%016" PRIx64 " %016" PRIx64
                      " %016" PRIx64 " %016" PRIx64 "\n",
                      env->cfm, sol, out0,
                      out_args[0], out_args[1], out_args[2], out_args[3]);
    }
    ia64_fw_r8_log_pei_hob(env, pc, out_args[0]);
    {
        static int dump_enabled = -1;
        static int dump_bundles = 32;
        if (dump_enabled == -1) {
            const char *s = getenv("QEMU_IA64_FW_R8_DUMP");
            dump_enabled = (s && *s) ? 1 : 0;
            const char *b = getenv("QEMU_IA64_FW_R8_DUMP_BUNDLES");
            if (b && *b) {
                dump_bundles = atoi(b);
                if (dump_bundles <= 0) {
                    dump_bundles = 32;
                }
            }
        }
        if (dump_enabled) {
            ia64_fw_dump_code(env, "r8_err_pc", pc, dump_bundles);
            if (env->b[7]) {
                ia64_fw_dump_code(env, "r8_err_b7", env->b[7], dump_bundles);
            }
            if (env->last_branch_from) {
                ia64_fw_dump_code(env, "r8_err_from",
                                  env->last_branch_from, dump_bundles);
            }
            if (env->last_branch_to) {
                ia64_fw_dump_code(env, "r8_err_to",
                                  env->last_branch_to, dump_bundles);
            }
#ifndef CONFIG_USER_ONLY
            ia64_dbg_probe_dump_mem(env, pc, "r8_r33", env->r[33], 128);
            ia64_dbg_probe_dump_mem(env, pc, "r8_r35", env->r[35], 128);
            ia64_dbg_probe_dump_mem(env, pc, "r8_r43", env->r[43], 128);
            {
                ia64_dbg_probe_dump_mem(env, pc, "r8_out0", out_args[0], 128);
                ia64_dbg_probe_dump_mem(env, pc, "r8_out3", out_args[3], 128);
            }
#endif
        }
    }
    if (target_enabled) {
        env->dbg_fw_r8_logged = 1;
    }
}

static void ia64_fw_sal_common(CPUIA64State *env, bool break_abi)
{
    uint8_t out0 = ia64_fw_out_base(env);
    bool from_call = ((env->last_b0_write_kind & 0xff) == 1);
    bool use_break_args = break_abi || !from_call;

    {
        static int trace_enabled = -1;
        static uint32_t trace_limit;
        static uint32_t trace_count;
        if (trace_enabled == -1) {
            trace_enabled = getenv("QEMU_IA64_FW_SAL_TRACE") ? 1 : 0;
            trace_limit = 16;
            const char *s = getenv("QEMU_IA64_FW_SAL_TRACE_LIMIT");
            if (s && *s) {
                trace_limit = (uint32_t)atoi(s);
            }
        }
        if (trace_enabled && trace_count < trace_limit) {
            uint8_t sof = env->cfm & 0x7f;
            uint8_t sol = (env->cfm >> 7) & 0x7f;
            uint8_t sor = (env->cfm >> 14) & 0xf;
            uint64_t out0v = ia64_fw_arg(env, out0, 0);
            uint64_t out1v = ia64_fw_arg(env, out0, 1);
            uint64_t out2v = ia64_fw_arg(env, out0, 2);
            uint64_t out3v = ia64_fw_arg(env, out0, 3);
            uint8_t lb_kind = env->last_branch_kind & 0xff;
            uint8_t lb_ri = (env->last_branch_kind >> 8) & 0xff;

            trace_count++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: SAL_CALL ip=%016" PRIx64 " b0=%016" PRIx64
                          " psr=%016" PRIx64 " cfm=%016" PRIx64
                          " r1=%016" PRIx64 " r9=%016" PRIx64
                          " r12=%016" PRIx64
                          " r28=%016" PRIx64 " r29=%016" PRIx64
                          " r30=%016" PRIx64 " r31=%016" PRIx64
                          " r32=%016" PRIx64 " r33=%016" PRIx64
                          " r34=%016" PRIx64 " r35=%016" PRIx64
                          " r36=%016" PRIx64 " r37=%016" PRIx64
                          " r38=%016" PRIx64 " r39=%016" PRIx64
                          " r40=%016" PRIx64 " r41=%016" PRIx64 "\n",
                          env->ip, env->b[0], env->psr, env->cfm,
                          env->r[1], env->r[9], env->r[12],
                          env->r[28], env->r[29], env->r[30], env->r[31],
                          env->r[32], env->r[33], env->r[34], env->r[35],
                          env->r[36], env->r[37], env->r[38], env->r[39],
                          env->r[40], env->r[41]);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: SAL_CALL_CTX last_branch from=%016" PRIx64
                          " to=%016" PRIx64 " kind=%u ri=%u insn=%011" PRIx64
                          " cfm=%016" PRIx64 " sof=%u sol=%u sor=%u out0=r%u"
                          " out0..3=%016" PRIx64 " %016" PRIx64
                          " %016" PRIx64 " %016" PRIx64 "\n",
                          env->last_branch_from, env->last_branch_to,
                          lb_kind, lb_ri, env->last_branch_insn,
                          env->cfm, sof, sol, sor, out0,
                          out0v, out1v, out2v, out3v);
        }
    }

    ia64_fw_try_install_sal_systab(env);

    uint64_t func_raw = use_break_args ? ia64_fw_arg_break(env, 0)
                                  : ia64_fw_arg(env, out0, 0);
    uint64_t args_trace[4] = {
        use_break_args ? ia64_fw_arg_break(env, 1) : ia64_fw_arg(env, out0, 1),
        use_break_args ? ia64_fw_arg_break(env, 2) : ia64_fw_arg(env, out0, 2),
        use_break_args ? ia64_fw_arg_break(env, 3) : ia64_fw_arg(env, out0, 3),
        use_break_args ? ia64_fw_arg_break(env, 4) : ia64_fw_arg(env, out0, 4),
    };
    IA64EfiGuid guid;
    if (ia64_fw_read_guid(env, func_raw, &guid) &&
        ia64_fw_guid_equal(&guid, &ia64_efi_guid_esal_pci)) {
        uint64_t func_id = use_break_args ? ia64_fw_arg_break(env, 1)
                                     : ia64_fw_arg(env, out0, 1);
        uint64_t pci_addr = use_break_args ? ia64_fw_arg_break(env, 2)
                                      : ia64_fw_arg(env, out0, 2);
        uint64_t size = use_break_args ? ia64_fw_arg_break(env, 3)
                                  : ia64_fw_arg(env, out0, 3);
        uint64_t value = use_break_args ? ia64_fw_arg_break(env, 4)
                                   : ia64_fw_arg(env, out0, 4);
        int64_t status = 0;
        uint64_t v0 = 0, v1 = 0, v2 = 0;

        uint16_t seg;
        uint8_t bus, devfn;
        uint16_t reg;
        ia64_fw_decode_pci_addr(pci_addr, &seg, &bus, &devfn, &reg);

        if (seg != 0 || reg > 0xff) {
            status = -1;
        } else if (func_id == 0) {
            uint32_t cfgaddr = 0x80000000U | ((uint32_t)bus << 16) |
                               ((uint32_t)devfn << 8) | (reg & ~3U);
            cpu_outl(0xcf8, cfgaddr);
            if (size == 1) {
                v0 = cpu_inb(0xcfc + (reg & 3));
            } else if (size == 2) {
                v0 = cpu_inw(0xcfc + (reg & 2));
            } else {
                v0 = cpu_inl(0xcfc);
            }
        } else if (func_id == 1) {
            uint32_t cfgaddr = 0x80000000U | ((uint32_t)bus << 16) |
                               ((uint32_t)devfn << 8) | (reg & ~3U);
            cpu_outl(0xcf8, cfgaddr);
            if (size == 1) {
                cpu_outb(0xcfc + (reg & 3), (uint8_t)value);
            } else if (size == 2) {
                cpu_outw(0xcfc + (reg & 2), (uint16_t)value);
            } else {
                cpu_outl(0xcfc, (uint32_t)value);
            }
        } else {
            status = -1;
        }

        if (ia64_fw_log_enabled()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: ESAL_PCI func=%" PRIu64
                          " seg=%u bus=%u devfn=%u reg=0x%x size=%" PRIu64
                          " value=%016" PRIx64 " -> status=%" PRId64
                          " v0=%016" PRIx64 "\n",
                          func_id, seg, bus, devfn, reg, size, value,
                          status, v0);
        }

        env->r[8] = (uint64_t)status;
        env->r[9] = v0;
        env->r[10] = v1;
        env->r[11] = v2;
        return;
    }
    if (ia64_fw_read_guid(env, func_raw, &guid) &&
        ia64_fw_guid_equal(&guid, &ia64_efi_guid_esal_fvb)) {
        CPUState *cs = env_cpu(env);
        uint64_t func_id = use_break_args ? ia64_fw_arg_break(env, 1)
                                     : ia64_fw_arg(env, out0, 1);
        uint64_t instance = use_break_args ? ia64_fw_arg_break(env, 2)
                                      : ia64_fw_arg(env, out0, 2);
        int64_t status = 0;
        uint64_t v0 = 0, v1 = 0, v2 = 0;
        bool trace = ia64_fw_fvb_trace_enabled();

        switch (func_id) {
        case IA64_ESAL_FVB_READ: {
            uint64_t lba = use_break_args ? ia64_fw_arg_break(env, 3)
                                     : ia64_fw_arg(env, out0, 3);
            uint64_t offset = use_break_args ? ia64_fw_arg_break(env, 4)
                                        : ia64_fw_arg(env, out0, 4);
            uint64_t num_bytes_ptr = use_break_args ? ia64_fw_arg_break(env, 5)
                                               : ia64_fw_arg(env, out0, 5);
            uint64_t buf_ptr = use_break_args ? ia64_fw_arg_break(env, 6)
                                         : ia64_fw_arg(env, out0, 6);
            uint64_t req_bytes = 0;
            uint64_t pa = 0;
            uint64_t avail = 0;
            uint64_t n = 0;
            g_autofree uint8_t *tmp = NULL;
            if (!num_bytes_ptr || !buf_ptr) {
                status = -1;
                goto fvb_read_log;
            }
            if (!ia64_fw_read_bytes_any(cs, num_bytes_ptr,
                                        (uint8_t *)&req_bytes,
                                        sizeof(req_bytes))) {
                status = -1;
                goto fvb_read_log;
            }
            if (!ia64_fw_fvb_translate(instance, lba, offset, &pa, &avail)) {
                status = -1;
                goto fvb_read_log;
            }
            uint64_t want = req_bytes;
            if (want > avail) {
                want = avail;
            }
            if (want > ia64_fw_flash_size()) {
                want = ia64_fw_flash_size();
            }
            n = (uint64_t)want;
            if (n) {
                tmp = g_malloc((size_t)n);
                if (address_space_read(&address_space_memory, (hwaddr)pa,
                                       MEMTXATTRS_UNSPECIFIED, tmp,
                                       (size_t)n) != MEMTX_OK ||
                    !ia64_fw_write_bytes_any(cs, buf_ptr, tmp, (size_t)n)) {
                    status = -1;
                    goto fvb_read_log;
                }
            }
            uint8_t out[8];
            stq_le_p(out, n);
            if (!ia64_fw_write_bytes_any(cs, num_bytes_ptr, out, sizeof(out))) {
                status = -1;
            }
fvb_read_log:
            if (ia64_fw_log_enabled() && (trace || status != 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: ESAL_FVB_READ inst=%" PRIu64
                              " lba=%" PRIu64 " off=%" PRIu64
                              " req=%" PRIu64 " got=%" PRIu64
                              " num_ptr=%016" PRIx64 " buf=%016" PRIx64
                              " pa=%016" PRIx64 " avail=%" PRIu64
                              " -> status=%" PRId64 "\n",
                              instance, lba, offset, req_bytes, n,
                              num_bytes_ptr, buf_ptr, pa, avail, status);
            }
            break;
        }
        case IA64_ESAL_FVB_WRITE: {
            uint64_t lba = use_break_args ? ia64_fw_arg_break(env, 3)
                                     : ia64_fw_arg(env, out0, 3);
            uint64_t offset = use_break_args ? ia64_fw_arg_break(env, 4)
                                        : ia64_fw_arg(env, out0, 4);
            uint64_t num_bytes_ptr = use_break_args ? ia64_fw_arg_break(env, 5)
                                               : ia64_fw_arg(env, out0, 5);
            uint64_t buf_ptr = use_break_args ? ia64_fw_arg_break(env, 6)
                                         : ia64_fw_arg(env, out0, 6);
            uint64_t req_bytes = 0;
            uint64_t pa = 0;
            uint64_t avail = 0;
            uint64_t n = 0;
            g_autofree uint8_t *tmp = NULL;
            if (!num_bytes_ptr || !buf_ptr) {
                status = -1;
                goto fvb_write_log;
            }
            if (!ia64_fw_read_bytes_any(cs, num_bytes_ptr,
                                        (uint8_t *)&req_bytes,
                                        sizeof(req_bytes))) {
                status = -1;
                goto fvb_write_log;
            }
            if (!ia64_fw_fvb_translate(instance, lba, offset, &pa, &avail)) {
                status = -1;
                goto fvb_write_log;
            }
            uint64_t want = req_bytes;
            if (want > avail) {
                want = avail;
            }
            if (want > ia64_fw_flash_size()) {
                want = ia64_fw_flash_size();
            }
            n = (uint64_t)want;
            if (n) {
                tmp = g_malloc((size_t)n);
                if (!ia64_fw_read_bytes_any(cs, buf_ptr, tmp, (size_t)n) ||
                    address_space_write(&address_space_memory, (hwaddr)pa,
                                        MEMTXATTRS_UNSPECIFIED, tmp,
                                        (size_t)n) != MEMTX_OK) {
                    status = -1;
                    goto fvb_write_log;
                }
            }
            uint8_t out[8];
            stq_le_p(out, n);
            if (!ia64_fw_write_bytes_any(cs, num_bytes_ptr, out, sizeof(out))) {
                status = -1;
            }
fvb_write_log:
            if (ia64_fw_log_enabled() && (trace || status != 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: ESAL_FVB_WRITE inst=%" PRIu64
                              " lba=%" PRIu64 " off=%" PRIu64
                              " req=%" PRIu64 " put=%" PRIu64
                              " num_ptr=%016" PRIx64 " buf=%016" PRIx64
                              " pa=%016" PRIx64 " avail=%" PRIu64
                              " -> status=%" PRId64 "\n",
                              instance, lba, offset, req_bytes, n,
                              num_bytes_ptr, buf_ptr, pa, avail, status);
            }
            break;
        }
        case IA64_ESAL_FVB_ERASE_BLOCK: {
            uint64_t lba = use_break_args ? ia64_fw_arg_break(env, 3)
                                     : ia64_fw_arg(env, out0, 3);
            uint64_t pa = 0, avail = 0;
            g_autofree uint8_t *tmp = NULL;
            if (!ia64_fw_fvb_translate(instance, lba, 0, &pa, &avail)) {
                status = -1;
                goto fvb_erase_log;
            }
            size_t n = IA64_IPF_FW_FLASH_BLOCK_SIZE;
            tmp = g_malloc(n);
            memset(tmp, 0xff, n);
            if (address_space_write(&address_space_memory, (hwaddr)pa,
                                    MEMTXATTRS_UNSPECIFIED, tmp, n) != MEMTX_OK) {
                status = -1;
            }
fvb_erase_log:
            if (ia64_fw_log_enabled() && (trace || status != 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: ESAL_FVB_ERASE inst=%" PRIu64
                              " lba=%" PRIu64 " pa=%016" PRIx64
                              " avail=%" PRIu64 " -> status=%" PRId64 "\n",
                              instance, lba, pa, avail, status);
            }
            break;
        }
        case IA64_ESAL_FVB_GET_VOLUME_ATTRS:
        case IA64_ESAL_FVB_SET_VOLUME_ATTRS: {
            uint64_t attrs_ptr = use_break_args ? ia64_fw_arg_break(env, 3)
                                           : ia64_fw_arg(env, out0, 3);
            if (!attrs_ptr) {
                status = -1;
                goto fvb_attr_log;
            }
            uint8_t out[4];
            stl_le_p(out, IA64_IPF_FW_FLASH_ATTRS);
            if (!ia64_fw_write_bytes_any(cs, attrs_ptr, out, sizeof(out))) {
                status = -1;
            }
fvb_attr_log:
            if (ia64_fw_log_enabled() && (trace || status != 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: ESAL_FVB_ATTRS inst=%" PRIu64
                              " attrs_ptr=%016" PRIx64 " -> status=%" PRId64 "\n",
                              instance, attrs_ptr, status);
            }
            break;
        }
        case IA64_ESAL_FVB_GET_PHYS_ADDR: {
            uint64_t base_ptr = use_break_args ? ia64_fw_arg_break(env, 3)
                                          : ia64_fw_arg(env, out0, 3);
            if (!base_ptr) {
                status = -1;
                goto fvb_phys_log;
            }
            uint8_t out[8];
            stq_le_p(out, ia64_fw_flash_base());
            if (!ia64_fw_write_bytes_any(cs, base_ptr, out, sizeof(out))) {
                status = -1;
            }
fvb_phys_log:
            if (ia64_fw_log_enabled() && (trace || status != 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: ESAL_FVB_GET_PHYS inst=%" PRIu64
                              " base_ptr=%016" PRIx64 " base=%016" PRIx64
                              " -> status=%" PRId64 "\n",
                              instance, base_ptr,
                              ia64_fw_flash_base(), status);
            }
            break;
        }
        case IA64_ESAL_FVB_GET_BLOCK_SIZE: {
            uint64_t lba = use_break_args ? ia64_fw_arg_break(env, 3)
                                     : ia64_fw_arg(env, out0, 3);
            uint64_t block_ptr = use_break_args ? ia64_fw_arg_break(env, 4)
                                           : ia64_fw_arg(env, out0, 4);
            uint64_t count_ptr = use_break_args ? ia64_fw_arg_break(env, 5)
                                           : ia64_fw_arg(env, out0, 5);
            uint64_t blocks = ia64_fw_flash_blocks();
            if (!block_ptr || !count_ptr) {
                status = -1;
                goto fvb_block_log;
            }
            if (lba >= blocks) {
                status = -1;
                goto fvb_block_log;
            }
            uint8_t out[8];
            stq_le_p(out, IA64_IPF_FW_FLASH_BLOCK_SIZE);
            if (!ia64_fw_write_bytes_any(cs, block_ptr, out, sizeof(out))) {
                status = -1;
                goto fvb_block_log;
            }
            stq_le_p(out, blocks - lba);
            if (!ia64_fw_write_bytes_any(cs, count_ptr, out, sizeof(out))) {
                status = -1;
            }
fvb_block_log:
            if (ia64_fw_log_enabled() && (trace || status != 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: ESAL_FVB_GET_BLOCK inst=%" PRIu64
                              " lba=%" PRIu64 " blk_ptr=%016" PRIx64
                              " cnt_ptr=%016" PRIx64 " -> status=%" PRId64 "\n",
                              instance, lba, block_ptr, count_ptr, status);
            }
            break;
        }
        case IA64_ESAL_FVB_ERASE_CUSTOM_RANGE: {
            uint64_t start_lba = use_break_args ? ia64_fw_arg_break(env, 3)
                                           : ia64_fw_arg(env, out0, 3);
            uint64_t offset_start = use_break_args ? ia64_fw_arg_break(env, 4)
                                              : ia64_fw_arg(env, out0, 4);
            uint64_t last_lba = use_break_args ? ia64_fw_arg_break(env, 5)
                                          : ia64_fw_arg(env, out0, 5);
            uint64_t offset_last = use_break_args ? ia64_fw_arg_break(env, 6)
                                             : ia64_fw_arg(env, out0, 6);
            uint64_t blocks = ia64_fw_flash_blocks();
            uint64_t flash_size = ia64_fw_flash_size();
            uint64_t flash_base = ia64_fw_flash_base();
            uint64_t start_off = start_lba * IA64_IPF_FW_FLASH_BLOCK_SIZE + offset_start;
            uint64_t end_off = last_lba * IA64_IPF_FW_FLASH_BLOCK_SIZE + offset_last;
            g_autofree uint8_t *tmp = NULL;
            if (start_lba >= blocks ||
                last_lba >= blocks ||
                start_off >= flash_size ||
                end_off >= flash_size ||
                start_off > end_off) {
                status = -1;
                goto fvb_range_log;
            }
            uint64_t len = end_off - start_off + 1;
            tmp = g_malloc((size_t)len);
            memset(tmp, 0xff, (size_t)len);
            if (address_space_write(&address_space_memory,
                                    (hwaddr)(flash_base + start_off),
                                    MEMTXATTRS_UNSPECIFIED, tmp, (size_t)len) != MEMTX_OK) {
                status = -1;
            }
fvb_range_log:
            if (ia64_fw_log_enabled() && (trace || status != 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: ESAL_FVB_ERASE_RANGE inst=%" PRIu64
                              " start_lba=%" PRIu64 " off_start=%" PRIu64
                              " last_lba=%" PRIu64 " off_last=%" PRIu64
                              " -> status=%" PRId64 "\n",
                              instance, start_lba, offset_start,
                              last_lba, offset_last, status);
            }
            break;
        }
        default:
            status = -1;
            break;
        }

        if (ia64_fw_log_enabled()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: ESAL_FVB func=%" PRIu64 " inst=%" PRIu64
                          " -> status=%" PRId64 "\n",
                          func_id, instance, status);
        }

        env->r[8] = (uint64_t)status;
        env->r[9] = v0;
        env->r[10] = v1;
        env->r[11] = v2;
        return;
    }
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
         * Match SKI's SAL emulation: return success with no state-info.
         *
         * Some firmware stacks (notably xenipf/EDK) treat failures from
         * SAL_GET_STATE_INFO as fatal during early DXE init.
         */
        if (ia64_fw_log_enabled()) {
            uint64_t a1 = use_break_args ? ia64_fw_arg_break(env, 1)
                                    : ia64_fw_arg(env, out0, 1);
            uint64_t a2 = use_break_args ? ia64_fw_arg_break(env, 2)
                                    : ia64_fw_arg(env, out0, 2);
            uint64_t a3 = use_break_args ? ia64_fw_arg_break(env, 3)
                                    : ia64_fw_arg(env, out0, 3);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: SAL_GET_STATE_INFO a1=%016" PRIx64
                          " a2=%016" PRIx64 " a3=%016" PRIx64
                          " -> status=0 v0=0 v1=0 v2=0\n",
                          a1, a2, a3);
        }
        status = 0;
        break;
    case IA64_SAL_GET_STATE_INFO_SIZE:
        /* SKI returns success and size 0. */
        if (ia64_fw_log_enabled()) {
            uint64_t a1 = use_break_args ? ia64_fw_arg_break(env, 1)
                                    : ia64_fw_arg(env, out0, 1);
            uint64_t a2 = use_break_args ? ia64_fw_arg_break(env, 2)
                                    : ia64_fw_arg(env, out0, 2);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IA64: SAL_GET_STATE_INFO_SIZE a1=%016" PRIx64
                          " a2=%016" PRIx64 " -> status=0 v0=0 v1=0 v2=0\n",
                          a1, a2);
        }
        status = 0;
        v0 = 0;
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
         *   arg1: encoded pci_config_addr
         *   arg2: access size (1/2/4[/8])
         *   arg3: type/mode (0 = legacy, 1 = extended)
         */
        static int sal_pci_failfast = -1;
        static int sal_pci_dump = -1;
        static int sal_pci_dump_bundles = -1;
        if (sal_pci_failfast == -1) {
            const char *s = getenv("QEMU_IA64_SAL_PCI_FAILFAST");
            sal_pci_failfast = (s && *s) ? 1 : 0;
        }
        if (sal_pci_dump == -1) {
            const char *s = getenv("QEMU_IA64_SAL_PCI_DUMP");
            sal_pci_dump = (s && *s) ? 1 : 0;
        }
        if (sal_pci_dump_bundles == -1) {
            sal_pci_dump_bundles = 64;
            const char *s = getenv("QEMU_IA64_SAL_PCI_DUMP_BUNDLES");
            if (s && *s) {
                sal_pci_dump_bundles = atoi(s);
                if (sal_pci_dump_bundles <= 0) {
                    sal_pci_dump_bundles = 64;
                }
            }
        }

        uint64_t arg1 = use_break_args ? ia64_fw_arg_break(env, 1)
                                  : ia64_fw_arg(env, out0, 1);
        uint64_t arg2 = use_break_args ? ia64_fw_arg_break(env, 2)
                                  : ia64_fw_arg(env, out0, 2);
        uint64_t arg3 = use_break_args ? ia64_fw_arg_break(env, 3)
                                  : ia64_fw_arg(env, out0, 3);
        uint64_t pci_addr = break_abi ? arg2 : arg1;
        uint64_t size_raw = break_abi ? arg1 : arg2;
        uint64_t size = size_raw;
        uint64_t type = arg3;
        if (break_abi) {
            uint64_t width_bytes;
            if (ia64_fw_pci_width_to_bytes(size, &width_bytes)) {
                size = width_bytes;
            }
        }
        bool size_valid = (size == 1 || size == 2 || size == 4);
        if (!size_valid && !break_abi) {
            uint64_t width_bytes;
            if (ia64_fw_pci_width_to_bytes(size, &width_bytes)) {
                size = width_bytes;
                size_valid = true;
            }
        }
        if (!size_valid) {
            if (ia64_fw_log_enabled()) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: SAL_PCI_CONFIG_READ invalid size=%" PRIu64
                              " addr=%016" PRIx64 " type=%" PRIu64
                              " break_abi=%d"
                              " raw=%" PRIu64
                              " cfm=%016" PRIx64 " out0=r%u"
                              " r28=%016" PRIx64 " r29=%016" PRIx64
                              " r30=%016" PRIx64 " r31=%016" PRIx64 "\n",
                              size, pci_addr, type, break_abi ? 1 : 0,
                              size_raw,
                              env->cfm, out0,
                              env->r[28], env->r[29], env->r[30], env->r[31]);
            }
        }
        if (sal_pci_failfast && !size_valid) {
            if (sal_pci_dump) {
                ia64_fw_dump_code(env, "caller", env->last_branch_from,
                                  sal_pci_dump_bundles);
                ia64_fw_dump_code(env, "callee", env->last_branch_to,
                                  sal_pci_dump_bundles);
                ia64_fw_dump_code(env, "b0", env->b[0],
                                  sal_pci_dump_bundles);
                ia64_fw_dump_code(env, "b0w", env->last_b0_write_pc,
                                  sal_pci_dump_bundles);
                ia64_fw_dump_code(env, "b0wprev", env->prev_b0_write_pc,
                                  sal_pci_dump_bundles);
            }
            cpu_abort(env_cpu(env),
                      "IA64: SAL_PCI_CONFIG_READ invalid size=%" PRIu64
                      " pci_addr=%016" PRIx64 " type=%" PRIu64 " break_abi=%d"
                      " lb_from=%016" PRIx64 " lb_to=%016" PRIx64
                      " b0=%016" PRIx64,
                      size, pci_addr, type, break_abi ? 1 : 0,
                      env->last_branch_from, env->last_branch_to, env->b[0]);
        }
        if (!size_valid) {
            status = -2;
            v0 = 0;
            v1 = 0;
            v2 = 0;
            break;
        }
        if (type != 0) {
            status = -2;
            if (ia64_fw_log_enabled()) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: SAL_PCI_CONFIG_READ unsupported type=%" PRIu64
                              " addr=%016" PRIx64 " size=%" PRIu64
                              " break_abi=%d -> status=%" PRId64 "\n",
                              type, pci_addr, size, break_abi ? 1 : 0, status);
            }
            break;
        }
        uint16_t seg = 0;
        uint8_t bus = 0, devfn = 0;
        uint16_t reg = 0;
        uint32_t cfgaddr = 0;
        bool cf8_addr = false;
        if (break_abi && (pci_addr & 0x80000000ULL)) {
            cf8_addr = true;
            bus = (pci_addr >> 16) & 0xff;
            devfn = (pci_addr >> 8) & 0xff;
            reg = pci_addr & 0xff;
            cfgaddr = (uint32_t)(pci_addr & ~3ULL);
        } else {
            ia64_fw_decode_pci_addr(pci_addr, &seg, &bus, &devfn, &reg);
            if (seg != 0 || reg > 0xff) {
                status = -2;
                if (ia64_fw_log_enabled()) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: SAL_PCI_CONFIG_READ seg=%u bus=%u devfn=%u"
                                  " reg=0x%x size=%" PRIu64 " break_abi=%d"
                                  " -> status=%" PRId64 " v0=%016" PRIx64 "\n",
                                  seg, bus, devfn, reg, size, break_abi ? 1 : 0,
                                  status, v0);
                }
                break;
            }
            cfgaddr = 0x80000000U | ((uint32_t)bus << 16) |
                      ((uint32_t)devfn << 8) | (reg & ~3U);
        }
        cpu_outl(0xcf8, cfgaddr);

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
                          "IA64: SAL_PCI_CONFIG_READ seg=%u bus=%u devfn=%u"
                          " reg=0x%x size=%" PRIu64 " type=%" PRIu64
                          " break_abi=%d cf8=%d"
                          " -> status=%" PRId64 " v0=%016" PRIx64 "\n",
                          seg, bus, devfn, reg, size, type, break_abi ? 1 : 0,
                          cf8_addr ? 1 : 0,
                          status, v0);
        }
        break;
    }
    case IA64_SAL_PCI_CONFIG_WRITE: {
        /*
         * Arguments:
         *   arg1: encoded pci_config_addr
         *   arg2: access size (1/2/4[/8])
         *   arg3: value
         *   arg4: type/mode (0 = legacy, 1 = extended)
         */
        static int sal_pci_failfast = -1;
        static int sal_pci_dump = -1;
        static int sal_pci_dump_bundles = -1;
        if (sal_pci_failfast == -1) {
            const char *s = getenv("QEMU_IA64_SAL_PCI_FAILFAST");
            sal_pci_failfast = (s && *s) ? 1 : 0;
        }
        if (sal_pci_dump == -1) {
            const char *s = getenv("QEMU_IA64_SAL_PCI_DUMP");
            sal_pci_dump = (s && *s) ? 1 : 0;
        }
        if (sal_pci_dump_bundles == -1) {
            sal_pci_dump_bundles = 64;
            const char *s = getenv("QEMU_IA64_SAL_PCI_DUMP_BUNDLES");
            if (s && *s) {
                sal_pci_dump_bundles = atoi(s);
                if (sal_pci_dump_bundles <= 0) {
                    sal_pci_dump_bundles = 64;
                }
            }
        }

        uint64_t arg1 = use_break_args ? ia64_fw_arg_break(env, 1)
                                  : ia64_fw_arg(env, out0, 1);
        uint64_t arg2 = use_break_args ? ia64_fw_arg_break(env, 2)
                                  : ia64_fw_arg(env, out0, 2);
        uint64_t arg3 = use_break_args ? ia64_fw_arg_break(env, 3)
                                  : ia64_fw_arg(env, out0, 3);
        uint64_t arg4 = use_break_args ? ia64_fw_arg_break(env, 4)
                                  : ia64_fw_arg(env, out0, 4);
        uint64_t pci_addr = break_abi ? arg2 : arg1;
        uint64_t size_raw = break_abi ? arg1 : arg2;
        uint64_t size = size_raw;
        uint64_t value = arg3;
        uint64_t type = arg4;
        if (break_abi) {
            uint64_t width_bytes;
            if (ia64_fw_pci_width_to_bytes(size, &width_bytes)) {
                size = width_bytes;
            }
        }
        bool size_valid = (size == 1 || size == 2 || size == 4);
        if (!size_valid && !break_abi) {
            uint64_t width_bytes;
            if (ia64_fw_pci_width_to_bytes(size, &width_bytes)) {
                size = width_bytes;
                size_valid = true;
            }
        }
        if (!size_valid) {
            if (ia64_fw_log_enabled()) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: SAL_PCI_CONFIG_WRITE invalid size=%" PRIu64
                              " addr=%016" PRIx64 " type=%" PRIu64
                              " break_abi=%d"
                              " raw=%" PRIu64
                              " cfm=%016" PRIx64 " out0=r%u"
                              " r28=%016" PRIx64 " r29=%016" PRIx64
                              " r30=%016" PRIx64 " r31=%016" PRIx64 "\n",
                              size, pci_addr, type, break_abi ? 1 : 0,
                              size_raw,
                              env->cfm, out0,
                              env->r[28], env->r[29], env->r[30], env->r[31]);
            }
        }
        if (sal_pci_failfast && !size_valid) {
            if (sal_pci_dump) {
                ia64_fw_dump_code(env, "caller", env->last_branch_from,
                                  sal_pci_dump_bundles);
                ia64_fw_dump_code(env, "callee", env->last_branch_to,
                                  sal_pci_dump_bundles);
                ia64_fw_dump_code(env, "b0", env->b[0],
                                  sal_pci_dump_bundles);
                ia64_fw_dump_code(env, "b0w", env->last_b0_write_pc,
                                  sal_pci_dump_bundles);
                ia64_fw_dump_code(env, "b0wprev", env->prev_b0_write_pc,
                                  sal_pci_dump_bundles);
            }
            cpu_abort(env_cpu(env),
                      "IA64: SAL_PCI_CONFIG_WRITE invalid size=%" PRIu64
                      " pci_addr=%016" PRIx64 " type=%" PRIu64 " break_abi=%d"
                      " lb_from=%016" PRIx64 " lb_to=%016" PRIx64
                      " b0=%016" PRIx64,
                      size, pci_addr, type, break_abi ? 1 : 0,
                      env->last_branch_from, env->last_branch_to, env->b[0]);
        }
        if (!size_valid) {
            status = -2;
            v0 = 0;
            v1 = 0;
            v2 = 0;
            break;
        }
        if (type != 0) {
            status = -2;
            if (ia64_fw_log_enabled()) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IA64: SAL_PCI_CONFIG_WRITE unsupported type=%" PRIu64
                              " addr=%016" PRIx64 " size=%" PRIu64
                              " break_abi=%d -> status=%" PRId64 "\n",
                              type, pci_addr, size, break_abi ? 1 : 0, status);
            }
            break;
        }
        uint16_t seg = 0;
        uint8_t bus = 0, devfn = 0;
        uint16_t reg = 0;
        uint32_t cfgaddr = 0;
        bool cf8_addr = false;
        if (break_abi && (pci_addr & 0x80000000ULL)) {
            cf8_addr = true;
            bus = (pci_addr >> 16) & 0xff;
            devfn = (pci_addr >> 8) & 0xff;
            reg = pci_addr & 0xff;
            cfgaddr = (uint32_t)(pci_addr & ~3ULL);
        } else {
            ia64_fw_decode_pci_addr(pci_addr, &seg, &bus, &devfn, &reg);
            if (seg != 0 || reg > 0xff) {
                status = -2;
                if (ia64_fw_log_enabled()) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "IA64: SAL_PCI_CONFIG_WRITE seg=%u bus=%u devfn=%u"
                                  " reg=0x%x size=%" PRIu64 " value=%016" PRIx64
                                  " break_abi=%d -> status=%" PRId64 "\n",
                                  seg, bus, devfn, reg, size, value,
                                  break_abi ? 1 : 0,
                                  status);
                }
                break;
            }
            cfgaddr = 0x80000000U | ((uint32_t)bus << 16) |
                      ((uint32_t)devfn << 8) | (reg & ~3U);
        }
        cpu_outl(0xcf8, cfgaddr);

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
                          "IA64: SAL_PCI_CONFIG_WRITE seg=%u bus=%u devfn=%u"
                          " reg=0x%x size=%" PRIu64 " value=%016" PRIx64
                          " type=%" PRIu64 " break_abi=%d cf8=%d -> status=%" PRId64 "\n",
                          seg, bus, devfn, reg, size, value,
                          type, break_abi ? 1 : 0,
                          cf8_addr ? 1 : 0,
                          status);
        }
        break;
    }
    case IA64_SAL_UPDATE_PAL:
        /* SKI returns success; firmware stacks may treat failure as fatal. */
        status = 0;
        v0 = 0;
        v1 = 0;
        v2 = 0;
        break;
    case IA64_SAL_PHYSICAL_ID_INFO:
        /* Minimal single-socket/single-core answer. */
        status = 0;
        v0 = 0;
        v1 = 0;
        v2 = 0;
        break;
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

    ia64_fw_trace_record(IA64_FW_TRACE_SAL, env->ip, func_raw, func,
                         args_trace, status, v0, v1, v2, use_break_args);

    env->r[8] = (uint64_t)status;
    env->r[9] = v0;
    env->r[10] = v1;
    env->r[11] = v2;
}

void HELPER(fw_sal)(CPUIA64State *env)
{
    ia64_fw_sal_common(env, false);
}

void HELPER(fw_sal_break)(CPUIA64State *env)
{
    ia64_fw_sal_common(env, true);
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
    uint64_t bsp = ia64_rse_get_bsp(env);
    uint8_t sof = env->cfm & 0x7f;
    if (!ia64_rse_is_lazy(env)) {
        ia64_rse_store_frame(env, bsp, sof);
    }
    env->ar[IA64_AR_BSP] = bsp;
    env->ar[IA64_AR_BSPSTORE] = bsp;
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
    uint64_t bsp = ia64_rse_get_bsp(env);
    uint8_t sof = env->cfm & 0x7f;
    ia64_rse_load_frame(env, bsp, sof);
    env->ar[IA64_AR_BSP] = bsp;
    env->ar[IA64_AR_RSC] = ia64_rsc_set_loadrs(rsc, 0);
}

void HELPER(cover)(CPUIA64State *env)
{
    static int log_count;
    uint64_t old_cfm = env->cfm;
    uint8_t sof = old_cfm & 0x7f;
    uint64_t bsp = ia64_rse_get_bsp(env);

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

    ia64_rse_store_frame(env, bsp, sof);
    env->ar[IA64_AR_BSP] = new_bsp;
    ia64_rse_update_loadrs(env, new_bsp);

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
    static int log_bspstore = -1;
    if (log_bspstore == -1) {
        log_bspstore = getenv("QEMU_IA64_LOG_BSPSTORE") ? 1 : 0;
    }
    if (log_bspstore && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64: set_bspstore pc=%016" PRIx64
                      " old=%016" PRIx64 " new=%016" PRIx64
                      " r1=%016" PRIx64 " r3=%016" PRIx64
                      " r8=%016" PRIx64 " r10=%016" PRIx64
                      " r12=%016" PRIx64
                      " ar.k3=%016" PRIx64
                      " ar.rsc=%016" PRIx64 " ar.lc=%016" PRIx64
                      " ar.ec=%016" PRIx64 " cfm=%016" PRIx64 "\n",
                      env->ip, env->ar[IA64_AR_BSPSTORE], bspstore,
                      env->r[1], env->r[3], env->r[8], env->r[10],
                      env->r[12], env->ar[3], env->ar[IA64_AR_RSC],
                      env->ar[65], env->ar[66], env->cfm);
    }
#ifndef CONFIG_USER_ONLY
    if (ia64_is_task_switch_pc(env, env->ip)) {
        ia64_rse_switch_bspstore(env, bspstore);
    }
#endif
    env->ar[IA64_AR_BSPSTORE] = bspstore;
    env->ar[IA64_AR_BSP] = bspstore;
    env->ar[IA64_AR_RSC] = ia64_rsc_set_loadrs(env->ar[IA64_AR_RSC], 0);
    env->ar[IA64_AR_RNAT] = 0;
    memset(&env->nat[32], 0, 96);
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
    /*
     * Data serialization: ensure all prior data memory accesses complete
     * before any subsequent data access. QEMU's memory model is already
     * sequentially consistent, but flush TLB to ensure translation consistency.
     */
    CPUState *cs = env_cpu(env);
    tlb_flush(cs);
}

void HELPER(srlz_i)(CPUIA64State *env)
{
    /*
     * Instruction serialization: ensure instruction cache coherency.
     * Flush TLB and JIT jump cache to handle translation consistency.
     * QEMU's TCG handles most self-modifying code automatically via
     * TLB-based SMC detection.
     */
    CPUState *cs = env_cpu(env);
    tlb_flush(cs);
    tcg_flush_jmp_cache(cs);
}
