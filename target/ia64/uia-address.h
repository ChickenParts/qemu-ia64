/*
 * Merced Unimplemented Instruction Address (UIA) post-result TRAP policy.
 *
 * The emulator reports CPU family 7 (original Itanium / Merced):
 * 44 physical address bits (IMPL_PA_MSB=43), and 51 within-region
 * virtual bits (IMPL_VA_MSB=50) plus 3 virtual region bits.
 * Intel Itanium Processor Reference Manual §3.2; legacy IA-64 SDM.
 *
 * Rev2.3 raised the minimum virtual address width for newer processors;
 * this policy deliberately models the CPU family actually advertised.
 * UIA uses the architecturally permitted TRAP choice, sharing vector
 * 0x5e00 with Lower-Privilege Transfer and preceding PSR.tb/ss traps.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef TARGET_IA64_UIA_ADDRESS_H
#define TARGET_IA64_UIA_ADDRESS_H
#include <stdbool.h>
#include <stdint.h>
#include "lower-privilege.h"

#define IA64_MERCED_IMPL_VA_MSB 50
#define IA64_MERCED_IMPL_PA_MSB 43

static inline bool ia64_uia_virtual_implemented(uint64_t va)
{
    /* VA[63:61] are the VRN, VA[60:51] sign-extend VA[50]. */
    uint64_t unused = (va >> 51) & UINT64_C(0x3ff);
    bool sign = ((va >> IA64_MERCED_IMPL_VA_MSB) & 1) != 0;
    return unused == (sign ? UINT64_C(0x3ff) : UINT64_C(0));
}

static inline bool ia64_uia_physical_implemented(uint64_t pa)
{
    /* Physical-mode firmware aliases/PMA are normalized by the caller. */
    return (pa >> (IA64_MERCED_IMPL_PA_MSB + 1)) == 0;
}

static inline bool ia64_uia_target_unimplemented(uint64_t target,
                                                  uint64_t source_psr,
                                                  uint64_t normalized_pa)
{
    if (source_psr & (UINT64_C(1) << 36)) {  /* PSR.it */
        return !ia64_uia_virtual_implemented(target);
    }
    return !ia64_uia_physical_implemented(normalized_pa);
}

static inline uint64_t ia64_uia_trap_isr(unsigned source_ri,
                                         uint64_t source_psr,
                                         bool concurrent_lp)
{
    /*
     * UIA has NO dedicated ISR.code bit. Unlike ia64_tb_isr(), which
     * assumes an enabled taken-branch trap, these causes are optional.
     */
    return ((uint64_t)(source_ri & 3) << IA64_TB_ISR_EI_SHIFT) |
           ((source_psr & IA64_TB_PSR_MASK) ? IA64_TB_CODE : 0) |
           ((source_psr & IA64_TB_SS_PSR_MASK) ? IA64_SS_CODE : 0) |
           ((source_psr & IA64_TB_IC_PSR_MASK) ? 0 :
                                            (UINT64_C(1) << IA64_TB_ISR_NI_BIT)) |
           (concurrent_lp ? IA64_LP_CODE : 0);
}
/*
 * The IA-64 rfi instruction is not a taken branch and never generates
 * PSR.tb/ss or PSR.lp traps. Its post-result UIA TRAP (SDM Vol2 §8)
 * therefore has only the source EI and NI fields, even when the
 * *restored* PSR has tb/ss/lp set. The next IPSR.ri comes from rfi,
 * not from the source instruction and must not be reset to zero.
 *
 * IA-32 (PSR.is=1) transition semantics remain a separate tranche:
 * do not apply the native Itanium address rule to that unsupported mode.
 */
static inline bool ia64_uia_itanium_rfi_unimplemented(uint64_t target,
                                                       uint64_t restored_psr,
                                                       uint64_t normalized_pa)
{
    return !(restored_psr & (UINT64_C(1) << 34)) &&
           ia64_uia_target_unimplemented(target, restored_psr, normalized_pa);
}

static inline uint64_t ia64_uia_rfi_isr(unsigned source_ri,
                                        uint64_t restored_psr)
{
    return ((uint64_t)(source_ri & 3) << IA64_TB_ISR_EI_SHIFT) |
           ((restored_psr & IA64_TB_IC_PSR_MASK) ? 0 :
                                            (UINT64_C(1) << IA64_TB_ISR_NI_BIT));
}

#endif /* TARGET_IA64_UIA_ADDRESS_H */
