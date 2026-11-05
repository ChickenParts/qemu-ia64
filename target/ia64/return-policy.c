/*
 * IA-64 system-emulation return policy
 *
 * Copyright (c) 2026 ChickenParts contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The current IA-64 Register Stack Engine implementation keeps private
 * snapshots for br.call so that stacked-register windows can be restored on
 * br.ret.  Firmware call gates and hand-written assembly do not always leave
 * those snapshots perfectly nested.  Blindly restoring the most recent
 * snapshot can therefore return to the right address with the wrong caller
 * window.
 *
 * helper_ret_restore() already has a b0/ar.pfs-correlated unwind path for
 * discarding stale snapshots before restoring the matching caller.  Enable
 * that correctness path by default for full-system emulation.
 *
 * Set QEMU_IA64_LEGACY_BLIND_RET_POP=1 to restore the historical behavior
 * during regression bisection.  An explicit QEMU_IA64_RET_UNWIND_PFS setting
 * is always preserved.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"

static bool ia64_env_true(const char *name)
{
    const char *value = g_getenv(name);

    return value && *value &&
           g_ascii_strcasecmp(value, "0") != 0 &&
           g_ascii_strcasecmp(value, "false") != 0 &&
           g_ascii_strcasecmp(value, "no") != 0 &&
           g_ascii_strcasecmp(value, "off") != 0;
}

static void ia64_return_policy_init(void)
{
    if (!g_getenv("QEMU_IA64_RET_UNWIND_PFS") &&
        !ia64_env_true("QEMU_IA64_LEGACY_BLIND_RET_POP")) {
        g_setenv("QEMU_IA64_RET_UNWIND_PFS", "1", false);
    }
}

type_init(ia64_return_policy_init);
