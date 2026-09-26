/*
 * IA-64 register stack engine helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "rse.h"

/* CFM and PFS.pfm share the architectural frame layout in bits 37:0. */
#define IA64_RSE_PFM_MASK ((UINT64_C(1) << 38) - 1)

static bool ia64_rse_view_base_valid(const struct IA64RSEReturnFrameView *view)
{
    if (!view || !view->base || view->count == 0 ||
        view->count > INT_MAX || view->stride < sizeof(uint64_t) ||
        view->count > SIZE_MAX / view->stride) {
        return false;
    }

    return true;
}

static bool ia64_rse_return_view_valid(
    const struct IA64RSEReturnFrameView *view)
{
    if (!ia64_rse_view_base_valid(view)) {
        return false;
    }

    return view->cfm_offset <= view->stride - sizeof(uint64_t) &&
           view->ret_addr_offset <= view->stride - sizeof(uint64_t);
}


static bool ia64_rse_stack_switch_view_valid(
    const struct IA64RSEReturnFrameView *view)
{
    if (!ia64_rse_view_base_valid(view)) {
        return false;
    }

    return view->cfm_offset <= view->stride - sizeof(uint64_t) &&
           view->bsp_offset <= view->stride - sizeof(uint64_t);
}

int ia64_rse_find_return_frame(const struct IA64RSEReturnFrameView *view,
                               uint64_t ret_addr, uint64_t pfs_cfm)
{
    const uint8_t *base;
    uint64_t target = ret_addr & ~UINT64_C(0xf);
    uint64_t caller_pfm = pfs_cfm & IA64_RSE_PFM_MASK;
    int index;

    if (!ia64_rse_return_view_valid(view) || target == 0) {
        return -1;
    }
    base = view->base;

    /*
     * The return address is the strongest identity component, but it is not
     * unique: recursive calls can share a return site, and unrelated callers
     * commonly share a CFM shape.  Only the exact architectural pair may
     * justify discarding intervening shadow frames.
     *
     * Search newest-to-oldest so indistinguishable recursive activations keep
     * the ordinary single-pop behavior instead of skipping an inner frame.
     */
    for (index = (int)view->count - 1; index >= 0; index--) {
        const uint8_t *frame = base + (size_t)index * view->stride;
        uint64_t frame_cfm;
        uint64_t frame_ret_addr;

        memcpy(&frame_cfm, frame + view->cfm_offset, sizeof(frame_cfm));
        memcpy(&frame_ret_addr, frame + view->ret_addr_offset,
               sizeof(frame_ret_addr));
        if (frame_ret_addr != 0 &&
            (frame_ret_addr & ~UINT64_C(0xf)) == target &&
            (frame_cfm & IA64_RSE_PFM_MASK) == caller_pfm) {
            return index;
        }
    }

    return -1;
}

int ia64_rse_find_stack_switch_boundary(
    const struct IA64RSEReturnFrameView *view,
    uint64_t bsp, uint64_t pfs_cfm)
{
    const uint8_t *base;
    uint64_t target_bsp = bsp & ~UINT64_C(0x7);
    uint64_t target_pfm = pfs_cfm & IA64_RSE_PFM_MASK;
    int first_at_or_above = -1;

    if (!ia64_rse_stack_switch_view_valid(view) || target_bsp == 0) {
        return -1;
    }
    base = view->base;

    /*
     * Search oldest-to-newest.  The matching frame is the first outstanding
     * call made by the activation restored by the stack switch.  That frame
     * and every newer frame must be discarded, while older outer calls remain
     * valid for the target activation's eventual ordinary return.
     */
    for (int index = 0; index < (int)view->count; index++) {
        const uint8_t *frame = base + (size_t)index * view->stride;
        uint64_t frame_cfm;
        uint64_t frame_bsp;

        memcpy(&frame_cfm, frame + view->cfm_offset, sizeof(frame_cfm));
        memcpy(&frame_bsp, frame + view->bsp_offset, sizeof(frame_bsp));
        frame_bsp &= ~UINT64_C(0x7);

        if (first_at_or_above < 0 && frame_bsp >= target_bsp) {
            first_at_or_above = index;
        }
        if (frame_bsp == target_bsp &&
            (frame_cfm & IA64_RSE_PFM_MASK) == target_pfm) {
            return index;
        }
    }

    return first_at_or_above;
}
