/*
 * IA-64 Processor Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef TARGET_IA64_PAL_H
#define TARGET_IA64_PAL_H

#define IA64_PAL_STATUS_SUCCESS        0
#define IA64_PAL_STATUS_UNIMPLEMENTED  (-1)

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

#endif
