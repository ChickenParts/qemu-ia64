/*
 * IA-64 Processor Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef TARGET_IA64_PAL_H
#define TARGET_IA64_PAL_H

#define IA64_PAL_STATUS_SUCCESS        0
#define IA64_PAL_STATUS_UNIMPLEMENTED  (-1)
#define IA64_PAL_STATUS_EINVAL         (-2)

#define IA64_PAL_CACHE_TYPE_INSTRUCTION 1
#define IA64_PAL_CACHE_TYPE_DATA        2
#define IA64_PAL_CACHE_LEVELS           3
#define IA64_PAL_UNIQUE_CACHES          4

#define IA64_PAL_CACHE_ATTR_WT          0
#define IA64_PAL_CACHE_ATTR_WB          1

#define IA64_PAL_TC_LEVELS              1
#define IA64_PAL_UNIQUE_TCS             2
#define IA64_PAL_VM_PAGE_SIZES UINT64_C(0x115557000)

/*
 * The IPF machine's default processor profile is the 800 MHz,
 * 4 MiB-L3 Merced configuration used by 460GX systems such as the
 * rx4610 and i2000.  TCG is not cycle accurate, but PAL must expose
 * one explicit, internally consistent processor contract.
 */
#define IA64_PAL_MERCED_L3_SIZE UINT64_C(0x00400000)
#define IA64_PAL_PLATFORM_BASE_HZ UINT64_C(100000000)
#define IA64_PAL_MERCED_PROC_RATIO_NUM 8
#define IA64_PAL_MERCED_PROC_RATIO_DEN 1
#define IA64_PAL_MERCED_BUS_RATIO_NUM  4
#define IA64_PAL_MERCED_BUS_RATIO_DEN  3
#define IA64_PAL_VIRTUAL_ITC_RATIO_NUM 10
#define IA64_PAL_VIRTUAL_ITC_RATIO_DEN 1

typedef enum IA64PALAction {
    IA64_PAL_ACTION_RETURN = 0,
    IA64_PAL_ACTION_HALT,
} IA64PALAction;

typedef struct IA64PALResult {
    int64_t status;
    uint64_t v0;
    uint64_t v1;
    uint64_t v2;
    IA64PALAction action;
} IA64PALResult;

IA64PALResult ia64_pal_result_unimplemented(void);
IA64PALResult ia64_pal_result_halt(void);
IA64PALResult ia64_pal_result_cache_summary(void);
IA64PALResult ia64_pal_result_cache_info(uint64_t level,
                                         uint64_t type);
IA64PALResult ia64_pal_result_vm_summary(unsigned int pkr_count,
                                         unsigned int dtr_count,
                                         unsigned int itr_count);
IA64PALResult ia64_pal_result_vm_info(uint64_t level,
                                      uint64_t type,
                                      unsigned int entry_count);
IA64PALResult ia64_pal_result_vm_page_size(void);
IA64PALResult ia64_pal_result_proc_get_features(uint64_t feature_set);
IA64PALResult ia64_pal_result_freq_base(void);
IA64PALResult ia64_pal_result_freq_ratios(void);

#endif
