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
    size_t bsp_offset;
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

/*
 * Locate the first active call frame invalidated by an architectural backing
 * store unwind.
 *
 * setjmp/longjmp-style code restores ar.bspstore and ar.pfs before br.ret.
 * The original call frame selected by b0 may already have returned, so it is
 * no longer present in the active shadow stack.  The restored backing-store
 * pointer plus PFS.PFM identify the target activation independently of the
 * stale inner return addresses.
 *
 * Return the oldest exact BSP/PFM match.  If the exact frame is absent, return
 * the oldest frame at or above the restored BSP, which is the conservative
 * boundary between surviving outer calls and invalidated inner calls.  Return
 * -1 when every active frame is below the restored BSP.
 */
int ia64_rse_find_stack_switch_boundary(
    const struct IA64RSEReturnFrameView *view,
    uint64_t bsp, uint64_t pfs_cfm);

/*
 * Recognize only the architectural first half of a non-local restore here.
 * b0 and PFS are intentionally absent: firmware is allowed to restore them
 * after BSPSTORE and before br.ret.
 */
bool ia64_rse_should_arm_stack_switch(uint64_t old_bspstore,
                                      uint64_t new_bspstore,
                                      uint64_t old_bsp,
                                      size_t shadow_depth,
                                      bool lazy_mode,
                                      bool task_switch);

/* Bind the late return identity at br.ret; reject an ordinary top return. */
int ia64_rse_bind_stack_switch_return(
    const struct IA64RSEReturnFrameView *view,
    uint64_t restored_bsp,
    uint64_t current_pfs,
    uint64_t current_b0,
    uint64_t top_ret_addr);

#endif /* TARGET_IA64_RSE_H */
