/*
 * IA-64 br.ret privilege demotion and LP > TB > SS trap priority.
 * Intel Itanium SDM Rev 2.3 Vol. 3, br (pp. 3:20–29);
 * Vol. 2, Table 8-3 and Lower-Privilege Transfer vector (2:205–206).
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef TARGET_IA64_LOWER_PRIVILEGE_H
#define TARGET_IA64_LOWER_PRIVILEGE_H

#include <stdbool.h>
#include <stdint.h>
#include "taken-branch.h"

#define IA64_LP_MASK  (UINT64_C(1) << 25)
#define IA64_LP_CODE  (UINT64_C(1) << 1)
#define IA64_LP_CPL_SHIFT 32
#define IA64_LP_CPL_MASK (UINT64_C(3) << IA64_LP_CPL_SHIFT)

/* br.ret can lower privilege, never grant privilege. */
static inline unsigned ia64_lp_return_cpl(unsigned old_cpl, unsigned saved_ppl)
{
    unsigned cpl = old_cpl & 3;
    unsigned ppl = saved_ppl & 3;
    return cpl >= ppl ? cpl : ppl;
}

static inline bool ia64_lp_should_trap(uint64_t source_psr,
                                      unsigned old_cpl,
                                      unsigned saved_ppl)
{
    return (source_psr & IA64_LP_MASK) != 0 &&
           ia64_lp_return_cpl(old_cpl, saved_ppl) > (old_cpl & 3);
}

static inline uint64_t ia64_lp_isr(uint32_t source_ri, uint64_t source_psr)
{
    /* ISR.code is a BIT VECTOR of concurrent trap conditions. */
    return ((uint64_t)(source_ri & 3) << IA64_TB_ISR_EI_SHIFT) |
           IA64_LP_CODE |
           ((source_psr & IA64_TB_PSR_MASK) ? IA64_TB_CODE : 0) |
           ((source_psr & IA64_TB_SS_PSR_MASK) ? IA64_SS_CODE : 0) |
           ((source_psr & IA64_TB_IC_PSR_MASK) ? 0 :
                                           (UINT64_C(1) << IA64_TB_ISR_NI_BIT));
}
#endif /* TARGET_IA64_LOWER_PRIVILEGE_H */
