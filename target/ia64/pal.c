/*
 * IA-64 Processor Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "pal.h"

IA64PALResult ia64_pal_result_unimplemented(void)
{
    return (IA64PALResult) {
        .status = IA64_PAL_STATUS_UNIMPLEMENTED,
        .action = IA64_PAL_ACTION_RETURN,
    };
}

IA64PALResult ia64_pal_result_halt(void)
{
    return (IA64PALResult) {
        .status = IA64_PAL_STATUS_SUCCESS,
        .action = IA64_PAL_ACTION_HALT,
    };
}
