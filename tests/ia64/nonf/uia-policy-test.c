/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independently check Merced PA/VA sign extension and shared UIA vector.
 */
#include <stdint.h>
#include <stdio.h>
#include "../../../target/ia64/uia-address.h"

#define IT (UINT64_C(1) << 36)
#define TB (UINT64_C(1) << 26)
#define SS (UINT64_C(1) << 40)
#define IC (UINT64_C(1) << 13)

static int check(uint64_t target, uint64_t psr, uint64_t physical,
                 int expected, const char *case_name)
{
    int actual = ia64_uia_target_unimplemented(target, psr, physical);
    if (actual != expected) {
        fprintf(stderr, "%s: target=%016llx psr=%016llx pa=%016llx "
                "actual=%d expected=%d\n", case_name,
                (unsigned long long)target, (unsigned long long)psr,
                (unsigned long long)physical, actual, expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    unsigned cases = 0;
    const uint64_t rgn = UINT64_C(1) << 61;
    const uint64_t vm_low_max = (UINT64_C(1) << 50) - 1;
    const uint64_t vm_high_min = (~vm_low_max) & ((UINT64_C(1) << 61) - 1);
    for (unsigned region = 0; region < 8; region++) {
        uint64_t prefix = region * rgn;
        uint64_t samples[][2] = {
            { 0, 0 }, { vm_low_max & ~UINT64_C(15), 0 },
            { UINT64_C(1) << 50, 1 },
            { UINT64_C(1) << 51, 1 },
            { vm_high_min, 0 },
            { vm_high_min + 0x100, 0 },
            { (UINT64_C(1) << 60) - 16, 1 },
            { (UINT64_C(1) << 61) - 16, 0 }
        };
        for (unsigned n = 0; n < sizeof(samples)/sizeof(samples[0]); n++) {
            for (unsigned f = 0; f < 4; f++) {
                uint64_t psr = IT | ((f & 1) ? TB : 0) |
                               ((f & 2) ? SS : 0);
                if (check(prefix | samples[n][0], psr, 0, samples[n][1],
                          "virtual")) {
                    return 1;
                }
                cases++;
            }
        }
    }
    const uint64_t pa[][2] = {
        { 0, 0 }, { 0xffffffff, 0 },
        { (UINT64_C(1) << 43), 0 },
        { (UINT64_C(1) << 44) - 16, 0 },
        { (UINT64_C(1) << 44), 1 },
        { (UINT64_C(1) << 45), 1 },
        { (UINT64_C(1) << 60), 1 }
    };
    for (unsigned n=0; n<sizeof(pa)/sizeof(pa[0]); n++) {
        for (unsigned mask = 0; mask < 8; mask++) {
            uint64_t psr = ((mask&1)?TB:0) | ((mask&2)?SS:0) |
                           ((mask&4)?IC:0);
            if (check(pa[n][0], psr, pa[n][0], pa[n][1], "physical")) {
                return 2;
            }
            cases++;
        }
    }
    /* Real physical-mode firmware aliases are normalized by the caller. */
    for (unsigned f = 0; f < 8; f++) {
        uint64_t aliased = UINT64_C(0xffffffffffee0000);
        uint64_t psr = ((f&1)?TB:0)|((f&2)?SS:0)|((f&4)?IC:0);
        if (check(aliased, psr, (uint32_t)aliased, 0, "firmware-alias")) {
            return 3;
        }
        cases++;
    }
    for (unsigned slot = 0; slot < 3; slot++) {
        for (unsigned flags = 0; flags < 16; flags++) {
            uint64_t psr = ((flags & 1)?TB:0) | ((flags&2)?SS:0) |
                           ((flags&4)?IC:0);
            uint64_t expected = ((uint64_t)slot << 41) |
                                ((flags&1)?4:0) | ((flags&2)?8:0) |
                                ((flags&4)?0:(UINT64_C(1)<<39)) |
                                ((flags&8)?2:0);
            if (ia64_uia_trap_isr(slot,psr,flags&8)!=expected) {
                fprintf(stderr,"UIA ISR policy mismatch slot=%u mask=%u\n",
                        slot,flags);
                return 4;
            }
            cases++;
        }
    }
    printf("IA64 MERCED UIA POLICY PASS: %u independent cases\n",cases);
    return cases==368 ? 0 : 5;
}
