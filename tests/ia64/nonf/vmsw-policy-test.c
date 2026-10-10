/*
 * Direct-production policy tests for IA-64 vmsw on the current no-VM model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../../../target/ia64/vmsw.h"

static int fail_encoding(uint64_t raw, bool expected, bool actual)
{
    fprintf(stderr,
            "vmsw raw=%011llx expected-valid=%u got=%u\n",
            (unsigned long long)raw, expected, actual);
    return 1;
}

int main(void)
{
    unsigned int count = 0;

    /* Exhaust x6 and every possible qp value. */
    for (unsigned int x6 = 0; x6 < 64; x6++) {
        for (unsigned int qp = 0; qp < 64; qp++) {
            uint64_t raw = ((uint64_t)x6 << 27) | qp;
            bool expected =
                (x6 == IA64_VMSW_X6_CLEAR || x6 == IA64_VMSW_X6_SET) &&
                qp == 0;
            bool actual = ia64_vmsw_encoding_valid(raw);

            if (actual != expected) {
                return fail_encoding(raw, expected, actual);
            }
            count++;
        }
    }

    /* Every remaining fixed bit must independently invalidate both words. */
    for (unsigned int bit = 0; bit < 41; bit++) {
        if (bit >= 27 && bit <= 32) {
            continue;
        }
        for (unsigned int x6 = IA64_VMSW_X6_CLEAR;
             x6 <= IA64_VMSW_X6_SET; x6++) {
            uint64_t raw = ((uint64_t)x6 << 27) | (UINT64_C(1) << bit);

            if (ia64_vmsw_encoding_valid(raw)) {
                return fail_encoding(raw, false, true);
            }
            count++;
        }
    }

    if (!ia64_vmsw_encoding_valid(
            (uint64_t)IA64_VMSW_X6_CLEAR << 27) ||
        ia64_vmsw_sets_vm((uint64_t)IA64_VMSW_X6_CLEAR << 27) ||
        !ia64_vmsw_encoding_valid(
            (uint64_t)IA64_VMSW_X6_SET << 27) ||
        !ia64_vmsw_sets_vm((uint64_t)IA64_VMSW_X6_SET << 27)) {
        fprintf(stderr, "vmsw legal-word classification failed\n");
        return 1;
    }

    for (uint64_t feature_set = 0; feature_set < 64; feature_set++) {
        uint64_t implemented = UINT64_MAX;
        uint64_t current = UINT64_MAX;
        uint64_t controllable = UINT64_MAX;
        bool valid = ia64_vmsw_no_vm_pal_features(
            feature_set, &implemented, &current, &controllable);

        if (valid != (feature_set == 0)) {
            fprintf(stderr, "PAL feature-set validity failed at %llu\n",
                    (unsigned long long)feature_set);
            return 1;
        }
        if (feature_set == 0) {
            if (implemented != IA64_PAL_PROC_FEATURE_NO_VM ||
                current != IA64_PAL_PROC_FEATURE_NO_VM ||
                controllable != 0 ||
                ((implemented | current | controllable) &
                 IA64_PAL_PROC_FEATURE_ENABLE_VMSW) != 0) {
                fprintf(stderr, "PAL no-VM feature vectors are inconsistent\n");
                return 1;
            }
        } else if (implemented != 0 || current != 0 || controllable != 0) {
            fprintf(stderr, "unsupported PAL feature set leaked values\n");
            return 1;
        }
    }

    printf("IA-64 VMSW POLICY PASS: %u encoding cases + 64 PAL sets\n",
           count);
    return 0;
}
