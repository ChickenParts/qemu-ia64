/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include "../../../target/ia64/b5.h"

int main(void)
{
    unsigned int count = 0;
    for (unsigned int major = 0; major < 16; major++) {
        for (unsigned int wh = 0; wh < 8; wh++) {
            int assigned = major == 1 && (wh & 1) != 0;
            int got_assigned = ia64_b5_wh_is_assigned(major, wh);
            int got_reserved = ia64_b5_is_reserved(major, wh);
            if (got_assigned != assigned ||
                got_reserved != (major == 1 && !assigned)) {
                fprintf(stderr,
                        "major=%u wh=%u assigned=%d/%d reserved=%d\n",
                        major, wh, assigned, got_assigned, got_reserved);
                return 1;
            }
            count++;
        }
    }
    if (count != 128) {
        return 1;
    }
    puts("IA-64 B5 POLICY PASS: 128 major/wh classifications");
    return 0;
}
