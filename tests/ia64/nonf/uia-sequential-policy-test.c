/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independent Merced sequential slot2 address and ISR oracle.
 */
#include <stdio.h>
#include <stdint.h>
#include "../../../target/ia64/uia-address.h"
#define IT (UINT64_C(1) << 36)
#define IC (UINT64_C(1) << 13)
#define SS (UINT64_C(1) << 40)
#define TB (UINT64_C(1) << 26)
#define LP (UINT64_C(1) << 25)
#define NI (UINT64_C(1) << 39)

int main(void)
{
    unsigned cases = 0;
    const uint64_t vregion = UINT64_C(1) << 61;
    const uint64_t vend = (UINT64_C(1) << 50) - 16;
    const uint64_t pend = (UINT64_C(1) << 44) - 16;
    const uint64_t regionmask = (UINT64_C(1) << 61) - 1;
    for (unsigned r = 0; r < 8; r++) {
        const uint64_t vrn = r * vregion;
        uint64_t samples[][2] = {
            {vrn | vend, 1},
            {vrn | (vend - 16), 0},
            {vrn | (vend - 0x1000), 0},
            {vrn | (UINT64_C(1) << 50), 0},
            {vrn | (regionmask - 16), 0},
        };
        for (unsigned n = 0; n < sizeof(samples)/sizeof(samples[0]); n++) {
            int actual = ia64_uia_sequential_edge_candidate(samples[n][0]);
            if (actual != (int)samples[n][1]) {
                fprintf(stderr, "virtual candidate region %u case %u: %d\n",
                        r, n, actual);
                return 1;
            }
            cases++;
        }
        uint64_t pa[] = {vrn | pend, vrn | (pend - 16),
                         vrn | (pend - 0x1000)};
        for (unsigned n = 0; n < sizeof(pa)/sizeof(pa[0]); n++) {
            int actual = ia64_uia_sequential_edge_candidate(pa[n]);
            if (actual != (n == 0)) {
                fprintf(stderr, "physical candidate region %u case %u: %d\n",
                        r, n, actual);
                return 2;
            }
            cases++;
        }
    }
    for (unsigned source_ri = 0; source_ri < 3; source_ri++) {
        for (unsigned flags = 0; flags < 16; flags++) {
            uint64_t psr = ((flags & 1) ? SS : 0) |
                           ((flags & 2) ? TB : 0) |
                           ((flags & 4) ? LP : 0) |
                           ((flags & 8) ? IC : 0);
            uint64_t expected = ((uint64_t)source_ri << 41) |
                                ((flags & 1) ? 8 : 0) |
                                ((flags & 8) ? 0 : NI);
            if (ia64_uia_sequential_isr(source_ri, psr) != expected) {
                fprintf(stderr, "sequential ISR mismatch ri=%u flags=%u\n",
                        source_ri, flags);
                return 3;
            }
            cases++;
        }
    }
    for (unsigned r = 0; r < 8; r++) {
        const uint64_t low = r * vregion + vend;
        if (!ia64_uia_target_unimplemented(low + 16, IT, 0) ||
            ia64_uia_target_unimplemented(low, IT, 0)) {
            return 4;
        }
        cases += 2;
    }
    if (!ia64_uia_target_unimplemented(pend + 16, 0, pend + 16) ||
        ia64_uia_target_unimplemented(pend, 0, pend)) {
        return 5;
    }
    cases += 2;
    printf("IA64 MERCED SEQUENTIAL UIA POLICY PASS: %u cases\n", cases);
    return cases == 130 ? 0 : 6;
}
