/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independent native Itanium rfi UIA eligibility and ISR cause oracle.
 */
#include <stdint.h>
#include <stdio.h>
#include "../../../target/ia64/uia-address.h"

#define IT (UINT64_C(1) << 36)
#define IS (UINT64_C(1) << 34)
#define LP (UINT64_C(1) << 25)
#define TB (UINT64_C(1) << 26)
#define SS (UINT64_C(1) << 40)
#define IC (UINT64_C(1) << 13)
#define NI (UINT64_C(1) << 39)

int main(void)
{
    unsigned cases = 0;

    for (unsigned source_ri = 0; source_ri < 3; source_ri++) {
        for (unsigned mask = 0; mask < 32; mask++) {
            uint64_t psr = ((mask & 1) ? LP : 0) |
                           ((mask & 2) ? TB : 0) |
                           ((mask & 4) ? SS : 0) |
                           ((mask & 8) ? IC : 0) |
                           ((mask & 16) ? (UINT64_C(2) << 41) : 0);
            uint64_t expected = ((uint64_t)source_ri << 41) |
                                ((mask & 8) ? 0 : NI);
            uint64_t actual = ia64_uia_rfi_isr(source_ri, psr);
            if (actual != expected) {
                fprintf(stderr, "rfi ISR mismatch source=%u mask=%u "
                        "expected=%llx actual=%llx\n", source_ri, mask,
                        (unsigned long long)expected,
                        (unsigned long long)actual);
                return 1;
            }
            cases++;
        }
    }

    /* Native mode uses restored IT, never the interrupted handler's IT. */
    const uint64_t invalid_pa = UINT64_C(1) << 44;
    const uint64_t invalid_va = UINT64_C(1) << 51;
    const uint64_t good = UINT64_C(0x10000);
    for (unsigned mask = 0; mask < 16; mask++) {
        uint64_t debug = ((mask & 1) ? LP : 0) |
                         ((mask & 2) ? TB : 0) |
                         ((mask & 4) ? SS : 0) |
                         ((mask & 8) ? IC : 0);
        for (unsigned virtual_mode = 0; virtual_mode < 2; virtual_mode++) {
            uint64_t psr = debug | (virtual_mode ? IT : 0);
            uint64_t invalid = virtual_mode ? invalid_va : invalid_pa;
            if (!ia64_uia_itanium_rfi_unimplemented(invalid, psr, invalid) ||
                ia64_uia_itanium_rfi_unimplemented(good, psr, good) ||
                ia64_uia_itanium_rfi_unimplemented(invalid, psr | IS, invalid)) {
                fprintf(stderr, "rfi UIA mode mismatch mask=%u IT=%u\n",
                        mask, virtual_mode);
                return 2;
            }
            cases += 3;
        }
    }

    printf("IA64 NATIVE RFI UIA POLICY PASS: %u independent cases\n", cases);
    return cases == 192 ? 0 : 3;
}
