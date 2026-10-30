/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent FP exception+SS ISR.code truth-table oracle. */
#include <stdint.h>
#include <stdio.h>
#include "../../../target/ia64/fp-trap-priority.h"

int main(void)
{
    unsigned checked = 0;
    for (unsigned fp_case = 0; fp_case < 32; fp_case++) {
        /* FP bit0 and all defined high/scalar/parallel precision flags. */
        uint16_t fp = UINT16_C(1);
        if (fp_case & 1) fp |= UINT16_C(1) << 7;
        if (fp_case & 2) fp |= UINT16_C(1) << 9;
        if (fp_case & 4) fp |= UINT16_C(1) << 11;
        if (fp_case & 8) fp |= UINT16_C(1) << 13;
        if (fp_case & 16) fp |= UINT16_C(1) << 14;
        for (unsigned flags = 0; flags < 16; flags++) {
            uint64_t psr = ((flags & 1) ? IA64_TB_SS_PSR_MASK : 0) |
                           ((flags & 2) ? IA64_TB_PSR_MASK : 0) |
                           ((flags & 4) ? IA64_TB_IC_PSR_MASK : 0) |
                           ((flags & 8) ? (UINT64_C(1) << 25) : 0);
            uint16_t expected = fp | ((flags & 1) ? UINT16_C(8) : 0);
            uint16_t actual = ia64_fp_trap_isr_code(fp, psr);
            if (actual != expected || !(actual & 1) ||
                !!(actual & 8) != !!(flags & 1) ||
                (actual & (UINT16_C(2) | UINT16_C(4) | UINT16_C(16)))) {
                fprintf(stderr,
                        "FP+SS priority policy fail: fp=0x%x psr=0x%llx actual=0x%x expected=0x%x\n",
                        fp, (unsigned long long)psr, actual, expected);
                return 1;
            }
            checked++;
        }
    }
    printf("IA64 FP+SS TRAP POLICY PASS: %u independent cases\n", checked);
    return checked == 512 ? 0 : 2;
}
