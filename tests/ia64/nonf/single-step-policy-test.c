/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../../../target/ia64/single-step.h"

int main(void)
{
    unsigned count = 0;
    for (unsigned slot = 0; slot < 3; ++slot) {
        for (unsigned mlx = 0; mlx < 2; ++mlx) {
            if (mlx && slot != 1) {
                continue; /* MLX L+X is the one valid long instruction. */
            }
            for (unsigned flags = 0; flags < 16; ++flags) {
                uint64_t psr =
                    ((flags & 1) ? IA64_TB_SS_PSR_MASK : 0) |
                    ((flags & 2) ? IA64_TB_PSR_MASK : 0) |
                    ((flags & 4) ? IA64_TB_IC_PSR_MASK : 0) |
                    ((flags & 8) ? (UINT64_C(1) << 25) : 0);
                unsigned next_ri = (mlx || slot == 2) ? 0 : slot + 1;
                uint64_t next_offset = (mlx || slot == 2) ? 16 : 0;
                unsigned trap_ei = mlx ? 2 : slot;
                uint64_t expected_isr = ((uint64_t)trap_ei << 41) |
                    UINT64_C(8) | ((flags & 4) ? 0 : (UINT64_C(1) << 39));
                for (unsigned bundle = 0; bundle < 256; ++bundle) {
                    uint64_t pc = UINT64_C(0x12340000) + 16 * bundle;
                    if (ia64_ss_enabled(psr) != ((flags & 1) != 0) ||
                        ia64_ss_next_ri(slot, mlx != 0) != next_ri ||
                        ia64_ss_next_iip(pc, slot, mlx != 0) !=
                            pc + next_offset ||
                        ia64_ss_saved_ipsr(psr, next_ri) !=
                            ((psr & ~IA64_TB_RI_MASK) |
                             ((uint64_t)next_ri << 41)) ||
                        ia64_ss_isr(trap_ei, psr) != expected_isr) {
                        fprintf(stderr, "IA64 SS policy fails slot=%u mlx=%u "
                                "flags=%u bundle=%u\n",
                                slot, mlx, flags, bundle);
                        return 1;
                    }
                    /* TB remains the higher-priority branch trap. */
                    if ((flags & 3) == 3 &&
                        (ia64_tb_isr(slot, psr) &
                         (IA64_TB_CODE | IA64_SS_CODE)) !=
                        (IA64_TB_CODE | IA64_SS_CODE)) {
                        return 2;
                    }
                    count++;
                }
            }
        }
    }
    printf("IA64 SINGLE-STEP POLICY PASS: %u independent cases\n", count);
    return count == 16384 ? 0 : 3;
}
