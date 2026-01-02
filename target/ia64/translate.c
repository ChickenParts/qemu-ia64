/*
 * IA-64 translation
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "tcg/tcg.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"
#include "disas/disas.h"
#include "exec/translation-block.h"
#include "exec/memop.h"
#include "qemu/log.h"
#include <ctype.h>
#include <inttypes.h>
#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_psr;
static TCGv_i64 cpu_cfm;
static TCGv_i64 cpu_pr;
static TCGv_i64 cpu_b[8];
static TCGv_i64 cpu_r[128];
static TCGv_i64 cpu_rr[8];
static TCGv_i64 cpu_cr_iva;
static TCGv_i64 cpu_cr_iip;
static TCGv_i64 cpu_cr_ipsr;
static TCGv_i64 cpu_cr_ifs;
static TCGv_i64 cpu_cr_isr;
static TCGv_i64 cpu_cr_ifa;
static TCGv_i64 cpu_cr_iim;
static TCGv_i64 cpu_cr_iha;

#define IA64_MAX_BCALL_LOG 4096
static bool ia64_bcall_log_inited;
static uint64_t ia64_bcall_log_min_pc;
static uint64_t ia64_bcall_log_max_pc;
static int ia64_bcall_log_limit;
static int ia64_bcall_log_count;

enum {
    IA64_B7_WRITE_MOV_M = 0,
    IA64_B7_WRITE_MOV_I = 1,
    IA64_B7_WRITE_BRCALL_REG = 2,
    IA64_B7_WRITE_BRCALL_IMM = 3,
};

static void ia64_init_bcall_log(void)
{
    if (ia64_bcall_log_inited) {
        return;
    }
    ia64_bcall_log_inited = true;

    ia64_bcall_log_min_pc = 0;
    ia64_bcall_log_max_pc = UINT64_MAX;
    /*
     * Logging br.call/brl.call is extremely noisy and slows down firmware bringup
     * under TCG. Default to disabled; enable explicitly via QEMU_IA64_BCALL_LOG_*.
     */
    ia64_bcall_log_limit = 0;

    const char *s = getenv("QEMU_IA64_BCALL_LOG_MIN_PC");
    if (s && *s) {
        ia64_bcall_log_min_pc = strtoull(s, NULL, 0) & ~0xFULL;
    }
    s = getenv("QEMU_IA64_BCALL_LOG_MAX_PC");
    if (s && *s) {
        ia64_bcall_log_max_pc = strtoull(s, NULL, 0) & ~0xFULL;
    }
    s = getenv("QEMU_IA64_BCALL_LOG_LIMIT");
    if (s && *s) {
        ia64_bcall_log_limit = atoi(s);
    }
    if (ia64_bcall_log_limit < 0) {
        ia64_bcall_log_limit = 0;
    }
    if (ia64_bcall_log_limit > IA64_MAX_BCALL_LOG) {
        ia64_bcall_log_limit = IA64_MAX_BCALL_LOG;
    }
}

static bool ia64_bcall_should_log(uint64_t pc)
{
    ia64_init_bcall_log();
    if (ia64_bcall_log_limit == 0) {
        return false;
    }
    if (pc < ia64_bcall_log_min_pc || pc > ia64_bcall_log_max_pc) {
        return false;
    }
    if (ia64_bcall_log_count >= ia64_bcall_log_limit) {
        return false;
    }
    ia64_bcall_log_count++;
    return true;
}

#define IA64_MAX_DBG_CALL_PCS 8
static bool ia64_dbg_call_pc_inited;
static uint64_t ia64_dbg_call_pcs[IA64_MAX_DBG_CALL_PCS];
static size_t ia64_dbg_call_pc_count;

static bool ia64_hang_abort_inited;
static uint64_t ia64_hang_abort_threshold;
static bool ia64_fw_dump_pc_inited;
static uint64_t ia64_fw_dump_pc;
static uint32_t ia64_fw_dump_bundles;
static bool ia64_watch_load_inited;
static uint64_t ia64_watch_load_addr;
static bool ia64_load_watch_range_inited;
static bool ia64_load_watch_range_enabled;
static uint64_t ia64_load_watch_range_lo;
static uint64_t ia64_load_watch_range_hi;

static bool ia64_fw_fastpath_inited;
static bool ia64_fw_fastpath_enabled;
static bool ia64_fw_r8_trace_inited;
static bool ia64_fw_r8_trace_enabled;
static bool ia64_fw_r8_trace_range_inited;
static uint64_t ia64_fw_r8_trace_min_pc;
static uint64_t ia64_fw_r8_trace_max_pc;

static bool ia64_get_fw_fastpath_enabled(void)
{
    if (ia64_fw_fastpath_inited) {
        return ia64_fw_fastpath_enabled;
    }
    ia64_fw_fastpath_inited = true;

    const char *s = getenv("QEMU_IA64_FW_FASTPATH");
    if (!s || !*s) {
        /*
         * Firmware (and EDK in particular) can do huge bytewise memset/memcpy
         * loops that are extremely slow under TCG. We have a helper that can
         * recognize and accelerate a few well-known sequences.
         *
         * Default to disabled and allow opting in via QEMU_IA64_FW_FASTPATH=1.
         */
        ia64_fw_fastpath_enabled = false;
        return false;
    }

    if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "false") ||
        !strcmp(s, "no")) {
        ia64_fw_fastpath_enabled = false;
    } else {
        ia64_fw_fastpath_enabled = true;
    }
    return ia64_fw_fastpath_enabled;
}

static bool ia64_get_fw_r8_trace_enabled(void)
{
    if (ia64_fw_r8_trace_inited) {
        return ia64_fw_r8_trace_enabled;
    }
    ia64_fw_r8_trace_inited = true;

    const char *s = getenv("QEMU_IA64_FW_R8_TRACE");
    const char *c = getenv("QEMU_IA64_FW_CALL_TRACE");
    bool enabled = false;
    if (s && *s && strcmp(s, "0") && strcmp(s, "off") &&
        strcmp(s, "false") && strcmp(s, "no")) {
        enabled = true;
    }
    if (!enabled && c && *c && strcmp(c, "0") && strcmp(c, "off") &&
        strcmp(c, "false") && strcmp(c, "no")) {
        enabled = true;
    }
    ia64_fw_r8_trace_enabled = enabled;
    if (!ia64_fw_r8_trace_enabled) {
        return false;
    }

    ia64_fw_r8_trace_range_inited = true;
    ia64_fw_r8_trace_min_pc = 0;
    ia64_fw_r8_trace_max_pc = UINT64_MAX;
    s = getenv("QEMU_IA64_FW_R8_TRACE_MIN_PC");
    if (s && *s) {
        ia64_fw_r8_trace_min_pc = strtoull(s, NULL, 0) & ~0xFULL;
    }
    s = getenv("QEMU_IA64_FW_R8_TRACE_MAX_PC");
    if (s && *s) {
        ia64_fw_r8_trace_max_pc = strtoull(s, NULL, 0) & ~0xFULL;
    }

    return ia64_fw_r8_trace_enabled;
}

static bool ia64_fw_r8_trace_match(uint64_t pc)
{
    if (!ia64_get_fw_r8_trace_enabled()) {
        return false;
    }
    if (!ia64_fw_r8_trace_range_inited) {
        return true;
    }
    return pc >= ia64_fw_r8_trace_min_pc && pc <= ia64_fw_r8_trace_max_pc;
}

static uint64_t ia64_get_hang_abort_threshold(void)
{
    if (ia64_hang_abort_inited) {
        return ia64_hang_abort_threshold;
    }
    ia64_hang_abort_inited = true;

    const char *s = getenv("QEMU_IA64_HANG_ABORT");
    if (!s || !*s) {
        ia64_hang_abort_threshold = 0;
        return 0;
    }

    if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
        !strcmp(s, "yes")) {
        ia64_hang_abort_threshold = 1000000;
        return ia64_hang_abort_threshold;
    }

    char *endp = NULL;
    uint64_t v = strtoull(s, &endp, 0);
    if (endp && endp != s) {
        ia64_hang_abort_threshold = v;
    } else {
        ia64_hang_abort_threshold = 1000000;
    }
    return ia64_hang_abort_threshold;
}

static bool ia64_get_fw_dump_pc(uint64_t *pc, uint32_t *bundles)
{
    if (!ia64_fw_dump_pc_inited) {
        ia64_fw_dump_pc_inited = true;
        ia64_fw_dump_pc = 0;
        ia64_fw_dump_bundles = 64;

        const char *s = getenv("QEMU_IA64_FW_DUMP_PC");
        if (s && *s) {
            char *endp = NULL;
            uint64_t v = strtoull(s, &endp, 0);
            if (endp && endp != s) {
                ia64_fw_dump_pc = v & ~0xFULL;
            }
        }
        s = getenv("QEMU_IA64_FW_DUMP_BUNDLES");
        if (s && *s) {
            int v = atoi(s);
            if (v > 0) {
                ia64_fw_dump_bundles = (uint32_t)v;
            }
        }
    }

    if (pc) {
        *pc = ia64_fw_dump_pc;
    }
    if (bundles) {
        *bundles = ia64_fw_dump_bundles;
    }
    return ia64_fw_dump_pc != 0;
}

static uint64_t ia64_get_watch_load_addr(void)
{
    if (ia64_watch_load_inited) {
        return ia64_watch_load_addr;
    }
    ia64_watch_load_inited = true;
    const char *s = getenv("QEMU_IA64_WATCH_LOAD_ADDR");
    if (s && *s) {
        char *endp = NULL;
        ia64_watch_load_addr = strtoull(s, &endp, 0);
        if (endp == s) {
            ia64_watch_load_addr = 0;
        }
    } else {
        ia64_watch_load_addr = 0;
    }
    return ia64_watch_load_addr;
}

static void ia64_init_load_watch_range(void)
{
    if (ia64_load_watch_range_inited) {
        return;
    }
    ia64_load_watch_range_inited = true;

    const char *s = getenv("QEMU_IA64_WATCH_LOAD_RANGE");
    if (!s || !*s) {
        return;
    }

    char *endp = NULL;
    uint64_t lo = strtoull(s, &endp, 0);
    if (!endp || endp == s) {
        return;
    }
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
    if (!*endp) {
        return;
    }

    char *endp2 = NULL;
    uint64_t hi = strtoull(endp, &endp2, 0);
    if (!endp2 || endp2 == endp) {
        return;
    }
    if (hi <= lo) {
        return;
    }

    ia64_load_watch_range_lo = lo;
    ia64_load_watch_range_hi = hi;
    ia64_load_watch_range_enabled = true;
}

static bool ia64_load_watch_range_match(void)
{
    ia64_init_load_watch_range();
    return ia64_load_watch_range_enabled;
}

static void ia64_init_dbg_call_pcs(void)
{
    if (ia64_dbg_call_pc_inited) {
        return;
    }
    ia64_dbg_call_pc_inited = true;

    const char *s = getenv("QEMU_IA64_DBG_CALL_PC");
    if (!s || !*s) {
        return;
    }

    while (*s && ia64_dbg_call_pc_count < IA64_MAX_DBG_CALL_PCS) {
        while (*s && (isspace((unsigned char)*s) || *s == ',')) {
            s++;
        }
        if (!*s) {
            break;
        }
        char *endp = NULL;
        uint64_t pc = strtoull(s, &endp, 0);
        if (!endp || endp == s) {
            break;
        }
        ia64_dbg_call_pcs[ia64_dbg_call_pc_count++] = pc;
        s = endp;
    }
}

static bool ia64_dbg_call_pc_match(uint64_t pc)
{
    ia64_init_dbg_call_pcs();
    for (size_t i = 0; i < ia64_dbg_call_pc_count; i++) {
        if (ia64_dbg_call_pcs[i] == pc) {
            return true;
        }
    }
    return false;
}

#define IA64_MAX_DBG_PROBES 32
typedef struct IA64DbgProbePoint {
    uint64_t pc; /* bundle address */
    int8_t ri;   /* -1 matches any slot */
} IA64DbgProbePoint;

static bool ia64_dbg_probe_inited;
static IA64DbgProbePoint ia64_dbg_probes[IA64_MAX_DBG_PROBES];
static size_t ia64_dbg_probe_count;
static bool ia64_dbg_probe_range_inited;
static bool ia64_dbg_probe_range_enabled;
static uint64_t ia64_dbg_probe_range_lo;
static uint64_t ia64_dbg_probe_range_hi;

static int ia64_dbg_cmp_enabled = -1;
static uint64_t ia64_dbg_cmp_pc;
static bool ia64_dbg_cmp_pc_set;

static int ia64_dbg_sxt_enabled = -1;
static uint64_t ia64_dbg_sxt_pc;
static bool ia64_dbg_sxt_pc_set;

static int ia64_dbg_iunit_enabled = -1;
static uint64_t ia64_dbg_iunit_pc;
static bool ia64_dbg_iunit_pc_set;

static int ia64_dbg_bunit_enabled = -1;
static uint64_t ia64_dbg_bunit_pc;
static bool ia64_dbg_bunit_pc_set;
static int ia64_dbg_munit_enabled = -1;
static uint64_t ia64_dbg_munit_pc;
static bool ia64_dbg_munit_pc_set;
static int ia64_dbg_bret_enabled = -1;
static uint64_t ia64_dbg_bret_pc;
static bool ia64_dbg_bret_pc_set;

static bool ia64_dbg_cmp_match(uint64_t pc)
{
    if (ia64_dbg_cmp_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_CMP");
        ia64_dbg_cmp_enabled = (s && *s) ? 1 : 0;
        ia64_dbg_cmp_pc_set = false;
        ia64_dbg_cmp_pc = 0;
        const char *p = getenv("QEMU_IA64_DBG_CMP_PC");
        if (p && *p) {
            char *endp = NULL;
            uint64_t v = strtoull(p, &endp, 0);
            if (endp && endp != p) {
                ia64_dbg_cmp_pc_set = true;
                ia64_dbg_cmp_pc = v;
            }
        }
    }
    if (!ia64_dbg_cmp_enabled) {
        return false;
    }
    if (!ia64_dbg_cmp_pc_set) {
        return true;
    }
    return pc == ia64_dbg_cmp_pc;
}

static bool ia64_dbg_sxt_match(uint64_t pc)
{
    if (ia64_dbg_sxt_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_SXT");
        ia64_dbg_sxt_enabled = (s && *s) ? 1 : 0;
        ia64_dbg_sxt_pc_set = false;
        ia64_dbg_sxt_pc = 0;
        const char *p = getenv("QEMU_IA64_DBG_SXT_PC");
        if (p && *p) {
            char *endp = NULL;
            uint64_t v = strtoull(p, &endp, 0);
            if (endp && endp != p) {
                ia64_dbg_sxt_pc_set = true;
                ia64_dbg_sxt_pc = v;
            }
        }
    }
    if (!ia64_dbg_sxt_enabled) {
        return false;
    }
    if (!ia64_dbg_sxt_pc_set) {
        return true;
    }
    return pc == ia64_dbg_sxt_pc;
}

static bool ia64_dbg_iunit_match(uint64_t pc)
{
    if (ia64_dbg_iunit_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_IUNIT");
        ia64_dbg_iunit_enabled = (s && *s) ? 1 : 0;
        ia64_dbg_iunit_pc_set = false;
        ia64_dbg_iunit_pc = 0;
        const char *p = getenv("QEMU_IA64_DBG_IUNIT_PC");
        if (p && *p) {
            char *endp = NULL;
            uint64_t v = strtoull(p, &endp, 0);
            if (endp && endp != p) {
                ia64_dbg_iunit_pc_set = true;
                ia64_dbg_iunit_pc = v;
            }
        }
    }
    if (!ia64_dbg_iunit_enabled) {
        return false;
    }
    if (!ia64_dbg_iunit_pc_set) {
        return true;
    }
    return pc == ia64_dbg_iunit_pc;
}

static bool ia64_dbg_bunit_match(uint64_t pc)
{
    if (ia64_dbg_bunit_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_BUNIT");
        ia64_dbg_bunit_enabled = (s && *s) ? 1 : 0;
        ia64_dbg_bunit_pc_set = false;
        ia64_dbg_bunit_pc = 0;
        const char *p = getenv("QEMU_IA64_DBG_BUNIT_PC");
        if (p && *p) {
            char *endp = NULL;
            uint64_t v = strtoull(p, &endp, 0);
            if (endp && endp != p) {
                ia64_dbg_bunit_pc_set = true;
                ia64_dbg_bunit_pc = v;
            }
        }
    }
    if (!ia64_dbg_bunit_enabled) {
        return false;
    }
    if (!ia64_dbg_bunit_pc_set) {
        return true;
    }
    return pc == ia64_dbg_bunit_pc;
}

static bool ia64_dbg_bret_match(uint64_t pc)
{
    if (ia64_dbg_bret_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_BRET");
        ia64_dbg_bret_enabled = (s && *s) ? 1 : 0;
        ia64_dbg_bret_pc_set = false;
        ia64_dbg_bret_pc = 0;
        const char *p = getenv("QEMU_IA64_DBG_BRET_PC");
        if (p && *p) {
            char *endp = NULL;
            uint64_t v = strtoull(p, &endp, 0);
            if (endp && endp != p) {
                ia64_dbg_bret_pc_set = true;
                ia64_dbg_bret_pc = v;
            }
        }
    }
    if (!ia64_dbg_bret_enabled) {
        return false;
    }
    if (!ia64_dbg_bret_pc_set) {
        return true;
    }
    return pc == ia64_dbg_bret_pc;
}

static bool ia64_dbg_munit_match(uint64_t pc)
{
    if (ia64_dbg_munit_enabled == -1) {
        const char *s = getenv("QEMU_IA64_DBG_MUNIT");
        ia64_dbg_munit_enabled = (s && *s) ? 1 : 0;
        ia64_dbg_munit_pc_set = false;
        ia64_dbg_munit_pc = 0;
        const char *p = getenv("QEMU_IA64_DBG_MUNIT_PC");
        if (p && *p) {
            char *endp = NULL;
            uint64_t v = strtoull(p, &endp, 0);
            if (endp && endp != p) {
                ia64_dbg_munit_pc_set = true;
                ia64_dbg_munit_pc = v;
            }
        }
    }
    if (!ia64_dbg_munit_enabled) {
        return false;
    }
    if (!ia64_dbg_munit_pc_set) {
        return true;
    }
    return pc == ia64_dbg_munit_pc;
}

static void ia64_init_dbg_probes(void)
{
    if (ia64_dbg_probe_inited) {
        return;
    }
    ia64_dbg_probe_inited = true;

    const char *s = getenv("QEMU_IA64_DBG_PROBE");
    if (!s || !*s) {
        return;
    }

    /*
     * Accept a comma/space-separated list of addresses.
     *
     * - If written as "<pc>:<ri>" or "<pc>@<ri>", probe that slot (ri=0..2).
     * - If written as an objdump-style instruction address (pc+0/6/12),
     *   derive ri from the low nibble and canonicalize to the bundle address.
     * - Otherwise probe all slots in that bundle.
     */
    while (*s && ia64_dbg_probe_count < IA64_MAX_DBG_PROBES) {
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
        s = endp;

        int8_t ri = -1;
        if (*s == ':' || *s == '@') {
            s++;
            long v = strtol(s, &endp, 0);
            if (endp && endp != s && v >= 0 && v <= 2) {
                ri = (int8_t)v;
            }
            s = endp ? endp : s;
        } else {
            uint8_t off = raw_pc & 0xF;
            if (off == 0x0) {
                ri = 0;
            } else if (off == 0x6) {
                ri = 1;
            } else if (off == 0xC) {
                ri = 2;
            }
        }

        ia64_dbg_probes[ia64_dbg_probe_count++] = (IA64DbgProbePoint){
            .pc = raw_pc & ~0xFULL,
            .ri = ri,
        };
    }
}

static void ia64_init_dbg_probe_range(void)
{
    if (ia64_dbg_probe_range_inited) {
        return;
    }
    ia64_dbg_probe_range_inited = true;

    const char *s = getenv("QEMU_IA64_DBG_PROBE_RANGE");
    if (!s || !*s) {
        return;
    }

    char *endp = NULL;
    uint64_t lo = strtoull(s, &endp, 0);
    if (!endp || endp == s) {
        return;
    }
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
    if (!*endp) {
        return;
    }
    char *endp2 = NULL;
    uint64_t hi = strtoull(endp, &endp2, 0);
    if (!endp2 || endp2 == endp) {
        return;
    }

    lo &= ~0xFULL;
    hi &= ~0xFULL;
    if (hi < lo) {
        uint64_t tmp = lo;
        lo = hi;
        hi = tmp;
    }

    ia64_dbg_probe_range_lo = lo;
    ia64_dbg_probe_range_hi = hi;
    ia64_dbg_probe_range_enabled = true;
}

static bool ia64_dbg_probe_match(uint64_t pc, int ri)
{
    ia64_init_dbg_probes();
    for (size_t i = 0; i < ia64_dbg_probe_count; i++) {
        if (ia64_dbg_probes[i].pc != pc) {
            continue;
        }
        if (ia64_dbg_probes[i].ri == -1 || ia64_dbg_probes[i].ri == ri) {
            return true;
        }
    }
    ia64_init_dbg_probe_range();
    if (ia64_dbg_probe_range_enabled &&
        pc >= ia64_dbg_probe_range_lo &&
        pc <= ia64_dbg_probe_range_hi) {
        return true;
    }
    return false;
}

static bool ia64_store_watch_inited;
static bool ia64_store_watch_enabled;
static uint64_t ia64_store_watch_addr;
static bool ia64_store_watch_range_inited;
static bool ia64_store_watch_range_enabled;
static uint64_t ia64_store_watch_range_lo;
static uint64_t ia64_store_watch_range_hi;
static bool ia64_store_watch_value_inited;
static bool ia64_store_watch_value_enabled;
static uint64_t ia64_store_watch_value;
static uint64_t ia64_store_watch_value_mask;
static bool ia64_watch_r33_inited;
static bool ia64_watch_r33_enabled;
static bool ia64_pei_store_watch_inited;
static bool ia64_pei_store_watch_enabled;
static bool ia64_pei_load_watch_inited;
static bool ia64_pei_load_watch_enabled;

static void ia64_init_store_watch(void)
{
    if (ia64_store_watch_inited) {
        return;
    }
    ia64_store_watch_inited = true;

    const char *s = getenv("QEMU_IA64_WATCH_STORE");
    if (!s || !*s) {
        return;
    }
    char *endp = NULL;
    ia64_store_watch_addr = strtoull(s, &endp, 0);
    if (endp == s) {
        return;
    }
    ia64_store_watch_enabled = true;
}

static bool ia64_store_watch_match(void)
{
    ia64_init_store_watch();
    return ia64_store_watch_enabled;
}

static void ia64_init_store_watch_range(void)
{
    if (ia64_store_watch_range_inited) {
        return;
    }
    ia64_store_watch_range_inited = true;

    const char *s = getenv("QEMU_IA64_WATCH_STORE_RANGE");
    if (!s || !*s) {
        return;
    }

    char *endp = NULL;
    uint64_t lo = strtoull(s, &endp, 0);
    if (!endp || endp == s) {
        return;
    }
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
    if (!*endp) {
        return;
    }
    char *endp2 = NULL;
    uint64_t hi = strtoull(endp, &endp2, 0);
    if (!endp2 || endp2 == endp) {
        return;
    }
    if (hi <= lo) {
        return;
    }

    ia64_store_watch_range_lo = lo;
    ia64_store_watch_range_hi = hi;
    ia64_store_watch_range_enabled = true;
}

static bool ia64_store_watch_range_match(void)
{
    ia64_init_store_watch_range();
    return ia64_store_watch_range_enabled;
}

static void ia64_init_store_watch_value(void)
{
    if (ia64_store_watch_value_inited) {
        return;
    }
    ia64_store_watch_value_inited = true;

    const char *s = getenv("QEMU_IA64_WATCH_STORE_VALUE");
    if (!s || !*s) {
        return;
    }
    char *endp = NULL;
    ia64_store_watch_value = strtoull(s, &endp, 0);
    if (endp == s) {
        return;
    }
    ia64_store_watch_value_enabled = true;

    const char *m = getenv("QEMU_IA64_WATCH_STORE_VALUE_MASK");
    if (m && *m) {
        char *endm = NULL;
        ia64_store_watch_value_mask = strtoull(m, &endm, 0);
        if (endm == m) {
            ia64_store_watch_value_mask = (1ULL << 61) - 1;
        }
    } else {
        ia64_store_watch_value_mask = (1ULL << 61) - 1;
    }
}

static bool ia64_store_watch_value_match(void)
{
    ia64_init_store_watch_value();
    return ia64_store_watch_value_enabled;
}

static bool ia64_watch_r33_match(void)
{
    if (!ia64_watch_r33_inited) {
        ia64_watch_r33_inited = true;
        const char *s = getenv("QEMU_IA64_WATCH_R33");
        ia64_watch_r33_enabled = (s && *s) ? true : false;
    }
    return ia64_watch_r33_enabled;
}

static bool ia64_pei_store_watch_match(void)
{
    if (!ia64_pei_store_watch_inited) {
        ia64_pei_store_watch_inited = true;
        const char *s = getenv("QEMU_IA64_PEI_STORE_WATCH");
        ia64_pei_store_watch_enabled = (s && *s) ? true : false;
    }
    return ia64_pei_store_watch_enabled;
}

static bool ia64_pei_load_watch_match(void)
{
    if (!ia64_pei_load_watch_inited) {
        ia64_pei_load_watch_inited = true;
        const char *s = getenv("QEMU_IA64_PEI_LOAD_WATCH");
        ia64_pei_load_watch_enabled = (s && *s) ? true : false;
    }
    return ia64_pei_load_watch_enabled;
}

static TCGv_i64 gen_phys_mode_addr(TCGv_i64 addr)
{
    /*
     * Mirror ia64_phys_mode_addr() for debugging instrumentation.
     *
     * - If canonically sign-extended 32-bit, or the firmware's 0x7fffffff
     *   prefix pattern, treat as 32-bit physical address.
     * - Otherwise mask off region bits (low 61 bits).
     */
    TCGv_i64 hi32 = tcg_temp_new_i64();
    tcg_gen_andi_i64(hi32, addr, 0xffffffff00000000ULL);

    TCGv_i64 out = tcg_temp_new_i64();
    TCGLabel *use_low32 = gen_new_label();
    TCGLabel *done = gen_new_label();
    tcg_gen_brcondi_i64(TCG_COND_EQ, hi32, 0, use_low32);
    tcg_gen_brcondi_i64(TCG_COND_EQ, hi32, 0xffffffff00000000ULL, use_low32);
    tcg_gen_brcondi_i64(TCG_COND_EQ, hi32, 0x7fffffff00000000ULL, use_low32);
    tcg_gen_andi_i64(out, addr, (1ULL << 61) - 1);
    tcg_gen_br(done);
    gen_set_label(use_low32);
    tcg_gen_andi_i64(out, addr, 0xffffffffULL);
    gen_set_label(done);
    return out;
}

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
    cpu_cr_iva = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr[2]),
                                        "cr_iva");
    cpu_cr_ifs = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_ifs),
                                        "cr_ifs");
    cpu_cr_isr = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_isr),
                                        "cr_isr");
    cpu_cr_ifa = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_ifa),
                                        "cr_ifa");
    cpu_cr_iim = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_iim),
                                        "cr_iim");
    cpu_cr_iha = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUIA64State, cr_iha),
                                        "cr_iha");
    
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

    for (int i = 0; i < 8; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "rr%d", i);
        cpu_rr[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPUIA64State, rr[i]),
                                           g_strdup(buf));
    }
}

typedef struct DisasContext {
    DisasContextBase base;
    CPUIA64State *env;
    int ri;
    uint64_t extra_bits;
    int mem_idx;
} DisasContext;

static TCGv_i64 gen_mem_addr(DisasContext *ctx, TCGv_i64 addr)
{
    if (ctx->mem_idx == MMU_PHYS_IDX) {
        return gen_phys_mode_addr(addr);
    }
    return addr;
}

static void gen_load_watch(DisasContext *ctx, TCGv_i64 addr, TCGv_i64 val,
                           uint32_t size)
{
    uint64_t watch_addr = ia64_get_watch_load_addr();
    bool watch_range = ia64_load_watch_range_match();

    if (!watch_addr && !watch_range && !ia64_pei_load_watch_match()) {
        return;
    }

    TCGLabel *done = gen_new_label();
    TCGLabel *log = gen_new_label();

    if (watch_addr && watch_range) {
        tcg_gen_brcondi_i64(TCG_COND_EQ, addr, watch_addr, log);
        tcg_gen_brcondi_i64(TCG_COND_LT, addr, ia64_load_watch_range_lo, done);
        tcg_gen_brcondi_i64(TCG_COND_GE, addr, ia64_load_watch_range_hi, done);
        gen_set_label(log);
    } else if (watch_addr) {
        tcg_gen_brcondi_i64(TCG_COND_NE, addr, watch_addr, done);
    } else {
        tcg_gen_brcondi_i64(TCG_COND_LT, addr, ia64_load_watch_range_lo, done);
        tcg_gen_brcondi_i64(TCG_COND_GE, addr, ia64_load_watch_range_hi, done);
    }

    if (watch_addr || watch_range) {
        gen_helper_dbg_load_watch(tcg_env,
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(ctx->ri),
                                  addr,
                                  tcg_constant_i32(size),
                                  val);
    }
    if (ia64_pei_load_watch_match()) {
        gen_helper_fw_pei_load_watch(tcg_env,
                                     tcg_constant_i64(ctx->base.pc_next),
                                     tcg_constant_i32(ctx->ri),
                                     addr,
                                     tcg_constant_i32(size),
                                     val);
    }
    gen_set_label(done);
}

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

/*
 * Stop bits per template (matches SKI's templates[] SB_Stop entries).
 * Bit 0: stop after slot0, bit 1: stop after slot1, bit 2: stop after slot2.
 */
static const uint8_t template_stop_mask[32] = {
    0x0, 0x4, 0x2, 0x6, 0x0, 0x4, 0x0, 0x0,
    0x0, 0x4, 0x1, 0x5, 0x0, 0x4, 0x0, 0x4,
    0x0, 0x4, 0x0, 0x4, 0x0, 0x0, 0x0, 0x4,
    0x0, 0x4, 0x0, 0x0, 0x0, 0x4, 0x0, 0x0,
};

static TCGLabel *gen_qp_skip(uint8_t qp);

static bool ia64_pc_in_fw(uint64_t pc)
{
    const uint64_t fw_base = 0x00000000ff000000ULL;
    const uint64_t fw_end = fw_base + (16ULL << 20);
    const uint64_t fw_wr_base = 0x0000000100000000ULL;
    const uint64_t fw_wr_end = fw_wr_base + (16ULL << 20);
    uint64_t hi32 = pc & 0xffffffff00000000ULL;
    uint64_t phys = (hi32 == 0 || hi32 == 0xffffffff00000000ULL) ?
                    (uint32_t)pc : (pc & ((1ULL << 61) - 1));
    return (phys >= fw_base && phys < fw_end) ||
           (phys >= fw_wr_base && phys < fw_wr_end);
}

static bool ia64_fw_break_hypercall_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_BREAK_HYPERCALL");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static void ia64_fw_break_hypercall_log(uint64_t pc, uint64_t imm, bool in_fw,
                                        bool enabled)
{
    static int log_enabled = -1;
    static int log_count;
    if (log_enabled == -1) {
        const char *s = getenv("QEMU_IA64_FW_BREAK_LOG");
        log_enabled = (s && *s) ? 1 : 0;
    }
    if (!log_enabled || log_count++ >= 16) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: fw_break pc=%016" PRIx64 " imm=%08" PRIx64
                  " in_fw=%d hypercall=%d\n",
                  pc, imm, in_fw ? 1 : 0, enabled ? 1 : 0);
}

static void gen_unimpl(DisasContext *ctx, uint64_t insn, const char *msg)
{
    uint8_t qp = insn & 0x3f;
    TCGLabel *skip = NULL;
    if (qp != 0) {
        skip = gen_qp_skip(qp);
    }
    gen_helper_unimpl(tcg_env,
                      tcg_constant_i64(ctx->base.pc_next),
                      tcg_constant_i32(ctx->ri),
                      tcg_constant_i64(insn),
                      tcg_constant_i64((uintptr_t)(msg ? msg : "")));
    tcg_gen_exit_tb(NULL, 0);
    if (skip) {
        gen_set_label(skip);
    } else {
        ctx->base.is_jmp = DISAS_NORETURN;
    }
}

static void gen_record_branch(DisasContext *ctx, uint64_t insn,
                              uint8_t kind, TCGv_i64 to)
{
    tcg_gen_st_i64(tcg_constant_i64(ctx->base.pc_next),
                   tcg_env, offsetof(CPUIA64State, last_branch_from));
    tcg_gen_st_i64(to, tcg_env, offsetof(CPUIA64State, last_branch_to));
    tcg_gen_st_i64(tcg_constant_i64(insn),
                   tcg_env, offsetof(CPUIA64State, last_branch_insn));
    tcg_gen_st_i64(tcg_constant_i64((uint64_t)kind | ((uint64_t)ctx->ri << 8)),
                   tcg_env, offsetof(CPUIA64State, last_branch_kind));
}

static void gen_record_b0_write(DisasContext *ctx, uint64_t insn,
                                uint8_t kind, TCGv_i64 val)
{
    TCGv_i64 old = tcg_temp_new_i64();

    tcg_gen_ld_i64(old, tcg_env, offsetof(CPUIA64State, last_b0_write_pc));
    tcg_gen_st_i64(old, tcg_env, offsetof(CPUIA64State, prev_b0_write_pc));
    tcg_gen_ld_i64(old, tcg_env, offsetof(CPUIA64State, last_b0_write_val));
    tcg_gen_st_i64(old, tcg_env, offsetof(CPUIA64State, prev_b0_write_val));
    tcg_gen_ld_i64(old, tcg_env, offsetof(CPUIA64State, last_b0_write_insn));
    tcg_gen_st_i64(old, tcg_env, offsetof(CPUIA64State, prev_b0_write_insn));
    tcg_gen_ld_i64(old, tcg_env, offsetof(CPUIA64State, last_b0_write_kind));
    tcg_gen_st_i64(old, tcg_env, offsetof(CPUIA64State, prev_b0_write_kind));

    tcg_gen_st_i64(tcg_constant_i64(ctx->base.pc_next),
                   tcg_env, offsetof(CPUIA64State, last_b0_write_pc));
    tcg_gen_st_i64(val, tcg_env, offsetof(CPUIA64State, last_b0_write_val));
    tcg_gen_st_i64(tcg_constant_i64(insn),
                   tcg_env, offsetof(CPUIA64State, last_b0_write_insn));
    tcg_gen_st_i64(tcg_constant_i64((uint64_t)kind | ((uint64_t)ctx->ri << 8)),
                   tcg_env, offsetof(CPUIA64State, last_b0_write_kind));

    gen_helper_record_b0_trace(tcg_env,
                               tcg_constant_i64(ctx->base.pc_next),
                               tcg_constant_i64(insn),
                               tcg_constant_i64((uint64_t)kind |
                                                ((uint64_t)ctx->ri << 8)),
                               val);
}

static MemOp memop_for_size_idx(unsigned size_idx)
{
    switch (size_idx & 0x3) {
    case 0:
        return MO_TE | MO_UB;
    case 1:
        return MO_TE | MO_UW;
    case 2:
        return MO_TE | MO_UL;
    case 3:
    default:
        return MO_TE | MO_64;
    }
}

static void gen_set_ri_const(uint8_t ri)
{
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_andi_i64(t, cpu_psr, ~PSR_RI_MASK);
    tcg_gen_ori_i64(t, t, ((uint64_t)(ri & 3) << PSR_RI_SHIFT));
    tcg_gen_mov_i64(cpu_psr, t);
}

static void gen_a_unit_nat1(uint8_t dst, uint8_t src)
{
    if (dst == 0) {
        return;
    }
    TCGv_i64 nat = tcg_temp_new_i64();
    gen_helper_gr_nat(nat, tcg_env, tcg_constant_i32(src));
    gen_helper_gr_nat_set(tcg_env, tcg_constant_i32(dst), nat);
}

static void gen_a_unit_nat2(uint8_t dst, uint8_t src1, uint8_t src2)
{
    if (dst == 0) {
        return;
    }
    TCGv_i64 nat_a = tcg_temp_new_i64();
    TCGv_i64 nat_b = tcg_temp_new_i64();
    TCGv_i64 nat = tcg_temp_new_i64();
    gen_helper_gr_nat(nat_a, tcg_env, tcg_constant_i32(src1));
    gen_helper_gr_nat(nat_b, tcg_env, tcg_constant_i32(src2));
    tcg_gen_or_i64(nat, nat_a, nat_b);
    gen_helper_gr_nat_set(tcg_env, tcg_constant_i32(dst), nat);
}

/* CFM layout matches SKI's cfmGet(): rrbs + (sor>>3) + soil + sof. */
#define IA64_CFM_SOF_SHIFT   0
#define IA64_CFM_SOF_MASK    0x7fULL
#define IA64_CFM_SOL_SHIFT   7
#define IA64_CFM_SOL_MASK    0x7fULL
#define IA64_CFM_SOR_SHIFT   14
#define IA64_CFM_SOR_MASK    0x0fULL /* SOR/8 */
#define IA64_CFM_RRBG_SHIFT  18
#define IA64_CFM_RRBG_MASK   0x7fULL
#define IA64_CFM_RRBF_SHIFT  25
#define IA64_CFM_RRBF_MASK   0x7fULL
#define IA64_CFM_RRBP_SHIFT  32
#define IA64_CFM_RRBP_MASK   0x3fULL

#define IA64_PR_ROT_BASE     16
#define IA64_PR_ROT_SIZE     48
#define IA64_FR_ROT_SIZE     96

static TCGv_i64 gen_pr_phys_index(uint8_t pr)
{
    if (pr < IA64_PR_ROT_BASE) {
        return tcg_constant_i64(pr);
    }

    /*
     * Rotating predicates: phys = pr + rrbp; wrap into [16,63] by subtracting 48
     * if it overflows (matches SKI's PrRd/PrWrt).
     */
    TCGv_i64 idx = tcg_temp_new_i64();
    TCGv_i64 rrbp = tcg_temp_new_i64();
    tcg_gen_shri_i64(rrbp, cpu_cfm, IA64_CFM_RRBP_SHIFT);
    tcg_gen_andi_i64(rrbp, rrbp, IA64_CFM_RRBP_MASK);
    tcg_gen_addi_i64(idx, rrbp, pr);

    TCGLabel *in_range = gen_new_label();
    tcg_gen_brcondi_i64(TCG_COND_LT, idx, 64, in_range);
    tcg_gen_addi_i64(idx, idx, -IA64_PR_ROT_SIZE);
    gen_set_label(in_range);
    return idx;
}

static TCGv_i64 gen_pr_read_bit(uint8_t pr)
{
    if (pr == 0) {
        return tcg_constant_i64(1);
    }

    TCGv_i64 idx = gen_pr_phys_index(pr);
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_shr_i64(t, cpu_pr, idx);
    tcg_gen_andi_i64(t, t, 1);
    return t;
}

static void gen_pr_write_bit(uint8_t pr, TCGv_i64 val01)
{
    if (pr == 0) {
        return;
    }

    TCGv_i64 idx = gen_pr_phys_index(pr);
    TCGv_i64 mask = tcg_temp_new_i64();
    tcg_gen_movi_i64(mask, 1);
    tcg_gen_shl_i64(mask, mask, idx);

    TCGv_i64 t_pr = tcg_temp_new_i64();
    tcg_gen_mov_i64(t_pr, cpu_pr);
    tcg_gen_andc_i64(t_pr, t_pr, mask);

    /* mask & (- (val01 & 1)) yields mask or 0 without branches. */
    TCGv_i64 v = tcg_temp_new_i64();
    tcg_gen_andi_i64(v, val01, 1);
    tcg_gen_neg_i64(v, v);
    tcg_gen_and_i64(v, v, mask);
    tcg_gen_or_i64(t_pr, t_pr, v);
    tcg_gen_ori_i64(t_pr, t_pr, 1); /* p0 always true */
    tcg_gen_mov_i64(cpu_pr, t_pr);
}

static TCGLabel *gen_qp_skip(uint8_t qp)
{
    if (qp == 0) {
        return NULL;
    }
    TCGLabel *skip = gen_new_label();
    TCGv_i64 t_qp = gen_pr_read_bit(qp);
    tcg_gen_brcondi_i64(TCG_COND_EQ, t_qp, 0, skip);
    return skip;
}

static void gen_set_predicates(uint8_t p_true, uint8_t p_false, TCGv_i64 cond)
{
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_andi_i64(t, cond, 1);

    if (p_true != 0) {
        gen_pr_write_bit(p_true, t);
    }
    if (p_false != 0) {
        TCGv_i64 f = tcg_temp_new_i64();
        tcg_gen_xori_i64(f, t, 1);
        gen_pr_write_bit(p_false, f);
    }
}

typedef enum IA64PredCombineMode {
    IA64_PRED_COMBINE_AND,
    IA64_PRED_COMBINE_OR,
    IA64_PRED_COMBINE_OR_ANDCM,
} IA64PredCombineMode;

static void gen_combine_predicates(uint8_t p_true, uint8_t p_false,
                                   TCGv_i64 cond, IA64PredCombineMode mode)
{
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_andi_i64(t, cond, 1);
    TCGv_i64 f = tcg_temp_new_i64();
    tcg_gen_xori_i64(f, t, 1);

    if (p_true != 0) {
        TCGv_i64 old = gen_pr_read_bit(p_true);
        TCGv_i64 newv = tcg_temp_new_i64();
        switch (mode) {
        case IA64_PRED_COMBINE_AND:
            tcg_gen_and_i64(newv, old, t);
            break;
        case IA64_PRED_COMBINE_OR:
        case IA64_PRED_COMBINE_OR_ANDCM:
            tcg_gen_or_i64(newv, old, t);
            break;
        default:
            g_assert_not_reached();
        }
        gen_pr_write_bit(p_true, newv);
    }

    if (p_false != 0) {
        TCGv_i64 old = gen_pr_read_bit(p_false);
        TCGv_i64 newv = tcg_temp_new_i64();
        switch (mode) {
        case IA64_PRED_COMBINE_AND:
        case IA64_PRED_COMBINE_OR_ANDCM:
            tcg_gen_and_i64(newv, old, f);
            break;
        case IA64_PRED_COMBINE_OR:
            tcg_gen_or_i64(newv, old, f);
            break;
        default:
            g_assert_not_reached();
        }
        gen_pr_write_bit(p_false, newv);
    }
}

static TCGv_i64 gen_load_ar(uint8_t idx)
{
    if (idx == IA64_AR_ITC) {
        TCGv_i64 t = tcg_temp_new_i64();
        gen_helper_get_itc(t, tcg_env);
        return t;
    }
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_ld_i64(t, tcg_env, offsetof(CPUIA64State, ar) + idx * 8);
    return t;
}

static void gen_store_ar(uint8_t idx, TCGv_i64 v)
{
    if (idx == IA64_AR_ITC) {
        gen_helper_set_itc(tcg_env, v);
        return;
    }
    tcg_gen_st_i64(v, tcg_env, offsetof(CPUIA64State, ar) + idx * 8);
}

static void gen_fr_load_lo(TCGv_i64 dst, uint8_t f)
{
    gen_helper_fr_get_lo(dst, tcg_env, tcg_constant_i32(f));
}

static void gen_fr_load_hi(TCGv_i64 dst, uint8_t f)
{
    gen_helper_fr_get_hi(dst, tcg_env, tcg_constant_i32(f));
}

static void gen_fr_store_lo(uint8_t f, TCGv_i64 val)
{
    gen_helper_fr_set_lo(tcg_env, tcg_constant_i32(f), val);
}

static void gen_fr_store_hi(uint8_t f, TCGv_i64 val)
{
    gen_helper_fr_set_hi(tcg_env, tcg_constant_i32(f), val);
}

static void gen_rotate_regs(void)
{
    /*
     * Rotate register rename bases (rrbg/rrbf/rrbp) as in SKI's rotate_regs().
     * This updates the architectural CFM.
     */
    TCGv_i64 cfm = tcg_temp_new_i64();
    tcg_gen_mov_i64(cfm, cpu_cfm);

    /* rrbg rotates only when SOR!=0. */
    TCGv_i64 sor8 = tcg_temp_new_i64();
    tcg_gen_shri_i64(sor8, cfm, IA64_CFM_SOR_SHIFT);
    tcg_gen_andi_i64(sor8, sor8, IA64_CFM_SOR_MASK);

    TCGLabel *skip_rrbg = gen_new_label();
    tcg_gen_brcondi_i64(TCG_COND_EQ, sor8, 0, skip_rrbg);
    {
        TCGv_i64 sor = tcg_temp_new_i64();
        tcg_gen_shli_i64(sor, sor8, 3);

        TCGv_i64 rrbg = tcg_temp_new_i64();
        tcg_gen_shri_i64(rrbg, cfm, IA64_CFM_RRBG_SHIFT);
        tcg_gen_andi_i64(rrbg, rrbg, IA64_CFM_RRBG_MASK);

        TCGv_i64 rrbg1 = tcg_temp_new_i64();
        TCGLabel *rrbg_not_zero = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_NE, rrbg, 0, rrbg_not_zero);
        tcg_gen_addi_i64(rrbg1, sor, -1);
        TCGLabel *rrbg_done = gen_new_label();
        tcg_gen_br(rrbg_done);
        gen_set_label(rrbg_not_zero);
        tcg_gen_addi_i64(rrbg1, rrbg, -1);
        gen_set_label(rrbg_done);

        /* Write rrbg back into CFM. */
        tcg_gen_andi_i64(cfm, cfm, ~(IA64_CFM_RRBG_MASK << IA64_CFM_RRBG_SHIFT));
        TCGv_i64 sh = tcg_temp_new_i64();
        tcg_gen_shli_i64(sh, rrbg1, IA64_CFM_RRBG_SHIFT);
        tcg_gen_or_i64(cfm, cfm, sh);
    }
    gen_set_label(skip_rrbg);

    /* rrbf rotates modulo 96. */
    {
        TCGv_i64 rrbf = tcg_temp_new_i64();
        tcg_gen_shri_i64(rrbf, cfm, IA64_CFM_RRBF_SHIFT);
        tcg_gen_andi_i64(rrbf, rrbf, IA64_CFM_RRBF_MASK);

        TCGv_i64 rrbf1 = tcg_temp_new_i64();
        TCGLabel *rrbf_not_zero = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_NE, rrbf, 0, rrbf_not_zero);
        tcg_gen_movi_i64(rrbf1, IA64_FR_ROT_SIZE - 1);
        TCGLabel *rrbf_done = gen_new_label();
        tcg_gen_br(rrbf_done);
        gen_set_label(rrbf_not_zero);
        tcg_gen_addi_i64(rrbf1, rrbf, -1);
        gen_set_label(rrbf_done);

        tcg_gen_andi_i64(cfm, cfm, ~(IA64_CFM_RRBF_MASK << IA64_CFM_RRBF_SHIFT));
        TCGv_i64 sh = tcg_temp_new_i64();
        tcg_gen_shli_i64(sh, rrbf1, IA64_CFM_RRBF_SHIFT);
        tcg_gen_or_i64(cfm, cfm, sh);
    }

    /* rrbp rotates modulo 48. */
    {
        TCGv_i64 rrbp = tcg_temp_new_i64();
        tcg_gen_shri_i64(rrbp, cfm, IA64_CFM_RRBP_SHIFT);
        tcg_gen_andi_i64(rrbp, rrbp, IA64_CFM_RRBP_MASK);

        TCGv_i64 rrbp1 = tcg_temp_new_i64();
        TCGLabel *rrbp_not_zero = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_NE, rrbp, 0, rrbp_not_zero);
        tcg_gen_movi_i64(rrbp1, IA64_PR_ROT_SIZE - 1);
        TCGLabel *rrbp_done = gen_new_label();
        tcg_gen_br(rrbp_done);
        gen_set_label(rrbp_not_zero);
        tcg_gen_addi_i64(rrbp1, rrbp, -1);
        gen_set_label(rrbp_done);

        tcg_gen_andi_i64(cfm, cfm, ~(IA64_CFM_RRBP_MASK << IA64_CFM_RRBP_SHIFT));
        TCGv_i64 sh = tcg_temp_new_i64();
        tcg_gen_shli_i64(sh, rrbp1, IA64_CFM_RRBP_SHIFT);
        tcg_gen_or_i64(cfm, cfm, sh);
    }

    tcg_gen_mov_i64(cpu_cfm, cfm);

    /*
     * Rotating GRs (when SOR!=0): our translator currently indexes stacked
     * registers directly. Mirror SKI's RRBG-based mapping by physically
     * rotating the GR32..(GR32+SOR-1) window in the helper.
     */
    {
        TCGLabel *skip = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_EQ, sor8, 0, skip);
        gen_helper_rotate_grs(tcg_env);
        gen_set_label(skip);
    }
}

static TCGv_i64 gen_load_cr_reg(uint8_t idx)
{
    switch (idx) {
    case 16: return cpu_cr_ipsr;
    case 17: return cpu_cr_isr;
    case 2:  return cpu_cr_iva;
    case 19: return cpu_cr_iip;
    case 20: return cpu_cr_ifa;
    case 23: return cpu_cr_ifs;
    case 24: return cpu_cr_iim;
    case 25: return cpu_cr_iha;
    case 65: /* cr.ivr: virtual interrupt request vector */
        break;
    default:
        break;
    }
    TCGv_i64 t = tcg_temp_new_i64();
    tcg_gen_ld_i64(t, tcg_env, offsetof(CPUIA64State, cr) + idx * 8);
    return t;
}

static void gen_store_cr_reg(uint8_t idx, TCGv_i64 v)
{
    switch (idx) {
    case 1:  /* cr.itm */
        gen_helper_set_itm(tcg_env, v);
        return;
    case 67: /* cr.eoi */
        gen_helper_eoi(tcg_env);
        return;
    case 72: /* cr.itv */
        gen_helper_set_itv(tcg_env, v);
        return;
    case 16: tcg_gen_mov_i64(cpu_cr_ipsr, v); break;
    case 17: tcg_gen_mov_i64(cpu_cr_isr, v); break;
    case 2:  tcg_gen_mov_i64(cpu_cr_iva, v); break;
    case 19: tcg_gen_mov_i64(cpu_cr_iip, v); break;
    case 20: tcg_gen_mov_i64(cpu_cr_ifa, v); break;
    case 23: tcg_gen_mov_i64(cpu_cr_ifs, v); break;
    case 24: tcg_gen_mov_i64(cpu_cr_iim, v); break;
    case 25: tcg_gen_mov_i64(cpu_cr_iha, v); break;
    case 21: /* cr.itir */ break;
    case 22: /* cr.iipa */ break;
    default:
        break;
    }
    tcg_gen_st_i64(v, tcg_env, offsetof(CPUIA64State, cr) + idx * 8);
}

static void gen_store_rr_reg(TCGv_i64 idx_reg, TCGv_i64 val)
{
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_shri_i64(tmp, idx_reg, 61);
    for (int i = 0; i < 8; i++) {
        TCGLabel *skip = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_NE, tmp, i, skip);
        tcg_gen_mov_i64(cpu_rr[i], val);
        gen_set_label(skip);
    }
}

static void gen_load_rr_reg(TCGv_i64 dest, TCGv_i64 idx_reg)
{
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_shri_i64(tmp, idx_reg, 61);
    for (int i = 0; i < 8; i++) {
        TCGLabel *skip = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_NE, tmp, i, skip);
        tcg_gen_mov_i64(dest, cpu_rr[i]);
        gen_set_label(skip);
    }
}

static void ia64_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    uint64_t tb_flags = ctx->base.tb->flags;
    ctx->env = cpu_env(cs);
    ctx->ri = tb_flags & 3;
    ctx->extra_bits = 0;
    /*
     * Select the data translation mode from TB flags.
     *
     * TB translation bakes in a constant MMU index for loads/stores. Include
     * PSR.DT and PSR.CPL in the TB key (see ia64_get_tb_cpu_state) and use
     * those bits here so TBs are never reused across incompatible modes.
     */
    if (!(tb_flags & (1ULL << 2))) { /* PSR.DT */
        ctx->mem_idx = MMU_PHYS_IDX;
    } else {
        uint8_t cpl = (tb_flags >> 4) & 3; /* PSR.CPL */
        ctx->mem_idx = (cpl == 3) ? MMU_USER_IDX : MMU_KERNEL_IDX;
    }
}

static void ia64_tr_tb_start(DisasContextBase *db, CPUState *cpu)
{
    DisasContext *ctx = container_of(db, DisasContext, base);

#ifndef CONFIG_USER_ONLY
    if (ctx->base.pc_next == 0) {
        gen_helper_null_pc_abort(tcg_env,
                                 tcg_constant_i64(ctx->base.pc_next),
                                 tcg_constant_i32(ctx->ri));
    }
    if (ia64_get_fw_fastpath_enabled()) {
        TCGv_i32 handled = tcg_temp_new_i32();
        gen_helper_fw_fastpath(handled, tcg_env,
                               tcg_constant_i64(ctx->base.pc_next),
                               tcg_constant_i32(ctx->ri));
        TCGLabel *skip = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_EQ, handled, 0, skip);
        tcg_gen_exit_tb(NULL, 0);
        gen_set_label(skip);
    }

#endif

    uint64_t hang_threshold = ia64_get_hang_abort_threshold();
    if (hang_threshold) {
        gen_helper_hang_abort(tcg_env,
                              tcg_constant_i64(ctx->base.pc_next),
                              tcg_constant_i32(ctx->ri),
                              tcg_constant_i64(hang_threshold));
    }

    if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
        qemu_log_mask_and_addr(CPU_LOG_EXEC, ctx->base.pc_next,
                               "IA64: TB start pc=%016" PRIx64 "\n",
                               ctx->base.pc_next);
    }
}

static void ia64_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->base.pc_next, ctx->ri);

#ifndef CONFIG_USER_ONLY
    if (ctx->ri == 0) {
        uint64_t dump_pc = 0;
        uint32_t dump_bundles = 0;
        if (ia64_get_fw_dump_pc(&dump_pc, &dump_bundles) &&
            ctx->base.pc_next == dump_pc) {
            gen_helper_fw_dump_pc(tcg_env,
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(dump_bundles));
        }
    }

    /*
     * xenipf/EDK firmware quirk: MP init assumes an MP buffer base pointer is
     * present on the stack; when missing it treats CPU0 as an AP and calls a
     * NULL rendezvous function pointer. Patch in a minimal MP buffer before
     * the signature compare executes.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        ctx->base.pc_next == 0x00000000ffe59900ULL) {
        gen_helper_fw_xenipf_mpbuffer_fix(tcg_env,
                                          tcg_constant_i64(ctx->base.pc_next));
    }

    /*
     * xenipf/EDK firmware: function-id 12 returns the PEI PPI list pointer.
     * The ROM handler zeros r9, so fix it up and skip the handler body.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffffb7a0ULL ||
         ctx->base.pc_next == 0x00000000ffffb7a0ULL)) {
        uint64_t ret_pc = (ctx->base.pc_next & 0xffffffff00000000ULL) |
                          0x00000000ffffb570ULL;
        gen_helper_fw_pei_ppi_fix(tcg_env,
                                  tcg_constant_i64(ctx->base.pc_next));
        tcg_gen_movi_i64(cpu_pc, ret_pc);
        gen_set_ri_const(0);
        tcg_gen_exit_tb(NULL, 0);
        return;
    }

    /*
     * xenipf/EDK firmware: ensure PEI core entry has the startup descriptor
     * in r32 plus the stack count in r10/r33 for early RSE init.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e170ULL ||
         ctx->base.pc_next == 0x00000000ffe2e170ULL)) {
        gen_helper_fw_pei_entry_fix(tcg_env,
                                    tcg_constant_i64(ctx->base.pc_next));
        gen_helper_fw_pei_rse_probe(tcg_env,
                                    tcg_constant_i64(ctx->base.pc_next),
                                    tcg_constant_i32(0));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e4b0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e4b0ULL)) {
        gen_helper_fw_pei_oldcore_clear(tcg_env,
                                        tcg_constant_i64(ctx->base.pc_next));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe228a0ULL ||
         ctx->base.pc_next == 0x00000000ffe228a0ULL)) {
        gen_helper_fw_pei_hob_init_fix(tcg_env,
                                       tcg_constant_i64(ctx->base.pc_next));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe65b40ULL ||
         ctx->base.pc_next == 0x00000000ffe65b40ULL)) {
        gen_helper_fw_autoscan_memmap_fix(tcg_env,
                                          tcg_constant_i64(ctx->base.pc_next));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe66020ULL ||
         ctx->base.pc_next == 0x00000000ffe66020ULL)) {
        gen_helper_fw_pei_install_mem_fix(tcg_env,
                                          tcg_constant_i64(ctx->base.pc_next));
        gen_helper_fw_pei_install_mem_call_probe(tcg_env,
                                                 tcg_constant_i64(ctx->base.pc_next));
        gen_helper_fw_pei_store_watch_toggle(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next));
        gen_helper_fw_pei_memdump(tcg_env,
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(0));
        gen_helper_fw_pei_install_mem_trace(tcg_env,
                                            tcg_constant_i64(ctx->base.pc_next),
                                            tcg_constant_i32(0));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe66030ULL ||
         ctx->base.pc_next == 0x00000000ffe66030ULL)) {
        gen_helper_fw_pei_install_mem_trace(tcg_env,
                                            tcg_constant_i64(ctx->base.pc_next),
                                            tcg_constant_i32(1));
        gen_helper_fw_pei_memdump(tcg_env,
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(1));
        gen_helper_fw_pei_store_watch_toggle(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe66200ULL ||
         ctx->base.pc_next == 0x00000000ffe66200ULL)) {
        gen_helper_fw_autoscan_memtop_fix(tcg_env,
                                          tcg_constant_i64(ctx->base.pc_next));
    }

    if (ctx->mem_idx != MMU_USER_IDX) {
        uint64_t pc_low = ctx->base.pc_next & ((1ULL << 61) - 1);
        if (pc_low == 0x1fffffff1ff4f520ULL ||
            pc_low == 0x1fffffff1ff4f530ULL ||
            pc_low == 0x1fffffff1ff4f540ULL) {
            gen_helper_fw_sal_call_probe(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next));
        }
        if (pc_low == 0x1fffffff1fff5b00ULL) {
            gen_helper_fw_sal_call_probe(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next));
        }
        if (pc_low == 0x1fffffff1ff3e770ULL ||
            pc_low == 0x1fffffff1ff3e780ULL ||
            pc_low == 0x1fffffff1ff3e7e0ULL ||
            pc_low == 0x1fffffff1ff3d1c0ULL ||
            pc_low == 0x1fffffff1ff3d1d0ULL ||
            pc_low == 0x1fffffff1ff3d420ULL ||
            pc_low == 0x1fffffff1ff4f420ULL) {
            gen_helper_fw_sal_call_probe(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next));
        }
        if (pc_low == 0x1fffffff1fff5bf0ULL) {
            gen_helper_fw_sal_ret_probe(tcg_env,
                                        tcg_constant_i64(ctx->base.pc_next));
        }
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe73770ULL ||
         ctx->base.pc_next == 0x00000000ffe73770ULL ||
         ctx->base.pc_next == 0x80000000ffe73790ULL ||
         ctx->base.pc_next == 0x00000000ffe73790ULL)) {
        gen_helper_dbg_probe(tcg_env,
                             tcg_constant_i64(ctx->base.pc_next),
                             tcg_constant_i32(ctx->ri));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e3a0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e3a0ULL)) {
        gen_helper_fw_boot_k4_fix(tcg_env,
                                  tcg_constant_i64(ctx->base.pc_next));
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(10));
    }

    /*
     * xenipf/EDK firmware: PEI startup path should hand off the SEC handoff
     * pointer in r9 and the PPI list in r10 before the PEI core copies them
     * into r32/r33. The ROM stub leaves these zeroed; override with our data.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e450ULL ||
         ctx->base.pc_next == 0x00000000ffe2e450ULL)) {
        gen_helper_fw_pei_startup_fix(tcg_env,
                                      tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 2 &&
        (ctx->base.pc_next == 0x80000000ffe2e450ULL ||
         ctx->base.pc_next == 0x00000000ffe2e450ULL ||
         ctx->base.pc_next == 0x80000000ffe2e460ULL ||
         ctx->base.pc_next == 0x00000000ffe2e460ULL)) {
        gen_helper_fw_pei_startup_call_fix(tcg_env,
                                           tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e450ULL ||
         ctx->base.pc_next == 0x00000000ffe2e450ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(7));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 1 &&
        (ctx->base.pc_next == 0x80000000ffe2e450ULL ||
         ctx->base.pc_next == 0x00000000ffe2e450ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(8));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 2 &&
        (ctx->base.pc_next == 0x80000000ffe2e450ULL ||
         ctx->base.pc_next == 0x00000000ffe2e450ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(9));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e4a0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e4a0ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(3));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 1 &&
        (ctx->base.pc_next == 0x80000000ffe2e4a0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e4a0ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(4));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 2 &&
        (ctx->base.pc_next == 0x80000000ffe2e4a0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e4a0ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(5));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e4b0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e4b0ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(6));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e640ULL ||
         ctx->base.pc_next == 0x00000000ffe2e640ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(0));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e660ULL ||
         ctx->base.pc_next == 0x00000000ffe2e660ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(1));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e740ULL ||
         ctx->base.pc_next == 0x00000000ffe2e740ULL)) {
        gen_helper_fw_bootloop_log(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(2));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe20410ULL ||
         ctx->base.pc_next == 0x00000000ffe20410ULL)) {
        gen_helper_fw_pei_core_entry_probe(tcg_env,
                                           tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe20490ULL ||
         ctx->base.pc_next == 0x00000000ffe20490ULL)) {
        gen_helper_fw_pei_startup_dump(tcg_env,
                                       tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe209e0ULL ||
         ctx->base.pc_next == 0x00000000ffe209e0ULL)) {
        gen_helper_fw_pei_dispatch_dump(tcg_env,
                                        tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe20a50ULL ||
         ctx->base.pc_next == 0x00000000ffe20a50ULL)) {
        gen_helper_fw_pei_dispatch_dump(tcg_env,
                                        tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX && ctx->ri == 0) {
        uint32_t pc_lo = (uint32_t)ctx->base.pc_next;
        if (pc_lo >= 0xffe20000u && pc_lo < 0xffe30000u) {
            gen_helper_fw_pei_err_watch(tcg_env,
                                        tcg_constant_i64(ctx->base.pc_next));
        }
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e1c0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e1c0ULL)) {
        gen_helper_fw_pei_rse_probe(tcg_env,
                                    tcg_constant_i64(ctx->base.pc_next),
                                    tcg_constant_i32(1));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e9b0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e9b0ULL ||
         ctx->base.pc_next == 0x80000000ffe2e8f0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e8f0ULL)) {
        gen_helper_fw_pei_callsite_probe(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e8e0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e8e0ULL)) {
        gen_helper_fw_pei_callsite_pre_probe(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e8f0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e8f0ULL)) {
        gen_helper_fw_pei_callsite_post_probe(tcg_env,
                                              tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e920ULL ||
         ctx->base.pc_next == 0x00000000ffe2e920ULL)) {
        gen_helper_fw_pei_callsite_use_probe(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e420ULL ||
         ctx->base.pc_next == 0x00000000ffe2e420ULL)) {
        gen_helper_fw_pei_ptr_chain_probe(tcg_env,
                                          tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e440ULL ||
         ctx->base.pc_next == 0x00000000ffe2e440ULL)) {
        gen_helper_fw_pei_ptr_chain_post_probe(tcg_env,
                                               tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe268b0ULL ||
         ctx->base.pc_next == 0x00000000ffe268b0ULL)) {
        gen_helper_fw_pei_err268_probe(tcg_env,
                                       tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2241cULL ||
         ctx->base.pc_next == 0x00000000ffe2241cULL)) {
        gen_helper_fw_pei_callfd_probe(tcg_env,
                                       tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e9b0ULL ||
         ctx->base.pc_next == 0x00000000ffe2e9b0ULL)) {
        gen_helper_fw_pei_fit_probe(tcg_env,
                                    tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2ea4cULL ||
         ctx->base.pc_next == 0x00000000ffe2ea4cULL)) {
        gen_helper_fw_pei_fit_compare_probe(tcg_env,
                                            tcg_constant_i64(ctx->base.pc_next));
    }

    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2e260ULL ||
         ctx->base.pc_next == 0x00000000ffe2e260ULL)) {
        gen_helper_fw_pei_rse_probe(tcg_env,
                                    tcg_constant_i64(ctx->base.pc_next),
                                    tcg_constant_i32(2));
    }

    /*
     * xenipf/EDK firmware: trace PeiCore's InstallPpi call site.
     * (br.call in slot 2 of the bundle at 0xffe20540.)
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 2 &&
        (ctx->base.pc_next == 0x80000000ffe20540ULL ||
         ctx->base.pc_next == 0x00000000ffe20540ULL)) {
        gen_helper_fw_pei_pre_install_probe(tcg_env,
                                            tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        (ctx->base.pc_next == 0x80000000ffe22590ULL ||
         ctx->base.pc_next == 0x00000000ffe22590ULL)) {
        gen_helper_fw_pei_startup_dump(tcg_env,
                                       tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        (ctx->base.pc_next == 0x80000000ffe22670ULL ||
         ctx->base.pc_next == 0x00000000ffe22670ULL)) {
        gen_helper_fw_pei_startup_dump(tcg_env,
                                       tcg_constant_i64(ctx->base.pc_next));
    }

    /*
     * xenipf/EDK firmware: dump PEI PPI database at the AfterMemMP assert loop
     * to confirm which PPIs are installed before DXE handoff.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe737a0ULL ||
         ctx->base.pc_next == 0x00000000ffe737a0ULL)) {
        gen_helper_fw_pei_ppi_dump(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next));
    }

    /*
     * xenipf/EDK firmware: dump PEI PPI database before the DXE IPL lookup
     * to see whether MemoryDiscovered/DxeIpl PPIs were installed.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe215c0ULL ||
         ctx->base.pc_next == 0x00000000ffe215c0ULL)) {
        gen_helper_fw_pei_ppi_dump(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next));
    }
    /*
     * xenipf/EDK firmware: dump PEI PPI database when PeiLocatePpi returns
     * NOT_FOUND for StatusCode PPI to verify PPI install state.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe268b0ULL ||
         ctx->base.pc_next == 0x00000000ffe268b0ULL)) {
        gen_helper_fw_pei_ppi_dump(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe2bc70ULL ||
         ctx->base.pc_next == 0x00000000ffe2bc70ULL)) {
        gen_helper_fw_pei_ppi_dump(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next));
    }

    /*
     * xenipf/EDK firmware: log status after AfterMemMP call site to capture
     * the failing EFI_STATUS that triggers the assert loop.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe73710ULL ||
         ctx->base.pc_next == 0x00000000ffe73710ULL)) {
        gen_helper_fw_pei_status_log(tcg_env,
                                     tcg_constant_i64(ctx->base.pc_next));
    }

    /*
     * xenipf/EDK firmware: trace AfterMemMP call chain and status stores.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe73700ULL ||
         ctx->base.pc_next == 0x00000000ffe73700ULL)) {
        gen_helper_fw_pei_aftermem_trace(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next),
                                         tcg_constant_i32(0));
        gen_helper_dbg_probe(tcg_env,
                             tcg_constant_i64(ctx->base.pc_next),
                             tcg_constant_i32(ctx->ri));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 2 &&
        (ctx->base.pc_next == 0x80000000ffe74740ULL ||
         ctx->base.pc_next == 0x00000000ffe74740ULL)) {
        gen_helper_fw_pei_aftermem_trace(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next),
                                         tcg_constant_i32(1));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe74750ULL ||
         ctx->base.pc_next == 0x00000000ffe74750ULL)) {
        gen_helper_fw_pei_aftermem_trace(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next),
                                         tcg_constant_i32(4));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe74790ULL ||
         ctx->base.pc_next == 0x00000000ffe74790ULL)) {
        gen_helper_fw_pei_aftermem_trace(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next),
                                         tcg_constant_i32(2));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe747c0ULL ||
         ctx->base.pc_next == 0x00000000ffe747c0ULL)) {
        gen_helper_fw_pei_aftermem_trace(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next),
                                         tcg_constant_i32(3));
    }

    /*
     * xenipf/EDK firmware: trace PeiMain's indirect call that leads to
     * PeiCreateHob, and the resulting OUT_OF_RESOURCES check.
     */
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 2 &&
        (ctx->base.pc_next == 0x80000000ffe24d20ULL ||
         ctx->base.pc_next == 0x00000000ffe24d20ULL)) {
        gen_helper_fw_pei_indcall_probe(tcg_env,
                                        tcg_constant_i64(ctx->base.pc_next),
                                        tcg_constant_i32(0));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe24d30ULL ||
         ctx->base.pc_next == 0x00000000ffe24d30ULL)) {
        gen_helper_fw_pei_indcall_probe(tcg_env,
                                        tcg_constant_i64(ctx->base.pc_next),
                                        tcg_constant_i32(1));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe24e50ULL ||
         ctx->base.pc_next == 0x00000000ffe24e50ULL)) {
        gen_helper_fw_pei_oor_probe(tcg_env,
                                    tcg_constant_i64(ctx->base.pc_next),
                                    tcg_constant_i32(0));
    }
    if (ctx->mem_idx != MMU_USER_IDX &&
        ctx->ri == 0 &&
        (ctx->base.pc_next == 0x80000000ffe24e60ULL ||
         ctx->base.pc_next == 0x00000000ffe24e60ULL)) {
        gen_helper_fw_pei_oor_probe(tcg_env,
                                    tcg_constant_i64(ctx->base.pc_next),
                                    tcg_constant_i32(1));
    }
#endif

    if (ia64_watch_r33_match()) {
        gen_helper_dbg_r33_watch(tcg_env,
                                 tcg_constant_i64(ctx->base.pc_next),
                                 tcg_constant_i32(ctx->ri));
    }
}

static void decode_a_unit(DisasContext *ctx, uint64_t insn)
{
    uint8_t major = (insn >> 37) & 0xf;
    uint8_t x2a = (insn >> 34) & 0x3;
    uint8_t ve = (insn >> 33) & 0x1;
    uint8_t x4 = (insn >> 29) & 0xf;
    uint8_t x2b = (insn >> 27) & 0x3;
    uint8_t r3 = (insn >> 20) & 0x7f;
    uint8_t r2 = (insn >> 13) & 0x7f;
    uint8_t r1 = (insn >> 6) & 0x7f;
    uint8_t qp = insn & 0x3f;

    /*
     * Most A-slot instructions are predicated normally (skip when qp==0),
     * but cmp.*.unc is special: it still writes p1/p2 (as 0/0) when qp==0.
     * Match SKI's cmpuncRd/cmpiuncRd + CMP_EX behavior by not skipping these
     * instructions and gating predicate results with qp explicitly.
     */
    bool is_unc_cmp = false;
    if (major == 0xC || major == 0xD || major == 0xE) {
        uint8_t ta = ve; /* bit 33 */
        uint8_t c = (insn >> 12) & 1;
        if (ta == 0 && c == 1) {
            if (x2a >= 2) {
                /* imm8 forms: bit 36 is sign, no tb field. */
                is_unc_cmp = true;
            } else {
                /* reg-reg forms: tb{36}=0 for A6/A6 cmp*.unc. */
                uint8_t tb = (insn >> 36) & 1;
                is_unc_cmp = (tb == 0);
            }
        }
    }

    TCGLabel *skip_label = is_unc_cmp ? NULL : gen_qp_skip(qp);
    bool handled = false;

    if (major == 0x8 && x2a == 0) {
        /* A1 bitwise/add/sub/etc. */
        /* A3: sub r1 = imm8, r3 (imm7b + sign bit at 36) */
        if (x4 == 0x9 && x2b == 1 && ve == 0) {
            uint64_t imm8 = extract64(insn, 13, 7) |
                            (extract64(insn, 36, 1) << 7);
            int64_t simm8 = sextract64(imm8, 0, 8);
            TCGv_i64 src = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(src, 0);
            } else {
                tcg_gen_mov_i64(src, cpu_r[r3]);
            }
            TCGv_i64 t = tcg_temp_new_i64();
            tcg_gen_movi_i64(t, simm8);
            tcg_gen_sub_i64(t, t, src);
            if (r1 != 0) {
                tcg_gen_mov_i64(cpu_r[r1], t);
            }
            gen_a_unit_nat1(r1, r3);
            handled = true;
        } else
        /* Immediate logical ops: and/andcm/or/xor r1 = imm8, r3 (imm7b + sign bit at 36) */
        if (x4 == 0xB && ve == 0) {
            uint64_t imm8 = extract64(insn, 13, 7) |
                            (extract64(insn, 36, 1) << 7);
            int64_t simm8 = sextract64(imm8, 0, 8);
            uint64_t imm = (uint64_t)simm8;
            TCGv_i64 src = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(src, 0);
            } else {
                tcg_gen_mov_i64(src, cpu_r[r3]);
            }
            TCGv_i64 t = tcg_temp_new_i64();
            switch (x2b) {
            case 0: /* and */
                tcg_gen_andi_i64(t, src, imm);
                break;
            case 1: /* andcm */
                /*
                 * andcm r1 = imm7, r3 is defined as:
                 *   r1 = imm7 & ~r3
                 * (immediate is the first operand, like the reg-reg form).
                 */
                tcg_gen_not_i64(t, src);
                tcg_gen_andi_i64(t, t, imm);
                break;
            case 2: /* or */
                tcg_gen_ori_i64(t, src, imm);
                break;
            case 3: /* xor */
                tcg_gen_xori_i64(t, src, imm);
                break;
            default:
                g_assert_not_reached();
            }
            if (r1 != 0) {
                tcg_gen_mov_i64(cpu_r[r1], t);
            }
            gen_a_unit_nat1(r1, r3);
            handled = true;
            /* handled */
        } else {
            TCGv_i64 t1 = tcg_temp_new_i64();
            TCGv_i64 t2 = tcg_temp_new_i64();
            if (r2 == 0) {
                tcg_gen_movi_i64(t1, 0);
            } else {
                tcg_gen_mov_i64(t1, cpu_r[r2]);
            }
            if (r3 == 0) {
                tcg_gen_movi_i64(t2, 0);
            } else {
                tcg_gen_mov_i64(t2, cpu_r[r3]);
            }

            bool a1_handled = true;
            switch (x4) {
            case 0: /* add */
                if (x2b == 0) {
                    tcg_gen_add_i64(t1, t1, t2);
                } else if (x2b == 1) {
                    /* add1: r1 = r2 + r3 + 1 */
                    tcg_gen_add_i64(t1, t1, t2);
                    tcg_gen_addi_i64(t1, t1, 1);
                } else {
                    a1_handled = false;
                }
                break;
            case 1: /* sub */
                if (x2b == 1) {
                    tcg_gen_sub_i64(t1, t1, t2);
                } else if (x2b == 0) {
                    /* sub1: r1 = r2 - r3 - 1 */
                    tcg_gen_sub_i64(t1, t1, t2);
                    tcg_gen_addi_i64(t1, t1, -1);
                } else {
                    a1_handled = false;
                }
                break;
            case 2: /* addp4 (A1): 32-bit pointer add */
                if (x2b != 0) {
                    a1_handled = false;
                    break;
                }
                /*
                 * SKI: DST = (WORD)(SRC1 + SRC2) | (BitfX(SRC2,32,2) << 61)
                 * WORD is 32-bit, BitfX(.,32,2) extracts bits 31..30.
                 */
                tcg_gen_add_i64(t1, t1, t2);
                tcg_gen_ext32u_i64(t1, t1);
                {
                    TCGv_i64 rb = tcg_temp_new_i64();
                    tcg_gen_shri_i64(rb, t2, 30);
                    tcg_gen_andi_i64(rb, rb, 3);
                    tcg_gen_shli_i64(rb, rb, 61);
                    tcg_gen_or_i64(t1, t1, rb);
                }
                break;
            case 3:
                switch (x2b) {
                case 0: tcg_gen_and_i64(t1, t1, t2); break;
                case 1: tcg_gen_andc_i64(t1, t1, t2); break;
                case 2: tcg_gen_or_i64(t1, t1, t2); break;
                case 3: tcg_gen_xor_i64(t1, t1, t2); break;
                default: break;
                }
                break;
            case 4: { /* shladd: r1 = (r2 << (x2b+1)) + r3 */
                uint8_t sh = x2b + 1;
                tcg_gen_shli_i64(t1, t1, sh);
                tcg_gen_add_i64(t1, t1, t2);
                break;
            }
            case 6: { /* shladdp4: r1 = (r2 << count2) + r3 (p4 pointer add) */
                uint8_t sh = x2b + 1;
                tcg_gen_shli_i64(t1, t1, sh);
                tcg_gen_add_i64(t1, t1, t2);
                tcg_gen_ext32u_i64(t1, t1);
                {
                    TCGv_i64 rb = tcg_temp_new_i64();
                    tcg_gen_shri_i64(rb, t2, 30);
                    tcg_gen_andi_i64(rb, rb, 3);
                    tcg_gen_shli_i64(rb, rb, 61);
                    tcg_gen_or_i64(t1, t1, rb);
                }
                break;
            }
            default:
                a1_handled = false;
                break;
            }
            if (!a1_handled) {
                handled = false;
            } else {
                if (r1 == 12) {
                    gen_helper_dbg_r12(tcg_env,
                                       tcg_constant_i64(ctx->base.pc_next),
                                       t1);
                }
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t1);
                }
                gen_a_unit_nat2(r1, r2, r3);
                handled = true;
            }
        }
    } else if (major == 0x8 && x2a == 2 && ve == 0) {
        /* adds r1 = imm14, r3 (A4) */
        uint64_t imm =
            extract64(insn, 13, 7) |               /* imm7b */
            (extract64(insn, 27, 6) << 7) |         /* imm6d */
            (extract64(insn, 36, 1) << 13);         /* sign */
        int64_t simm = sextract64(imm, 0, 14);
        TCGv_i64 t = tcg_temp_new_i64();
        if (r3 == 0) {
            tcg_gen_movi_i64(t, simm);
        } else {
            tcg_gen_addi_i64(t, cpu_r[r3], simm);
        }
        if (r1 != 0) {
            if (r1 == 12) {
                gen_helper_dbg_r12(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   t);
            }
            tcg_gen_mov_i64(cpu_r[r1], t);
        }
        gen_a_unit_nat1(r1, r3);
        handled = true;
    } else if (major == 0x8 && x2a == 3 && ve == 0) {
        /* addp4 r1 = imm14, r3 (A4) */
        uint64_t imm =
            extract64(insn, 13, 7) |               /* imm7b */
            (extract64(insn, 27, 6) << 7) |         /* imm6d */
            (extract64(insn, 36, 1) << 13);         /* sign */
        int64_t simm = sextract64(imm, 0, 14);

        TCGv_i64 src2 = tcg_temp_new_i64();
        if (r3 == 0) {
            tcg_gen_movi_i64(src2, 0);
        } else {
            tcg_gen_mov_i64(src2, cpu_r[r3]);
        }

        TCGv_i64 sum = tcg_temp_new_i64();
        tcg_gen_addi_i64(sum, src2, simm);
        tcg_gen_ext32u_i64(sum, sum);

        TCGv_i64 rb = tcg_temp_new_i64();
        tcg_gen_shri_i64(rb, src2, 30);
        tcg_gen_andi_i64(rb, rb, 3);
        tcg_gen_shli_i64(rb, rb, 61);
        tcg_gen_or_i64(sum, sum, rb);

        if (r1 != 0) {
            tcg_gen_mov_i64(cpu_r[r1], sum);
        }
        gen_a_unit_nat1(r1, r3);
        handled = true;
    } else if (major == 0x9) {
        /* addl r1 = imm22, r3 (A5; r3 is only 2 bits, r0-r3). */
        uint8_t r1_a5 = extract64(insn, 6, 7);
        uint8_t r3_a5 = extract64(insn, 20, 2);
        uint64_t imm =
            extract64(insn, 13, 7) |               /* imm7b */
            (extract64(insn, 27, 9) << 7) |        /* imm9d */
            (extract64(insn, 22, 5) << 16) |       /* imm5c */
            (extract64(insn, 36, 1) << 21);        /* sign */
        int64_t simm = sextract64(imm, 0, 22);
        TCGv_i64 t = tcg_temp_new_i64();
        if (r3_a5 == 0) {
            tcg_gen_movi_i64(t, simm);
        } else {
            tcg_gen_addi_i64(t, cpu_r[r3_a5], simm);
        }
        if (r1_a5 != 0) {
            if (r1_a5 == 12) {
                gen_helper_dbg_r12(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   t);
            }
            tcg_gen_mov_i64(cpu_r[r1_a5], t);
        }
        gen_a_unit_nat1(r1_a5, r3_a5);
        handled = true;
    } else if (major == 0xC || major == 0xD || major == 0xE) {
        /*
         * Integer compares (formats A6/A7/A8), including cmp4.*.
         *
         * Encoding reference: SKI's encoding.opcode / execTbl.
         * - tb==0, ta==0: cmp.{lt,ltu,eq}[.unc]
         * - tb==0, ta==1: cmp.{eq,ne}.{and,or,or.andcm}
         * - tb==1:        cmp.{gt,le,ge,lt}.{and,or,or.andcm} (r0 vs r3)
         *
         * Note: bit 36 is tb only for reg-reg forms (x2 in {0,1}); for imm8
         * forms (x2 in {2,3}), bit 36 is imm8[7] (sign).
         */
        uint8_t p2 = extract64(insn, 27, 6);
        uint8_t p1 = extract64(insn, 6, 6);
        uint8_t ta = ve; /* bit 33 */
        uint8_t c = (insn >> 12) & 1;
        bool is_imm = (x2a == 2 || x2a == 3);
        bool is_32 = (x2a == 1 || x2a == 3);
        uint8_t tb = 0;
        if (!is_imm) {
            tb = (insn >> 36) & 1;
        }
        if (ia64_dbg_cmp_match(ctx->base.pc_next)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "dbg_cmp pc=%016" PRIx64 " ri=%u insn=%011" PRIx64
                          " qp=%u p1=%u p2=%u r2=%u r3=%u"
                          " major=%u is32=%u imm=%u ta=%u c=%u tb=%u\n",
                          ctx->base.pc_next, ctx->ri, insn,
                          qp, p1, p2, r2, r3,
                          major, is_32, is_imm, ta, c, tb);
        }

        TCGv_i64 op1 = tcg_temp_new_i64();
        TCGv_i64 op2 = tcg_temp_new_i64();

        if (is_imm) {
            uint64_t imm = extract64(insn, 13, 7) |
                           (extract64(insn, 36, 1) << 7);
            int64_t simm = sextract64(imm, 0, 8);
            tcg_gen_movi_i64(op1, simm);
        } else if (tb) {
            tcg_gen_movi_i64(op1, 0);
        } else if (r2 == 0) {
            tcg_gen_movi_i64(op1, 0);
        } else {
            tcg_gen_mov_i64(op1, cpu_r[r2]);
        }

        if (r3 == 0) {
            tcg_gen_movi_i64(op2, 0);
        } else {
            tcg_gen_mov_i64(op2, cpu_r[r3]);
        }

        TCGv_i64 cond = tcg_temp_new_i64();

        if (!is_imm && tb) {
            /* A7: compare r0 (0) against r3 with combining write modes. */
            TCGv_i64 lhs = op1;
            TCGv_i64 rhs = op2;
            if (is_32) {
                tcg_gen_ext32s_i64(rhs, rhs);
                /* lhs is 0 already. */
            }

            /* Relation is selected by (ta,c). */
            if (ta == 0 && c == 0) {
                tcg_gen_setcond_i64(TCG_COND_GT, cond, lhs, rhs);
            } else if (ta == 0 && c == 1) {
                tcg_gen_setcond_i64(TCG_COND_LE, cond, lhs, rhs);
            } else if (ta == 1 && c == 0) {
                tcg_gen_setcond_i64(TCG_COND_GE, cond, lhs, rhs);
            } else {
                tcg_gen_setcond_i64(TCG_COND_LT, cond, lhs, rhs);
            }

            /* Combine mode is selected by major (op). */
            switch (major) {
            case 0xC: /* .and: clear both when cond==0 */
                gen_combine_predicates(p1, p2, cond, IA64_PRED_COMBINE_AND);
                break;
            case 0xD: /* .or: set both when cond==1 */
                gen_combine_predicates(p1, p2, cond, IA64_PRED_COMBINE_OR);
                break;
            case 0xE: /* .or.andcm: set p1=1,p2=0 when cond==1 */
                gen_combine_predicates(p1, p2, cond, IA64_PRED_COMBINE_OR_ANDCM);
                break;
            default:
                g_assert_not_reached();
            }
            handled = true;
        } else if (ta) {
            /*
             * Predicate combining compares against r2/imm8 and r3.
             * major selects combine mode; c selects eq (0) vs ne (1).
             */
            TCGv_i64 lhs = op1;
            TCGv_i64 rhs = op2;
            if (is_32) {
                tcg_gen_ext32u_i64(lhs, lhs);
                tcg_gen_ext32u_i64(rhs, rhs);
            }

            if (c) {
                tcg_gen_setcond_i64(TCG_COND_NE, cond, lhs, rhs);
            } else {
                tcg_gen_setcond_i64(TCG_COND_EQ, cond, lhs, rhs);
            }

            switch (major) {
            case 0xC: /* .and */
                gen_combine_predicates(p1, p2, cond, IA64_PRED_COMBINE_AND);
                break;
            case 0xD: /* .or */
                gen_combine_predicates(p1, p2, cond, IA64_PRED_COMBINE_OR);
                break;
            case 0xE: /* .or.andcm */
                gen_combine_predicates(p1, p2, cond, IA64_PRED_COMBINE_OR_ANDCM);
                break;
            default:
                g_assert_not_reached();
            }
            handled = true;
        } else {
            /* Plain compares: major selects relation; c selects .unc (write 0/0 if qp==0). */
            TCGv_i64 lhs = op1;
            TCGv_i64 rhs = op2;

            switch (major) {
            case 0xC: /* lt / lt.unc */
                if (is_32) {
                    tcg_gen_ext32s_i64(lhs, lhs);
                    tcg_gen_ext32s_i64(rhs, rhs);
                }
                if (ia64_dbg_cmp_match(ctx->base.pc_next)) {
                    gen_helper_dbg_cmp(tcg_env, tcg_constant_i64(ctx->base.pc_next),
                                       lhs, rhs,
                                       tcg_constant_i32(p1),
                                       tcg_constant_i32(p2));
                }
                tcg_gen_setcond_i64(TCG_COND_LT, cond, lhs, rhs);
                break;
            case 0xD: /* ltu / ltu.unc */
                if (is_32) {
                    tcg_gen_ext32u_i64(lhs, lhs);
                    tcg_gen_ext32u_i64(rhs, rhs);
                }
                if (ia64_dbg_cmp_match(ctx->base.pc_next)) {
                    gen_helper_dbg_cmp(tcg_env, tcg_constant_i64(ctx->base.pc_next),
                                       lhs, rhs,
                                       tcg_constant_i32(p1),
                                       tcg_constant_i32(p2));
                }
                tcg_gen_setcond_i64(TCG_COND_LTU, cond, lhs, rhs);
                break;
            case 0xE: /* eq / eq.unc */
                if (is_32) {
                    tcg_gen_ext32u_i64(lhs, lhs);
                    tcg_gen_ext32u_i64(rhs, rhs);
                }
                if (ia64_dbg_cmp_match(ctx->base.pc_next)) {
                    gen_helper_dbg_cmp(tcg_env, tcg_constant_i64(ctx->base.pc_next),
                                       lhs, rhs,
                                       tcg_constant_i32(p1),
                                       tcg_constant_i32(p2));
                }
                tcg_gen_setcond_i64(TCG_COND_EQ, cond, lhs, rhs);
                break;
            default:
                g_assert_not_reached();
            }

            if (c) {
                /* .unc: p1 = cond & qp; p2 = !cond & qp. */
                TCGv_i64 qp_val = gen_pr_read_bit(qp);

                TCGv_i64 p1v = tcg_temp_new_i64();
                tcg_gen_and_i64(p1v, cond, qp_val);

                TCGv_i64 p2v = tcg_temp_new_i64();
                tcg_gen_xori_i64(p2v, cond, 1);
                tcg_gen_and_i64(p2v, p2v, qp_val);

                gen_pr_write_bit(p1, p1v);
                gen_pr_write_bit(p2, p2v);
                if (ia64_dbg_cmp_match(ctx->base.pc_next)) {
                    gen_helper_dbg_cmp_post(tcg_env, tcg_constant_i64(ctx->base.pc_next),
                                             lhs, rhs,
                                             tcg_constant_i32(p1),
                                             tcg_constant_i32(p2));
                }
            } else {
                gen_set_predicates(p1, p2, cond);
                if (ia64_dbg_cmp_match(ctx->base.pc_next)) {
                    gen_helper_dbg_cmp_post(tcg_env, tcg_constant_i64(ctx->base.pc_next),
                                             lhs, rhs,
                                             tcg_constant_i32(p1),
                                             tcg_constant_i32(p2));
                }
            }

            handled = true;
        }
    }

    /* A9: packed add/sub (padd/psub) in byte/halfword/word lanes. */
    if (!handled && major == 0x8 && x2a == 1) {
        uint8_t za = extract64(insn, 36, 1);
        uint8_t zb = ve; /* bit 33 in A9 encodings */
        uint8_t size = 0;

        if (za == 1 && zb == 0) {
            size = 4;
        } else if (za == 0 && zb == 1) {
            size = 2;
        } else if (za == 0 && zb == 0) {
            size = 1;
        }

        if (size) {
            TCGv_i64 a = tcg_temp_new_i64();
            TCGv_i64 b = tcg_temp_new_i64();
            if (r2 == 0) {
                tcg_gen_movi_i64(a, 0);
            } else {
                tcg_gen_mov_i64(a, cpu_r[r2]);
            }
            if (r3 == 0) {
                tcg_gen_movi_i64(b, 0);
            } else {
                tcg_gen_mov_i64(b, cpu_r[r3]);
            }

            TCGv_i64 res = tcg_temp_new_i64();
            bool op_ok = false;

            if (x4 == 0x0) {
                /* padd{1,2,4}[.sss/.uuu/.uus] */
                if (size == 1) {
                    switch (x2b) {
                    case 0: gen_helper_padd1(res, tcg_env, a, b); op_ok = true; break;
                    case 1: gen_helper_padd1_sss(res, tcg_env, a, b); op_ok = true; break;
                    case 2: gen_helper_padd1_uuu(res, tcg_env, a, b); op_ok = true; break;
                    case 3: gen_helper_padd1_uus(res, tcg_env, a, b); op_ok = true; break;
                    }
                } else if (size == 2) {
                    switch (x2b) {
                    case 0: gen_helper_padd2(res, tcg_env, a, b); op_ok = true; break;
                    case 1: gen_helper_padd2_sss(res, tcg_env, a, b); op_ok = true; break;
                    case 2: gen_helper_padd2_uuu(res, tcg_env, a, b); op_ok = true; break;
                    case 3: gen_helper_padd2_uus(res, tcg_env, a, b); op_ok = true; break;
                    }
                } else if (size == 4 && x2b == 0) {
                    gen_helper_padd4(res, tcg_env, a, b);
                    op_ok = true;
                }
            } else if (x4 == 0x1) {
                /* psub{1,2,4}[.sss/.uuu/.uus] */
                if (size == 1) {
                    switch (x2b) {
                    case 0: gen_helper_psub1(res, tcg_env, a, b); op_ok = true; break;
                    case 1: gen_helper_psub1_sss(res, tcg_env, a, b); op_ok = true; break;
                    case 2: gen_helper_psub1_uuu(res, tcg_env, a, b); op_ok = true; break;
                    case 3: gen_helper_psub1_uus(res, tcg_env, a, b); op_ok = true; break;
                    }
                } else if (size == 2) {
                    switch (x2b) {
                    case 0: gen_helper_psub2(res, tcg_env, a, b); op_ok = true; break;
                    case 1: gen_helper_psub2_sss(res, tcg_env, a, b); op_ok = true; break;
                    case 2: gen_helper_psub2_uuu(res, tcg_env, a, b); op_ok = true; break;
                    case 3: gen_helper_psub2_uus(res, tcg_env, a, b); op_ok = true; break;
                    }
                } else if (size == 4 && x2b == 0) {
                    gen_helper_psub4(res, tcg_env, a, b);
                    op_ok = true;
                }
            }

            if (op_ok) {
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], res);
                    TCGv_i64 nat_a = tcg_temp_new_i64();
                    TCGv_i64 nat_b = tcg_temp_new_i64();
                    gen_helper_gr_nat(nat_a, tcg_env, tcg_constant_i32(r2));
                    gen_helper_gr_nat(nat_b, tcg_env, tcg_constant_i32(r3));
                    TCGv_i64 nat = tcg_temp_new_i64();
                    tcg_gen_or_i64(nat, nat_a, nat_b);
                    gen_helper_gr_nat_set(tcg_env, tcg_constant_i32(r1), nat);
                }
                handled = true;
            }
        }
    }

    if (skip_label) {
        gen_set_label(skip_label);
    }
    if (!handled) {
        gen_unimpl(ctx, insn, "A-slot");
    }
}

static void decode_b_unit(DisasContext *ctx, uint64_t insn)
{
    if (insn == 0) {
        /* nop.b 0 */
        return;
    }
    uint8_t qp = insn & 0x3f;
    uint8_t major = (insn >> 37) & 0xf;
    uint8_t x6 = (insn >> 27) & 0x3f;

    if (ia64_dbg_bunit_match(ctx->base.pc_next)) {
        uint8_t btype = extract64(insn, 6, 3);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dbg_bunit pc=%016" PRIx64 " ri=%u insn=%011" PRIx64
                      " qp=%u major=%u x6=0x%x btype=%u\n",
                      ctx->base.pc_next, ctx->ri, insn,
                      qp, major, x6, btype);
        gen_helper_dbg_bunit_pred(tcg_env,
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(qp));
    }

    TCGLabel *skip_label = gen_qp_skip(qp);

    /* rfi: op=0, x6=0x8 */
    if (major == 0x0 && x6 == 0x8) {
        gen_helper_rfi(tcg_env);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (major == 0x0 && (x6 == 0xC || x6 == 0xD)) {
        /* B8: bsw.0 / bsw.1 */
        gen_helper_bsw(tcg_env, tcg_constant_i32(x6 == 0xD));
    } else if (major == 0x0 && x6 == 0x2) {
        /* cover: create a covering frame for interruption/sigtramp paths */
        gen_helper_cover(tcg_env);
    } else if (major == 0x0 && (x6 == 0x20 || x6 == 0x21)) {
        /* B4: br.cond/br.ia b2 (x6=0x20) and br.ret b2 (x6=0x21). */
        uint8_t b2 = extract64(insn, 13, 3);
        TCGv_i64 tgt = tcg_temp_new_i64();
        tcg_gen_andi_i64(tgt, cpu_b[b2], ~0xFULL);
        if (x6 == 0x21 && ia64_dbg_bret_match(ctx->base.pc_next)) {
            gen_helper_dbg_bret(tcg_env,
                                tcg_constant_i64(ctx->base.pc_next),
                                tcg_constant_i32(b2));
        }
        /*
         * Our simplified RSE model pushes a stacked-register snapshot on
         * br.call.  Some early-kernel PAL stubs return via br.ia b0 instead of
         * br.ret b0, but other sequences use b0 as a normal branch target (e.g.
         * PAL call paths that set b0 manually).  Unwind only when we can tell
         * b0 came from a call.
         */
        if (ctx->mem_idx != MMU_USER_IDX && x6 == 0x20 && b2 == 6) {
            TCGv_i64 k5 = gen_load_ar(IA64_AR_K5);
            tcg_gen_andi_i64(k5, k5, ~0xFULL);
            TCGLabel *no_pal = gen_new_label();
            tcg_gen_brcond_i64(TCG_COND_NE, tgt, k5, no_pal);
            gen_helper_fw_pal(tcg_env);
            TCGv_i64 ret = tcg_temp_new_i64();
            tcg_gen_andi_i64(ret, cpu_b[0], ~0xFULL);
            tcg_gen_mov_i64(cpu_pc, ret);
            gen_set_ri_const(0);
            if (qp == 0) {
                ctx->base.is_jmp = DISAS_NORETURN;
            }
            tcg_gen_exit_tb(NULL, 0);
            gen_set_label(no_pal);
        }
        if (x6 == 0x20 && b2 != 0) {
            gen_helper_manual_call_enter(tcg_env,
                                          tcg_constant_i64(ctx->base.pc_next));
        }
        if (x6 == 0x21) {
            gen_helper_ret_restore(tcg_env);
        } else if (b2 == 0) {
            gen_helper_ret_restore_b0(tcg_env);
        }

        /*
         * Firmware-preboot handoff: Xen/KVM GFW returns via br.ret b0 where
         * b0==0, leaving the VMM to enter the guest kernel. When enabled by
         * the IPF machine, detect that return and jump to the loaded -kernel.
         */
        if (b2 == 0) {
            TCGLabel *no_handoff = gen_new_label();
            TCGv_i64 active = tcg_temp_new_i64();
            tcg_gen_ld_i64(active, tcg_env,
                           offsetof(CPUIA64State, fw_preboot_active));
            tcg_gen_brcondi_i64(TCG_COND_EQ, active, 0, no_handoff);
            tcg_gen_brcondi_i64(TCG_COND_NE, tgt, 0, no_handoff);
            gen_helper_fw_enter_kernel(tcg_env);
            tcg_gen_exit_tb(NULL, 0);
            gen_set_label(no_handoff);
        }

        gen_helper_check_null_branch(tcg_env,
                                     tcg_constant_i64(ctx->base.pc_next),
                                     tcg_constant_i32(ctx->ri),
                                     tcg_constant_i64(insn),
                                     tgt);
        gen_record_branch(ctx, insn, (x6 == 0x21) ? 2 : 1, tgt);
        tcg_gen_mov_i64(cpu_pc, tgt);
        gen_set_ri_const(0);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (major == 0x1) {
        /* br.call b1 = b2 */
        uint8_t b1 = extract64(insn, 6, 3);
        uint8_t b2 = extract64(insn, 13, 3);
        /*
         * If b1 == b2 (common "br.call b0=b0" pattern), the target register
         * must be read before writing the return pointer.
         */
        TCGv_i64 tgt = tcg_temp_new_i64();
        tcg_gen_andi_i64(tgt, cpu_b[b2], ~0xFULL);
        if (ia64_bcall_should_log(ctx->base.pc_next)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "br.call(reg) pc=%016" PRIx64 " insn=%011" PRIx64
                          " b1=%u b2=%u\n",
                          ctx->base.pc_next, insn, b1, b2);
        }
        if (ia64_dbg_call_pc_match(ctx->base.pc_next)) {
            gen_helper_dbg_call(tcg_env, tcg_constant_i64(ctx->base.pc_next));
        }
        gen_helper_call(tcg_env, tcg_constant_i64(ctx->base.pc_next), tgt);
        gen_helper_check_null_branch(tcg_env,
                                     tcg_constant_i64(ctx->base.pc_next),
                                     tcg_constant_i32(ctx->ri),
                                     tcg_constant_i64(insn),
                                     tgt);
        if (b1 == 7) {
            gen_helper_fw_b7_write(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(IA64_B7_WRITE_BRCALL_REG),
                                   tcg_constant_i32(b2),
                                   tcg_constant_i64(ctx->base.pc_next + 16));
        }
        tcg_gen_movi_i64(cpu_b[b1], ctx->base.pc_next + 16);
        if (b1 == 0) {
            gen_record_b0_write(ctx, insn, 1,
                                tcg_constant_i64(ctx->base.pc_next + 16));
        }
        gen_record_branch(ctx, insn, 3, tgt);
        tcg_gen_mov_i64(cpu_pc, tgt);
        gen_set_ri_const(0);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (major == 0x4) {
        uint8_t btype = extract64(insn, 6, 3); /* btype{8:6} */
        uint64_t imm = extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
        int64_t disp = sextract64(imm, 0, 21) << 4;

        if (btype == 0) {
            /* B1: br.cond target25 */
            gen_record_branch(ctx, insn, 4,
                              tcg_constant_i64(ctx->base.pc_next + disp));
            tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next + disp);
            gen_set_ri_const(0);
            if (qp == 0) {
                ctx->base.is_jmp = DISAS_NORETURN;
            }
            tcg_gen_exit_tb(NULL, 0);
	        } else if (btype == 2 || btype == 3) {
	            /* B1: br.wexit/br.wtop target25 */
	            const bool is_wtop = (btype == 3);
	            /*
	             * Per SKI brWtopEx/brWexitEx + tgtPrRd:
	             *   - SRC2 is the qualifying predicate value (qp), not p63.
	             *   - The branch condition is based on qp and ar.ec.
	             *
	             * This is critical for compiler-generated software-pipelined
	             * loops (e.g. Linux strlen()) which use br.wtop as their loop
	             * backedge and rely on qp-driven iteration.
	             */
	            TCGv_i64 qp_val = gen_pr_read_bit(qp);
	            TCGv_i64 ec = gen_load_ar(66); /* ar.ec */
	            /* EC_CNT is bits 63..58 of ar.ec (see SKI state.h). */
	            TCGv_i64 ec_cnt = tcg_temp_new_i64();
	            tcg_gen_shri_i64(ec_cnt, ec, 58);
	            tcg_gen_andi_i64(ec_cnt, ec_cnt, 0x3f);

	            /* cond = qp || (ec > 1) for wtop, inverted for wexit. */
	            TCGv_i64 ec_gt1 = tcg_temp_new_i64();
	            tcg_gen_setcondi_i64(TCG_COND_GT, ec_gt1, ec_cnt, 1);
	            TCGv_i64 cond = tcg_temp_new_i64();
	            tcg_gen_or_i64(cond, qp_val, ec_gt1);
	            if (!is_wtop) {
	                tcg_gen_xori_i64(cond, cond, 1);
            }
            tcg_gen_andi_i64(cond, cond, 1);

            /* PrWrt(63, 0) */
	            gen_pr_write_bit(63, tcg_constant_i64(0));

	            /*
	             * if (qp) rotate_regs();
	             * else if (EC_CNT > 0) { EC_CNT--; rotate_regs(); }
	             */
	            TCGLabel *after_sidefx = gen_new_label();
	            TCGLabel *check_ec = gen_new_label();
	            tcg_gen_brcondi_i64(TCG_COND_EQ, qp_val, 0, check_ec);
	            gen_rotate_regs();
	            tcg_gen_br(after_sidefx);
	            gen_set_label(check_ec);
	            tcg_gen_brcondi_i64(TCG_COND_EQ, ec_cnt, 0, after_sidefx);
	            TCGv_i64 ec_cnt1 = tcg_temp_new_i64();
	            tcg_gen_addi_i64(ec_cnt1, ec_cnt, -1);
	            /* Write EC_CNT back into ar.ec, preserving other fields. */
	            TCGv_i64 ec_masked = tcg_temp_new_i64();
	            tcg_gen_andi_i64(ec_masked, ec, ~(0x3fULL << 58));
	            TCGv_i64 ec_cnt_sh = tcg_temp_new_i64();
	            tcg_gen_shli_i64(ec_cnt_sh, ec_cnt1, 58);
	            TCGv_i64 ec_new = tcg_temp_new_i64();
	            tcg_gen_or_i64(ec_new, ec_masked, ec_cnt_sh);
	            gen_store_ar(66, ec_new);
	            gen_rotate_regs();
	            gen_set_label(after_sidefx);

	            /* Conditional branch on cond. */
	            TCGLabel *not_taken = gen_new_label();
            tcg_gen_brcondi_i64(TCG_COND_EQ, cond, 0, not_taken);
            tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next + disp);
            gen_set_ri_const(0);
            tcg_gen_exit_tb(NULL, 0);
            gen_set_label(not_taken);
        } else if (btype == 5) {
            /* B2: br.cloop target25 */
            TCGv_i64 lc = gen_load_ar(65); /* ar.lc */
            TCGLabel *not_taken = gen_new_label();
            tcg_gen_brcondi_i64(TCG_COND_EQ, lc, 0, not_taken);

            /* If taken, decrement LC and branch. */
            TCGv_i64 lc1 = tcg_temp_new_i64();
            tcg_gen_addi_i64(lc1, lc, -1);
            gen_store_ar(65, lc1);

            tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next + disp);
            gen_set_ri_const(0);
            tcg_gen_exit_tb(NULL, 0);

            gen_set_label(not_taken);
	        } else if (btype == 6 || btype == 7) {
	            /* B2: br.cexit/br.ctop target25 */
	            const bool is_ctop = (btype == 7);
	            TCGv_i64 lc = gen_load_ar(65); /* ar.lc */
	            TCGv_i64 ec = gen_load_ar(66); /* ar.ec */
	            TCGv_i64 ec_cnt = tcg_temp_new_i64();
	            tcg_gen_shri_i64(ec_cnt, ec, 58);
	            tcg_gen_andi_i64(ec_cnt, ec_cnt, 0x3f);

	            TCGv_i64 lc_gt0 = tcg_temp_new_i64();
	            tcg_gen_setcondi_i64(TCG_COND_NE, lc_gt0, lc, 0);
	            TCGv_i64 ec_gt1 = tcg_temp_new_i64();
	            tcg_gen_setcondi_i64(TCG_COND_GT, ec_gt1, ec_cnt, 1);

            TCGv_i64 cond_base = tcg_temp_new_i64();
            tcg_gen_or_i64(cond_base, lc_gt0, ec_gt1);

            TCGv_i64 cond = tcg_temp_new_i64();
            if (is_ctop) {
                tcg_gen_andi_i64(cond, cond_base, 1);
            } else {
                tcg_gen_xori_i64(cond, cond_base, 1);
                tcg_gen_andi_i64(cond, cond, 1);
            }

            /*
             * if (lc > 0) { lc--; PrWrt(63,1); rotate_regs(); }
             * else if (ec > 0) { ec--; PrWrt(63,0); rotate_regs(); }
             * else { PrWrt(63,0); }
	             */
	            TCGLabel *after_sidefx = gen_new_label();
	            TCGLabel *check_ec = gen_new_label();
	            tcg_gen_brcondi_i64(TCG_COND_EQ, lc_gt0, 0, check_ec);
	            {
	                TCGv_i64 lc1 = tcg_temp_new_i64();
	                tcg_gen_addi_i64(lc1, lc, -1);
	                gen_store_ar(65, lc1);
	                gen_pr_write_bit(63, tcg_constant_i64(1));
	                gen_rotate_regs();
	                tcg_gen_br(after_sidefx);
	            }
	            gen_set_label(check_ec);
	            tcg_gen_brcondi_i64(TCG_COND_EQ, ec_cnt, 0, after_sidefx);
	            {
	                TCGv_i64 ec_cnt1 = tcg_temp_new_i64();
	                tcg_gen_addi_i64(ec_cnt1, ec_cnt, -1);
	                TCGv_i64 ec_masked = tcg_temp_new_i64();
	                tcg_gen_andi_i64(ec_masked, ec, ~(0x3fULL << 58));
	                TCGv_i64 ec_cnt_sh = tcg_temp_new_i64();
	                tcg_gen_shli_i64(ec_cnt_sh, ec_cnt1, 58);
	                TCGv_i64 ec_new = tcg_temp_new_i64();
	                tcg_gen_or_i64(ec_new, ec_masked, ec_cnt_sh);
	                gen_store_ar(66, ec_new);
	                gen_pr_write_bit(63, tcg_constant_i64(0));
	                gen_rotate_regs();
	            }
	            gen_set_label(after_sidefx);

            /* Conditional branch on cond. */
            TCGLabel *not_taken = gen_new_label();
            tcg_gen_brcondi_i64(TCG_COND_EQ, cond, 0, not_taken);
            gen_helper_check_null_branch(tcg_env,
                                         tcg_constant_i64(ctx->base.pc_next),
                                         tcg_constant_i32(ctx->ri),
                                         tcg_constant_i64(insn),
                                         tcg_constant_i64(ctx->base.pc_next + disp));
            gen_record_branch(ctx, insn, 4,
                              tcg_constant_i64(ctx->base.pc_next + disp));
            tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next + disp);
            gen_set_ri_const(0);
            tcg_gen_exit_tb(NULL, 0);
            gen_set_label(not_taken);
        } else {
            gen_unimpl(ctx, insn, "B-slot major=4 btype");
        }
    } else if (major == 0x5) {
        /* br.call target25 */
        uint8_t b1 = extract64(insn, 6, 3);
        uint64_t imm = extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
        int64_t disp = sextract64(imm, 0, 21) << 4;
        uint64_t tgt = ctx->base.pc_next + disp;
        if (ia64_bcall_should_log(ctx->base.pc_next)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "br.call pc=%016" PRIx64 " insn=%011" PRIx64
                          " b1=%u disp=%" PRId64 " tgt=%016" PRIx64 "\n",
                          ctx->base.pc_next, insn, b1, disp,
                          tgt);
        }
        if (ia64_dbg_call_pc_match(ctx->base.pc_next)) {
            gen_helper_dbg_call(tcg_env, tcg_constant_i64(ctx->base.pc_next));
        }
        gen_helper_call(tcg_env, tcg_constant_i64(ctx->base.pc_next),
                        tcg_constant_i64(tgt));
        gen_helper_check_null_branch(tcg_env,
                                     tcg_constant_i64(ctx->base.pc_next),
                                     tcg_constant_i32(ctx->ri),
                                     tcg_constant_i64(insn),
                                     tcg_constant_i64(tgt));
        if (b1 == 7) {
            gen_helper_fw_b7_write(tcg_env,
                                   tcg_constant_i64(ctx->base.pc_next),
                                   tcg_constant_i32(IA64_B7_WRITE_BRCALL_IMM),
                                   tcg_constant_i32(0),
                                   tcg_constant_i64(ctx->base.pc_next + 16));
        }
        tcg_gen_movi_i64(cpu_b[b1], ctx->base.pc_next + 16);
        if (b1 == 0) {
            gen_record_b0_write(ctx, insn, 1,
                                tcg_constant_i64(ctx->base.pc_next + 16));
        }
        gen_record_branch(ctx, insn, 5, tcg_constant_i64(tgt));
        tcg_gen_movi_i64(cpu_pc, tgt);
        gen_set_ri_const(0);
        if (qp == 0) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        tcg_gen_exit_tb(NULL, 0);
    } else if (((insn >> 37) & 0xf) == 0x2 && ((insn >> 27) & 0x3f) == 0x0) {
        /* nop.b */
        /* nothing */
    } else {
        gen_unimpl(ctx, insn, "B-slot");
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
        /*
         * IA-64 integer A-unit operations can be issued in M-slots as well
         * (e.g. "adds"/"mov" idioms in kernel prologues and MLX bundles).
         * Decode those here before falling back to memory/control decoding.
         */
        if (major >= 0x8 && major <= 0xE) {
            decode_a_unit(ctx, insn);
            break;
        }
        switch (major) {
        case 0x0: {
            /*
             * Privileged/control ops (op=0) in the M-unit.
             * Decode using x3/x4/x2 (SKI encoding.opcode).
             */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint8_t x3 = extract64(insn, 33, 3);
            uint8_t x4 = extract64(insn, 27, 4);
            uint8_t x2 = extract64(insn, 31, 2);

            if (x3 == 5) {
                /*
                 * M22: chk.a.{nc,clr} r1, target25
                 *
                 * Linux uses advanced integer loads (ld*.a) together with
                 * chk.a.clr recovery branches (notably in genl_op_iter_next()).
                 * Model a minimal ALAT so that chk.a.clr can detect aliasing
                 * stores and take the recovery path.
                 */
                uint8_t r1 = extract64(insn, 6, 7);
                uint64_t imm =
                    extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
                int64_t disp = sextract64(imm, 0, 21) << 4;
                uint64_t tgt = ctx->base.pc_next + disp;

                TCGv_i64 miss = tcg_temp_new_i64();
                gen_helper_alat_check_gr(miss, tcg_env,
                                         tcg_constant_i32(r1),
                                         tcg_constant_i32(1));

                TCGLabel *not_taken = gen_new_label();
                tcg_gen_brcondi_i64(TCG_COND_EQ, miss, 0, not_taken);
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                tcg_gen_exit_tb(NULL, 0);
                gen_set_label(not_taken);
            } else if (x3 == 4) {
                /* M22: chk.a.nc r1, target25 */
                uint8_t r1 = extract64(insn, 6, 7);
                uint64_t imm =
                    extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
                int64_t disp = sextract64(imm, 0, 21) << 4;
                uint64_t tgt = ctx->base.pc_next + disp;

                TCGv_i64 miss = tcg_temp_new_i64();
                gen_helper_alat_check_gr(miss, tcg_env,
                                         tcg_constant_i32(r1),
                                         tcg_constant_i32(0));

                TCGLabel *not_taken = gen_new_label();
                tcg_gen_brcondi_i64(TCG_COND_EQ, miss, 0, not_taken);
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                tcg_gen_exit_tb(NULL, 0);
                gen_set_label(not_taken);
            } else if (x3 == 7 && x4 == 0 && x2 == 0) {
                /*
                 * chk.a.clr f1, target25
                 *
                 * The kernel uses advanced floating-point loads (ldf*.a) with
                 * chk.a.clr recovery branches. We do not model the ALAT yet;
                 * treat advanced loads as always successful, so the check does
                 * not branch.
                 */
                uint8_t f1 = extract64(insn, 6, 7) & 0x7f;
                uint64_t imm =
                    extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
                int64_t disp = sextract64(imm, 0, 21) << 4;
                uint64_t tgt = ctx->base.pc_next + disp;

                TCGv_i64 miss = tcg_temp_new_i64();
                gen_helper_alat_check_fr(miss, tcg_env,
                                         tcg_constant_i32(f1),
                                         tcg_constant_i32(1));

                TCGLabel *not_taken = gen_new_label();
                tcg_gen_brcondi_i64(TCG_COND_EQ, miss, 0, not_taken);
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                tcg_gen_exit_tb(NULL, 0);
                gen_set_label(not_taken);
            } else if (x3 == 6 && x4 == 0 && x2 == 0) {
                /* M23: chk.a.nc f1, target25 */
                uint8_t f1 = extract64(insn, 6, 7) & 0x7f;
                uint64_t imm =
                    extract64(insn, 13, 20) | (extract64(insn, 36, 1) << 20);
                int64_t disp = sextract64(imm, 0, 21) << 4;
                uint64_t tgt = ctx->base.pc_next + disp;

                TCGv_i64 miss = tcg_temp_new_i64();
                gen_helper_alat_check_fr(miss, tcg_env,
                                         tcg_constant_i32(f1),
                                         tcg_constant_i32(0));

                TCGLabel *not_taken = gen_new_label();
                tcg_gen_brcondi_i64(TCG_COND_EQ, miss, 0, not_taken);
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                tcg_gen_exit_tb(NULL, 0);
                gen_set_label(not_taken);
            } else if (x3 == 0 && (x4 == 0x6 || x4 == 0x7)) {
                /* M44: ssm/rsm imm24 (per ski encoding.format + encoding.imm) */
                uint64_t imm21a = extract64(insn, 6, 21);
                uint64_t i = extract64(insn, 36, 1);
                uint64_t i2d = extract64(insn, 31, 2);
                uint64_t imm24 = (i << 23) | (i2d << 21) | imm21a;
                TCGv_i64 old_psr = tcg_temp_new_i64();
                tcg_gen_mov_i64(old_psr, cpu_psr);
                if (x4 == 0x6) {
                    tcg_gen_ori_i64(cpu_psr, cpu_psr, imm24);
                } else {
                    TCGv_i64 mask = tcg_temp_new_i64();
                    tcg_gen_movi_i64(mask, ~imm24);
                    tcg_gen_and_i64(cpu_psr, cpu_psr, mask);
                }
                if (imm24 & (IA64_PSR_DT | IA64_PSR_IT)) {
                    TCGv_i64 new_psr = tcg_temp_new_i64();
                    tcg_gen_mov_i64(new_psr, cpu_psr);
                    gen_helper_fw_psr_update_log(tcg_env,
                                                 tcg_constant_i64(ctx->base.pc_next),
                                                 old_psr, new_psr,
                                                 tcg_constant_i64(imm24));
                }
                /* PSR update affects translation mode; end TB here. */
                ctx->base.is_jmp = DISAS_TOO_MANY;
            } else if (x3 == 0 && x2 == 0 && x4 == 0x1) {
                /* M48: nop.m/hint.m imm21 (hints ignored) */
            } else if (x3 == 0 && x2 == 2 && (x4 == 0x2 || x4 == 0x3)) {
                /* M24: mf/mf.a (full memory fence) */
                tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
            } else if (x3 == 0 && x2 == 1 && x4 == 0x0) {
                /* M24: invala (invalidate ALAT) */
                gen_helper_alat_invalidate_all(tcg_env);
            } else if (x3 == 0 && x2 == 3 && x4 == 0x0) {
                /* M24: srlz.d */
                gen_helper_srlz_d(tcg_env);
            } else if (x3 == 0 && x2 == 3 && x4 == 0x1) {
                /* M24: srlz.i */
                gen_helper_srlz_i(tcg_env);
            } else if (x3 == 0 && x2 == 3 && x4 == 0x3) {
                /* M24: sync.i (treat like srlz.i in this model) */
                gen_helper_srlz_i(tcg_env);
            } else if (x3 == 0 && x2 == 0 && x4 == 0xC) {
                /* M25: flushrs */
                gen_helper_flushrs(tcg_env);
            } else if (x3 == 0 && x2 == 0 && x4 == 0xA) {
                /* M25: loadrs */
                gen_helper_loadrs(tcg_env);
            } else if (x3 == 0 && x2 == 2 && x4 == 0x8) {
                /* M30: mov.m ar3 = imm8 */
                uint8_t ar = extract64(insn, 20, 7);
                uint64_t imm8 = extract64(insn, 13, 7) | (extract64(insn, 36, 1) << 7);
                int64_t simm8 = sextract64(imm8, 0, 8);
                TCGv_i64 t = tcg_temp_new_i64();
                tcg_gen_movi_i64(t, simm8);
                if (ar == 18) {
                    gen_helper_set_bspstore(tcg_env, t);
                } else if (ar == 66) {
                    TCGv_i64 ec = tcg_temp_new_i64();
                    tcg_gen_andi_i64(ec, t, 0x3f);
                    tcg_gen_shli_i64(ec, ec, 58);
                    gen_store_ar(ar, ec);
                } else if (ar == 65) {
                    gen_helper_dbg_ar_lc_store(tcg_env,
                                               tcg_constant_i64(ctx->base.pc_next),
                                               t);
                    gen_store_ar(ar, t);
                } else if (ar == IA64_AR_K4) {
                    gen_helper_fw_ar_k4_store(tcg_env,
                                              tcg_constant_i64(ctx->base.pc_next),
                                              tcg_constant_i32(-1),
                                              t);
                } else if (ar == IA64_AR_K5) {
                    gen_helper_fw_ar_k5_store(tcg_env,
                                              tcg_constant_i64(ctx->base.pc_next),
                                              tcg_constant_i32(-1),
                                              t);
                } else {
                    gen_store_ar(ar, t);
                }
            } else if (x3 == 0 && x2 == 0 && x4 == 0x0) {
                /* M37: break.m imm21 */
                uint64_t imm = (extract64(insn, 36, 1) << 20) |
                               extract64(insn, 6, 20);
                bool in_fw = ia64_pc_in_fw(ctx->base.pc_next);
                bool fw_hypercall = ia64_fw_break_hypercall_enabled();
                ia64_fw_break_hypercall_log(ctx->base.pc_next, imm, in_fw, fw_hypercall);
                if (imm == 0x80000 || imm == 0x80001) {
                    TCGv_i64 timm = tcg_temp_new_i64();
                    tcg_gen_movi_i64(timm, imm);
                    TCGv_i64 ret = tcg_temp_new_i64();
                    gen_helper_ssc(ret, tcg_env, timm);
                    tcg_gen_mov_i64(cpu_r[8], ret);
                } else if ((imm & 0xff) == 0) {
                    /*
                     * Xen IA-64 guest firmware hypercalls encode the call
                     * number in the BREAK immediate shifted left by 8:
                     *   imm = hypercall_nr << 8
                     *
                     * Handle the essential PAL/SAL calls directly so the GFW
                     * can run under TCG without taking a break fault.
                     */
                    uint64_t nr = imm >> 8;
                    bool allow_fw = in_fw || fw_hypercall;
                    if (!allow_fw) {
                        TCGLabel *do_break = gen_new_label();
                        TCGLabel *done = gen_new_label();
                        TCGv_i64 active = tcg_temp_new_i64();

                        tcg_gen_ld_i64(active, tcg_env,
                                       offsetof(CPUIA64State, fw_preboot_active));
                        tcg_gen_brcondi_i64(TCG_COND_EQ, active, 0, do_break);

                        if (nr == 0x0) {
                            /*
                             * Firmware break(0) call gate.
                             *
                             * Some xenipf/EDK paths end up executing a break(0)
                             * without a proper IVT installed yet. Handle it as a
                             * firmware status hook to avoid trapping into the
                             * empty break vector.
                             */
                            gen_helper_fw_break0(tcg_env,
                                                 tcg_constant_i64(ctx->base.pc_next));
                        } else if (nr == 0x1100) {
                            /* FW_HYPERCALL_SAL_CALL */
                            gen_helper_fw_sal_break(tcg_env);
                        } else if (nr == 0x1000) {
                            /* FW_HYPERCALL_PAL_CALL */
                            gen_helper_fw_pal(tcg_env);
                        } else {
                            gen_unimpl(ctx, insn, "break.m hypercall");
                        }

                        tcg_gen_br(done);
                        gen_set_label(do_break);
                        gen_helper_breaki(tcg_env, tcg_constant_i64(imm));
                        if (qp == 0) {
                            ctx->base.is_jmp = DISAS_NORETURN;
                        }
                        gen_set_label(done);
                    } else if (nr == 0x0) {
                        /*
                         * Firmware break(0) call gate.
                         *
                         * Some xenipf/EDK paths end up executing a break(0)
                         * without a proper IVT installed yet. Handle it as a
                         * firmware status hook to avoid trapping into the
                         * empty break vector.
                         */
                        gen_helper_fw_break0(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next));
                    } else if (nr == 0x1100) {
                        /* FW_HYPERCALL_SAL_CALL */
                        gen_helper_fw_sal_break(tcg_env);
                    } else if (nr == 0x1000) {
                        /* FW_HYPERCALL_PAL_CALL */
                        gen_helper_fw_pal(tcg_env);
                    } else {
                        if (in_fw) {
                            gen_unimpl(ctx, insn, "break.m hypercall");
                        } else {
                            gen_helper_breaki(tcg_env, tcg_constant_i64(imm));
                            if (qp == 0) {
                                ctx->base.is_jmp = DISAS_NORETURN;
                            }
                        }
                    }
                } else {
                    gen_helper_breaki(tcg_env, tcg_constant_i64(imm));
                    if (qp == 0) {
                        ctx->base.is_jmp = DISAS_NORETURN;
                    }
                }
            } else {
                gen_unimpl(ctx, insn, "M-slot");
            }

            if (skip_label) {
                gen_set_label(skip_label);
            }
            break;
        }
        case 0x1: {
            /* mov to/from control regs and region regs */
            uint8_t x3 = (insn >> 33) & 0x7;
            uint8_t x6 = (insn >> 27) & 0x3f;
            if (x3 == 1) {
                /* M20: chk.s.m r2, target25 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint64_t imm =
                    (extract64(insn, 36, 1) << 20) |
                    (extract64(insn, 20, 13) << 7) |
                    extract64(insn, 6, 7);
                int64_t disp = sextract64(imm, 0, 21) << 4;
                uint64_t tgt = ctx->base.pc_next + disp;

                TCGv_i64 nat = tcg_temp_new_i64();
                gen_helper_gr_nat(nat, tcg_env, tcg_constant_i32(r2));

                TCGLabel *not_taken = gen_new_label();
                tcg_gen_brcondi_i64(TCG_COND_EQ, nat, 0, not_taken);
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                tcg_gen_exit_tb(NULL, 0);
                gen_set_label(not_taken);

                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x30) {
                /* M28: fc/fc.i r3 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                gen_helper_fc(tcg_env, va);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && (x6 == 0x1a || x6 == 0x1b || x6 == 0x1e)) {
                /* M46: thash/ttag/tpa r1 = r3 (SKI encoding.opcode). */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }

                TCGv_i64 dst = tcg_temp_new_i64();
                if (x6 == 0x1e) {
                    gen_helper_tpa(dst, tcg_env, va);
                } else {
                    TCGv_i64 old_ifa = tcg_temp_new_i64();
                    tcg_gen_ld_i64(old_ifa, tcg_env,
                                   offsetof(CPUIA64State, cr_ifa));
                    tcg_gen_st_i64(va, tcg_env, offsetof(CPUIA64State, cr_ifa));
                    if (x6 == 0x1a) {
                        gen_helper_thash(dst, tcg_env);
                    } else {
                        gen_helper_ttag(dst, tcg_env);
                    }
                    tcg_gen_st_i64(old_ifa, tcg_env,
                                   offsetof(CPUIA64State, cr_ifa));
                }

                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], dst);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 6) {
                /* alloc r1 = ar.pfs, i, l, o, r (M34 format) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint64_t sof = extract64(insn, 13, 7);
                uint64_t sol = extract64(insn, 20, 7);
                uint64_t sor = extract64(insn, 27, 4);
                TCGv_i64 old_pfs = tcg_temp_new_i64();
                gen_helper_alloc(old_pfs, tcg_env,
                                 tcg_constant_i64(sof),
                                 tcg_constant_i64(sol),
                                 tcg_constant_i64(sor));
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], old_pfs);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x25) {
                /* M36: mov r1 = psr */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], cpu_psr);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x2d) {
                /* mov psr.l = r2 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }

                TCGv_i64 old_psr = tcg_temp_new_i64();
                tcg_gen_mov_i64(old_psr, cpu_psr);

                TCGv_i64 lo32 = tcg_temp_new_i64();
                tcg_gen_andi_i64(lo32, src, 0xffffffffULL);

                TCGv_i64 upper = tcg_temp_new_i64();
                tcg_gen_andi_i64(upper, cpu_psr, ~0xffffffffULL);
                tcg_gen_or_i64(cpu_psr, upper, lo32);

                {
                    TCGv_i64 new_psr = tcg_temp_new_i64();
                    tcg_gen_mov_i64(new_psr, cpu_psr);
                    gen_helper_fw_psr_update_log(tcg_env,
                                                 tcg_constant_i64(ctx->base.pc_next),
                                                 old_psr, new_psr,
                                                 tcg_constant_i64(IA64_PSR_DT | IA64_PSR_IT));
                }

                /* PSR update affects translation mode; end TB here. */
                ctx->base.is_jmp = DISAS_TOO_MANY;
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x17) {
                /* mov r1 = cpuid[r3] */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                TCGv_i64 idx = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(idx, 0);
                } else {
                    tcg_gen_mov_i64(idx, cpu_r[r3]);
                }
                TCGv_i64 dst = tcg_temp_new_i64();
                gen_helper_get_cpuid(dst, tcg_env, idx);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], dst);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x0) {
                /* mov rr[r3] = r2 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 idx = tcg_temp_new_i64();
                TCGv_i64 val = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(idx, 0);
                } else {
                    tcg_gen_mov_i64(idx, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(val, 0);
                } else {
                    tcg_gen_mov_i64(val, cpu_r[r2]);
                }
                gen_store_rr_reg(idx, val);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x10) {
                /* mov r1 = rr[r3] */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                if (r1 != 0) {
                    TCGv_i64 idx = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(idx, 0);
                    } else {
                        tcg_gen_mov_i64(idx, cpu_r[r3]);
                    }
                    gen_load_rr_reg(cpu_r[r1], idx);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x1e) {
                /* tpa r1 = r3 (M46) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                TCGv_i64 src = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r3]);
                }
                TCGv_i64 dst = tcg_temp_new_i64();
                gen_helper_tpa(dst, tcg_env, src);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], dst);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x9) {
                /* ptc.l */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r2]);
                }
                gen_helper_ptc_l(tcg_env, va, tar);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0xa) {
                /* ptc.g */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r2]);
                }
                gen_helper_ptc_g(tcg_env, va, tar);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0xb) {
                /* ptc.ga */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r2]);
                }
                gen_helper_ptc_ga(tcg_env, va, tar);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0xc) {
                /* ptr.d */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 range = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(range, 0);
                } else {
                    tcg_gen_mov_i64(range, cpu_r[r2]);
                }
                gen_helper_ptr_d(tcg_env, va, range);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0xd) {
                /* ptr.i */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                TCGv_i64 range = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(range, 0);
                } else {
                    tcg_gen_mov_i64(range, cpu_r[r2]);
                }
                gen_helper_ptr_i(tcg_env, va, range);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x34) {
                /* ptc.e r3 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                TCGv_i64 va = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(va, 0);
                } else {
                    tcg_gen_mov_i64(va, cpu_r[r3]);
                }
                gen_helper_ptc_e(tcg_env, va, tcg_constant_i64(0));
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0xe) {
                /* itr.d */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 pte = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(pte, 0);
                } else {
                    tcg_gen_mov_i64(pte, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r3]);
                }
                gen_helper_itr_d(tcg_env, pte, tar);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0xf) {
                /* itr.i */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                TCGv_i64 pte = tcg_temp_new_i64();
                TCGv_i64 tar = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(pte, 0);
                } else {
                    tcg_gen_mov_i64(pte, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(tar, 0);
                } else {
                    tcg_gen_mov_i64(tar, cpu_r[r3]);
                }
                gen_helper_itr_i(tcg_env, pte, tar);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && (x6 == 0x2e || x6 == 0x2f)) {
                /* itc.d / itc.i */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                if (x6 == 0x2e) {
                    gen_helper_itc_d(tcg_env, cpu_r[r2]);
                } else {
                    gen_helper_itc_i(tcg_env, cpu_r[r2]);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x2c) {
                /* mov cr[r3] = r2 (M32 format) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t cr = extract64(insn, 20, 7);
                gen_store_cr_reg(cr, cpu_r[r2]);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x24) {
                /* mov r1 = cr[r3] (M33 format) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t cr = extract64(insn, 20, 7);
                TCGv_i64 t = gen_load_cr_reg(cr);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x22) {
                /* mov r1 = ar[r3] (M31 format) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t ar = extract64(insn, 20, 7);
                TCGv_i64 t = gen_load_ar(ar);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 0 && x6 == 0x2a) {
                /* mov ar[r3] = r2 (M29 format) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t ar = extract64(insn, 20, 7);
                TCGv_i64 t = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(t, 0);
                } else {
                    tcg_gen_mov_i64(t, cpu_r[r2]);
                }
                if (ar == 18) {
                    gen_helper_set_bspstore(tcg_env, t);
                } else if (ar == 66) {
                    TCGv_i64 ec = tcg_temp_new_i64();
                    tcg_gen_andi_i64(ec, t, 0x3f);
                    tcg_gen_shli_i64(ec, ec, 58);
                    gen_store_ar(ar, ec);
                } else if (ar == 3) {
                    gen_helper_fw_ar_k3_store(tcg_env,
                                              tcg_constant_i64(ctx->base.pc_next),
                                              tcg_constant_i32(r2),
                                              t);
                } else if (ar == IA64_AR_K4) {
                    gen_helper_fw_ar_k4_store(tcg_env,
                                              tcg_constant_i64(ctx->base.pc_next),
                                              tcg_constant_i32(r2),
                                              t);
                } else if (ar == IA64_AR_K5) {
                    gen_helper_fw_ar_k5_store(tcg_env,
                                              tcg_constant_i64(ctx->base.pc_next),
                                              tcg_constant_i32(r2),
                                              t);
                } else {
                    gen_store_ar(ar, t);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            } else if (x3 == 7 && x6 == 0x38) {
                /* mov b[r3] = r2 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t b = extract64(insn, 20, 3);
                TCGv_i64 t = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(t, 0);
                } else {
                    tcg_gen_mov_i64(t, cpu_r[r2]);
                }
                if (b == 7) {
                    gen_helper_fw_b7_write(tcg_env,
                                           tcg_constant_i64(ctx->base.pc_next),
                                           tcg_constant_i32(IA64_B7_WRITE_MOV_M),
                                           tcg_constant_i32(r2),
                                           t);
                }
                tcg_gen_mov_i64(cpu_b[b], t);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            gen_unimpl(ctx, insn, "M-slot");
            break;
        }
        case 0x4:
        case 0x5: {
            /* Integer loads/stores (M1/M2/M3 for loads, M4/M5 for stores) */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint8_t x6 = extract64(insn, 30, 6);
            uint8_t x = extract64(insn, 27, 1);
            uint8_t m = extract64(insn, 36, 1);
            uint8_t r3 = extract64(insn, 20, 7);
            uint8_t r2 = extract64(insn, 13, 7);
            uint8_t r1 = extract64(insn, 6, 7);
            bool is_imm = (major == 0x5);
            uint8_t x_mem = is_imm ? 0 : x;
            int64_t imm9 = 0;
            bool handled = false;

            if (ia64_dbg_munit_match(ctx->base.pc_next)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "dbg_munit pc=%016" PRIx64 " ri=%u insn=%011"
                              PRIx64 " qp=%u major=%u x6=0x%x x=%u m=%u"
                              " r1=%u r2=%u r3=%u imm=%u\n",
                              ctx->base.pc_next, ctx->ri, insn,
                              qp, major, x6, x, m, r1, r2, r3, is_imm);
            }

            /* M19: getf.sig r1 = f2 (op=4 m=0 x=1 x6=0x1c) */
            if (!is_imm && m == 0 && x == 1 && x6 == 0x1c) {
                uint8_t f2 = r2;
                TCGv_i64 t = tcg_temp_new_i64();
                gen_helper_getf_sig(t, tcg_env, tcg_constant_i32(f2));
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                handled = true;
            }
            /* M19: getf.exp r1 = f2 (op=4 m=0 x=1 x6=0x1d) */
            if (!handled && !is_imm && m == 0 && x == 1 && x6 == 0x1d) {
                uint8_t f2 = r2 & 0x7f;
                TCGv_i64 t = tcg_temp_new_i64();
                gen_fr_load_hi(t, f2);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                handled = true;
            }
            if (handled) {
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            if (is_imm) {
                /* M3: imm in r2-field; M5: imm in r1-field. */
                uint64_t imm7 = (x6 < 0x30) ? r2 : r1;
                imm9 = sextract64(imm7 |
                                  (extract64(insn, 27, 1) << 7) |
                                  (extract64(insn, 36, 1) << 8),
                                  0, 9);
            }

            /* Base address */
            TCGv_i64 base = tcg_temp_new_i64();
            TCGv_i64 addr = tcg_temp_new_i64();
            if (r3 == 0) {
                tcg_gen_movi_i64(base, 0);
            } else {
                tcg_gen_mov_i64(base, cpu_r[r3]);
            }
            tcg_gen_mov_i64(addr, base);

            /* M2 reg+reg: m=1/x=0, use r2 as index (no scale) */
            bool reg_index = (!is_imm && x_mem == 0 && m);
            if (reg_index) {
                TCGv_i64 t = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_mov_i64(t, addr);
                } else {
                    tcg_gen_add_i64(t, addr, cpu_r[r2]);
                }
                tcg_gen_mov_i64(addr, t);
            }
            TCGv_i64 mem_addr = gen_mem_addr(ctx, addr);

            if (!handled && x_mem == 1 && !is_imm && !m) {
                if (x6 <= 0x7) {
                    /* cmpxchg{1,2,4,8} (ignore acq/rel/hints) */
                    MemOp mop = memop_for_size_idx(x6) | MO_ALIGN;
                    TCGv_i64 cmp = gen_load_ar(32); /* ar.ccv */
                    TCGv_i64 val = tcg_temp_new_i64();
                    if (r2 == 0) {
                        tcg_gen_movi_i64(val, 0);
                    } else {
                        tcg_gen_mov_i64(val, cpu_r[r2]);
                    }
                    TCGv_i64 old = tcg_temp_new_i64();
                    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
                    tcg_gen_atomic_cmpxchg_i64(old, mem_addr, cmp, val,
                                               ctx->mem_idx, mop);
                    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
                    gen_helper_alat_invalidate(tcg_env, mem_addr,
                                               tcg_constant_i32(1U << (x6 & 0x3)));
                    if (r1 != 0) {
                        tcg_gen_mov_i64(cpu_r[r1], old);
                    }
                    handled = true;
                } else if (x6 >= 0x8 && x6 <= 0xB) {
                    /* xchg{1,2,4,8} */
                    MemOp mop = memop_for_size_idx(x6) | MO_ALIGN;
                    TCGv_i64 val = tcg_temp_new_i64();
                    if (r2 == 0) {
                        tcg_gen_movi_i64(val, 0);
                    } else {
                        tcg_gen_mov_i64(val, cpu_r[r2]);
                    }
                    TCGv_i64 old = tcg_temp_new_i64();
                    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
                    tcg_gen_atomic_xchg_i64(old, mem_addr, val,
                                            ctx->mem_idx, mop);
                    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
                    gen_helper_alat_invalidate(tcg_env, mem_addr,
                                               tcg_constant_i32(1U << (x6 & 0x3)));
                    if (r1 != 0) {
                        tcg_gen_mov_i64(cpu_r[r1], old);
                    }
                    handled = true;
                } else if (x6 == 0x12 || x6 == 0x13 ||
                           x6 == 0x16 || x6 == 0x17) {
                    /* fetchadd{4,8}.{acq,rel} r1 = [r3], inc3 */
                    uint8_t s = extract64(insn, 15, 1);
                    uint8_t i2b = extract64(insn, 13, 2);
                    int64_t mag = (i2b == 3) ? 1 : (1LL << (4 - i2b));
                    int64_t inc3 = s ? -mag : mag;

                    MemOp mop = memop_for_size_idx(x6) | MO_ALIGN;
                    TCGv_i64 inc = tcg_temp_new_i64();
                    tcg_gen_movi_i64(inc, inc3);
                    TCGv_i64 old = tcg_temp_new_i64();
                    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
                    tcg_gen_atomic_fetch_add_i64(old, mem_addr, inc,
                                                 ctx->mem_idx, mop);
                    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
                    gen_helper_alat_invalidate(tcg_env, mem_addr,
                                               tcg_constant_i32(1U << (x6 & 0x3)));
                    if (r1 != 0) {
                        tcg_gen_mov_i64(cpu_r[r1], old);
                    }
                    handled = true;
                }
            } else if (x_mem == 0 && x6 == 0x1b) {
                /* ld8.fill: load + NaT from ar.unat */
                if (r1 != 0) {
                    TCGv_i64 val = tcg_temp_new_i64();
                    tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx, MO_TE | MO_64);
                    gen_load_watch(ctx, mem_addr, val, 8);
                    tcg_gen_mov_i64(cpu_r[r1], val);

                    TCGv_i64 unat = gen_load_ar(IA64_AR_UNAT);
                    TCGv_i64 pos = tcg_temp_new_i64();
                    TCGv_i64 nat = tcg_temp_new_i64();
                    tcg_gen_shri_i64(pos, mem_addr, 3);
                    tcg_gen_andi_i64(pos, pos, 0x3f);
                    tcg_gen_shr_i64(nat, unat, pos);
                    tcg_gen_andi_i64(nat, nat, 1);
                    gen_helper_gr_nat_set(tcg_env, tcg_constant_i32(r1), nat);
                }
                if (is_imm && r3 != 0 && imm9) {
                    /* M3: post-increment update by imm9. */
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && x6 <= 0x1f) {
                /* ld1/ld2/ld4/ld8 (plus hint/acq/a variants) */
                bool speculative = (x6 >= 0x4 && x6 <= 0x7) ||
                                   (x6 >= 0xC && x6 <= 0xF);
                bool advanced = (x6 >= 0x8 && x6 <= 0xF);
                uint32_t size = 1U << (x6 & 0x3);
                if (speculative) {
                    if (r1 != 0) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        gen_helper_ld_s(val, tcg_env, addr,
                                        tcg_constant_i32(size),
                                        tcg_constant_i32(r1),
                                        tcg_constant_i32(advanced ? 1 : 0));
                        gen_load_watch(ctx, mem_addr, val, size);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    }
                } else {
                    MemOp mop = memop_for_size_idx(x6);
                    if (r1 != 0) {
                        if (ia64_get_watch_load_addr() ||
                            ia64_load_watch_range_match() ||
                            ia64_pei_load_watch_match()) {
                            TCGv_i64 val = tcg_temp_new_i64();
                            tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx, mop);
                            gen_load_watch(ctx, mem_addr, val, size);
                            tcg_gen_mov_i64(cpu_r[r1], val);
                        } else {
                            tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx, mop);
                        }
                    }
                    if (r1 != 0 && advanced) {
                        gen_helper_alat_record_gr(tcg_env,
                                                  tcg_constant_i32(r1),
                                                  mem_addr,
                                                  tcg_constant_i32(size));
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    /* M3: post-increment update by imm9. */
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && (x6 == 0x20 || x6 == 0x28)) {
                /* ld1.c.clr{,.acq}: treat as zero-extended byte load */
                if (r1 != 0) {
                    if (ia64_get_watch_load_addr() ||
                        ia64_load_watch_range_match() ||
                        ia64_pei_load_watch_match()) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UB);
                        gen_load_watch(ctx, mem_addr, val, 1);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UB);
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && (x6 == 0x21 || x6 == 0x29)) {
                /* ld2.c.clr{,.acq}: treat as zero-extended 16-bit load */
                if (r1 != 0) {
                    if (ia64_get_watch_load_addr() ||
                        ia64_load_watch_range_match() ||
                        ia64_pei_load_watch_match()) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UW);
                        gen_load_watch(ctx, mem_addr, val, 2);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UW);
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && (x6 == 0x22 || x6 == 0x2a)) {
                /* ld4.c.clr{,.acq}: treat as zero-extended 32-bit load */
                if (r1 != 0) {
                    if (ia64_get_watch_load_addr() ||
                        ia64_load_watch_range_match() ||
                        ia64_pei_load_watch_match()) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UL);
                        gen_load_watch(ctx, mem_addr, val, 4);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UL);
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && (x6 == 0x23 || x6 == 0x2b)) {
                /* ld8.c.clr{,.acq}: treat as normal 64-bit load (NaT not modeled) */
                if (r1 != 0) {
                    if (ia64_get_watch_load_addr() ||
                        ia64_load_watch_range_match() ||
                        ia64_pei_load_watch_match()) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_64);
                        gen_load_watch(ctx, mem_addr, val, 8);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx,
                                            MO_TE | MO_64);
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && x6 == 0x26) {
                /* ld4.c.nc: treat as zero-extended 32-bit load (NaT/advanced checks not modeled) */
                if (r1 != 0) {
                    if (ia64_get_watch_load_addr() ||
                        ia64_load_watch_range_match() ||
                        ia64_pei_load_watch_match()) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UL);
                        gen_load_watch(ctx, mem_addr, val, 4);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UL);
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && x6 == 0x24) {
                /* ld1.c.nc variants: treat as zero-extended byte load */
                if (r1 != 0) {
                    if (ia64_get_watch_load_addr() ||
                        ia64_load_watch_range_match() ||
                        ia64_pei_load_watch_match()) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UB);
                        gen_load_watch(ctx, mem_addr, val, 1);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UB);
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && x6 == 0x25) {
                /* ld2.c.nc variants: treat as zero-extended 16-bit load */
                if (r1 != 0) {
                    if (ia64_get_watch_load_addr() ||
                        ia64_load_watch_range_match() ||
                        ia64_pei_load_watch_match()) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UW);
                        gen_load_watch(ctx, mem_addr, val, 2);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UW);
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && x6 == 0x27) {
                /* ld8.c.nc variants: treat as normal 64-bit load */
                if (r1 != 0) {
                    if (ia64_get_watch_load_addr() ||
                        ia64_load_watch_range_match() ||
                        ia64_pei_load_watch_match()) {
                        TCGv_i64 val = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(val, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_64);
                        gen_load_watch(ctx, mem_addr, val, 8);
                        tcg_gen_mov_i64(cpu_r[r1], val);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_r[r1], mem_addr, ctx->mem_idx,
                                            MO_TE | MO_64);
                    }
                }
                if (is_imm && r3 != 0 && imm9) {
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            } else if (x_mem == 0 && x6 >= 0x30 && x6 <= 0x3b) {
                /* st1/st2/st4/st8 (including .rel/.spill variants) */
                uint32_t size = 1U << (x6 & 0x3);
                MemOp mop = memop_for_size_idx(x6);
                TCGv_i64 new_unat = NULL;
                if (x6 == 0x3b) {
                    TCGv_i64 nat = tcg_temp_new_i64();
                    if (r2 == 0) {
                        tcg_gen_movi_i64(nat, 0);
                    } else {
                        gen_helper_gr_nat(nat, tcg_env, tcg_constant_i32(r2));
                    }
                    TCGv_i64 pos = tcg_temp_new_i64();
                    tcg_gen_shri_i64(pos, mem_addr, 3);
                    tcg_gen_andi_i64(pos, pos, 0x3f);

                    TCGv_i64 mask = tcg_temp_new_i64();
                    tcg_gen_movi_i64(mask, 1);
                    tcg_gen_shl_i64(mask, mask, pos);

                    TCGv_i64 unat = gen_load_ar(IA64_AR_UNAT);
                    TCGv_i64 nat_shift = tcg_temp_new_i64();
                    tcg_gen_shl_i64(nat_shift, nat, pos);
                    new_unat = tcg_temp_new_i64();
                    tcg_gen_andc_i64(new_unat, unat, mask);
                    tcg_gen_or_i64(new_unat, new_unat, nat_shift);
                }
                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                TCGv_i64 watch_addr = mem_addr;
                if (ia64_store_watch_match()) {
                    TCGLabel *skip_watch = gen_new_label();
                    tcg_gen_brcondi_i64(TCG_COND_NE, watch_addr,
                                        ia64_store_watch_addr,
                                        skip_watch);
                    gen_helper_dbg_mem_watch(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next),
                                             tcg_constant_i32(ctx->ri),
                                             watch_addr,
                                             tcg_constant_i32(size),
                                             src);
                    gen_set_label(skip_watch);
                }
                if (ia64_store_watch_range_match()) {
                    TCGLabel *skip_watch = gen_new_label();
                    tcg_gen_brcondi_i64(TCG_COND_LT, watch_addr,
                                        ia64_store_watch_range_lo,
                                        skip_watch);
                    tcg_gen_brcondi_i64(TCG_COND_GE, watch_addr,
                                        ia64_store_watch_range_hi,
                                        skip_watch);
                    gen_helper_dbg_mem_watch(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next),
                                             tcg_constant_i32(ctx->ri),
                                             watch_addr,
                                             tcg_constant_i32(size),
                                             src);
                    gen_set_label(skip_watch);
                }
                if (ia64_pei_store_watch_match()) {
                    gen_helper_fw_pei_store_watch(tcg_env,
                                                  tcg_constant_i64(ctx->base.pc_next),
                                                  tcg_constant_i32(ctx->ri),
                                                  watch_addr,
                                                  tcg_constant_i32(size),
                                                  src);
                }
                if (ia64_store_watch_value_match() && size == 8) {
                    uint64_t effective_mask = ia64_store_watch_value_mask;
                    if (effective_mask) {
                        TCGv_i64 masked = tcg_temp_new_i64();
                        TCGv_i64 masked_watch = tcg_temp_new_i64();
                        tcg_gen_andi_i64(masked, src, effective_mask);
                        tcg_gen_movi_i64(masked_watch,
                                         ia64_store_watch_value & effective_mask);
                        TCGLabel *skip_watch = gen_new_label();
                        tcg_gen_brcond_i64(TCG_COND_NE, masked,
                                           masked_watch, skip_watch);
                        gen_helper_dbg_mem_watch(tcg_env,
                                                 tcg_constant_i64(ctx->base.pc_next),
                                                 tcg_constant_i32(ctx->ri),
                                                 watch_addr,
                                                 tcg_constant_i32(size),
                                                 src);
                        gen_set_label(skip_watch);
                    }
                }
                tcg_gen_qemu_st_i64(src, mem_addr, ctx->mem_idx, mop);
                if (new_unat) {
                    gen_store_ar(IA64_AR_UNAT, new_unat);
                }
                {
                    gen_helper_alat_invalidate(tcg_env, mem_addr,
                                               tcg_constant_i32(size));
                }
                if (is_imm && r3 != 0 && imm9) {
                    /* M5: post-increment update by imm9. */
                    tcg_gen_addi_i64(cpu_r[r3], base, imm9);
                }
                handled = true;
            }
            if (!handled) {
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                gen_unimpl(ctx, insn, "M-slot");
                break;
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
        case 0x6:
        case 0x7: {
            /* Assorted M-slot ops in the op=6/7 space. */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint8_t m = extract64(insn, 36, 1);
            uint8_t x = extract64(insn, 27, 1);
            uint8_t x6 = extract64(insn, 30, 6);
            uint8_t x3 = extract64(insn, 33, 3);
            bool handled = false;

            /*
             * lfetch* [r3] (M18-style encoding)
             * major=6, x3=5, m=0, x=0, x6={0x2c..0x2f}.
             * Treat as a hint; for .fault variants, perform a byte probe load
             * (discarded) so translation faults are raised in the right place.
             */
            if ((major == 0x6 || major == 0x7) && x3 == 5 &&
                (x6 == 0x2c || x6 == 0x2d || x6 == 0x2e || x6 == 0x2f)) {
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                /*
                 * Post-increment:
                 * - M14 (op=6, m=1): r3 += r2 (ldPiRd in SKI)
                 * - M15 (op=7):      r3 += imm9 where
                 *                      imm9 = sign_ext(s<<8 | i<<7 | imm7b, 9)
                 *   See SKI's encoding.imm (M15) + lfetch{,F}PiEx.
                 *
                 * Linux __copy_user uses lfetch.fault [rX],128.  Getting the
                 * imm9 decode wrong (e.g. sign-extending only 8 bits) can
                 * send rX backwards and fault on a bogus prefetch address.
                 */
                TCGv_i64 post_inc = NULL;
                int64_t imm9 = 0;
                if (major == 0x6 && m) {
                    post_inc = (r2 == 0) ? tcg_constant_i64(0) : cpu_r[r2];
                } else if (major == 0x7) {
                    uint64_t raw = (uint64_t)r2 | ((uint64_t)x << 7) |
                                   ((uint64_t)m << 8);
                    imm9 = sextract64(raw, 0, 9);
                }

                if (x6 == 0x2e || x6 == 0x2f) {
                    /* .fault variants: raise translation faults at the hint site */
                    TCGv_i64 addr = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(addr, 0);
                    } else {
                        tcg_gen_mov_i64(addr, cpu_r[r3]);
                    }
                    TCGv_i64 mem_addr = gen_mem_addr(ctx, addr);
                    TCGv_i64 tmp = tcg_temp_new_i64();
                    tcg_gen_qemu_ld_i64(tmp, mem_addr, ctx->mem_idx, MO_TE | MO_UB);
                }
                if (r3 != 0) {
                    if (major == 0x6 && m && post_inc) {
                        tcg_gen_add_i64(cpu_r[r3], cpu_r[r3], post_inc);
                    } else if (major == 0x7 && imm9) {
                        tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], imm9);
                    }
                }
                handled = true;
            }

            /*
             * ldf{e,8,s,d} variants.
             *
             * SKI models:
             * - ldf8 as "dword" load (integer significand, exponent=bias+63)
             * - ldfe/ldfd/ldfs as float loads with format-specific conversion
             *
             * GCC uses ldf8/setf.sig + fnorm + getf.exp to implement ia64_fls().
             */
            if (!handled && x == 0) {
                bool is_ldfe = (x6 == 0x00 || x6 == 0x04 || x6 == 0x08 ||
                                x6 == 0x0c || x6 == 0x20 || x6 == 0x24);
                bool is_ldf8 = (x6 == 0x01 || x6 == 0x05 || x6 == 0x09 ||
                                x6 == 0x0d || x6 == 0x21 || x6 == 0x25);
                bool is_ldfs = (x6 == 0x02 || x6 == 0x06 || x6 == 0x0a ||
                                x6 == 0x0e || x6 == 0x22 || x6 == 0x26);
                bool is_ldfd = (x6 == 0x03 || x6 == 0x07 || x6 == 0x0b ||
                                x6 == 0x0f || x6 == 0x23 || x6 == 0x27);

                if (is_ldfe || is_ldf8 || is_ldfs || is_ldfd) {
                    uint8_t f1 = extract64(insn, 6, 7) & 0x7f;
                    uint8_t r2 = extract64(insn, 13, 7);
                    uint8_t r3 = extract64(insn, 20, 7);

                    TCGv_i64 addr = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(addr, 0);
                    } else {
                        tcg_gen_mov_i64(addr, cpu_r[r3]);
                    }
                    TCGv_i64 mem_addr = gen_mem_addr(ctx, addr);

                    TCGv_i64 mant = tcg_temp_new_i64();
                    TCGv_i64 expw = tcg_temp_new_i64();
                    tcg_gen_movi_i64(mant, 0);
                    tcg_gen_movi_i64(expw, 0);

                    if (is_ldf8) {
                        /* dword -> FP significand */
                        tcg_gen_qemu_ld_i64(mant, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_64);
                        tcg_gen_movi_i64(expw, IA64_FP_SEXP(0, IA64_FP_EXP_INTEGER));
                    } else if (is_ldfe) {
                        /*
                         * ext (80-bit) in 16 bytes:
                         * - low 64: mantissa (explicit integer bit)
                         * - high: sign at bit48, exponent(15) at bits63:49
                         */
                        TCGv_i64 hi = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(mant, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_64);
                        TCGv_i64 addr2 = tcg_temp_new_i64();
                        tcg_gen_addi_i64(addr2, addr, 8);
                        TCGv_i64 mem_addr2 = gen_mem_addr(ctx, addr2);
                        tcg_gen_qemu_ld_i64(hi, mem_addr2, ctx->mem_idx,
                                            MO_TE | MO_64);

                        TCGv_i64 sign = tcg_temp_new_i64();
                        TCGv_i64 exp15 = tcg_temp_new_i64();
                        tcg_gen_shri_i64(sign, hi, 31);    /* bit48 -> bit17 */
                        tcg_gen_andi_i64(sign, sign, 0x20000);
                        tcg_gen_shri_i64(exp15, hi, 49);
                        tcg_gen_andi_i64(exp15, exp15, 0x7fff);

                        TCGv_i64 exp17 = tcg_temp_new_i64();
                        tcg_gen_addi_i64(exp17, exp15, 0xc000);
                        TCGLabel *have_exp = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_NE, exp15, 0, have_exp);
                        tcg_gen_movi_i64(exp17, 0);
                        gen_set_label(have_exp);

                        tcg_gen_or_i64(expw, sign, exp17);
                    } else if (is_ldfs) {
                        /* IEEE single (32-bit) */
                        TCGv_i64 bits32 = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(bits32, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_UL);
                        tcg_gen_andi_i64(bits32, bits32, 0xffffffffULL);

                        TCGv_i64 sign = tcg_temp_new_i64();
                        TCGv_i64 exp8 = tcg_temp_new_i64();
                        TCGv_i64 frac = tcg_temp_new_i64();
                        tcg_gen_shri_i64(sign, bits32, 14); /* bit31 -> bit17 */
                        tcg_gen_andi_i64(sign, sign, 0x20000);
                        tcg_gen_shri_i64(exp8, bits32, 23);
                        tcg_gen_andi_i64(exp8, exp8, 0xff);
                        tcg_gen_andi_i64(frac, bits32, 0x7fffff);

                        TCGv_i64 mant_tmp = tcg_temp_new_i64();
                        TCGv_i64 exp17 = tcg_temp_new_i64();
                        tcg_gen_mov_i64(mant_tmp, frac);
                        tcg_gen_addi_i64(exp17, exp8, 0xff80);

                        TCGLabel *denorm = gen_new_label();
                        TCGLabel *ldfs_done = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_EQ, exp8, 0, denorm);
                        tcg_gen_ori_i64(mant_tmp, mant_tmp, 1ULL << 23);
                        tcg_gen_shli_i64(mant_tmp, mant_tmp, 40);
                        tcg_gen_or_i64(expw, sign, exp17);
                        tcg_gen_mov_i64(mant, mant_tmp);
                        tcg_gen_br(ldfs_done);
                        gen_set_label(denorm);
                        tcg_gen_shli_i64(mant_tmp, mant_tmp, 40);
                        tcg_gen_mov_i64(mant, mant_tmp);
                        tcg_gen_or_i64(expw, sign, tcg_constant_i64(0));
                        gen_set_label(ldfs_done);
                    } else {
                        /* IEEE double (64-bit) */
                        TCGv_i64 bits64 = tcg_temp_new_i64();
                        tcg_gen_qemu_ld_i64(bits64, mem_addr, ctx->mem_idx,
                                            MO_TE | MO_64);

                        TCGv_i64 sign = tcg_temp_new_i64();
                        TCGv_i64 exp11 = tcg_temp_new_i64();
                        TCGv_i64 frac = tcg_temp_new_i64();
                        tcg_gen_shri_i64(sign, bits64, 46); /* bit63 -> bit17 */
                        tcg_gen_andi_i64(sign, sign, 0x20000);
                        tcg_gen_shri_i64(exp11, bits64, 52);
                        tcg_gen_andi_i64(exp11, exp11, 0x7ff);
                        tcg_gen_andi_i64(frac, bits64, 0x000fffffffffffffULL);

                        TCGv_i64 mant_tmp = tcg_temp_new_i64();
                        TCGv_i64 exp17 = tcg_temp_new_i64();
                        tcg_gen_mov_i64(mant_tmp, frac);
                        tcg_gen_addi_i64(exp17, exp11, 0xfc00);

                        TCGLabel *denorm = gen_new_label();
                        TCGLabel *ldfd_done = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_EQ, exp11, 0, denorm);
                        tcg_gen_ori_i64(mant_tmp, mant_tmp, 1ULL << 52);
                        tcg_gen_shli_i64(mant_tmp, mant_tmp, 11);
                        tcg_gen_or_i64(expw, sign, exp17);
                        tcg_gen_mov_i64(mant, mant_tmp);
                        tcg_gen_br(ldfd_done);
                        gen_set_label(denorm);
                        tcg_gen_shli_i64(mant_tmp, mant_tmp, 11);
                        tcg_gen_mov_i64(mant, mant_tmp);
                        tcg_gen_or_i64(expw, sign, tcg_constant_i64(0));
                        gen_set_label(ldfd_done);
                    }

                    if (f1 > 1) {
                        gen_fr_store_lo(f1, mant);
                        gen_fr_store_hi(f1, expw);
                    }

                    if (major == 0x6 && m == 1) {
                        /* M7: f1 = [r3], r2 (post-increment by r2). */
                        if (r3 != 0 && r2 != 0) {
                            tcg_gen_add_i64(cpu_r[r3], cpu_r[r3], cpu_r[r2]);
                        }
                    } else if (major == 0x7) {
                        /* M8: f1 = [r3], imm9 (post-increment by imm9). */
                        int64_t imm9 = sextract64((uint64_t)r2 |
                                                  ((uint64_t)x << 7) |
                                                  ((uint64_t)m << 8),
                                                  0, 9);
                        if (r3 != 0 && imm9 != 0) {
                            tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], imm9);
                        }
                    }
                    handled = true;
                }
            }

            /*
             * stf8 [r3]=f2{,imm8} (seen in memset/entry save areas)
             * - major=6/7, x3=6, x6=0x31
             * - r3 is base address
             * - r2 is FP register index
             * - imm8 is encoded as (r1 | (x << 7)); 0 => no update
             */
            if (!handled && (major == 0x6 || major == 0x7) && x3 == 6 &&
                m == 0 && x6 == 0x31) {
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t f2 = extract64(insn, 13, 7) & 0x7f;
                uint8_t r3 = extract64(insn, 20, 7);
                uint32_t imm8 = (uint32_t)r1 | ((uint32_t)x << 7);

                TCGv_i64 addr = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(addr, 0);
                } else {
                    tcg_gen_mov_i64(addr, cpu_r[r3]);
                }
                TCGv_i64 mem_addr = gen_mem_addr(ctx, addr);

                TCGv_i64 lo = tcg_temp_new_i64();
                gen_fr_load_lo(lo, f2);
                tcg_gen_qemu_st_i64(lo, mem_addr, ctx->mem_idx, MO_TE | MO_64);
                gen_helper_alat_invalidate(tcg_env, mem_addr,
                                           tcg_constant_i32(8));

                if (r3 != 0 && imm8 != 0) {
                    tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], imm8);
                }
                handled = true;
            }

            /*
             * ldf.fill f1=[r3]{,imm8}
             * - major=6/7, x3=3, x6=0x1b
             * - r3 is base address
             * - r2 is the post-increment byte count low 7 bits
             * - imm8 is encoded as (r2 | (x << 7)); 0 => no update
             * Load 16 bytes (2x64) into the emulated FP register.
             */
            if (!handled && (major == 0x6 || major == 0x7) && x3 == 3 &&
                x6 == 0x1b) {
                uint8_t f1 = extract64(insn, 6, 7) & 0x7f;
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                int32_t simm8 = 0;
                if (major == 0x7) {
                    uint64_t raw = (uint64_t)r2 | ((uint64_t)x << 7);
                    simm8 = sextract64(raw, 0, 8);
                }

                TCGv_i64 addr = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(addr, 0);
                } else {
                    tcg_gen_mov_i64(addr, cpu_r[r3]);
                }
                TCGv_i64 mem_addr = gen_mem_addr(ctx, addr);

                TCGv_i64 lo = tcg_temp_new_i64();
                TCGv_i64 hi = tcg_temp_new_i64();
                tcg_gen_qemu_ld_i64(lo, mem_addr, ctx->mem_idx, MO_TE | MO_64);
                TCGv_i64 addr2 = tcg_temp_new_i64();
                tcg_gen_addi_i64(addr2, addr, 8);
                TCGv_i64 mem_addr2 = gen_mem_addr(ctx, addr2);
                tcg_gen_qemu_ld_i64(hi, mem_addr2, ctx->mem_idx, MO_TE | MO_64);

                if (f1 > 1) {
                    gen_fr_store_lo(f1, lo);
                    gen_fr_store_hi(f1, hi);
                }

                if (r3 != 0 && simm8 != 0) {
                    tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], simm8);
                }
                handled = true;
            }

            /*
             * stf.spill [r3]=f2{,imm} (used in Linux entry/ctx switch)
             * - major=6/7, x3=7, x6=0x3b
             * - r3 is the base address register
             * - r2 is the FP register index
             * - post-increment is imm7 (major=6) or signed imm8 (major=7),
             *   encoded as (r1 | (x << 7)); 0 => no update
             */
            if (!handled && (major == 0x6 || major == 0x7) && x3 == 7 &&
                m == 0 && x6 == 0x3b) {
                uint8_t r1 = extract64(insn, 6, 7); /* imm7 payload */
                uint8_t f2 = extract64(insn, 13, 7) & 0x7f;
                uint8_t r3 = extract64(insn, 20, 7);
                /*
                 * Linux's memset and entry paths use positive post-increments
                 * like 32 and 128. Treat the encoded imm8 as an unsigned byte
                 * count; sign-extending would turn 128 into -128 and corrupt
                 * memory by walking pointers backwards.
                 */
                uint32_t imm = (major == 0x7) ? ((uint32_t)r1 | ((uint32_t)x << 7))
                                              : (uint32_t)r1;

                TCGv_i64 addr = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(addr, 0);
                } else {
                    tcg_gen_mov_i64(addr, cpu_r[r3]);
                }
                TCGv_i64 mem_addr = gen_mem_addr(ctx, addr);

                TCGv_i64 lo = tcg_temp_new_i64();
                TCGv_i64 hi = tcg_temp_new_i64();
                gen_fr_load_lo(lo, f2);
                gen_fr_load_hi(hi, f2);

                tcg_gen_qemu_st_i64(lo, mem_addr, ctx->mem_idx, MO_TE | MO_64);
                TCGv_i64 addr2 = tcg_temp_new_i64();
                tcg_gen_addi_i64(addr2, addr, 8);
                TCGv_i64 mem_addr2 = gen_mem_addr(ctx, addr2);
                tcg_gen_qemu_st_i64(hi, mem_addr2, ctx->mem_idx, MO_TE | MO_64);

                if (r3 != 0 && imm != 0) {
                    tcg_gen_addi_i64(cpu_r[r3], cpu_r[r3], imm);
                }
                handled = true;
            }

            /* M18: setf.sig f1 = r2 (op=6 m=0 x=1 x6=0x1c) */
            if (!handled && major == 0x6 && m == 0 && x == 1 && x6 == 0x1c) {
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t f1 = extract64(insn, 6, 7);
                TCGv_i64 val = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(val, 0);
                } else {
                    tcg_gen_mov_i64(val, cpu_r[r2]);
                }
                gen_helper_setf_sig(tcg_env, tcg_constant_i32(f1), val);
                handled = true;
            }

            /* M18: setf.exp f1 = r2 (op=6 m=0 x=1 x6=0x1d) */
            if (!handled && major == 0x6 && m == 0 && x == 1 && x6 == 0x1d) {
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t f1 = extract64(insn, 6, 7) & 0x7f;
                TCGv_i64 expw = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(expw, 0);
                } else {
                    tcg_gen_mov_i64(expw, cpu_r[r2]);
                }
                tcg_gen_andi_i64(expw, expw, 0x3ffffULL);
                if (f1 > 1) {
                    gen_fr_store_lo(f1, tcg_constant_i64(1ULL << 63));
                    gen_fr_store_hi(f1, expw);
                }
                handled = true;
            }

            if (!handled) {
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                gen_unimpl(ctx, insn, "M-slot op6/7");
                break;
            }
            if (skip_label) {
                gen_set_label(skip_label);
            }
            break;
        }
        default:
            gen_unimpl(ctx, insn, "M-slot");
            break;
        }
        break;
    case SLOT_I:
        /* I-unit instructions */
        /* Allow A-unit ops that are legal in I-slots. */
        if (major == 0x8 || major == 0x9 ||
            major == 0xC || major == 0xD || major == 0xE) {
            decode_a_unit(ctx, insn);
            break;
        }
        if (ia64_dbg_iunit_match(ctx->base.pc_next)) {
            uint8_t dbg_x3 = extract64(insn, 33, 3);
            uint8_t dbg_x6 = extract64(insn, 27, 6);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "dbg_iunit pc=%016" PRIx64 " ri=%u insn=%011" PRIx64
                          " major=%u x3=%u x6=0x%x qp=%u\n",
                          ctx->base.pc_next, ctx->ri, insn,
                          major, dbg_x3, dbg_x6, (unsigned)(insn & 0x3f));
        }
        switch (major) {
        case 0x0: {
            /* break / nop.i / hint.i / mov / mov pr */
            uint8_t x3 = extract64(insn, 33, 3);
            uint8_t x6 = extract64(insn, 27, 6);
            if (x3 == 1) {
                /* I20: chk.s.i r2, target25 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint64_t imm =
                    (extract64(insn, 36, 1) << 20) |
                    (extract64(insn, 20, 13) << 7) |
                    extract64(insn, 6, 7);
                int64_t disp = sextract64(imm, 0, 21) << 4;
                uint64_t tgt = ctx->base.pc_next + disp;

                TCGv_i64 nat = tcg_temp_new_i64();
                gen_helper_gr_nat(nat, tcg_env, tcg_constant_i32(r2));

                TCGLabel *not_taken = gen_new_label();
                tcg_gen_brcondi_i64(TCG_COND_EQ, nat, 0, not_taken);
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                tcg_gen_exit_tb(NULL, 0);
                gen_set_label(not_taken);

                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && (x6 == 0x10 || x6 == 0x11 || x6 == 0x12 ||
                            x6 == 0x14 || x6 == 0x15 || x6 == 0x16)) {
                /* I29: zxt{1,2,4} / sxt{1,2,4} r1 = r3 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);

                if (ia64_dbg_sxt_match(ctx->base.pc_next)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "dbg_sxt pc=%016" PRIx64 " ri=%u insn=%011" PRIx64
                                  " qp=%u r1=%u r3=%u x6=0x%x\n",
                                  ctx->base.pc_next, ctx->ri, insn, qp, r1, r3, x6);
                }

                if (r1 != 0) {
                    TCGv_i64 src = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r3]);
                    }

                    if (x6 == 0x10) {
                        tcg_gen_andi_i64(cpu_r[r1], src, 0xff);
                    } else if (x6 == 0x11) {
                        tcg_gen_andi_i64(cpu_r[r1], src, 0xffff);
                    } else if (x6 == 0x12) {
                        tcg_gen_andi_i64(cpu_r[r1], src, 0xffffffffULL);
                    } else if (x6 == 0x14) {
                        tcg_gen_ext8s_i64(cpu_r[r1], src);
                    } else if (x6 == 0x15) {
                        tcg_gen_ext16s_i64(cpu_r[r1], src);
                    } else {
                        tcg_gen_ext32s_i64(cpu_r[r1], src);
                    }
                }

                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x30) {
                /* mov r1 = ip (I25) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                if (r1 != 0) {
                    tcg_gen_movi_i64(cpu_r[r1], ctx->base.pc_next);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x33) {
                /* mov r1 = pr (I25) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], cpu_pr);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x1c) {
                /* czx1.r r1 = r3 (I??): index of first zero byte from right (0..8) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);

                TCGv_i64 src = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r3]);
                }

                /* has_zero_byte mask */
                TCGv_i64 t = tcg_temp_new_i64();
                TCGv_i64 tmp = tcg_temp_new_i64();
                tcg_gen_addi_i64(t, src, -0x0101010101010101ULL);
                tcg_gen_not_i64(tmp, src);
                tcg_gen_and_i64(t, t, tmp);
                tcg_gen_andi_i64(t, t, 0x8080808080808080ULL);

                TCGv_i64 idx = tcg_temp_new_i64();
                tcg_gen_ctzi_i64(idx, t, 64);
                tcg_gen_shri_i64(idx, idx, 3);

                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], idx);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x2a) {
                /* I26: mov.i ar3 = r2 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t ar = extract64(insn, 20, 7);
                TCGv_i64 t = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(t, 0);
                } else {
                    tcg_gen_mov_i64(t, cpu_r[r2]);
                }
                if (ar == 18) {
                    gen_helper_set_bspstore(tcg_env, t);
                } else if (ar == 66) {
                    /*
                     * ar.ec is a bitfield register: EC_CNT is bits 63..58.
                     * Linux uses software-pipelined loops (e.g. __copy_user)
                     * and expects mov.i ar.ec=X to populate EC_CNT, not the
                     * low bits of the raw register.
                     */
                    TCGv_i64 ec = tcg_temp_new_i64();
                    tcg_gen_andi_i64(ec, t, 0x3f);
                    tcg_gen_shli_i64(ec, ec, 58);
                    gen_store_ar(ar, ec);
                } else if (ar == 65) {
                    gen_helper_dbg_ar_lc_store(tcg_env,
                                               tcg_constant_i64(ctx->base.pc_next),
                                               t);
                    gen_store_ar(ar, t);
                } else if (ar == IA64_AR_K4) {
                    gen_helper_fw_ar_k4_store(tcg_env,
                                              tcg_constant_i64(ctx->base.pc_next),
                                              tcg_constant_i32(r2),
                                              t);
                } else {
                    gen_store_ar(ar, t);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x0a) {
                /* I27: mov.i ar3 = imm8 (imm7b + sign bit) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t ar = extract64(insn, 20, 7);
                uint64_t imm8 = extract64(insn, 13, 7) | (extract64(insn, 36, 1) << 7);
                int64_t simm8 = sextract64(imm8, 0, 8);
                TCGv_i64 t = tcg_temp_new_i64();
                tcg_gen_movi_i64(t, simm8);
                if (ar == 18) {
                    gen_helper_set_bspstore(tcg_env, t);
                } else if (ar == 66) {
                    TCGv_i64 ec = tcg_temp_new_i64();
                    tcg_gen_andi_i64(ec, t, 0x3f);
                    tcg_gen_shli_i64(ec, ec, 58);
                    gen_store_ar(ar, ec);
                } else if (ar == 65) {
                    gen_helper_dbg_ar_lc_store(tcg_env,
                                               tcg_constant_i64(ctx->base.pc_next),
                                               t);
                    gen_store_ar(ar, t);
                } else if (ar == IA64_AR_K4) {
                    gen_helper_fw_ar_k4_store(tcg_env,
                                              tcg_constant_i64(ctx->base.pc_next),
                                              tcg_constant_i32(-1),
                                              t);
                } else {
                    gen_store_ar(ar, t);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x32) {
                /* I28: mov.i r1 = ar3 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t ar = extract64(insn, 20, 7);
                TCGv_i64 t = gen_load_ar(ar);
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], t);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x01) {
                /* I18: nop.i / hint.i imm21 */
                break;
            }
            if (x3 == 2 && x6 == 0x00) {
                /* mov pr.rot = imm (used by software-pipelined loops) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint64_t imm = (uint64_t)extract64(insn, 6, 7) << 16;

                TCGv_i64 t_pr = tcg_temp_new_i64();
                tcg_gen_andi_i64(t_pr, cpu_pr, (1ULL << IA64_PR_ROT_BASE) - 1);
                tcg_gen_ori_i64(t_pr, t_pr, imm);
                tcg_gen_ori_i64(t_pr, t_pr, 1); /* p0 always true */
                tcg_gen_mov_i64(cpu_pr, t_pr);

                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 7) {
                /* I21: mov{,.ret} b1 = r2, tag13 (we ignore tag/ih/hints for now) */
                static int movb_log_count;
                static int movb_log_enabled = -1;
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t b = extract64(insn, 6, 3);
                uint8_t r2 = extract64(insn, 13, 7);
                if (movb_log_enabled == -1) {
                    movb_log_enabled = getenv("QEMU_IA64_MOVB_LOG") ? 1 : 0;
                }
                if (movb_log_enabled && movb_log_count < 64) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "mov.b pc=%016" PRIx64 " insn=%011" PRIx64
                                  " b=%u r2=%u\n",
                                  ctx->base.pc_next, insn, b, r2);
                    movb_log_count++;
                }
                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                if (movb_log_enabled) {
                    gen_helper_dbg_movb(tcg_env,
                                        tcg_constant_i64(ctx->base.pc_next),
                                        tcg_constant_i32(b),
                                        tcg_constant_i32(r2),
                                        src);
                }
                if (b == 7) {
                    gen_helper_fw_b7_write(tcg_env,
                                           tcg_constant_i64(ctx->base.pc_next),
                                           tcg_constant_i32(IA64_B7_WRITE_MOV_I),
                                           tcg_constant_i32(r2),
                                           src);
                }
                tcg_gen_mov_i64(cpu_b[b], src);
                if (b == 0) {
                    gen_record_b0_write(ctx, insn, 2, src);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x31) {
                /* mov r1 = b2 (I22) */
                static int movrb_log_count;
                static int movrb_log_enabled = -1;
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t b2 = extract64(insn, 13, 3);
                if (movrb_log_enabled == -1) {
                    movrb_log_enabled = getenv("QEMU_IA64_MOVRB_LOG") ? 1 : 0;
                }
                if (movrb_log_enabled && movrb_log_count < 64) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "mov.r=br pc=%016" PRIx64 " insn=%011" PRIx64
                                  " r1=%u b2=%u\n",
                                  ctx->base.pc_next, insn, r1, b2);
                    movrb_log_count++;
                }
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], cpu_b[b2]);
                }
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 3) {
                /* mov pr = r2, mask17 (I23) */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint64_t s = extract64(insn, 36, 1);
                uint64_t mask8c = extract64(insn, 24, 8);
                uint64_t mask7a = extract64(insn, 6, 7);
                uint64_t imm = (s << 16) | (mask8c << 8) | (mask7a << 1);
                int64_t mask17 = sextract64(imm, 0, 17);
                uint64_t mask = (uint64_t)mask17;

                TCGv_i64 t_pr = tcg_temp_new_i64();
                TCGv_i64 t_src = tcg_temp_new_i64();
                TCGv_i64 t_masked = tcg_temp_new_i64();
                tcg_gen_mov_i64(t_pr, cpu_pr);
                if (r2 == 0) {
                    tcg_gen_movi_i64(t_src, 0);
                } else {
                    tcg_gen_mov_i64(t_src, cpu_r[r2]);
                }
                tcg_gen_andi_i64(t_masked, t_src, mask);
                tcg_gen_andi_i64(t_pr, t_pr, ~mask);
                tcg_gen_or_i64(t_pr, t_pr, t_masked);
                tcg_gen_ori_i64(t_pr, t_pr, 1); /* p0 always true */
                tcg_gen_mov_i64(cpu_pr, t_pr);
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }
            if (x3 == 0 && x6 == 0) {
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint64_t imm = (extract64(insn, 36, 1) << 20) | extract64(insn, 6, 20);
                if (imm == 0x80000 || imm == 0x80001) {
                    TCGv_i64 timm = tcg_temp_new_i64();
                    tcg_gen_movi_i64(timm, imm);
                    TCGv_i64 ret = tcg_temp_new_i64();
                    gen_helper_ssc(ret, tcg_env, timm);
                    tcg_gen_mov_i64(cpu_r[8], ret);
                } else {
                    gen_helper_breaki(tcg_env, tcg_constant_i64(imm));
                    if (qp == 0) {
                        ctx->base.is_jmp = DISAS_NORETURN;
                    }
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            gen_unimpl(ctx, insn, "I-slot");
            break;
        }
        case 0x7: {
            /* I-unit multimedia/shift group (op=7). Implement the basic shifts. */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);

            uint8_t za = extract64(insn, 36, 1);
            uint8_t zb = extract64(insn, 33, 1);
            uint8_t ve = extract64(insn, 32, 1);
            uint8_t x2a = extract64(insn, 34, 2);
            uint8_t x2b = extract64(insn, 28, 2);
            uint8_t x2c = extract64(insn, 30, 2);
            uint8_t r1 = extract64(insn, 6, 7);
            uint8_t r2 = extract64(insn, 13, 7);
            uint8_t r3 = extract64(insn, 20, 7);

            /* I9: popcnt r1 = r3 */
            if (za == 0 && zb == 1 && ve == 0 && x2a == 1 && x2b == 1 && x2c == 2) {
                TCGv_i64 src = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r3]);
                }
                if (r1 != 0) {
                    tcg_gen_ctpop_i64(cpu_r[r1], src);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            /* I3 mux1: za=0 zb=0 ve=0 x2a=3 x2b=2 x2c=2 : r1 = r2, mbtype4 */
            if (za == 0 && zb == 0 && ve == 0 && x2a == 3 && x2b == 2 && x2c == 2) {
                uint8_t mbt4c = extract64(insn, 20, 4);
                const uint8_t *map = NULL;

                static const uint8_t map_brcst[8] = {7, 7, 7, 7, 7, 7, 7, 7};
                static const uint8_t map_mix[8] = {0, 4, 2, 6, 1, 5, 3, 7};
                static const uint8_t map_shuf[8] = {0, 4, 1, 5, 2, 6, 3, 7};
                static const uint8_t map_alt[8] = {0, 2, 4, 6, 1, 3, 5, 7};
                static const uint8_t map_rev[8] = {7, 6, 5, 4, 3, 2, 1, 0};

                switch (mbt4c) {
                case 0x0: /* @brcst */
                    map = map_brcst;
                    break;
                case 0x8: /* @mix */
                    map = map_mix;
                    break;
                case 0x9: /* @shuf */
                    map = map_shuf;
                    break;
                case 0xA: /* @alt */
                    map = map_alt;
                    break;
                case 0xB: /* @rev */
                    map = map_rev;
                    break;
                default:
                    map = NULL;
                    break;
                }

                if (!map) {
                    gen_unimpl(ctx, insn, "mux1 mbtype");
                    return;
                }

                if (r1 != 0) {
                    TCGv_i64 src = tcg_temp_new_i64();
                    TCGv_i64 dst = tcg_temp_new_i64();
                    TCGv_i64 t = tcg_temp_new_i64();

                    if (r2 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r2]);
                    }

                    tcg_gen_movi_i64(dst, 0);
                    for (int i = 0; i < 8; i++) {
                        int src_shift = (7 - map[i]) * 8;
                        int dst_shift = (7 - i) * 8;
                        if (src_shift) {
                            tcg_gen_shri_i64(t, src, src_shift);
                        } else {
                            tcg_gen_mov_i64(t, src);
                        }
                        tcg_gen_andi_i64(t, t, 0xff);
                        if (dst_shift) {
                            tcg_gen_shli_i64(t, t, dst_shift);
                        }
                        tcg_gen_or_i64(dst, dst, t);
                    }
                    tcg_gen_mov_i64(cpu_r[r1], dst);
                }

                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            /* I4 mux2: za=0 zb=1 ve=0 x2a=3 x2b=2 x2c=2 : r1 = r2, mhtype8 */
            if (za == 0 && zb == 1 && ve == 0 && x2a == 3 && x2b == 2 && x2c == 2) {
                uint8_t mht8c = extract64(insn, 20, 8);
                uint8_t mht = (uint8_t)~mht8c; /* SKI reverses endianness */
                uint8_t sel[4] = {
                    (uint8_t)((mht >> 6) & 0x3),
                    (uint8_t)((mht >> 4) & 0x3),
                    (uint8_t)((mht >> 2) & 0x3),
                    (uint8_t)((mht >> 0) & 0x3),
                };

                if (r1 != 0) {
                    TCGv_i64 src = tcg_temp_new_i64();
                    TCGv_i64 dst = tcg_temp_new_i64();
                    TCGv_i64 t = tcg_temp_new_i64();

                    if (r2 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r2]);
                    }

                    tcg_gen_movi_i64(dst, 0);
                    for (int i = 0; i < 4; i++) {
                        int src_shift = (3 - sel[i]) * 16;
                        int dst_shift = (3 - i) * 16;
                        if (src_shift) {
                            tcg_gen_shri_i64(t, src, src_shift);
                        } else {
                            tcg_gen_mov_i64(t, src);
                        }
                        tcg_gen_andi_i64(t, t, 0xffff);
                        if (dst_shift) {
                            tcg_gen_shli_i64(t, t, dst_shift);
                        }
                        tcg_gen_or_i64(dst, dst, t);
                    }
                    tcg_gen_mov_i64(cpu_r[r1], dst);
                }

                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            /* I2 unpack4.{h,l}: za=1 zb=0 ve=0 x2a=2 x2c=1 */
            if (za == 1 && zb == 0 && ve == 0 && x2a == 2 && x2c == 1 &&
                (x2b == 0 || x2b == 2)) {
                TCGv_i64 a = tcg_temp_new_i64();
                TCGv_i64 b = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(a, 0);
                } else {
                    tcg_gen_mov_i64(a, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(b, 0);
                } else {
                    tcg_gen_mov_i64(b, cpu_r[r3]);
                }

                if (r1 != 0) {
                    TCGv_i64 dst = tcg_temp_new_i64();
                    if (x2b == 0) {
                        /* unpack4.h == mix4.l: dst.w0=src1.w0, dst.w1=src2.w0 */
                        TCGv_i64 hi1 = tcg_temp_new_i64();
                        TCGv_i64 hi2 = tcg_temp_new_i64();
                        tcg_gen_shri_i64(hi1, a, 32);
                        tcg_gen_andi_i64(hi1, hi1, 0xffffffffULL);
                        tcg_gen_shri_i64(hi2, b, 32);
                        tcg_gen_andi_i64(hi2, hi2, 0xffffffffULL);
                        tcg_gen_shli_i64(hi1, hi1, 32);
                        tcg_gen_or_i64(dst, hi1, hi2);
                    } else {
                        /* unpack4.l == mix4.r: dst.w0=src1.w1, dst.w1=src2.w1 */
                        TCGv_i64 lo1 = tcg_temp_new_i64();
                        TCGv_i64 lo2 = tcg_temp_new_i64();
                        tcg_gen_andi_i64(lo1, a, 0xffffffffULL);
                        tcg_gen_andi_i64(lo2, b, 0xffffffffULL);
                        tcg_gen_shli_i64(lo1, lo1, 32);
                        tcg_gen_or_i64(dst, lo1, lo2);
                    }
                    tcg_gen_mov_i64(cpu_r[r1], dst);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            /* I2 mix4.{l,r}: za=1 zb=0 ve=0 x2a=2 x2c=2 */
            if (za == 1 && zb == 0 && ve == 0 && x2a == 2 && x2c == 2 &&
                (x2b == 0 || x2b == 2)) {
                TCGv_i64 a = tcg_temp_new_i64();
                TCGv_i64 b = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(a, 0);
                } else {
                    tcg_gen_mov_i64(a, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(b, 0);
                } else {
                    tcg_gen_mov_i64(b, cpu_r[r3]);
                }

                if (r1 != 0) {
                    TCGv_i64 dst = tcg_temp_new_i64();
                    if (x2b == 2) {
                        /* mix4.l: dst.w0=src1.w0, dst.w1=src2.w0 */
                        TCGv_i64 hi1 = tcg_temp_new_i64();
                        TCGv_i64 hi2 = tcg_temp_new_i64();
                        tcg_gen_shri_i64(hi1, a, 32);
                        tcg_gen_andi_i64(hi1, hi1, 0xffffffffULL);
                        tcg_gen_shri_i64(hi2, b, 32);
                        tcg_gen_andi_i64(hi2, hi2, 0xffffffffULL);
                        tcg_gen_shli_i64(hi1, hi1, 32);
                        tcg_gen_or_i64(dst, hi1, hi2);
                    } else {
                        /* mix4.r: dst.w0=src1.w1, dst.w1=src2.w1 */
                        TCGv_i64 lo1 = tcg_temp_new_i64();
                        TCGv_i64 lo2 = tcg_temp_new_i64();
                        tcg_gen_andi_i64(lo1, a, 0xffffffffULL);
                        tcg_gen_andi_i64(lo2, b, 0xffffffffULL);
                        tcg_gen_shli_i64(lo1, lo1, 32);
                        tcg_gen_or_i64(dst, lo1, lo2);
                    }
                    tcg_gen_mov_i64(cpu_r[r1], dst);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            /* I7 shl: za=1 zb=1 ve=0 x2a=0 x2b=0 x2c=1 : r1 = r2, r3 */
            if (za == 1 && zb == 1 && ve == 0 && x2a == 0 && x2b == 0 && x2c == 1) {
                TCGv_i64 src = tcg_temp_new_i64();
                TCGv_i64 cnt = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(cnt, 0);
                } else {
                    tcg_gen_mov_i64(cnt, cpu_r[r3]);
                }
                if (r1 != 0) {
                    /*
                     * Shift counts are not masked. Per SKI: if CNT >= 64 the
                     * result is 0.
                     */
                    TCGLabel *ge64 = gen_new_label();
                    TCGLabel *done = gen_new_label();
                    tcg_gen_brcondi_i64(TCG_COND_GEU, cnt, 64, ge64);
                    tcg_gen_shl_i64(cpu_r[r1], src, cnt);
                    tcg_gen_br(done);
                    gen_set_label(ge64);
                    tcg_gen_movi_i64(cpu_r[r1], 0);
                    gen_set_label(done);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            /*
             * I5 shr/shr.u: za=1 zb=1 ve=0 x2a=0 x2c=0 : r1 = r3, r2
             * - x2b=2 => shr (arithmetic)
             * - x2b=0 => shr.u (logical)
             */
            if (za == 1 && zb == 1 && ve == 0 && x2a == 0 && x2c == 0 &&
                (x2b == 0 || x2b == 2)) {
                TCGv_i64 val = tcg_temp_new_i64();
                TCGv_i64 cnt = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(val, 0);
                } else {
                    tcg_gen_mov_i64(val, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(cnt, 0);
                } else {
                    tcg_gen_mov_i64(cnt, cpu_r[r2]);
                }
                if (r1 != 0) {
                    /*
                     * Shift counts are not masked. Per SKI:
                     * - shr.u: if CNT >= 64 the result is 0
                     * - shr:   if CNT >= 64 the result is SRC1 >> 63
                     */
                    TCGLabel *ge64 = gen_new_label();
                    TCGLabel *done = gen_new_label();
                    tcg_gen_brcondi_i64(TCG_COND_GEU, cnt, 64, ge64);
                    if (x2b == 0) {
                        tcg_gen_shr_i64(cpu_r[r1], val, cnt);
                    } else {
                        tcg_gen_sar_i64(cpu_r[r1], val, cnt);
                    }
                    tcg_gen_br(done);
                    gen_set_label(ge64);
                    if (x2b == 0) {
                        tcg_gen_movi_i64(cpu_r[r1], 0);
                    } else {
                        tcg_gen_sari_i64(cpu_r[r1], val, 63);
                    }
                    gen_set_label(done);
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }

            gen_unimpl(ctx, insn, "I-slot");
            break;
        }
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
            /* I15: pos6 = 63 - cpos6d; len4 = len4d + 1 (SKI encoding.imm). */
            uint8_t cpos = extract64(insn, 31, 6);
            uint8_t pos = 63 - cpos;
            uint8_t len = extract64(insn, 27, 4) + 1;
            uint64_t mask = ((len >= 64) ? ~0ULL : ((1ULL << len) - 1)) << pos;
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
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = NULL;

            uint8_t x2a = extract64(insn, 34, 2);
            uint8_t ve = extract64(insn, 33, 1);
            bool handled = false;

            /*
             * I12/I13: dep.z
             *
             * SKI encoding.opcode:
             *   I12 dep.z r1 = r2,   pos6, len6 : op=5 x2=1 x=1 y=0
             *   I13 dep.z r1 = imm8, pos6, len6 : op=5 x2=1 x=1 y=1
             *
             * Immediate formation (SKI encoding.imm):
             *   len6 = len6d + 1
             *   pos6 = 63 - cpos6c
             */
            if (x2a == 1 && ve == 1) {
                skip_label = gen_qp_skip(qp);

                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t y = extract64(insn, 26, 1);
                uint8_t cpos = extract64(insn, 20, 6);
                uint8_t pos = 63 - cpos;
                uint8_t len = extract64(insn, 27, 6) + 1;

                /*
                 * IA-64 allows pos + len > 64; bits beyond 63 are lost.
                 * len is 1-64 (encoded as len-1), pos is 0-63.
                 */
                {
                    TCGv_i64 src = tcg_temp_new_i64();
                    if (y == 0) {
                        uint8_t r2 = extract64(insn, 13, 7);
                        if (r2 == 0) {
                            tcg_gen_movi_i64(src, 0);
                        } else {
                            tcg_gen_mov_i64(src, cpu_r[r2]);
                        }
                    } else {
                        uint64_t imm8 = extract64(insn, 13, 7) |
                                        (extract64(insn, 36, 1) << 7);
                        int64_t simm8 = sextract64(imm8, 0, 8);
                        tcg_gen_movi_i64(src, simm8);
                    }

                    TCGv_i64 t = tcg_temp_new_i64();
                    if (len == 64) {
                        tcg_gen_mov_i64(t, src);
                    } else {
                        tcg_gen_andi_i64(t, src, (1ULL << len) - 1);
                    }
                    if (pos) {
                        tcg_gen_shli_i64(t, t, pos);
                    }
                    if (r1 != 0) {
                        tcg_gen_mov_i64(cpu_r[r1], t);
                    }
                    handled = true;
                }
            } else if (x2a == 0) {
                /* Test instructions: tbit.* / tnat.* (SKI I16/I17). */
                uint8_t p2 = extract64(insn, 27, 6);
                uint8_t p1 = extract64(insn, 6, 6);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t pos = extract64(insn, 14, 6);
                uint8_t tb = extract64(insn, 36, 1);
                uint8_t ta = extract64(insn, 33, 1);
                uint8_t c = extract64(insn, 12, 1);
                uint8_t bit13 = extract64(insn, 13, 1);

                /*
                 * Encoding/semantics follow SKI's tables:
                 * - ta/tb select the predicate write mode:
                 *     00: base ("... .z" or "... .z.unc")
                 *     01: .and
                 *     10: .or
                 *     11: .or.andcm
                 * - In base mode (ta=tb=0), c selects ".unc" (0 => base, 1 => .unc),
                 *   and the test is always the ".z" form (no base ".nz" opcode).
                 * - In the non-base modes, c selects z vs nz (0 => z, 1 => nz).
                 *
                 * NaT bits are tracked for the stacked GRs; use the helper for tnat.
                 */
                enum {
                    TEST_BASE = 0,
                    TEST_AND,
                    TEST_OR,
                    TEST_OAC,
                } mode;

                bool is_unc = false;
                bool is_z = true;
                if (ta == 0 && tb == 0) {
                    mode = TEST_BASE;
                    is_unc = (c != 0);
                    is_z = true;
                } else if (ta == 0 && tb == 1) {
                    mode = TEST_AND;
                    is_z = (c == 0);
                } else if (ta == 1 && tb == 0) {
                    mode = TEST_OR;
                    is_z = (c == 0);
                } else {
                    mode = TEST_OAC;
                    is_z = (c == 0);
                }

                if (!is_unc) {
                    skip_label = gen_qp_skip(qp);
                }

                /* Compute CMPRES1 (0/1), CMPRES2 is implied as ~CMPRES1. */
                TCGv_i64 cmpres1 = tcg_temp_new_i64();
                if (bit13) {
                    /* tnat.* p1,p2 = r3 */
                    TCGv_i64 nat = tcg_temp_new_i64();
                    gen_helper_gr_nat(nat, tcg_env, tcg_constant_i32(r3));
                    if (is_z) {
                        tcg_gen_xori_i64(cmpres1, nat, 1);
                    } else {
                        tcg_gen_mov_i64(cmpres1, nat);
                    }
                } else {
                    /* tbit.* p1,p2 = r3, pos6 */
                    TCGv_i64 src = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r3]);
                    }

                    tcg_gen_shri_i64(cmpres1, src, pos);
                    tcg_gen_andi_i64(cmpres1, cmpres1, 1);
                    if (is_z) {
                        tcg_gen_xori_i64(cmpres1, cmpres1, 1);
                    }
                }

                TCGv_i64 zero = tcg_temp_new_i64();
                tcg_gen_movi_i64(zero, 0);
                TCGv_i64 one = tcg_temp_new_i64();
                tcg_gen_movi_i64(one, 1);

                if (is_unc && qp != 0) {
                    /*
                     * ".unc": if qp false, clear both destination predicates; otherwise
                     * perform the normal operation.
                     */
                    TCGLabel *doit = gen_new_label();
                    TCGLabel *done = gen_new_label();
                    TCGv_i64 t_qp = gen_pr_read_bit(qp);
                    tcg_gen_brcondi_i64(TCG_COND_NE, t_qp, 0, doit);
                    gen_pr_write_bit(p1, zero);
                    gen_pr_write_bit(p2, zero);
                    tcg_gen_br(done);
                    gen_set_label(doit);
                    /* Fall through into the normal write below. */
                    switch (mode) {
                    case TEST_BASE:
                        gen_set_predicates(p1, p2, cmpres1);
                        break;
                    case TEST_AND: {
                        TCGLabel *skip = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_NE, cmpres1, 0, skip);
                        gen_pr_write_bit(p1, zero);
                        gen_pr_write_bit(p2, zero);
                        gen_set_label(skip);
                        break;
                    }
                    case TEST_OR: {
                        TCGLabel *skip = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_EQ, cmpres1, 0, skip);
                        gen_pr_write_bit(p1, one);
                        gen_pr_write_bit(p2, one);
                        gen_set_label(skip);
                        break;
                    }
                    case TEST_OAC: {
                        TCGLabel *skip = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_EQ, cmpres1, 0, skip);
                        gen_pr_write_bit(p1, one);
                        gen_pr_write_bit(p2, zero);
                        gen_set_label(skip);
                        break;
                    }
                    default:
                        g_assert_not_reached();
                    }
                    gen_set_label(done);
                } else {
                    switch (mode) {
                    case TEST_BASE:
                        gen_set_predicates(p1, p2, cmpres1);
                        break;
                    case TEST_AND: {
                        TCGLabel *skip = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_NE, cmpres1, 0, skip);
                        gen_pr_write_bit(p1, zero);
                        gen_pr_write_bit(p2, zero);
                        gen_set_label(skip);
                        break;
                    }
                    case TEST_OR: {
                        TCGLabel *skip = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_EQ, cmpres1, 0, skip);
                        gen_pr_write_bit(p1, one);
                        gen_pr_write_bit(p2, one);
                        gen_set_label(skip);
                        break;
                    }
                    case TEST_OAC: {
                        TCGLabel *skip = gen_new_label();
                        tcg_gen_brcondi_i64(TCG_COND_EQ, cmpres1, 0, skip);
                        gen_pr_write_bit(p1, one);
                        gen_pr_write_bit(p2, zero);
                        gen_set_label(skip);
                        break;
                    }
                    default:
                        g_assert_not_reached();
                    }
                }

                handled = true;
            } else if (x2a == 1 && ve == 1) {
                /* shl r1 = r2, count6 (count encoded as 63 - imm6) */
                skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t inv = extract64(insn, 20, 6);
                uint8_t cnt = 63 - inv;

                TCGv_i64 src = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r2]);
                }
                if (r1 != 0) {
                    tcg_gen_shli_i64(cpu_r[r1], src, cnt);
                }
                handled = true;
            } else if (x2a == 1 && ve == 0) {
                /*
                 * Distinguish between:
                 * - shr/shr.u r1 = r3, count6 (right shift)
                 *   where (63-count) is redundantly encoded in bits 27..32
                 * - extr/extr.u r1 = r3, pos6, len6 (len encoded as len-1)
                 */
                skip_label = gen_qp_skip(qp);
                uint8_t pos_or_cnt = extract64(insn, 14, 6);
                uint8_t inv = extract64(insn, 27, 6);

                if (inv == (uint8_t)(63 - pos_or_cnt)) {
                    /* shr / shr.u */
                    uint8_t r1 = extract64(insn, 6, 7);
                    uint8_t r3 = extract64(insn, 20, 7);
                    uint8_t cnt = pos_or_cnt;
                    bool is_arith = extract64(insn, 13, 1);

                    TCGv_i64 src = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r3]);
                    }
                    if (r1 != 0) {
                        if (is_arith) {
                            tcg_gen_sari_i64(cpu_r[r1], src, cnt);
                        } else {
                            tcg_gen_shri_i64(cpu_r[r1], src, cnt);
                        }
                    }
                    handled = true;
                } else {
                    /* extr / extr.u */
                    uint8_t r1 = extract64(insn, 6, 7);
                    uint8_t r3 = extract64(insn, 20, 7);
                    uint8_t pos = extract64(insn, 14, 6);
                    uint8_t len = extract64(insn, 27, 6) + 1;
                    bool is_signed = extract64(insn, 13, 1);

                    /*
                     * IA-64 allows pos + len > 64; bits beyond 63 are zero.
                     * len is 1-64 (encoded as len-1), pos is 0-63.
                     */
                    TCGv_i64 src = tcg_temp_new_i64();
                    if (r3 == 0) {
                        tcg_gen_movi_i64(src, 0);
                    } else {
                        tcg_gen_mov_i64(src, cpu_r[r3]);
                    }
                    TCGv_i64 t = tcg_temp_new_i64();
                    tcg_gen_shri_i64(t, src, pos);
                    if (len < 64) {
                        tcg_gen_andi_i64(t, t, (1ULL << len) - 1);
                        if (is_signed) {
                            tcg_gen_shli_i64(t, t, 64 - len);
                            tcg_gen_sari_i64(t, t, 64 - len);
                        }
                    }
                    if (r1 != 0) {
                        tcg_gen_mov_i64(cpu_r[r1], t);
                    }
                    handled = true;
                }
            } else if (x2a == 3 && ve == 0) {
                /* I10: shrp r1 = r2, r3, count6 */
                skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                uint8_t cnt = extract64(insn, 27, 6);

                TCGv_i64 src1 = tcg_temp_new_i64();
                TCGv_i64 src2 = tcg_temp_new_i64();
                if (r2 == 0) {
                    tcg_gen_movi_i64(src1, 0);
                } else {
                    tcg_gen_mov_i64(src1, cpu_r[r2]);
                }
                if (r3 == 0) {
                    tcg_gen_movi_i64(src2, 0);
                } else {
                    tcg_gen_mov_i64(src2, cpu_r[r3]);
                }

                if (r1 != 0) {
                    if (cnt == 0) {
                        tcg_gen_mov_i64(cpu_r[r1], src2);
                    } else {
                        TCGv_i64 hi = tcg_temp_new_i64();
                        TCGv_i64 lo = tcg_temp_new_i64();
                        tcg_gen_shli_i64(hi, src1, 64 - cnt);
                        tcg_gen_shri_i64(lo, src2, cnt);
                        tcg_gen_or_i64(cpu_r[r1], hi, lo);
                    }
                }
                handled = true;
            } else if (x2a == 3 && ve == 1) {
                /* dep r1 = imm1, r3, pos6, len6 (I14) */
                skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                /* I14: pos6 = 63 - cpos6b; len6 = len6d + 1 (SKI encoding.imm). */
                uint8_t cpos = extract64(insn, 14, 6);
                uint8_t pos = 63 - cpos;
                uint8_t len = extract64(insn, 27, 6) + 1;
                /*
                 * IA-64 allows pos + len > 64; bits beyond 63 are lost.
                 * C shift semantics naturally truncate the high bits.
                 */
                uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1) << pos;
                uint64_t imm1 = extract64(insn, 36, 1);

                TCGv_i64 src = tcg_temp_new_i64();
                if (r3 == 0) {
                    tcg_gen_movi_i64(src, 0);
                } else {
                    tcg_gen_mov_i64(src, cpu_r[r3]);
                }
                if (mask) {
                    tcg_gen_andi_i64(src, src, ~mask);
                    TCGv_i64 tval = tcg_temp_new_i64();
                    tcg_gen_movi_i64(tval, imm1 ? mask : 0);
                    tcg_gen_or_i64(src, src, tval);
                }
                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], src);
                }
                handled = true;
            }

            if (skip_label) {
                gen_set_label(skip_label);
            }
            if (!handled) {
                gen_unimpl(ctx, insn, "I-slot");
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
        /* F-unit instructions (minimal subset for kernel/libgcc helpers). */
        if (insn != 0) {
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip = gen_qp_skip(qp);
            uint8_t f_major = (insn >> 37) & 0xf;
            bool handled = false;

            /* nop.f / hint.f */
            if (f_major == 0x0 &&
                extract64(insn, 33, 3) == 0 &&
                extract64(insn, 31, 2) == 0 &&
                extract64(insn, 27, 4) == 0x1) {
                if (skip) {
                    gen_set_label(skip);
                }
                break;
            }

            if (f_major == 0x0) {
                /* F8: frcpa.s* f1,p2 = f2,f3 */
                uint8_t x3 = extract64(insn, 33, 3);
                uint8_t x2 = extract64(insn, 31, 2);
                if (x3 == 3 && x2 == 0) {
                    uint8_t p2 = extract64(insn, 27, 6);
                    uint8_t f3 = extract64(insn, 20, 7);
                    uint8_t f2 = extract64(insn, 13, 7);
                    uint8_t f1 = extract64(insn, 6, 7);
                    gen_helper_frcpa_s1(tcg_env,
                                        tcg_constant_i32(f1),
                                        tcg_constant_i32(p2),
                                        tcg_constant_i32(f2),
                                        tcg_constant_i32(f3));
                    handled = true;
                }
            }

            if (!handled && f_major == 0x0) {
                /*
                 * F10: fcvt.fxu.trunc.s1 f1 = f2
                 *
                 * Used by the Xen GFW firmware's FP runtime sequences.
                 *
                 * Encoding per SKI:
                 *   op{40:37}=0 x{33}=0 x6{32:27}=0x1b sf{35:34}=1
                 */
                uint8_t x = extract64(insn, 33, 1);
                uint8_t sf = extract64(insn, 34, 2);
                uint8_t x6 = extract64(insn, 27, 6);
                uint8_t f3 = extract64(insn, 20, 7);
                if (x == 0 && sf == 1 && x6 == 0x1b && f3 == 0) {
                    uint8_t f2 = extract64(insn, 13, 7);
                    uint8_t f1 = extract64(insn, 6, 7);
                    gen_helper_fcvt_fxu_trunc_s1(tcg_env,
                                                 tcg_constant_i32(f1),
                                                 tcg_constant_i32(f2));
                    handled = true;
                }
            }

            if (!handled && f_major == 0x0) {
                /* F9: fmerge.{s,ns,se} f1 = f2, f3 */
                uint8_t x = extract64(insn, 33, 1);
                uint8_t x6 = extract64(insn, 27, 6);
                if (x == 0 && (x6 == 0x10 || x6 == 0x11 || x6 == 0x12)) {
                    uint8_t f3 = extract64(insn, 20, 7);
                    uint8_t f2 = extract64(insn, 13, 7);
                    uint8_t f1 = extract64(insn, 6, 7);

                    TCGv_i64 a = tcg_temp_new_i64();
                    TCGv_i64 b = tcg_temp_new_i64();
                    gen_fr_load_lo(a, f2);
                    gen_fr_load_lo(b, f3);

                    /* Bit-level approximation of SKI's spill merge. */
                    const uint64_t SIGN_MASK = 0x8000000000000000ULL;
                    const uint64_t EXP_MASK  = 0x7ff0000000000000ULL;
                    const uint64_t SIGNEXP_MASK = SIGN_MASK | EXP_MASK;

                    TCGv_i64 res = tcg_temp_new_i64();
                    if (x6 == 0x10) {
                        /* fmerge.s: sign from a, rest from b */
                        TCGv_i64 sign = tcg_temp_new_i64();
                        TCGv_i64 mag = tcg_temp_new_i64();
                        tcg_gen_andi_i64(sign, a, SIGN_MASK);
                        tcg_gen_andi_i64(mag, b, ~SIGN_MASK);
                        tcg_gen_or_i64(res, mag, sign);
                    } else if (x6 == 0x11) {
                        /* fmerge.ns: inverted sign from a, rest from b */
                        TCGv_i64 sign = tcg_temp_new_i64();
                        TCGv_i64 mag = tcg_temp_new_i64();
                        tcg_gen_andi_i64(sign, a, SIGN_MASK);
                        tcg_gen_xori_i64(sign, sign, SIGN_MASK);
                        tcg_gen_andi_i64(mag, b, ~SIGN_MASK);
                        tcg_gen_or_i64(res, mag, sign);
                    } else {
                        /* fmerge.se: sign+exp from a, mantissa from b */
                        TCGv_i64 se = tcg_temp_new_i64();
                        TCGv_i64 mant = tcg_temp_new_i64();
                        tcg_gen_andi_i64(se, a, SIGNEXP_MASK);
                        tcg_gen_andi_i64(mant, b, ~SIGNEXP_MASK);
                        tcg_gen_or_i64(res, mant, se);
                    }

                    if (f1 > 1) {
                        gen_fr_store_lo(f1, res);
                        gen_fr_store_hi(f1, tcg_constant_i64(0));
                    }
                    handled = true;
                }
            }

            if (!handled && f_major == 0x0) {
                /*
                 * F9: fsxt.{r,l} f1 = f2, f3
                 *
                 * Used by the kernel for integer arithmetic lowered into
                 * FP-unit ops (e.g. size_t multiplication in percpu setup).
                 *
                 * Mirror SKI's bit-level behavior on the "dword" payload:
                 *  - fsxt.r: sign-extend low 32 bits of f3 using bit31 of f2
                 *  - fsxt.l: sign-extend high 32 bits of f3 using sign bit of f2
                 *
                 * We model the integer payload in f[][0] and ignore the full
                 * FP status/classification for now.
                 */
                uint8_t x = extract64(insn, 33, 1);
                uint8_t x6 = extract64(insn, 27, 6);
                if (x == 0 && (x6 == 0x3c || x6 == 0x3d)) {
                    uint8_t f3 = extract64(insn, 20, 7);
                    uint8_t f2 = extract64(insn, 13, 7);
                    uint8_t f1 = extract64(insn, 6, 7);

                    TCGv_i64 a = tcg_temp_new_i64();
                    TCGv_i64 b = tcg_temp_new_i64();
                    gen_fr_load_lo(a, f2);
                    gen_fr_load_lo(b, f3);

                    TCGv_i64 sign = tcg_temp_new_i64();
                    TCGv_i64 ext = tcg_temp_new_i64();
                    TCGv_i64 low = tcg_temp_new_i64();
                    TCGv_i64 res = tcg_temp_new_i64();

                    if (x6 == 0x3c) {
                        /* fsxt.r */
                        tcg_gen_shri_i64(sign, a, 31);
                        tcg_gen_andi_i64(sign, sign, 1);
                        tcg_gen_neg_i64(ext, sign);      /* 0 or -1 */
                        tcg_gen_shli_i64(ext, ext, 32);  /* 0 or 0xffffffff00000000 */
                        tcg_gen_andi_i64(low, b, 0xffffffffULL);
                    } else {
                        /* fsxt.l */
                        tcg_gen_shri_i64(sign, a, 63);
                        tcg_gen_andi_i64(sign, sign, 1);
                        tcg_gen_neg_i64(ext, sign);
                        tcg_gen_shli_i64(ext, ext, 32);
                        tcg_gen_shri_i64(low, b, 32);
                        tcg_gen_andi_i64(low, low, 0xffffffffULL);
                    }
                    tcg_gen_or_i64(res, ext, low);

                    if (f1 > 1) {
                        gen_fr_store_lo(f1, res);
                        gen_fr_store_hi(f1, tcg_constant_i64(0));
                    }
                    handled = true;
                }
            }

            /*
             * fnorm.*: normalize an unnormalized significand.
             *
             * GCC uses ldf8/setf.sig (dword -> FP register) + fnorm + getf.exp
             * to implement ia64_fls().  Per SKI, fnorm shares the fma encoding
             * when f4==f1 (1.0) and f2==f0 (0.0).
             */
            if (!handled && f_major >= 0x8 && f_major <= 0xD) {
                uint8_t x = extract64(insn, 36, 1);
                uint8_t f4 = extract64(insn, 27, 7) & 0x7f;
                uint8_t f3 = extract64(insn, 20, 7) & 0x7f;
                uint8_t f2 = extract64(insn, 13, 7) & 0x7f;
                uint8_t f1 = extract64(insn, 6, 7) & 0x7f;

                if (x == 0 && f4 == 1 && f2 == 0) {
                    TCGv_i64 mant = tcg_temp_new_i64();
                    TCGv_i64 expw = tcg_temp_new_i64();
                    gen_fr_load_lo(mant, f3);
                    gen_fr_load_hi(expw, f3);

                    TCGv_i64 sign = tcg_temp_new_i64();
                    TCGv_i64 exp = tcg_temp_new_i64();
                    tcg_gen_andi_i64(sign, expw, 0x20000);
                    tcg_gen_andi_i64(exp, expw, 0x1ffff);

                    /* Default exponent for integer significands if unset. */
                    TCGLabel *have_exp = gen_new_label();
                    tcg_gen_brcondi_i64(TCG_COND_NE, exp, 0, have_exp);
                    tcg_gen_movi_i64(exp, IA64_FP_EXP_INTEGER);
                    gen_set_label(have_exp);

                    TCGLabel *zero = gen_new_label();
                    TCGLabel *done = gen_new_label();
                    tcg_gen_brcondi_i64(TCG_COND_EQ, mant, 0, zero);

                    /* Normalize mantissa and adjust exponent. */
                    TCGv_i64 lz = tcg_temp_new_i64();
                    tcg_gen_clzi_i64(lz, mant, 64);

                    TCGv_i64 mant_norm = tcg_temp_new_i64();
                    tcg_gen_shl_i64(mant_norm, mant, lz);

                    TCGv_i64 exp_norm = tcg_temp_new_i64();
                    tcg_gen_sub_i64(exp_norm, exp, lz);
                    tcg_gen_andi_i64(exp_norm, exp_norm, 0x1ffff);

                    TCGv_i64 expw_norm = tcg_temp_new_i64();
                    tcg_gen_or_i64(expw_norm, sign, exp_norm);

                    if (f1 > 1) {
                        gen_fr_store_lo(f1, mant_norm);
                        gen_fr_store_hi(f1, expw_norm);
                    }
                    tcg_gen_br(done);

                    gen_set_label(zero);
                    if (f1 > 1) {
                        gen_fr_store_lo(f1, tcg_constant_i64(0));
                        gen_fr_store_hi(f1, tcg_constant_i64(0));
                    }
                    gen_set_label(done);
                    handled = true;
                }

                /*
                 * FMA-family operations (fma/fms/fnma) used by reciprocal and
                 * division sequences in firmware and runtime code.
                 *
                 * For now, model them using host long double arithmetic.
                 */
                if (!handled && x == 0) {
                    if (f_major == 0x8 || f_major == 0x9) {
                        gen_helper_fma_s1(tcg_env,
                                          tcg_constant_i32(f1),
                                          tcg_constant_i32(f3),
                                          tcg_constant_i32(f4),
                                          tcg_constant_i32(f2));
                        handled = true;
                    } else if (f_major == 0xA || f_major == 0xB) {
                        gen_helper_fms_s1(tcg_env,
                                          tcg_constant_i32(f1),
                                          tcg_constant_i32(f3),
                                          tcg_constant_i32(f4),
                                          tcg_constant_i32(f2));
                        handled = true;
                    } else if (f_major == 0xC || f_major == 0xD) {
                        gen_helper_fnma_s1(tcg_env,
                                           tcg_constant_i32(f1),
                                           tcg_constant_i32(f3),
                                           tcg_constant_i32(f4),
                                           tcg_constant_i32(f2));
                        handled = true;
                    }
                }
            }

            if (f_major == 0xE) {
                /* F2 xma.* (xmpy.* pseudo-ops use f2=0) */
                uint8_t x = extract64(insn, 36, 1);
                uint8_t x2 = extract64(insn, 34, 2);
                if (x == 1 && (x2 == 0 || x2 == 2 || x2 == 3)) {
                    uint8_t f4 = extract64(insn, 27, 7);
                    uint8_t f3 = extract64(insn, 20, 7);
                    uint8_t f2 = extract64(insn, 13, 7);
                    uint8_t f1 = extract64(insn, 6, 7);
                    if (x2 == 0) {
                        gen_helper_xma_l(tcg_env,
                                         tcg_constant_i32(f1),
                                         tcg_constant_i32(f3),
                                         tcg_constant_i32(f4),
                                         tcg_constant_i32(f2));
                    } else if (x2 == 2) {
                        gen_helper_xma_hu(tcg_env,
                                          tcg_constant_i32(f1),
                                          tcg_constant_i32(f3),
                                          tcg_constant_i32(f4),
                                          tcg_constant_i32(f2));
                    } else {
                        gen_helper_xma_h(tcg_env,
                                         tcg_constant_i32(f1),
                                         tcg_constant_i32(f3),
                                         tcg_constant_i32(f4),
                                         tcg_constant_i32(f2));
                    }
                    if (skip) {
                        gen_set_label(skip);
                    }
                    break;
                }
            }
            if (skip) {
                gen_set_label(skip);
            }
            if (!handled) {
                gen_unimpl(ctx, insn, "F-slot");
            }
        }
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
        if (insn == 0) {
            /* Treat zero X-slot as nop (e.g., MLX filler). */
            break;
        }
        if (major == 0x0) {
            /*
             * X-slot no-ops/hints.
             *
             * The kernel uses MLX bundles with a standalone M-slot op plus
             * an unused L+X pair. GAS encodes the unused X slot as a nop.x
             * which is not necessarily all-zero (e.g. insn=0x8000000).
             */
            uint8_t x3 = extract64(insn, 33, 3);
            uint8_t x2 = extract64(insn, 31, 2);
            uint8_t x4 = extract64(insn, 27, 4);
            if (x3 == 0 && x2 == 0 && x4 == 0x1) {
                /* nop.x/hint.x imm21 */
                break;
            }
            gen_unimpl(ctx, insn, "X-slot op0");
        }
        if (major == 0x6) {
            /* movl r1 = imm64 (X2 format), uses prior L-slot as extra_bits */
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
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
            if (skip_label) {
                gen_set_label(skip_label);
            }
        } else if (major == 0xC || major == 0xD) {
            /* X3/X4: brl.cond / brl.call target64, uses prior L-slot extra_bits */
            static int brl_log_count;
            static int brl_log_enabled = -1;
            uint8_t qp = insn & 0x3f;
            TCGLabel *skip_label = gen_qp_skip(qp);
            uint64_t i = extract64(insn, 36, 1);
            uint64_t imm20b = extract64(insn, 13, 20);
            uint64_t imm39 = extract64(ctx->extra_bits, 2, 39);
            uint64_t v = (i << 59) | (imm39 << 20) | imm20b;
            int64_t disp = sextract64(v, 0, 60) << 4;
            uint64_t tgt = ctx->base.pc_next + disp;
            if (major == 0xD) {
                uint8_t b1 = extract64(insn, 6, 3);
                if (brl_log_enabled == -1) {
                    brl_log_enabled = getenv("QEMU_IA64_BRL_LOG") ? 1 : 0;
                }
                if (brl_log_enabled && brl_log_count < 32) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "brl.call pc=%016" PRIx64 " insn=%011" PRIx64
                                  " L=%011" PRIx64 " b1=%u tgt=%016" PRIx64 "\n",
                                  ctx->base.pc_next, insn, ctx->extra_bits, b1, tgt);
                    brl_log_count++;
                }
                if (ia64_dbg_call_pc_match(ctx->base.pc_next)) {
                    gen_helper_dbg_call(tcg_env, tcg_constant_i64(ctx->base.pc_next));
                }
                gen_helper_call(tcg_env, tcg_constant_i64(ctx->base.pc_next),
                                tcg_constant_i64(tgt));
                gen_helper_check_null_branch(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next),
                                             tcg_constant_i32(ctx->ri),
                                             tcg_constant_i64(insn),
                                             tcg_constant_i64(tgt));
                tcg_gen_movi_i64(cpu_b[b1], ctx->base.pc_next + 16);
                if (b1 == 0) {
                    gen_record_b0_write(ctx, insn, 1,
                                        tcg_constant_i64(ctx->base.pc_next + 16));
                }
                gen_record_branch(ctx, insn, 7, tcg_constant_i64(tgt));
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                if (qp == 0) {
                    ctx->base.is_jmp = DISAS_NORETURN;
                }
                tcg_gen_exit_tb(NULL, 0);
            } else {
                gen_helper_check_null_branch(tcg_env,
                                             tcg_constant_i64(ctx->base.pc_next),
                                             tcg_constant_i32(ctx->ri),
                                             tcg_constant_i64(insn),
                                             tcg_constant_i64(tgt));
                gen_record_branch(ctx, insn, 6, tcg_constant_i64(tgt));
                tcg_gen_movi_i64(cpu_pc, tgt);
                gen_set_ri_const(0);
                if (qp == 0) {
                    ctx->base.is_jmp = DISAS_NORETURN;
                }
                tcg_gen_exit_tb(NULL, 0);
            }
            if (skip_label) {
                gen_set_label(skip_label);
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
    uint8_t stop_mask = template_stop_mask[template];

    if (ctx->ri == 0) {
        ctx->extra_bits = 0;
    }

    /*
     * libgcc division/modulus helpers.
     *
     * These functions are implemented in the kernel image using floating-point
     * instructions (fnorm/frcpa/fma/...), which are not fully modeled yet.
     * Emulate the whole helper by computing the result directly and returning
     * to the caller (as if the function executed br.ret b0).
     *
     * vmlinux: stuff/vmlinux-ia64-main
     */
    if (ctx->ri == 0) {
        uint64_t pc = ctx->base.pc_next;
        bool handled = true;
        TCGv_i64 res = tcg_temp_new_i64();

        switch (pc) {
        case 0xa0000001011adce0ULL: /* __divdi3 */
            gen_helper_divdi3(res, tcg_env, cpu_r[32], cpu_r[33]);
            break;
        case 0xa0000001011adde0ULL: /* __divsi3 */
            gen_helper_divsi3(res, tcg_env, cpu_r[32], cpu_r[33]);
            break;
        case 0xa0000001011adea0ULL: /* __moddi3 */
            gen_helper_moddi3(res, tcg_env, cpu_r[32], cpu_r[33]);
            break;
        case 0xa0000001011adfa0ULL: /* __modsi3 */
            gen_helper_modsi3(res, tcg_env, cpu_r[32], cpu_r[33]);
            break;
        case 0xa0000001011ae080ULL: /* __udivdi3 */
            gen_helper_udivdi3(res, tcg_env, cpu_r[32], cpu_r[33]);
            break;
        case 0xa0000001011ae180ULL: /* __udivsi3 */
            gen_helper_udivsi3(res, tcg_env, cpu_r[32], cpu_r[33]);
            break;
        case 0xa0000001011ae240ULL: /* __umoddi3 */
            gen_helper_umoddi3(res, tcg_env, cpu_r[32], cpu_r[33]);
            break;
        case 0xa0000001011ae340ULL: /* __umodsi3 */
            gen_helper_umodsi3(res, tcg_env, cpu_r[32], cpu_r[33]);
            break;
        default:
            handled = false;
            break;
        }

        if (handled) {
            /* Return value is in r8 per IA-64 calling convention. */
            tcg_gen_mov_i64(cpu_r[8], res);
            gen_helper_ret_restore(tcg_env);

            TCGv_i64 tgt = tcg_temp_new_i64();
            tcg_gen_andi_i64(tgt, cpu_b[0], ~0xFULL);
            tcg_gen_mov_i64(cpu_pc, tgt);
            gen_set_ri_const(0);

            ctx->base.is_jmp = DISAS_NORETURN;
            tcg_gen_exit_tb(NULL, 0);
            return;
        }
    }

    /*
     * Synthetic firmware entry points (PAL + SAL) used for direct-kernel boot.
     *
     * hw/ia64/ipf.c builds a minimal SAL system table which points at fixed PAL
     * and SAL procedure addresses in low RAM. Linux calls into those addresses
     * via __va() (region 7 direct map); physical-mode PAL calls may reach the
     * raw physical address as well.
     */
    if (ctx->ri == 0) {
        uint64_t pc = ctx->base.pc_next;
        uint64_t pal_pc = IA64_RGN7_BASE | IA64_IPF_FW_PAL_PROC_ADDR;
        uint64_t sal_pc = IA64_RGN7_BASE | IA64_IPF_FW_SAL_PROC_ADDR;
        uint64_t pc_low61 = pc & ((1ULL << 61) - 1);
        bool is_pal = (pc == pal_pc) || (pc == IA64_IPF_FW_PAL_PROC_ADDR) ||
                      (pc_low61 == IA64_IPF_FW_PAL_LEGACY_ADDR);
        bool is_sal = (pc == sal_pc) || (pc == IA64_IPF_FW_SAL_PROC_ADDR);

        if (is_pal || is_sal) {
            if (is_sal) {
                gen_helper_fw_sal(tcg_env);
                gen_helper_ret_restore(tcg_env);
            } else {
                gen_helper_fw_pal(tcg_env);
                gen_helper_ret_restore_b0(tcg_env);
            }

            TCGv_i64 tgt = tcg_temp_new_i64();
            tcg_gen_andi_i64(tgt, cpu_b[0], ~0xFULL);
            tcg_gen_mov_i64(cpu_pc, tgt);
            gen_set_ri_const(0);

            ctx->base.is_jmp = DISAS_NORETURN;
            tcg_gen_exit_tb(NULL, 0);
            return;
        }
    }

    if (ctx->ri == 2 && template_table[template][1] == SLOT_L) {
        ctx->extra_bits = slot1;
    }

    enum SlotType type = template_table[template][ctx->ri];
    uint64_t insn = 0;
    switch (ctx->ri) {
    case 0: insn = slot0; break;
    case 1: insn = slot1; break;
    case 2: insn = slot2; break;
    }

    if (type == SLOT_RES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IA64 reserved template tmpl=%02x pc=%016" PRIx64
                      " ri=%d low=%016" PRIx64 " high=%016" PRIx64 "\n",
                      template, ctx->base.pc_next, ctx->ri, low, high);
    }

    qemu_log_mask_and_addr(CPU_LOG_EXEC, ctx->base.pc_next,
                           "IA64: pc=%016" PRIx64 " ri=%d tmpl=%02x slot=%d insn=%011" PRIx64 "\n",
                           ctx->base.pc_next, ctx->ri, template, type, insn);

    uint8_t cur_ri = ctx->ri;
    if (ia64_dbg_probe_match(ctx->base.pc_next, ctx->ri)) {
        gen_helper_dbg_probe(tcg_env,
                             tcg_constant_i64(ctx->base.pc_next),
                             tcg_constant_i32(ctx->ri));
    }

    decode_insn(ctx, insn, type);

    if (ctx->base.is_jmp == DISAS_NEXT &&
        ia64_pc_in_fw(ctx->base.pc_next) &&
        ia64_fw_r8_trace_match(ctx->base.pc_next)) {
        gen_helper_fw_r8_watch(tcg_env,
                               tcg_constant_i64(ctx->base.pc_next),
                               tcg_constant_i32(ctx->ri),
                               tcg_constant_i64(insn));
    }
    
    ctx->ri++;
    if (ctx->ri == 3) {
        ctx->ri = 0;
        ctx->base.pc_next += 16;
    }
    
    if (ctx->base.is_jmp == DISAS_NEXT) {
        if (stop_mask & (1U << cur_ri)) {
            ctx->base.is_jmp = DISAS_TOO_MANY;
        }
    }
}

static void ia64_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    vaddr next_ip = ctx->base.pc_next;
    uint8_t next_ri = ctx->ri;

    /*
     * If we bailed out before completing a bundle, pc_next may still point
     * at the current bundle base. Ensure the TB accounts for the bytes
     * we actually consumed so translate-all does not see a zero-sized TB.
     */
    if (ctx->base.pc_next == ctx->base.pc_first) {
        ctx->base.pc_next += 16;
    }
    
    switch (ctx->base.is_jmp) {
    case DISAS_TOO_MANY:
    case DISAS_NEXT:
        /* Commit next IP + RI. */
        tcg_gen_movi_i64(cpu_pc, next_ip);
        gen_set_ri_const(next_ri);
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
