/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <inttypes.h>
#include <stdio.h>
#include "../../../target/ia64/ifs.h"

int main(void)
{
    /* Independent expected tables, not a duplicate copy of production. */
    const uint64_t states[] = {
        0, 1, 7, 63, 0x8000, UINT64_C(1) << 37,
        (UINT64_C(1) << 38) - 1, UINT64_C(1) << 38,
        UINT64_C(1) << 52, UINT64_C(3) << 58,
        IA64_IFS_V, IA64_IFS_V | UINT64_C(0x12345),
        IA64_IFS_V | (UINT64_C(1) << 52),
        UINT64_MAX
    };
    unsigned count = 0;
    for (int ic = 0; ic <= 1; ic++) {
        for (unsigned i = 0; i < sizeof(states)/sizeof(states[0]); i++) {
            for (unsigned j = 0; j < sizeof(states)/sizeof(states[0]); j++) {
                uint64_t ifs = states[i], cfm = states[j];
                uint64_t expected_int = ic ? (ifs & ~(UINT64_C(1)<<63)) : ifs;
                uint64_t expected_cov = ic ? ifs :
                    ((UINT64_C(1)<<63) | (cfm & ((UINT64_C(1)<<38)-1)));
                uint64_t expected_rfi = (ifs >> 63) ?
                    (ifs & ((UINT64_C(1)<<38)-1)) : cfm;
                if (ia64_ifs_on_interruption(ifs, ic) != expected_int ||
                    ia64_ifs_on_cover(ifs, cfm, ic) != expected_cov ||
                    ia64_ifs_rfi_cfm(cfm, ifs) != expected_rfi) {
                    fprintf(stderr, "IFS mismatch ic=%d ifs=%016" PRIx64
                            " cfm=%016" PRIx64 "\n", ic, ifs, cfm);
                    return 1;
                }
                count++;
            }
        }
    }
    printf("IA-64 IFS FIELD POLICY PASS: %u state combinations\n", count);
    return count == 392 ? 0 : 1;
}
