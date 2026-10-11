/*
 * IA-64 B5 indirect-call whether-hint policy.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Intel Itanium SDM Vol. 3 rev. 2.3, Table 4-53 assigns only odd wh:
 * 1=.sptk, 3=.spnt, 5=.dptk, 7=.dpnt.  Even cells are Reserved.
 */
#ifndef TARGET_IA64_B5_H
#define TARGET_IA64_B5_H

#include <stdint.h>

static inline int ia64_b5_wh_is_assigned(unsigned int major,
                                          unsigned int wh)
{
    return major == 1 && ((wh & 7) == 1 || (wh & 7) == 3 ||
                          (wh & 7) == 5 || (wh & 7) == 7);
}

static inline int ia64_b5_is_reserved(unsigned int major, unsigned int wh)
{
    return major == 1 && !ia64_b5_wh_is_assigned(major, wh);
}

#endif /* TARGET_IA64_B5_H */
