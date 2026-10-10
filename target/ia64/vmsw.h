/*
 * IA-64 B8 vmsw encoding and current no-VM processor policy.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Keep the fixed-field and PAL feature-set decisions independent of QEMU
 * execution internals so host tests exercise the same production policy.
 */
#ifndef TARGET_IA64_VMSW_H
#define TARGET_IA64_VMSW_H

#include <stdbool.h>
#include <stdint.h>

#define IA64_VMSW_X6_CLEAR 0x18U
#define IA64_VMSW_X6_SET   0x19U

#define IA64_PAL_PROC_FEATURE_NO_VM \
    (UINT64_C(1) << 40)
#define IA64_PAL_PROC_FEATURE_ENABLE_VMSW \
    (UINT64_C(1) << 54)

static inline unsigned int ia64_vmsw_x6(uint64_t insn)
{
    return (insn >> 27) & 0x3f;
}

/*
 * vmsw is unpredicated.  Only x6[32:27] distinguishes vmsw.0 and vmsw.1;
 * every other bit in the 41-bit B-slot word, including qp[5:0], is fixed 0.
 */
static inline bool ia64_vmsw_encoding_valid(uint64_t insn)
{
    unsigned int x6 = ia64_vmsw_x6(insn);

    return (x6 == IA64_VMSW_X6_CLEAR || x6 == IA64_VMSW_X6_SET) &&
           (insn & ~(UINT64_C(0x3f) << 27)) == 0;
}

static inline bool ia64_vmsw_sets_vm(uint64_t insn)
{
    return ia64_vmsw_x6(insn) == IA64_VMSW_X6_SET;
}

/*
 * The only currently exposed QEMU IA-64 model has no virtual-machine
 * environment.  PAL feature set 0 therefore exposes the read-only NoVM
 * fact, leaves EnableVmsw clear, and advertises neither field as software
 * controllable.  Unsupported feature-set numbers retain the existing
 * invalid-argument result.
 */
static inline bool ia64_vmsw_no_vm_pal_features(
    uint64_t feature_set,
    uint64_t *implemented,
    uint64_t *current,
    uint64_t *controllable)
{
    *implemented = 0;
    *current = 0;
    *controllable = 0;
    if (feature_set != 0) {
        return false;
    }

    *implemented = IA64_PAL_PROC_FEATURE_NO_VM;
    *current = IA64_PAL_PROC_FEATURE_NO_VM;
    return true;
}

#endif /* TARGET_IA64_VMSW_H */
