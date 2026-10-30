/*
 * Concurrent floating-point result traps and PSR.ss single-step.
 *
 * Intel Itanium SDM rev2.3 Volume 2 Table 8-3 and FP Trap vector 0x5d00:
 * ISR.code contains the FP result trap plus the lower-priority SS flag.
 * This policy is NOT used for pre-result FP faults (vector 0x5c00).
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef TARGET_IA64_FP_TRAP_PRIORITY_H
#define TARGET_IA64_FP_TRAP_PRIORITY_H

#include <stdint.h>
#include "single-step.h"

static inline uint16_t ia64_fp_trap_isr_code(uint16_t fp_trap_code,
                                             uint64_t psr)
{
    return fp_trap_code |
           (ia64_ss_enabled(psr) ? (uint16_t)IA64_SS_CODE : 0);
}

#endif /* TARGET_IA64_FP_TRAP_PRIORITY_H */
