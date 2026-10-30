#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-IVT F1 parallel FP completion + PSR.ss priority matrix.

Reference oracles are independent of QEMU's FP result implementation:
  scripts/run-ia64-f1-exception-tests.py (packed low-lane I/FPA)
  scripts/run-ia64-f1-tests.py (exact fpms/fpnma results).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_f1_ss_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT = 0x400000
BREAK, GENERAL, FP_FAULT, FP_TRAP, SINGLE_STEP = (
    0x2c00, 0x5400, 0x5c00, 0x5d00, 0x6000)
IC, SS, RI, MFL = 1 << 13, 1 << 40, 3 << 41, 0x10
I = 0x20
PASS, FAIL = "0x46317370", "0x46317366"
SENTINEL = (0x123456789abcdef0, 0x23456)
ONE = (0x3f8000003f800000, 0x1003e)
ROUND_LO = (0x0000000033c00000, 0x1003e)
ROUND_HI = (0x33c0000000000000, 0x1003e)
ROUND_BOTH = (0x33c0000033c00000, 0x1003e)
EXACT_A = (0x3f80000040000000, 0x1003e)
EXACT_B = (0x4040000040800000, 0x1003e)
EXACT_C = (0x40a0000040c00000, 0x1003e)

# class, PSR.ss, F1 arithmetic opcode
PROFILES = {
    "fpma-low-ss": ("low", True, "fpma.s0"),
    "fpma-low-no-ss": ("low", False, "fpma.s0"),
    "fpma-high-ss": ("high", True, "fpma.s0"),
    "fpma-both-ss": ("both", True, "fpma.s0"),
    "fpma-masked-ss": ("masked", True, "fpma.s0"),
    "fpma-invalid-high-ss": ("invalid", True, "fpma.s0"),
    "fpms-exact-ss": ("exact-ms", True, "fpms.s1"),
    "fpnma-exact-ss": ("exact-nma", True, "fpnma.s2"),
    "fpma-false-qp-ss": ("false", True, "fpma.s0"),
}


def oracle(kind: str, ss: bool):
    """Vector, full ISR.code (without source EI), output, FPSR, dirty."""
    if kind in ("low", "high", "both"):
        ieee = (1 | ((1 << 9) | (1 << 10) if kind != "high" else 0) |
                ((1 << 13) | (1 << 14) if kind != "low" else 0))
        bits = ((0x3f800001 if kind != "low" else 0x3f800000) << 32 |
                (0x3f800001 if kind != "high" else 0x3f800000), 0x1003e)
        return (FP_TRAP, ieee | (8 if ss else 0),
                bits, 0x1f | (I << 13), MFL)
    if kind == "invalid":
        return (FP_FAULT, 1, SENTINEL, 0x3e, 0)
    if kind == "masked":
        return (SINGLE_STEP, 8, (0x3f8000003f800001, 0x1003e),
                0x3f | (I << 13), MFL)
    if kind == "exact-ms":
        return (SINGLE_STEP, 8, (0xc000000040000000, 0x1003e),
                0x3f, MFL)
    if kind == "exact-nma":
        return (SINGLE_STEP, 8, (0x40000000c0000000, 0x1003e),
                0x3f, MFL)
    if kind == "false":
        return (SINGLE_STEP, 8, SENTINEL, 0x1f, 0)
    raise ValueError(kind)


def fill(lines: list[str], data: list[str], reg: int,
         value: tuple[int, int], name: str) -> None:
    data.extend([".align 16", name + ":",
                 f".quad {value[0]:#x}, {value[1]:#x}"])
    literal(lines, 14, name)
    emit(lines, "m", f"ldf.fill f{reg}=[r14]")


def spill_check(lines: list[str], expected: tuple[int, int]) -> None:
    literal(lines, 14, "output")
    emit(lines, "m", "stf.spill [r14]=f6")
    emit(lines, "m", "ld8 r9=[r14],8")
    emit(lines, "m", "ld8 r10=[r14]")
    compare(lines, 9, hex(expected[0]))
    compare(lines, 10, hex(expected[1]))


def fpsr_check(lines: list[str], expected: int) -> None:
    emit(lines, "m", "mov r9=ar.fpsr")
    compare(lines, 9, hex(expected))


def source_values(kind: str):
    if kind == "invalid":
        return ((0x000000003f800000, 0x1003e),
                (0x7f8000003f800000, 0x1003e), ONE)
    if kind in ("exact-ms", "exact-nma"):
        return EXACT_A, EXACT_B, EXACT_C
    return ONE, ONE, (
        ROUND_HI if kind == "high" else
        ROUND_BOTH if kind == "both" else ROUND_LO)


def generate(case: str) -> str:
    kind, ss, opcode = PROFILES[case]
    vector, isr_code, result, fpsr_result, dirty = oracle(kind, ss)
    initial_fpsr = (0x3e if kind == "invalid" else
                    0x3f if kind in ("masked", "exact-ms", "exact-nma") else
                    0x1f)
    f7, f8, f9 = source_values(kind)
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    data: list[str] = []

    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")
    fill(lines, data, 6, SENTINEL, "dst")
    fill(lines, data, 7, f7, "addend")
    fill(lines, data, 8, f8, "left")
    fill(lines, data, 9, f9, "right")
    literal(lines, 20, hex(initial_fpsr))
    emit(lines, "m", "mov ar.fpsr=r20")
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    if kind == "false":
        emit(lines, "i", "cmp.ne p5,p4=r0,r0")  # p5 false.

    # Enter at the F instruction in slot2 via IPSR, leaving live PSR.ic=0.
    # Neither mov psr.l nor 24-bit ssm can enable PSR.ss (bit40).
    literal(lines, 14, "source_bundle")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex(IC | (SS if ss else 0) | (2 << 41)))
    emit(lines, "i", "or r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

    lines += ["source_bundle:", "{ .mmf", "nop.m 0", "nop.m 0",
              ("(p5) " if kind == "false" else "") +
              f"{opcode} f6=f7,f8,f9", ";;", "}", "after_source:"]

    compare(lines, 15, 1)
    spill_check(lines, result)
    fpsr_check(lines, fpsr_result)
    emit(lines, "m", "mov r9=psr")
    emit(lines, "i", "and r9=0x30,r9")
    compare(lines, 9, hex(dirty))
    emit(lines, "b", "br.cond.sptk pass")

    lines.append("pass:")
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)

    lines += [f".org {IVT}", "ivt_base:",
              f".org {IVT + BREAK}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT + GENERAL}", "general_vector:"]
    emit(lines, "b", "br.cond.sptk fail")
    for vec, name in ((FP_FAULT, "fp_fault_vector"),
                      (FP_TRAP, "fp_trap_vector"),
                      (SINGLE_STEP, "single_step_vector")):
        lines += [f".org {IVT + vec}", name + ":"]
        emit(lines, "b", "br.cond.sptk checks" if vec == vector
             else "br.cond.sptk fail")

    lines += [f".org {IVT + 0x9000}", "checks:"]
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex((2 << 41) | isr_code))
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, "source_bundle" if kind == "invalid" else
            "after_source")
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(IC | SS | RI | MFL))
    emit(lines, "i", "and r10=r9,r14")
    compare(lines, 10, hex(IC | (SS if ss else 0) |
                           ((2 << 41) if kind == "invalid" else 0) | dirty))
    if kind != "invalid":
        emit(lines, "m", "mov r9=cr.iipa")
        literal(lines, 14, "source_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
    spill_check(lines, result)
    fpsr_check(lines, fpsr_result)
    if kind == "invalid":
        literal(lines, 14, "after_source")
        emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(SS | (RI if kind == "invalid" else 0)))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

    lines += [".data", ".align 16", "output:", ".quad 0,0", *data]
    return "\n".join(lines) + "\n"


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="./build/qemu-system-ia64")
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-f1-ss"))
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("--assemble-only", action="store_true")
    p.add_argument("--only", choices=list(PROFILES))
    a = p.parse_args()
    if not 0 < a.timeout <= 60:
        p.error("--timeout must be in (0,60]")
    a.out = a.out.resolve()
    chosen = [a.only] if a.only else list(PROFILES)
    results, failures = [], []
    for case in chosen:
        try:
            results.append(ivt.run_one(case, a, case_spec=(
                oracle(PROFILES[case][0], PROFILES[case][1])[0], PASS, FAIL),
                source=generate(case)))
        except RuntimeError as e:
            failures.append((case, str(e)))
            result_file = a.out / case / "result.json"
            if result_file.exists():
                results.append(json.loads(result_file.read_text()))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "summary.json").write_text(json.dumps({
        "reference": "Intel SDM rev2.3 Vol2 Table8-3, Vol3 F1",
        "cases": len(chosen), "results": results,
        "failures": failures
    }, indent=2) + "\n")
    n = sum(v.get("execution") == ("not-run" if a.assemble_only
                                           else "pass") for v in results)
    print(f"IA64 F1+SS {'ASSEMBLY' if a.assemble_only else 'EXECUTION'}: "
          f"{n}/{len(chosen)} profiles passed")
    if failures:
        raise RuntimeError("; ".join(f"{case}: {err}" for case, err in failures))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
