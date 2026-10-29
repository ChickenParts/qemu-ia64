/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdint.h>
#include "../../../target/ia64/taken-branch.h"

int main(void)
{
    unsigned count = 0;
    const uint64_t psrs[] = {
        0, IA64_TB_PSR_MASK, IA64_TB_PSR_MASK | IA64_TB_IC_PSR_MASK,
        IA64_TB_PSR_MASK | IA64_TB_SS_PSR_MASK,
        IA64_TB_PSR_MASK | IA64_TB_IC_PSR_MASK | IA64_TB_SS_PSR_MASK,
        IA64_TB_IC_PSR_MASK | IA64_TB_SS_PSR_MASK,
        (UINT64_C(1) << 41) | IA64_TB_PSR_MASK,
        (UINT64_C(2) << 41) | IA64_TB_PSR_MASK | IA64_TB_IC_PSR_MASK
    };
    for (unsigned p = 0; p < sizeof(psrs) / sizeof(psrs[0]); ++p) {
        const uint64_t psr = psrs[p];
        for (unsigned slot = 0; slot < 4; slot++) {
            for (unsigned target = 0; target < 256; target++) {
                uint64_t expected = ((uint64_t)slot << 41) | 4;
                if (!(psr & (UINT64_C(1) << 13))) expected |= UINT64_C(1) << 39;
                if (psr & (UINT64_C(1) << 40)) expected |= 8;
                if (ia64_tb_isr(slot, psr) != expected ||
                    ia64_tb_enabled(psr) != !!(psr & (UINT64_C(1) << 26)) ||
                    ia64_tb_target_iip(target) != (target & ~UINT64_C(15)) ||
                    ia64_tb_saved_ipsr(psr) != (psr & ~(UINT64_C(3) << 41))) {
                    fprintf(stderr,"TB trap policy failed psr=%016llx slot=%u target=%u\n",
                            (unsigned long long)psr, slot, target);
                    return 1;
                }
                count++;
            }
        }
    }
    printf("IA-64 TAKEN BRANCH POLICY PASS: %u independent cases\n", count);
    return count == 8192 ? 0 : 1;
}
