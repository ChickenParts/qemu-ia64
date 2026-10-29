/*
 * Itanium taken-branch trap architectural state, SDM Vol 2 rev 2.3
 * §7.1 and §8 (vector 0x5f00, ISR.code.tb), SPDX LGPL-2.0-or-later.
 *
 * The target bundle pointer is in IIP, whereas IIPA and ISR.ei name
 * the source instruction. A taken branch's side effects have committed.
 */
#ifndef TARGET_IA64_TAKEN_BRANCH_H
#define TARGET_IA64_TAKEN_BRANCH_H
#include <stdint.h>
#include <stdbool.h>

#define IA64_TB_PSR_MASK (UINT64_C(1) << 26)
#define IA64_TB_SS_PSR_MASK (UINT64_C(1) << 40)
#define IA64_TB_IC_PSR_MASK (UINT64_C(1) << 13)
#define IA64_TB_RI_MASK (UINT64_C(3) << 41)
#define IA64_TB_ISR_EI_SHIFT 41
#define IA64_TB_ISR_NI_BIT 39
#define IA64_TB_CODE (UINT64_C(1) << 2)
#define IA64_SS_CODE (UINT64_C(1) << 3)

static inline bool ia64_tb_enabled(uint64_t psr)
{
    return (psr & IA64_TB_PSR_MASK) != 0;
}

/* Taken Itanium branches always target the first slot of a bundle. */
static inline uint64_t ia64_tb_target_iip(uint64_t target)
{
    return target & ~UINT64_C(15);
}

static inline uint64_t ia64_tb_saved_ipsr(uint64_t psr)
{
    return psr & ~IA64_TB_RI_MASK;
}

/* Lower-priority single step is reported in the ISR trap bit vector. */
static inline uint64_t ia64_tb_isr(uint32_t ri, uint64_t psr)
{
    return ((uint64_t)(ri & 3) << IA64_TB_ISR_EI_SHIFT) |
           IA64_TB_CODE |
           ((psr & IA64_TB_SS_PSR_MASK) ? IA64_SS_CODE : 0) |
           ((psr & IA64_TB_IC_PSR_MASK) ? 0 :
                                      (UINT64_C(1) << IA64_TB_ISR_NI_BIT));
}
#endif /* TARGET_IA64_TAKEN_BRANCH_H */
