/*
 * Itanium architectural CR.IFS — Intel SDM Vol 2 rev 2.3, §3.3.5.7.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * IFS.ifm = [37:0], IFS.v = bit63, [62:38] reserved.  There is NO IFS.EC.
 * AR.EC is independent of IFS and is not restored by rfi.
 */
#ifndef TARGET_IA64_IFS_H
#define TARGET_IA64_IFS_H
#include <stdbool.h>
#include <stdint.h>

#define IA64_IFS_IFM_MASK ((UINT64_C(1) << 38) - 1)
#define IA64_IFS_V        (UINT64_C(1) << 63)

/* When interruption collection was enabled, hardware invalidates IFS.v.
 * The other bits are architecturally undefined on that interruption, so
 * deliberately do not manufacture a saved CFM from them.
 */
static inline uint64_t ia64_ifs_on_interruption(uint64_t ifs, bool psr_ic)
{
    return psr_ic ? (ifs & ~IA64_IFS_V) : ifs;
}

/* cover writes IFS only with collection DISABLED. */
static inline uint64_t ia64_ifs_on_cover(uint64_t ifs, uint64_t cfm,
                                          bool psr_ic)
{
    return psr_ic ? ifs : IA64_IFS_V | (cfm & IA64_IFS_IFM_MASK);
}

/* rfi restores only the defined 38-bit frame marker when IFS.v is set. */
static inline uint64_t ia64_ifs_rfi_cfm(uint64_t current_cfm, uint64_t ifs)
{
    return (ifs & IA64_IFS_V) ? (ifs & IA64_IFS_IFM_MASK) : current_cfm;
}
#endif /* TARGET_IA64_IFS_H */
