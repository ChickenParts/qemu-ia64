/*
 * IA-64 B1/B2 branch-table legality; Intel SDM Vol. 3 rev. 2.3 Table 4-47.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef TARGET_IA64_B12_H
#define TARGET_IA64_B12_H
#include <stdint.h>
typedef enum IA64B12Class {
    IA64_B12_OTHER = 0,
    IA64_B12_COND,
    IA64_B12_WEXIT,
    IA64_B12_WTOP,
    IA64_B12_CLOOP,
    IA64_B12_CEXIT,
    IA64_B12_CTOP,
    IA64_B12_RESERVED,
} IA64B12Class;
static inline IA64B12Class ia64_b12_classify(unsigned int major,
                                             unsigned int btype)
{
    if (major != 4) {
        return IA64_B12_OTHER;
    }
    switch (btype & 7) {
    case 0: return IA64_B12_COND;
    case 2: return IA64_B12_WEXIT;
    case 3: return IA64_B12_WTOP;
    case 5: return IA64_B12_CLOOP;
    case 6: return IA64_B12_CEXIT;
    case 7: return IA64_B12_CTOP;
    default: return IA64_B12_RESERVED;
    }
}
/* Brown Reserved btypes fault independently of qp; B2 qp must be zero. */
static inline int ia64_b12_faults_before_predication(IA64B12Class kind,
                                                     unsigned int qp)
{
    return kind == IA64_B12_RESERVED ||
           ((kind == IA64_B12_CLOOP ||
             kind == IA64_B12_CEXIT ||
             kind == IA64_B12_CTOP) && (qp & 0x3f) != 0);
}
#endif /* TARGET_IA64_B12_H */
