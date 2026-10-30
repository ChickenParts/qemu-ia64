#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free real IVT tests for FP trap/fault versus PSR.ss priority."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_fp_ss_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT = 0x400000
BREAK, GENERAL, FP_FAULT, FP_TRAP, SINGLE_STEP = (
    0x2c00, 0x5400, 0x5c00, 0x5d00, 0x6000)
IC, SS, RI = 1 << 13, 1 << 40, 3 << 41
PASS, FAIL = "0x66737370", "0x66737366"

# Mode / SS enabled. Source instruction always occupies F slot 2.
PROFILES = {
    "fp-inexact-ss": ("inexact", True),
    "fp-inexact-no-ss": ("inexact", False),
    "fp-masked-ss": ("masked", True),
    "fp-invalid-ss": ("invalid", True),
    "fp-false-qp-ss": ("false", True),
}


def expected_vector(mode: str) -> int:
    return (FP_TRAP if mode == "inexact" else
            FP_FAULT if mode == "invalid" else SINGLE_STEP)


def expected_isr(mode: str, ss: bool) -> int:
    ei = 2 << 41
    if mode == "inexact":
        return ei | 1 | (1 << 13) | (1 << 14) | (8 if ss else 0)
    if mode == "invalid":
        # Fault code is NOT the trap bit vector, so no bit3 for SS.
        return ei | 1
    return ei | 8


def check_fp_state(lines: list[str], mode: str) -> None:
    if mode in ("invalid", "false"):
        sig, exp = "0x1234", "0x23456"
    else:
        sig, exp = "2", "0x1003e"
    emit(lines, "m", "getf.sig r9=f6")
    compare(lines, 9, sig)
    emit(lines, "m", "getf.exp r9=f6")
    compare(lines, 9, exp)
    emit(lines, "m", "mov r9=ar.fpsr")
    fpsr = ("62" if mode == "invalid" else
            "31" if mode == "false" else
            "0x4003f" if mode == "masked" else "0x4001f")
    compare(lines, 9, fpsr)


def generate(case: str) -> str:
    mode, ss = PROFILES[case]
    vec = expected_vector(mode)
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")
    literal(lines, 14, "source")
    emit(lines, "m", "ldf.fill f7=[r14]")
    literal(lines, 14, "sentinel")
    emit(lines, "m", "ldf.fill f6=[r14]")
    literal(lines, 20, "62" if mode == "invalid" else
            "63" if mode == "masked" else "31")
    emit(lines, "m", "mov ar.fpsr=r20")
    emit(lines, "m", "rsm 0x10")
    emit(lines, "m", "srlz.d")
    if mode == "false":
        emit(lines, "i", "cmp.ne p6,p7=r0,r0")

    # RFI enables PSR.ss BIT40 from CR.IPSR, entering at actual F slot2.
    # The live PSR still has PSR.ic clear; no earlier filler NOP is stepped.
    literal(lines, 14, "source_bundle")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex(IC | (SS if ss else 0)))
    emit(lines, "i", "or r9=r9,r14")
    literal(lines, 14, hex(RI))
    emit(lines, "i", "andcm r9=r9,r14")
    literal(lines, 14, hex(2 << 41))
    emit(lines, "i", "or r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

    lines += ["source_bundle:", "{ .mmf", "nop.m 0", "nop.m 0",
              ("(p6) " if mode == "false" else "") + "fcvt.fx.s0 f6=f7",
              ";;", "}", "after_source:"]
    compare(lines, 15, 1)
    check_fp_state(lines, mode)
    emit(lines, "b", "br.cond.sptk pass")

    lines.append("pass:")
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)

    # Each vector is 0x100 apart; use dedicated stubs and a common check
    # outside the IVT instead of allowing the handler to overlap vectors.
    lines += [f".org {IVT}", "ivt_base:",
              f".org {IVT + BREAK}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT + GENERAL}", "general_vector:"]
    emit(lines, "b", "br.cond.sptk fail")
    for offset, label in ((FP_FAULT, "fp_fault_vector"),
                          (FP_TRAP, "fp_trap_vector"),
                          (SINGLE_STEP, "ss_vector")):
        lines += [f".org {IVT + offset}", label + ":"]
        emit(lines, "b", "br.cond.sptk exception_checks" if offset == vec
             else "br.cond.sptk fail")

    lines += [f".org {IVT + 0x9000}", "exception_checks:"]
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex(expected_isr(mode, ss)))
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, "source_bundle" if mode == "invalid" else
            "after_source")
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(IC | SS | RI))
    emit(lines, "i", "and r10=r9,r14")
    compare(lines, 10, hex(IC | (SS if ss else 0) |
                           ((2 << 41) if mode == "invalid" else 0)))
    if mode != "invalid":
        emit(lines, "m", "mov r9=cr.iipa")
        literal(lines, 14, "source_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
    # FP trap and completed SS save committed result/FPSR; FP fault does
    # not modify destination or FPSR (only the faulting source is examined).
    check_fp_state(lines, mode)
    if mode == "invalid":
        literal(lines, 14, "after_source")
        emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(SS | (RI if mode == "invalid" else 0)))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

    lines += [".data", ".align 16", "source:"]
    if mode == "invalid":
        lines += [".quad 0x8000000000000000, 0x1ffff"]
    else:
        lines += [".quad 0xc000000000000000, 0xffff"]
    lines += [".align 16", "sentinel:",
              ".quad 0x1234, 0x23456"]
    return "\n".join(lines) + "\n"


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="./build/qemu-system-ia64")
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-fp-ss-priority"))
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("--assemble-only", action="store_true")
    p.add_argument("--only", choices=list(PROFILES))
    args = p.parse_args()
    if not 0 < args.timeout <= 60:
        p.error("--timeout must be in (0,60]")
    args.out = args.out.resolve()
    cases = [args.only] if args.only else list(PROFILES)
    results, failed = [], []
    for case in cases:
        try:
            results.append(ivt.run_one(
                case, args, case_spec=(expected_vector(PROFILES[case][0]),
                                       PASS, FAIL), source=generate(case)))
        except RuntimeError as exc:
            failed.append((case, str(exc)))
            result_file = args.out / case / "result.json"
            if result_file.exists():
                results.append(json.loads(result_file.read_text()))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps({
        "reference": "Intel Itanium SDM Volume2 rev2.3 Table 8-3, vector 0x5d00",
        "scope": "FP trap+SS vector/ISR priority and pre-result FP fault",
        "results": results, "failed": failed,
    }, indent=2) + "\n")
    passed = sum(r.get("execution") == ("not-run" if args.assemble_only
                                              else "pass") for r in results)
    print(f"IA64 FP+SS {'ASSEMBLY' if args.assemble_only else 'EXECUTION'}: "
          f"{passed}/{len(cases)} profiles passed")
    if failed:
        raise RuntimeError("; ".join(f"{case}: {error}" for case, error in failed))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
