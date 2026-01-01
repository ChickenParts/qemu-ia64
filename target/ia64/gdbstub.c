/*
 * IA-64 GDB server stub
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "gdbstub/helpers.h"

/*
 * IA-64 GDB register layout (extended):
 *   0-127:     General registers (r0-r127)
 *   128-135:   Branch registers (b0-b7)
 *   136:       IP (instruction pointer)
 *   137:       CFM (current frame marker)
 *   138:       PSR (processor status register)
 *   139:       PR (predicate registers)
 *   140-267:   Floating-point registers (f0-f127) - 128-bit each
 *   268-395:   Application registers (ar0-ar127)
 *   396-523:   Control registers (cr0-cr127)
 *   524:       NaT collection (64-bit bitmap for r0-r63)
 *
 * Total: 525 registers
 *
 * Note: FP registers are stored as 128-bit values (16 bytes each).
 * The IA-64 82-bit format is extended to 128 bits for transfer.
 */

#define GDB_REG_GR_BASE    0
#define GDB_REG_BR_BASE    128
#define GDB_REG_IP         136
#define GDB_REG_CFM        137
#define GDB_REG_PSR        138
#define GDB_REG_PR         139
#define GDB_REG_FR_BASE    140
#define GDB_REG_AR_BASE    268
#define GDB_REG_CR_BASE    396
#define GDB_REG_NAT        524
#define GDB_NUM_REGS       525

int ia64_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n)
{
    CPUIA64State *env = cpu_env(cs);
    uint64_t val;

    if (n >= GDB_REG_GR_BASE && n <= 127) {
        /* General registers r0-r127 */
        val = env->r[n];
        return gdb_get_regl(mem_buf, val);
    } else if (n >= GDB_REG_BR_BASE && n <= 135) {
        /* Branch registers b0-b7 */
        val = env->b[n - GDB_REG_BR_BASE];
        return gdb_get_regl(mem_buf, val);
    } else if (n == GDB_REG_IP) {
        /* IP (instruction pointer) */
        val = env->ip;
        return gdb_get_regl(mem_buf, val);
    } else if (n == GDB_REG_CFM) {
        /* CFM (current frame marker) */
        val = env->cfm;
        return gdb_get_regl(mem_buf, val);
    } else if (n == GDB_REG_PSR) {
        /* PSR (processor status register) */
        val = env->psr;
        return gdb_get_regl(mem_buf, val);
    } else if (n == GDB_REG_PR) {
        /* PR (predicate registers) */
        val = env->pr;
        return gdb_get_regl(mem_buf, val);
    } else if (n >= GDB_REG_FR_BASE && n < GDB_REG_AR_BASE) {
        /* Floating-point registers f0-f127 (128-bit each) */
        int fr = n - GDB_REG_FR_BASE;
        uint64_t lo = env->f[fr][0];
        uint64_t hi = env->f[fr][1];
        gdb_get_regl(mem_buf, lo);
        gdb_get_regl(mem_buf, hi);
        return 16;
    } else if (n >= GDB_REG_AR_BASE && n < GDB_REG_CR_BASE) {
        /* Application registers ar0-ar127 */
        int ar = n - GDB_REG_AR_BASE;
        val = env->ar[ar];
        return gdb_get_regl(mem_buf, val);
    } else if (n >= GDB_REG_CR_BASE && n < GDB_REG_NAT) {
        /* Control registers cr0-cr127 */
        int cr = n - GDB_REG_CR_BASE;
        val = env->cr[cr];
        return gdb_get_regl(mem_buf, val);
    } else if (n == GDB_REG_NAT) {
        /* NaT collection bitmap for r0-r63 */
        val = 0;
        for (int i = 0; i < 64; i++) {
            if (env->nat[i]) {
                val |= (1ULL << i);
            }
        }
        return gdb_get_regl(mem_buf, val);
    }

    return 0;
}

int ia64_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n)
{
    CPUIA64State *env = cpu_env(cs);
    uint64_t val = ldq_le_p(mem_buf);

    if (n == 0) {
        /* r0 is always zero, ignore writes */
        return 8;
    } else if (n >= 1 && n <= 127) {
        /* General registers r1-r127 */
        env->r[n] = val;
        return 8;
    } else if (n >= GDB_REG_BR_BASE && n <= 135) {
        /* Branch registers b0-b7 */
        env->b[n - GDB_REG_BR_BASE] = val;
        return 8;
    } else if (n == GDB_REG_IP) {
        /* IP (instruction pointer) */
        env->ip = val;
        return 8;
    } else if (n == GDB_REG_CFM) {
        /* CFM (current frame marker) */
        env->cfm = val;
        return 8;
    } else if (n == GDB_REG_PSR) {
        /* PSR (processor status register) */
        env->psr = val;
        return 8;
    } else if (n == GDB_REG_PR) {
        /* PR (predicate registers) */
        env->pr = val;
        return 8;
    } else if (n >= GDB_REG_FR_BASE && n < GDB_REG_AR_BASE) {
        /* Floating-point registers f0-f127 (128-bit each) */
        int fr = n - GDB_REG_FR_BASE;
        if (fr == 0) {
            /* f0 is always 0.0, ignore writes */
            return 16;
        } else if (fr == 1) {
            /* f1 is always 1.0, ignore writes */
            return 16;
        }
        env->f[fr][0] = ldq_le_p(mem_buf);
        env->f[fr][1] = ldq_le_p(mem_buf + 8);
        return 16;
    } else if (n >= GDB_REG_AR_BASE && n < GDB_REG_CR_BASE) {
        /* Application registers ar0-ar127 */
        int ar = n - GDB_REG_AR_BASE;
        env->ar[ar] = val;
        return 8;
    } else if (n >= GDB_REG_CR_BASE && n < GDB_REG_NAT) {
        /* Control registers cr0-cr127 */
        int cr = n - GDB_REG_CR_BASE;
        env->cr[cr] = val;
        return 8;
    } else if (n == GDB_REG_NAT) {
        /* NaT collection bitmap for r0-r63 */
        for (int i = 0; i < 64; i++) {
            env->nat[i] = (val >> i) & 1;
        }
        return 8;
    }

    return 0;
}
