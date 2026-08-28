/*
 * IA-64 Processor Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef TARGET_IA64_PAL_H
#define TARGET_IA64_PAL_H

#include "clock.h"

#define IA64_PAL_STATUS_SUCCESS        0
#define IA64_PAL_STATUS_UNIMPLEMENTED  (-1)
#define IA64_PAL_STATUS_EINVAL         (-2)

#define IA64_PAL_CACHE_TYPE_INSTRUCTION 1
#define IA64_PAL_CACHE_TYPE_DATA        2
#define IA64_PAL_CACHE_LEVELS           3
#define IA64_PAL_UNIQUE_CACHES          5

#define IA64_PAL_TC_LEVELS              1
#define IA64_PAL_UNIQUE_TCS             2
#define IA64_PAL_VM_PAGE_SIZES UINT64_C(0x115557000)

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
IA64PALResult ia64_pal_result_proc_get_features(
    uint64_t feature_set);
IA64PALResult ia64_pal_result_freq_base(void);
IA64PALResult ia64_pal_result_freq_ratios(void);

#endif
