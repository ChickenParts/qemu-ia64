#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""IA-64 B9 break.b real-IVT tests and nop.b / hint.b controls.

Proves that the all-zero B-slot is break.b 0 (not nop.b), that
B-slot fault EI and CR.IIM are architectural, and that false predicates
suppress the break fault. Uses the same real qemu-system-ia64 IVT
harness as the F-unit exception regressions.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py"
)
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT_OFFSET = 0x400000
BREAK_VECTOR = 0x2c00
PASS, FAIL = "0x62397070", "0x62397066"
EI_MASK = 3 << 41

PROFILES = {
    "break-zero-slot2": (0, 2),
    "break-middle-slot1": (0x12345, 1),
    "break-high-bit-slot0": (0x1abcde, 0),
    "false-break": (None, -1),
    "nop-hint": (None, -1),
}


def b9_bundle(lines: list[str], instruction: str, slot: int) -> None:
    if slot == 2:
        emit(lines, "b", instruction)
    elif slot == 1:
        lines.extend(["{ .mbb", "nop.m 0", instruction, "nop.b 0", ";;", "}"])
    elif slot == 0:
        lines.extend(["{ .bbb", instruction, "nop.b 0", "nop.b 0", ";;", "}"])
    else:
        raise ValueError(f"invalid B9 bundle slot {slot}")


def verify_bare_exception(lines: list[str], immediate: int, slot: int) -> None:
    compare(lines, 12, hex(BREAK_VECTOR))
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex(slot << 41))
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, "fault_bundle")
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(EI_MASK))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex(slot << 41))
    emit(lines, "m", "mov r9=cr.iim")
    compare(lines, 9, hex(immediate))
    # Break is a fault, never a completion trap: skip the original
    # bundle and return to the next bundle with RI cleared.
    literal(lines, 14, "after_fault")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(EI_MASK))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")


def generate(case: str) -> str:
    if case not in PROFILES:
        raise ValueError(f"unknown B9 test profile {case}")
    immediate, slot = PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)
    literal(lines, 14, "handler")
    emit(lines, "i", "mov b6=r14")
    # PSR.mfl/mfh are neither side effects of B9 hints nor break.
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")

    if immediate is not None:
        lines.append("fault_bundle:")
        b9_bundle(lines, f"break.b {immediate:#x}", slot)
        lines.append("after_fault:")
        compare(lines, 15, 1)
    elif case == "false-break":
        emit(lines, "i", "cmp.eq p4,p5=r0,r0")
        b9_bundle(lines, "(p5) break.b 0x55", 2)
        compare(lines, 15, 0)
    else:
        for instr in ("nop.b 0", "nop.b 0x12345", "hint.b 0",
                      "hint.b 0x1abcde"):
            b9_bundle(lines, instr, 2)
        compare(lines, 15, 0)

    emit(lines, "m", "mov r9=psr")
    emit(lines, "i", "and r9=0x30,r9")
    compare(lines, 9, 0)

    lines.append("pass:")
    # The PASS logger itself uses break.m 0; redirect that vector to
    # pass_spin so it cannot re-enter our final handler and emit FAIL.
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)

    lines += [
        f".org {IVT_OFFSET}", "ivt_base:",
        f".org {IVT_OFFSET + BREAK_VECTOR}", "break_vector:",
    ]
    literal(lines, 12, hex(BREAK_VECTOR))
    emit(lines, "b", "br.cond.sptk b6")
    lines.append("handler:")
    if immediate is None:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        verify_bare_exception(lines, immediate, slot)

    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="./build/qemu-system-ia64")
    parser.add_argument("--out", type=Path, default=Path("scratch/ia64-b9-ivt"))
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--assemble-only", action="store_true")
    parser.add_argument("--only", choices=list(PROFILES))
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error("--timeout must be in (0,60]")
    args.out = args.out.resolve()
    # GNU as explicitly disables B-slot hints by default for some Itanium
    # generations. Opt in so this is a real assembler/guest oracle, not
    # manually fabricated opcodes.
    os.environ.setdefault("IA64_AS", "ia64-linux-gnu-as -mhint.b=ok")
    samples = [args.only] if args.only else list(PROFILES)
    result = []
    for case in samples:
        evidence = ivt.run_one(
            case, args, case_spec=(BREAK_VECTOR, PASS, FAIL),
            source=generate(case))
        result.append(evidence)

    summary = {
        "cases": len(result),
        "source": "Intel Itanium Architecture SDM Volume 3 rev 2.3 B9",
        "vector": hex(BREAK_VECTOR),
        "coverage": [
            "all-zero B slot must deliver break.b 0",
            "B-slot 0/1/2 ISR.ei and IPSR.ri",
            "21-bit immediate including most significant bit",
            "real CR.IIP and CR.IIM",
            "false-predicate break suppression",
            "nop.b/hint.b no-op, no false faults",
            "no unrelated PSR floating-register dirty state",
        ],
        "results": result,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    label = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B9 {label} PASS: {len(result)} profiles")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError,
            subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
