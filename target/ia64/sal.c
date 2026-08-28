/*
 * IA-64 System Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "sal.h"

static IA64SALResult ia64_sal_result(int64_t status,
                                     uint64_t v0,
                                     uint64_t v1,
                                     uint64_t v2)
{
    return (IA64SALResult) {
        .status = status,
        .v0 = v0,
        .v1 = v1,
        .v2 = v2,
    };
}

IA64SALResult ia64_sal_result_freq_base(uint64_t selector)
{
    uint64_t frequency;

    switch (selector) {
    case IA64_SAL_FREQ_BASE_PLATFORM:
        frequency = IA64_PLATFORM_BASE_HZ;
        break;
    case IA64_SAL_FREQ_BASE_INTERVAL_TIMER:
        frequency = IA64_ITC_HZ;
        break;
    case IA64_SAL_FREQ_BASE_REALTIME_CLOCK:
        frequency = IA64_RTC_HZ;
        break;
    default:
        return ia64_sal_result(IA64_SAL_STATUS_EINVAL,
                               0, 0, 0);
    }

    return ia64_sal_result(IA64_SAL_STATUS_SUCCESS,
                           frequency,
                           IA64_SAL_NO_DRIFT_INFO, 0);
}
