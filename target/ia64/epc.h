/*
 * IA-64 B8 epc architectural privilege decision.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * This standalone evaluator intentionally has no dependency on QEMU
 * execution internals, so host tests can exhaustively exercise the
 * PFS/CPL/IT/access-rights decision independently of translation.
 */
#ifndef TARGET_IA64_EPC_H
#define TARGET_IA64_EPC_H

#include <stdbool.h>
#include <stdint.h>

enum { IA64_EPC_ILLEGAL = 4 };

/*
 * CPL and PPL are architectural unsigned two-bit levels (0 most privileged).
 * Only AR=7, the execute-only/promotion page, can change CPL with PSR.it=1.
 * A missing translation is NOT permission to elevate.
 */
static inline unsigned int ia64_epc_next_cpl(unsigned int cpl, unsigned int ppl,
                                              bool instruction_translation,
                                              bool fetch_translation_valid,
                                              unsigned int fetch_ar,
                                              unsigned int fetch_pl)
{
    cpl &= 3;
    ppl &= 3;
    if (ppl < cpl) {
        return IA64_EPC_ILLEGAL;
    }
    if (!instruction_translation) {
        return 0;
    }
    if (fetch_translation_valid && fetch_ar == 7 && fetch_pl < cpl) {
        return fetch_pl;
    }
    return cpl;
}

#endif /* TARGET_IA64_EPC_H */
