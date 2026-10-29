/*
 * IA-64 PSR.ss post-instruction single-stepping.
 *
 * Intel Itanium SDM rev2.3, Vol 2 §5.5.3 and §8; vectors 0x6000/0x5f00.
 * A long MLX L+X instruction occupies two slots, but retires once.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef TARGET_IA64_SINGLE_STEP_H
#define TARGET_IA64_SINGLE_STEP_H

#include <stdbool.h>
#include <stdint.h>
#include "taken-branch.h"

static inline bool ia64_ss_enabled(uint64_t psr)
{
    return (psr & IA64_TB_SS_PSR_MASK) != 0;
}

static inline unsigned ia64_ss_next_ri(unsigned slot, bool mlx_pair)
{
    return (mlx_pair || slot >= 2) ? 0 : slot + 1;
}

static inline uint64_t ia64_ss_next_iip(uint64_t bundle, unsigned slot,
                                         bool mlx_pair)
{
    return (bundle & ~UINT64_C(15)) +
           ((mlx_pair || slot >= 2) ? UINT64_C(16) : 0);
}

static inline uint64_t ia64_ss_saved_ipsr(uint64_t psr, unsigned next_ri)
{
    return (psr & ~IA64_TB_RI_MASK) |
           ((uint64_t)(next_ri & 3) << IA64_TB_ISR_EI_SHIFT);
}

static inline uint64_t ia64_ss_isr(unsigned source_ri, uint64_t psr)
{
    return ((uint64_t)(source_ri & 3) << IA64_TB_ISR_EI_SHIFT) |
           IA64_SS_CODE |
           ((psr & IA64_TB_IC_PSR_MASK) ? 0 :
              (UINT64_C(1) << IA64_TB_ISR_NI_BIT));
}

#endif /* TARGET_IA64_SINGLE_STEP_H */
