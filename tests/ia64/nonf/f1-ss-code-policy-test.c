/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exhaustive independent ISR.code truth table for F1 parallel FP completion.
 *
 * F1 IEEE O/U/I/FPA cause bits occupy LO[10:7], HI[14:11].
 * Bit0 marks FP trap; concurrent PSR.ss adds bit3. All PSR.tb/lp/ic
 * combinations are irrelevant to this already selected higher-priority
 * floating-point trap vector (0x5d00).
 */
#include <stdint.h>
#include <stdio.h>
#include "../../../target/ia64/fp-trap-priority.h"

int main(void)
{
    unsigned count = 0;
    for (unsigned lanes = 0; lanes < 256; ++lanes) {
        const uint16_t fp = UINT16_C(1) | ((uint16_t)lanes << 7);
        for (unsigned psr_flags = 0; psr_flags < 16; ++psr_flags) {
            uint64_t psr =
                ((psr_flags & 1) ? IA64_TB_SS_PSR_MASK : 0) |
                ((psr_flags & 2) ? IA64_TB_PSR_MASK : 0) |
                ((psr_flags & 4) ? IA64_TB_IC_PSR_MASK : 0) |
                ((psr_flags & 8) ? (UINT64_C(1) << 25) : 0);
            const uint16_t expected = fp |
                ((psr_flags & 1) ? (uint16_t)IA64_SS_CODE : 0);
            const uint16_t actual = ia64_fp_trap_isr_code(fp, psr);

            if (actual != expected ||
                (actual & (UINT16_C(0xff) << 7)) !=
                (fp & (UINT16_C(0xff) << 7)) ||
                (actual & UINT16_C(7)) != 1 ||
                !!(actual & 8) != !!(psr_flags & 1)) {
                fprintf(stderr, "F1+SS ISR failed lanes=%u flags=%u "
                        "actual=%04x expected=%04x\n",
                        lanes, psr_flags, actual, expected);
                return 1;
            }
            ++count;
        }
    }
    printf("IA64 F1+SS ISR POLICY PASS: %u independent cases\n", count);
    return count == 4096 ? 0 : 2;
}
