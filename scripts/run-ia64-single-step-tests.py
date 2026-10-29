#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""GNU-assembled IA64 IVT tests: single-step/branch priority and RFI restart."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_ss_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare
IVT, LPV, TBV, SSV = 0x400000, 0x5e00, 0x5f00, 0x6000
BREAK, GENERAL = 0x2c00, 0x5400
SS, TB, IC, RI_MASK = 1 << 40, 1 << 26, 1 << 13, 3 << 41
PASS, FAIL = "0x73737061", "0x73736661"

# kind, source slot (L+X uses architectural slot1), PSR.ss, PSR.tb.
PROFILES = {
    "alu-m0": ("alu", 0, True, False),
    "alu-i1": ("alu", 1, True, False),
    "alu-i2": ("alu", 2, True, False),
    "nop-m0": ("nopm", 0, True, False),
    "nop-f2": ("nopf", 2, True, False),
    "mlx-movl": ("mlx", 1, True, False),
    "false-qp-i1": ("false", 1, True, False),
    "br-taken-ss": ("taken", 2, True, False),
    "br-taken-tb-ss": ("taken", 2, True, True),
    "br-not-taken-ss": ("untaken", 2, True, False),
    "br-not-taken-tb-ss": ("untaken", 2, True, True),
    "ss-disabled": ("alu", 2, False, False),
}

def setup(lines: list[str], kind: str, slot: int, ss: bool, tb: bool) -> None:
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)  # counter advanced only by the expected vector
    literal(lines, 11, 0)  # observable source state, nonbanked GR
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")
    if kind in ("false", "untaken"):
        emit(lines, "i", "cmp.ne p6,p7=r0,r0")  # p6=false
    # PSR.ss is bit40: set it in IPSR, then enter the test using RFI.
    literal(lines, 14, "source_bundle")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex((SS if ss else 0) | (TB if tb else 0) | IC))
    emit(lines, "i", "or r9=r9,r14")
    literal(lines, 14, hex(RI_MASK))
    emit(lines, "i", "andcm r9=r9,r14")
    # Enter *at* the source instruction, not at earlier filler NOPs.
    # PSR.ss would correctly trap on those NOPs before the requested slot.
    if slot:
        literal(lines, 14, hex(slot << 41))
        emit(lines, "i", "or r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

def source_bundle(lines: list[str], kind: str, slot: int) -> None:
    lines.append("source_bundle:")
    if kind == "mlx":
        lines.extend(["{ .mlx", "nop.m 0", "movl r11=0x1234", ";;", "}"])
        return
    if kind == "nopf":
        lines.extend(["{ .mmf", "nop.m 0", "nop.m 0", "nop.f 0", ";;", "}"])
        return
    if kind in ("taken", "untaken"):
        insn = ("br.cond.sptk target_bundle" if kind == "taken"
                else "(p6) br.cond.sptk target_bundle")
        lines.extend(["{ .mib", "nop.m 0", "nop.i 0", insn, ";;", "}"])
        return
    if kind == "nopm":
        lines.extend(["{ .mii", "nop.m 0", "nop.i 0", "nop.i 0", ";;", "}"])
        return
    insn = "(p6) adds r11=1,r0" if kind == "false" else "adds r11=1,r0"
    words = ["nop.m 0", "nop.i 0", "nop.i 0"]
    words[slot] = insn
    lines.extend(["{ .mii", *words, ";;", "}"])

def generate(case: str) -> str:
    kind, slot, ss, tb = PROFILES[case]
    taken = kind == "taken"
    expect_trap = ss or (tb and taken)
    expected_vec = (TBV if taken and tb else SSV) if expect_trap else None
    next_ri = 0 if taken or slot == 2 or kind == "mlx" else slot + 1
    next_label = ("target_bundle" if taken else
                  "post_source" if next_ri == 0 else "source_bundle")
    expected_code = (4 | (8 if ss else 0)) if expected_vec == TBV else 8
    state = (0x1234 if kind == "mlx" else
             0 if kind in ("false", "untaken", "taken", "nopm", "nopf") else 1)
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, kind, slot, ss, tb)
    source_bundle(lines, kind, slot)
    lines.append("post_source:")
    if taken:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        compare(lines, 15, 1 if expect_trap else 0)
        compare(lines, 11, state)
        emit(lines, "b", "br.cond.sptk pass")
    lines.append("target_bundle:")
    if taken:
        compare(lines, 15, 1 if expect_trap else 0)
        compare(lines, 11, state)
        emit(lines, "b", "br.cond.sptk pass")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    lines.append("pass:")
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [f".org {IVT}", "ivt_base:",
              f".org {IVT + BREAK}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT + GENERAL}", "general_vector:"]
    emit(lines, "b", "br.cond.sptk fail")
    for vec, label in ((LPV, "lp_vector"), (TBV, "tb_vector"),
                       (SSV, "ss_vector")):
        lines += [f".org {IVT + vec}", label + ":"]
        insn = "br.cond.sptk trap_checks" if vec == expected_vec else "br.cond.sptk fail"
        emit(lines, "b", insn)
    lines += [f".org {IVT + 0x9000}", "trap_checks:"]
    if expect_trap:
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        emit(lines, "m", "mov r9=cr.isr")
        compare(lines, 9, hex((slot << 41) | expected_code))
        emit(lines, "m", "mov r9=cr.iipa")
        literal(lines, 14, "source_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.iip")
        literal(lines, 14, next_label)
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.ipsr")
        literal(lines, 14, hex(RI_MASK | SS | TB | IC))
        emit(lines, "i", "and r10=r9,r14")
        compare(lines, 10, hex((next_ri << 41) | (SS if ss else 0) |
                               (TB if tb else 0) | IC))
        compare(lines, 11, state)
        # The resumed instruction executes once with SS/TB cleared.
        literal(lines, 14, hex(SS | TB))
        emit(lines, "i", "andcm r9=r9,r14")
        emit(lines, "m", "mov cr.ipsr=r9")
        emit(lines, "b", "rfi")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    return "\n".join(lines) + "\n"

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="./build/qemu-system-ia64")
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-single-step"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--assemble-only", action="store_true")
    ap.add_argument("--only", choices=list(PROFILES))
    args = ap.parse_args()
    if not 0 < args.timeout <= 60:
        ap.error("--timeout must be in (0,60]")
    args.out = args.out.resolve()
    chosen = [args.only] if args.only else list(PROFILES)
    # Run every profile even after the first failure. The shared harness
    # writes each result.json before raising, so preserve all guest logs in
    # the Actions artifact and make one CI cycle diagnose every mismatch.
    results = []
    failures = []
    for c in chosen:
        try:
            results.append(ivt.run_one(c, args, case_spec=(SSV, PASS, FAIL),
                                       source=generate(c)))
        except RuntimeError as exc:
            failures.append((c, str(exc)))
            result_file = args.out / c / "result.json"
            if result_file.exists():
                results.append(json.loads(result_file.read_text()))
    if failures:
        print("IA64 SINGLE-STEP FAILURES: " + ", ".join(
            name for name, _ in failures), file=sys.stderr)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps({
        "reference": "Intel SDM rev2.3 Vol2 5.5.3 and section 8",
        "scope": "PSR.ss sequential/MLX/taken/untaken and TB+SS precedence",
        "cases": len(chosen), "results": results,
    }, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    passed = sum(r.get("execution") == ("not-run" if args.assemble_only
                                              else "pass") for r in results)
    print(f"IA64 SINGLE-STEP {mode}: {passed}/{len(chosen)} profiles passed")
    if failures:
        raise RuntimeError("; ".join(f"{case}: {error}"
                                     for case, error in failures))

if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
