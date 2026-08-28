/*
 * IA-64 System Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_IA64_SAL_H
#define TARGET_IA64_SAL_H

#include "clock.h"

#define IA64_SAL_STATUS_SUCCESS  0
#define IA64_SAL_STATUS_EINVAL   (-2)

#define IA64_SAL_FREQ_BASE_PLATFORM       0
#define IA64_SAL_FREQ_BASE_INTERVAL_TIMER 1
#define IA64_SAL_FREQ_BASE_REALTIME_CLOCK 2
#define IA64_SAL_NO_DRIFT_INFO UINT64_MAX

typedef struct IA64SALResult {
    int64_t status;
    uint64_t v0;
    uint64_t v1;
    uint64_t v2;
} IA64SALResult;

IA64SALResult ia64_sal_result_freq_base(uint64_t selector);

#endif
