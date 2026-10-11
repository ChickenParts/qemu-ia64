#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free B8 vmsw real-IVT validation for the no-VM CPU model."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_ivt_vmsw", ROOT / "scripts/run-ia64-fp-exception-tests.py"
)
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT_OFFSET = 0x400000
BREAK_VECTOR = 0x2c00
GENERAL_VECTOR = 0x5400
PASS, FAIL = "0x766d7370", "0x766d7366"
CPL_SHIFT = 32
VM = 1 << 46
RI_MASK = 3 << 41

# name: set PSR.vm request, starting CPL, B-slot
PROFILES = {
    "clear-cpl0-b0": (False, 0, 0),
    "set-cpl0-b1": (True, 0, 1),
    "clear-cpl3-b2": (False, 3, 2),
    "set-cpl3-b1": (True, 3, 1),
}


def vmsw_bundle(lines: list[str], set_vm: bool, slot: int) -> None:
    insn = "vmsw.1" if set_vm else "vmsw.0"
    if slot == 2:
        emit(lines, "b", insn)
    elif slot == 1:
        lines.extend(["{ .mbb", "nop.m 0", insn, "nop.b 0", ";;", "}"])
    elif slot == 0:
        lines.extend(["{ .bbb", insn, "nop.b 0", "nop.b 0", ";;", "}"])
    else:
        raise ValueError(f"invalid B slot {slot}")


def generate(case: str) -> str:
    if case not in PROFILES:
        raise ValueError(f"unknown vmsw test profile {case}")
    set_vm, cpl, slot = PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]

    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, "fault_handler")
    emit(lines, "i", "mov b6=r14")
    literal(lines, 15, 0)

    literal(lines, 14, "fault_bundle")
    emit(lines, "m", "mov cr.iip=r14")
    literal(lines, 20, hex(cpl << CPL_SHIFT))
    emit(lines, "m", "mov cr.ipsr=r20")
    emit(lines, "b", "rfi")
    emit(lines, "b", "br.cond.sptk fail")

    lines.append("fault_bundle:")
    vmsw_bundle(lines, set_vm, slot)
    lines.append("after_vmsw:")

    compare(lines, 15, 1)
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex((3 << CPL_SHIFT) | VM))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex(cpl << CPL_SHIFT))

    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)

    lines += [f".org {IVT_OFFSET}", "ivt_base:",
              f".org {IVT_OFFSET + BREAK_VECTOR}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT_OFFSET + GENERAL_VECTOR}",
              "general_vector:", "fault_handler:"]

    compare(lines, 15, 0)
    literal(lines, 15, 1)

    # Feature absence is first: even CPL3 receives Illegal Operation (code 0),
    # not Privileged Operation (code 1).  ISR.ei identifies the B slot.
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex(slot << 41))
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, "fault_bundle")
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(RI_MASK | (3 << CPL_SHIFT) | VM))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex((slot << 41) | (cpl << CPL_SHIFT)))
    emit(lines, "m", "mov r9=cr.iim")
    compare(lines, 9, 0)

    # Resume after the faulting group without changing saved CPL or PSR.vm.
    literal(lines, 14, "after_vmsw")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(RI_MASK))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

    return "\n".join(lines) + "\n"


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="./build/qemu-system-ia64")
    p.add_argument("--out", type=Path,
                   default=Path("scratch/ia64-b8-vmsw-execution"))
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("--assemble-only", action="store_true")
    p.add_argument("--only", choices=list(PROFILES))
    args = p.parse_args()
    if not 0 < args.timeout <= 60:
        p.error("--timeout must be in (0, 60]")
    args.out = args.out.resolve()
    selected = [args.only] if args.only else list(PROFILES)
    results = []
    for case in selected:
        evidence = ivt.run_one(case, args,
                               case_spec=(GENERAL_VECTOR, PASS, FAIL),
                               source=generate(case))
        results.append(evidence)
    report = {
        "scope": "current no-VM IA-64 CPU model",
        "cases": len(results),
        "coverage": [
            "vmsw.0 and vmsw.1",
            "CPL0 and CPL3 feature-absence precedence",
            "B-slot 0, 1 and 2 ISR.ei/IPSR.ri reporting",
            "real General Exception IVT delivery and RFI recovery",
            "no PSR.vm or CPL state commit",
        ],
        "results": results,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B8 VMSW {mode} PASS: {len(results)}/{len(PROFILES)} profiles")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
