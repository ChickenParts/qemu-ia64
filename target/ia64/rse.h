/*
 * IA-64 register stack engine helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_IA64_RSE_H
#define TARGET_IA64_RSE_H

/*
 * Describe the return-identity fields inside an array of shadow frames.
 * Keeping this view independent of CPUIA64State lets common-code unit tests
 * exercise the selector without including target-only cpu.h.
 */
struct IA64RSEReturnFrameView {
    const void *base;
    size_t count;
    size_t stride;
    size_t cfm_offset;
    size_t ret_addr_offset;
};

/*
 * Locate the shadow call frame selected by a br.ret.
 *
 * A non-local return may discard intervening calls only when both the
 * normalized branch target and ar.pfs.pfm identify the same saved frame.
 * Either component alone is ambiguous.  Return the newest exact match, or -1
 * when the caller must retain the ordinary single-pop behavior.
 */
int ia64_rse_find_return_frame(const struct IA64RSEReturnFrameView *view,
                               uint64_t ret_addr, uint64_t pfs_cfm);

#endif /* TARGET_IA64_RSE_H */
