/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include "../../../target/ia64/b12.h"

int main(void)
{
    unsigned count = 0, wrong = 0;
    for (unsigned major = 0; major < 16; major++) {
        for (unsigned btype = 0; btype < 8; btype++) {
            IA64B12Class kind = ia64_b12_classify(major, btype);
            bool counted_or_while = major == 4 &&
                (btype == 2 || btype == 3 || btype == 5 ||
                 btype == 6 || btype == 7);
            bool while_form = major == 4 && (btype == 2 || btype == 3);
            if (!!ia64_b12_while_uses_qp_as_operand(kind) != while_form) {
                fprintf(stderr, "qp role major=%u btype=%u\n", major, btype);
                return 1;
            }
            for (unsigned ri = 0; ri < 3; ri++) {
                int expected_slot_fault = counted_or_while && ri != 2;
                if (!!ia64_b12_loop_wrong_slot(kind, ri) != expected_slot_fault) {
                    fprintf(stderr, "slot major=%u btype=%u ri=%u\n",
                            major, btype, ri);
                    return 1;
                }
                for (unsigned qp = 0; qp < 64; qp++) {
                    int reserved = major == 4 &&
                        (btype == 1 || btype == 4 ||
                         ((btype == 5 || btype == 6 ||
                           btype == 7) && qp != 0));
                    int fault = ia64_b12_faults_before_predication(kind, qp)
                                || ia64_b12_loop_wrong_slot(kind, ri);
                    if (fault != (reserved || expected_slot_fault)) {
                        fprintf(stderr, "fault major=%u type=%u ri=%u qp=%u\n",
                                major, btype, ri, qp);
                        return 1;
                    }
                    count++;
                    if (expected_slot_fault) {
                        wrong++;
                    }
                }
            }
        }
    }
    printf("IA-64 B1/B2 LOOP POLICY PASS: %u classifications, %u slot faults\n",
           count, wrong);
    return count == 24576 ? 0 : 1;
}
