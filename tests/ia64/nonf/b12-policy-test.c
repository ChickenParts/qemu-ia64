/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include "../../../target/ia64/b12.h"
int main(void)
{
    const IA64B12Class kinds[8] = {
        IA64_B12_COND, IA64_B12_RESERVED, IA64_B12_WEXIT,
        IA64_B12_WTOP, IA64_B12_RESERVED, IA64_B12_CLOOP,
        IA64_B12_CEXIT, IA64_B12_CTOP,
    };
    unsigned int count = 0;
    for (unsigned int major = 0; major < 16; major++) {
        for (unsigned int btype = 0; btype < 8; btype++) {
            IA64B12Class want = major == 4 ? kinds[btype] : IA64_B12_OTHER;
            IA64B12Class got = ia64_b12_classify(major, btype);
            if (want != got) {
                fprintf(stderr, "class major=%u btype=%u want=%u got=%u\n",
                        major, btype, want, got);
                return 1;
            }
            for (unsigned int qp = 0; qp < 64; qp++) {
                int counted = want == IA64_B12_CLOOP ||
                              want == IA64_B12_CEXIT || want == IA64_B12_CTOP;
                int should_fault = want == IA64_B12_RESERVED ||
                                   (counted && qp != 0);
                if (ia64_b12_faults_before_predication(got, qp) != should_fault) {
                    fprintf(stderr, "fault major=%u btype=%u qp=%u\n",
                            major, btype, qp);
                    return 1;
                }
                count++;
            }
        }
    }
    printf("IA-64 B1/B2 POLICY PASS: %u classifications\n", count);
    return count == 8192 ? 0 : 1;
}
