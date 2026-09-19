#!/usr/bin/env python3
"""Static invariants for the coordinated IA-64 Rooster bring-up branch."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    source = ROOT / path
    if not source.is_file():
        raise AssertionError(f"required branch file is missing: {path}")
    return source.read_text(errors="replace")


def require(text: str, needle: str, owner: str) -> None:
    if needle not in text:
        raise AssertionError(f"{owner}: missing required contract text: {needle!r}")


def forbid(text: str, pattern: str, owner: str) -> None:
    if re.search(pattern, text, re.I | re.M):
        raise AssertionError(f"{owner}: forbidden dependency matched: {pattern!r}")


def main() -> int:
    documentation = read("docs/ia64-rooster-efi.md")
    harness = read("scripts/run-ia64-efi-app.sh")
    matrix = read("scripts/run-ia64-rooster-firmware-matrix.py")
    frontier = read("docs/ia64-hob-migration-frontier.md")
    causality = read("hw/ia64/hob-migration-causality.c")
    causality_workflow = read(".github/workflows/ia64-fv-hob-causality.yml")
    helper_h = read("target/ia64/helper.h")
    helper_c = read("target/ia64/helper.c")
    rse = read("target/ia64/rse.c")
    rse_test = read("tests/unit/test-ia64-rse.c")
    translate = read("target/ia64/translate.c")
    environment = read("docs/ia64-environment-variables.md")

    require(documentation, "EFI/BOOT/BOOTIA64.EFI", "EFI contract")
    require(documentation, "not part of the Rooster boot contract", "EFI contract")
    require(harness, "-kernel is deliberately unsupported", "EFI harness")
    require(harness, "EFI/BOOT/BOOTIA64.EFI", "EFI harness")
    require(matrix, "IA64_CALL_NULL_FIX", "firmware matrix")
    require(matrix, '"0"', "firmware matrix")
    require(frontier, "permanent HOB list", "HOB frontier")
    require(frontier, "EFI_HOB_TYPE_FV", "HOB frontier")
    require(causality, "QEMU_IA64_PEI_FV_HOB_RESTORE", "causality probe")
    require(causality_workflow, 'IA64_CALL_NULL_FIX: "0"', "causality workflow")
    require(helper_h, "DEF_HELPER_5(dbg_gp_write", "GP provenance helper")
    require(helper_c, 'getenv("QEMU_IA64_TRACE_GP_ZERO_ABORT")',
            "GP provenance helper")
    require(helper_c, '"gp_write pc=%016"', "GP provenance helper")
    require(translate, 'getenv("QEMU_IA64_TRACE_GP_WRITES")',
            "GP provenance translator")
    require(translate, "tcg_gen_mov_i64(old_gp, cpu_r[1])",
            "GP provenance translator")
    require(translate, "gen_helper_dbg_gp_write", "GP provenance translator")
    require(environment, "QEMU_IA64_TRACE_GP_WRITES_MIN_PC",
            "GP provenance documentation")
    require(environment, "QEMU_IA64_TRACE_GP_ZERO_ABORT",
            "GP provenance documentation")
    require(helper_c, "struct IA64RSEReturnFrameView view",
            "architectural return reconciliation")
    require(helper_c, "ia64_rse_find_return_frame(&view, b0, pfs_cfm)",
            "architectural return reconciliation")
    require(rse, "view->cfm_offset", "architectural return frame view")
    require(rse, "view->ret_addr_offset", "architectural return frame view")
    require(rse, "IA64_RSE_PFM_MASK",
            "architectural return PFM width")
    require(rse, "UINT64_C(1) << 38",
            "architectural return PFM width")
    forbid(rse, r"<<\s*46", "architectural return PFM width")
    forbid(rse, r'#include\s+"cpu\.h"',
           "target-independent return frame selector")
    require(rse, "The return address is the strongest identity",
            "architectural return reconciliation")
    require(rse_test, "/ia64/rse/nonlocal-return-address-wins",
            "architectural return reconciliation test")
    require(rse_test, "/ia64/rse/pfs-non-pfm-bits-ignored",
            "architectural PFS masking test")
    require(environment, "br.ret` always reconciles",
            "architectural return reconciliation documentation")
    forbid(helper_c, r'getenv\("QEMU_IA64_RET_UNWIND_PFS"\)',
           "architectural return reconciliation")
    forbid(environment, r"QEMU_IA64_RET_UNWIND_PFS",
           "architectural return reconciliation documentation")
    forbid(rse, r"0x1ff[0-9a-f]{5,}",
           "architectural return reconciliation")

    # The causality experiment may restore records that already exist in guest
    # memory, but it may never manufacture a DXE target or key off a firmware
    # instruction address.
    forbid(causality, r"CALL_NULL|call.null|DXE_CORE_(?:IP|GP|TARGET)",
           "causality probe")
    forbid(causality, r"0x1ff[0-9a-f]{5,}", "causality probe")
    forbid(causality, r"cpu_set_pc|env->ip|br\.call", "causality probe")

    # The normal build must not include the semantic repair experiment.  Its
    # dedicated workflow inserts the source transiently for an A/B run.
    meson = read("hw/ia64/meson.build")
    forbid(meson, r"hob-migration-causality\.c", "normal IA-64 build")

    # Firmware and payload identity belong in test evidence, not as hidden
    # QEMU behavior selected by a filename or byte signature.  The harness
    # must recognize and reject -kernel, so inspect only its execution stanza.
    execution = harness[harness.index("exec scripts/run-ia64-firmware.sh"):]
    forbid(execution, r"(?:^|\s)-kernel(?:\s|=)",
           "EFI harness execution")
    forbid(matrix, r"CALL_NULL_FIX[\"']?\s*[:=]\s*[\"']?1",
           "firmware matrix")

    print("IA-64 Rooster EFI branch invariants: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"check-ia64-rooster-branch.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
