/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include "../../../target/ia64/lower-privilege.h"

int main(void)
{
    unsigned count = 0;
    for (unsigned old = 0; old < 4; ++old) {
        for (unsigned ppl = 0; ppl < 4; ++ppl) {
            for (unsigned flags = 0; flags < 16; ++flags) {
                for (unsigned ri = 0; ri < 4; ++ri) {
                    uint64_t psr = (uint64_t)old << 32;
                    if (flags & 1) psr |= UINT64_C(1) << 25;
                    if (flags & 2) psr |= UINT64_C(1) << 26;
                    if (flags & 4) psr |= UINT64_C(1) << 40;
                    if (flags & 8) psr |= UINT64_C(1) << 13;
                    unsigned expected_cpl = old > ppl ? old : ppl;
                    bool expected_lp = (flags & 1) && ppl > old;
                    uint64_t code = ((uint64_t)ri << 41) | 2 |
                                    ((flags & 2) ? 4 : 0) |
                                    ((flags & 4) ? 8 : 0) |
                                    ((flags & 8) ? 0 : (UINT64_C(1) << 39));
                    if (ia64_lp_return_cpl(old,ppl) != expected_cpl ||
                        ia64_lp_should_trap(psr,old,ppl) != expected_lp ||
                        ia64_lp_isr(ri,psr) != code) {
                        fprintf(stderr,"LP mismatch old=%u ppl=%u flags=%u ri=%u\n",
                                old,ppl,flags,ri);
                        return 1;
                    }
                    count++;
                }
            }
        }
    }
    printf("IA-64 PSR.LP RETURN POLICY PASS: %u cases\n",count);
    return count == 1024 ? 0 : 1;
}
