/*
 * IA-64 Processor Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef TARGET_IA64_PAL_H
#define TARGET_IA64_PAL_H

#define IA64_PAL_STATUS_SUCCESS        0
#define IA64_PAL_STATUS_UNIMPLEMENTED  (-1)

typedef struct IA64PALResult {
    int64_t status;
    uint64_t v0;
    uint64_t v1;
    uint64_t v2;
} IA64PALResult;

IA64PALResult ia64_pal_result_unimplemented(void);

#endif
