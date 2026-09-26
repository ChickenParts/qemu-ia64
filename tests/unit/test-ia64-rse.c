/*
 * IA-64 register stack return-frame tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "target/ia64/rse.h"

struct IA64RSEFrame {
    uint64_t padding;
    uint64_t cfm;
    uint64_t ret_addr;
    uint64_t bsp;
};

typedef struct TestIA64RSEState {
    struct IA64RSEFrame *rse_frames;
    size_t rse_depth;
} TestIA64RSEState;

static void init_frames(TestIA64RSEState *env,
                        struct IA64RSEFrame *frames,
                        size_t count)
{
    memset(env, 0, sizeof(*env));
    memset(frames, 0, sizeof(*frames) * count);
    env->rse_frames = frames;
    env->rse_depth = count;
}

static int find_return_frame(const TestIA64RSEState *env,
                             uint64_t ret_addr, uint64_t pfs_cfm)
{
    const struct IA64RSEReturnFrameView view = {
        .base = env->rse_frames,
        .count = env->rse_depth,
        .stride = sizeof(*env->rse_frames),
        .cfm_offset = offsetof(struct IA64RSEFrame, cfm),
        .ret_addr_offset = offsetof(struct IA64RSEFrame, ret_addr),
        .bsp_offset = offsetof(struct IA64RSEFrame, bsp),
    };

    return ia64_rse_find_return_frame(&view, ret_addr, pfs_cfm);
}

static int find_stack_switch_boundary(const TestIA64RSEState *env,
                                      uint64_t bsp, uint64_t pfs_cfm)
{
    const struct IA64RSEReturnFrameView view = {
        .base = env->rse_frames,
        .count = env->rse_depth,
        .stride = sizeof(*env->rse_frames),
        .cfm_offset = offsetof(struct IA64RSEFrame, cfm),
        .ret_addr_offset = offsetof(struct IA64RSEFrame, ret_addr),
        .bsp_offset = offsetof(struct IA64RSEFrame, bsp),
    };

    return ia64_rse_find_stack_switch_boundary(&view, bsp, pfs_cfm);
}

static void test_empty(void)
{
    TestIA64RSEState env = { 0 };

    g_assert_cmpint(find_return_frame(&env, 0x1000, 0x301), ==, -1);
}

static void test_top_exact_match(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[2];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].ret_addr = 0x2000;
    frames[1].cfm = 0x302;

    g_assert_cmpint(find_return_frame(&env, 0x2002, 0x302), ==, 1);
}

static void test_nonlocal_return_address_wins(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[3];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].ret_addr = 0x3000;
    frames[1].cfm = 0x30a;
    frames[2].ret_addr = 0x4000;
    frames[2].cfm = 0x30a;

    /*
     * The newest frame deliberately has the same CFM.  Only the older frame
     * matches both architectural identity components.
     */
    g_assert_cmpint(find_return_frame(&env, 0x3001, 0x30a), ==, 1);
}

static void test_conflicting_components_do_not_match(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[3];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].ret_addr = 0x2000;
    frames[1].cfm = 0x30a;
    frames[2].ret_addr = 0x3000;
    frames[2].cfm = 0x38a;

    /*
     * The return address and PFS each match a frame, but not the same frame.
     * Discarding either frame would turn inconsistent state into corruption.
     */
    g_assert_cmpint(find_return_frame(&env, 0x2001, 0x38a), ==, -1);
}

static void test_return_address_only_does_not_match(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[2];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].ret_addr = 0x2000;
    frames[1].cfm = 0x302;

    g_assert_cmpint(find_return_frame(&env, 0x2002, 0x777), ==, -1);
}

static void test_pfs_only_does_not_match(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[2];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].ret_addr = 0x2000;
    frames[1].cfm = 0x302;

    g_assert_cmpint(find_return_frame(&env, 0x9000, 0x302), ==, -1);
}

static void test_duplicate_exact_match_prefers_newest(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[3];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].ret_addr = 0x2000;
    frames[1].cfm = 0x30a;
    frames[2].ret_addr = 0x2000;
    frames[2].cfm = 0x30a;

    g_assert_cmpint(find_return_frame(&env, 0x2001, 0x30a), ==, 2);
}

static void test_zero_target_does_not_match(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[1];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0;
    frames[0].cfm = 0x201;

    g_assert_cmpint(find_return_frame(&env, 0, 0x201), ==, -1);
}

static void test_no_match(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[2];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].ret_addr = 0x2000;
    frames[1].cfm = 0x302;

    g_assert_cmpint(find_return_frame(&env, 0x9000, 0x777), ==, -1);
}

static void test_pfs_non_pfm_bits_ignored(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[1];
    uint64_t pfs;

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].ret_addr = 0x2000;
    frames[0].cfm = 0x302;

    /*
     * PFS.pfm is bits 37:0.  Reserved bit 40, PEC in bits 57:52, and PPL in
     * bits 63:62 must not take part in frame identity.
     */
    pfs = UINT64_C(0x302) |
          (UINT64_C(1) << 40) |
          (UINT64_C(0x2a) << 52) |
          (UINT64_C(3) << 62);

    g_assert_cmpint(find_return_frame(&env, 0x2002, pfs), ==, 0);
}

static void test_stack_switch_exact_boundary(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[4];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].bsp = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].bsp = 0x1800;
    frames[1].cfm = 0x30a;
    frames[2].bsp = 0x1800;
    frames[2].cfm = 0x30a;
    frames[3].bsp = 0x2000;
    frames[3].cfm = 0x407;

    /* The oldest target-frame call is the unwind boundary. */
    g_assert_cmpint(find_stack_switch_boundary(&env, 0x1803, 0x30a), ==, 1);
}

static void test_stack_switch_ignores_newer_same_pfm(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[4];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].bsp = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].bsp = 0x1800;
    frames[1].cfm = 0x30a;
    frames[2].bsp = 0x1a00;
    frames[2].cfm = 0x30a;
    frames[3].bsp = 0x2000;
    frames[3].cfm = 0x407;

    g_assert_cmpint(find_stack_switch_boundary(&env, 0x1800, 0x30a), ==, 1);
}

static void test_stack_switch_monotonic_fallback(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[4];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].bsp = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].bsp = 0x1700;
    frames[1].cfm = 0x222;
    frames[2].bsp = 0x1900;
    frames[2].cfm = 0x333;
    frames[3].bsp = 0x2000;
    frames[3].cfm = 0x444;

    /* Preserve frames below the restored BSP even without an exact PFM. */
    g_assert_cmpint(find_stack_switch_boundary(&env, 0x1800, 0x777), ==, 2);
}

static void test_stack_switch_all_frames_survive(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[2];

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].bsp = 0x1000;
    frames[0].cfm = 0x201;
    frames[1].bsp = 0x1800;
    frames[1].cfm = 0x302;

    g_assert_cmpint(find_stack_switch_boundary(&env, 0x2000, 0x777), ==, -1);
}

static void test_stack_switch_pfs_non_pfm_bits_ignored(void)
{
    TestIA64RSEState env;
    struct IA64RSEFrame frames[1];
    uint64_t pfs;

    init_frames(&env, frames, G_N_ELEMENTS(frames));
    frames[0].bsp = 0x1800;
    frames[0].cfm = 0x30a;
    pfs = UINT64_C(0x30a) |
          (UINT64_C(0x2a) << 52) |
          (UINT64_C(3) << 62);

    g_assert_cmpint(find_stack_switch_boundary(&env, 0x1800, pfs), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ia64/rse/empty", test_empty);
    g_test_add_func("/ia64/rse/top-exact-match", test_top_exact_match);
    g_test_add_func("/ia64/rse/nonlocal-return-address-wins",
                    test_nonlocal_return_address_wins);
    g_test_add_func("/ia64/rse/conflicting-components",
                    test_conflicting_components_do_not_match);
    g_test_add_func("/ia64/rse/return-address-only",
                    test_return_address_only_does_not_match);
    g_test_add_func("/ia64/rse/pfs-only", test_pfs_only_does_not_match);
    g_test_add_func("/ia64/rse/duplicate-exact-prefers-newest",
                    test_duplicate_exact_match_prefers_newest);
    g_test_add_func("/ia64/rse/zero-target", test_zero_target_does_not_match);
    g_test_add_func("/ia64/rse/no-match", test_no_match);
    g_test_add_func("/ia64/rse/pfs-non-pfm-bits-ignored",
                    test_pfs_non_pfm_bits_ignored);
    g_test_add_func("/ia64/rse/stack-switch-exact-boundary",
                    test_stack_switch_exact_boundary);
    g_test_add_func("/ia64/rse/stack-switch-oldest-exact",
                    test_stack_switch_ignores_newer_same_pfm);
    g_test_add_func("/ia64/rse/stack-switch-monotonic-fallback",
                    test_stack_switch_monotonic_fallback);
    g_test_add_func("/ia64/rse/stack-switch-all-frames-survive",
                    test_stack_switch_all_frames_survive);
    g_test_add_func("/ia64/rse/stack-switch-pfs-mask",
                    test_stack_switch_pfs_non_pfm_bits_ignored);
    return g_test_run();
}
