/*
 * Exhaustive direct-production evaluator tests for IA-64 epc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../../../target/ia64/epc.h"

int main(void)
{
    unsigned int count = 0;
    for (unsigned int cpl = 0; cpl < 4; cpl++) {
        for (unsigned int ppl = 0; ppl < 4; ppl++) {
            for (unsigned int it = 0; it < 2; it++) {
                for (unsigned int found = 0; found < 2; found++) {
                    for (unsigned int ar = 0; ar < 8; ar++) {
                        for (unsigned int pl = 0; pl < 4; pl++) {
                            /* Independent three-stage reference truth table. */
                            unsigned int expected = cpl;
                            if (ppl < cpl) {
                                expected = IA64_EPC_ILLEGAL;
                            } else if (!it) {
                                expected = 0;
                            } else if (found && ar == 7 && pl < cpl) {
                                expected = pl;
                            }
                            unsigned int actual =
                                ia64_epc_next_cpl(cpl, ppl, it != 0,
                                                  found != 0, ar, pl);
                            if (actual != expected) {
                                fprintf(stderr,
                                        "epc cpl=%u ppl=%u it=%u found=%u "
                                        "ar=%u pl=%u expected=%u got=%u\n",
                                        cpl, ppl, it, found, ar, pl,
                                        expected, actual);
                                return 1;
                            }
                            count++;
                        }
                    }
                }
            }
        }
    }
    /* 4*4*2*2*8*4 = 2048; no corner is skipped. */
    if (count != 2048) {
        fprintf(stderr, "unexpected epc coverage count %u\n", count);
        return 1;
    }
    printf("IA-64 EPC HOST LOGIC PASS: %u/2048 combinations\n", count);
    return 0;
}
