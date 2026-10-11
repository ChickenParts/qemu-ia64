/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include "../../../target/ia64/b4.h"

int main(void)
{
    unsigned int count = 0;

    for (unsigned int major = 0; major < 16; major++) {
        for (unsigned int x6 = 0; x6 < 64; x6++) {
            for (unsigned int btype = 0; btype < 8; btype++) {
                IA64B4Class expected = IA64_B4_NOT_B4;
                if (major == 0 && x6 == 0x20) {
                    expected = btype == 0 ? IA64_B4_COND :
                               btype == 1 ? IA64_B4_IA32 :
                                            IA64_B4_RESERVED;
                } else if (major == 0 && x6 == 0x21) {
                    expected = btype == 4 ? IA64_B4_RET : IA64_B4_RESERVED;
                }
                IA64B4Class actual = ia64_b4_classify(major, x6, btype);
                if (actual != expected) {
                    fprintf(stderr,
                            "class major=%u x6=%u btype=%u expected=%u got=%u\n",
                            major, x6, btype, expected, actual);
                    return 1;
                }
                for (unsigned int qp = 0; qp < 64; qp++) {
                    int want_fault = expected == IA64_B4_RESERVED ||
                                     (expected == IA64_B4_IA32 && qp != 0);
                    int got_fault =
                        ia64_b4_faults_before_predication(actual, qp);
                    if (got_fault != want_fault) {
                        fprintf(stderr,
                                "fault major=%u x6=%u btype=%u qp=%u "
                                "expected=%d got=%d\n",
                                major, x6, btype, qp,
                                want_fault, got_fault);
                        return 1;
                    }
                    count++;
                }
            }
        }
    }
    if (count != 16U * 64U * 8U * 64U) {
        return 1;
    }
    printf("IA-64 B4 POLICY PASS: %u classifications\n", count);
    return 0;
}
