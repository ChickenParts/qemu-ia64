/*
 * IA-64 B4 indirect branch encoding policy.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Intel Itanium SDM Vol. 3 rev. 2.3, Tables 4-49 and 4-50:
 *   x6=0x20: btype 0 br.cond, btype 1 br.ia, 2..7 Reserved
 *   x6=0x21: btype 4 br.ret, all other values Reserved
 */
#ifndef TARGET_IA64_B4_H
#define TARGET_IA64_B4_H

#include <stdint.h>

typedef enum IA64B4Class {
    IA64_B4_NOT_B4 = 0,
    IA64_B4_COND,
    IA64_B4_IA32,
    IA64_B4_RET,
    IA64_B4_RESERVED,
} IA64B4Class;

static inline IA64B4Class ia64_b4_classify(unsigned int major,
                                            unsigned int x6,
                                            unsigned int btype)
{
    if (major != 0 || (x6 != 0x20 && x6 != 0x21)) {
        return IA64_B4_NOT_B4;
    }
    btype &= 7;
    if (x6 == 0x20) {
        if (btype == 0) {
            return IA64_B4_COND;
        }
        if (btype == 1) {
            return IA64_B4_IA32;
        }
        return IA64_B4_RESERVED;
    }
    return btype == 4 ? IA64_B4_RET : IA64_B4_RESERVED;
}

/* Reserved (brown) cells fault unconditionally.  br.ia is architecturally
 * unpredicated and faults when its encoded qp is not PR0.
 */
static inline int ia64_b4_faults_before_predication(IA64B4Class kind,
                                                     unsigned int qp)
{
    return kind == IA64_B4_RESERVED ||
           (kind == IA64_B4_IA32 && (qp & 0x3f) != 0);
}

#endif /* TARGET_IA64_B4_H */
