#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free B8 epc real-system privilege/ITLB/IVT validation.

RFI installs a chosen CPL (and optionally IT=1). A real itc.i provides
the execute-only promotion page for IT-enabled profiles. The guest tests
PFS.ppl fault priority, saved interruption state, and post-epc CPL.
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
    "ia64_ivt_epc", ROOT / "scripts/run-ia64-fp-exception-tests.py"
)
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT_OFFSET = 0x400000
BREAK_VECTOR = 0x2c00
ILLEGAL_VECTOR = 0x5400
PASS, FAIL = "0x65706370", "0x65706366"
IT = 1 << 36
CPL_SHIFT = 32
RI_MASK = 3 << 41
TEXT = ivt.TEXT

# name: it, starting cpl, pfs.ppl, instruction ar, pl, resulting cpl,
# expected Illegal Operation fault, source B slot
PROFILES = {
    "phys-promote-b0": (0, 3, 3, 0, 0, 0, False, 0),
    "phys-promote-b1": (0, 3, 3, 0, 0, 0, False, 1),
    "phys-promote-b2": (0, 3, 3, 0, 0, 0, False, 2),
    "phys-illegal-pfs": (0, 3, 0, 0, 0, 3, True, 1),
    "phys-already-cpl0": (0, 0, 0, 0, 0, 0, False, 2),
    "it-ar7-pl0": (1, 3, 3, 7, 0, 0, False, 2),
    "it-ar7-pl2": (1, 3, 3, 7, 2, 2, False, 2),
    "it-ar7-pl3": (1, 3, 3, 7, 3, 3, False, 2),
    "it-ar1-pl3": (1, 3, 3, 1, 3, 3, False, 2),
    "it-illegal-pfs": (1, 3, 0, 7, 0, 3, True, 2),
}


def epc_bundle(lines: list[str], slot: int) -> None:
    if slot == 2:
        emit(lines, "b", "epc")
    elif slot == 1:
        lines.extend(["{ .mbb", "nop.m 0", "epc", "nop.b 0", ";;", "}"])
    elif slot == 0:
        lines.extend(["{ .bbb", "epc", "nop.b 0", "nop.b 0", ";;", "}"])
    else:
        raise ValueError(f"invalid B slot {slot}")


def generate(case: str) -> str:
    if case not in PROFILES:
        raise ValueError(f"unknown epc test profile {case}")
    it, cpl, ppl, ar, pl, expected, fault, slot = PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]

    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, "fault_handler" if fault else "fail")
    emit(lines, "i", "mov b6=r14")
    literal(lines, 15, 0)

    # PFS.ppl is bits [63:62]; test before any elevation. Keeping it
    # independent of the RFI-installed PSR.cpl exposes the illegal path.
    literal(lines, 20, hex(ppl << 62))
    emit(lines, "i", "mov ar.pfs=r20")

    if it:
        # One 16-MiB identity-mapped translation covers both TEXT and
        # the IVT, guaranteeing all guest instructions fetch through this
        # instruction translation entry (region0, RID0).
        literal(lines, 14, hex(TEXT))
        emit(lines, "m", "mov cr.ifa=r14")
        literal(lines, 14, hex(24 << 2))
        emit(lines, "m", "mov cr.itir=r14")
        pte = TEXT | (ar << 9) | (pl << 7) | (1 << 6) | (1 << 5) | 1
        literal(lines, 20, hex(pte))
        # GNU IA-64 assigns LAST M-unit instructions to the M;MI
        # template: slot 0 is followed immediately by a stop.  A
        # plain MII template cannot encode the required stop after M.
        lines.extend(["{ .mmi", "itc.i r20", ";;",
                      "nop.m 0", "nop.i 0", ";;", "}"])
        emit(lines, "m", "srlz.i")

    # RFI from CPL0 installs CPL3 and optional PSR.it (normal executing
    # code cannot directly modify privileged CPL with ssm/rsm).
    literal(lines, 14, "fault_bundle")
    emit(lines, "m", "mov cr.iip=r14")
    literal(lines, 20, hex((cpl << CPL_SHIFT) | (IT if it else 0)))
    emit(lines, "m", "mov cr.ipsr=r20")
    emit(lines, "b", "rfi")
    emit(lines, "b", "br.cond.sptk fail")

    lines.append("fault_bundle:")
    epc_bundle(lines, slot)
    lines.append("after_epc:")

    compare(lines, 15, 1 if fault else 0)
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex((3 << CPL_SHIFT) | IT))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex((expected << CPL_SHIFT) | (IT if it else 0)))

    # PFS must not be modified by epc, whether successful or faulted.
    emit(lines, "i", "mov r9=ar.pfs")
    compare(lines, 9, hex(ppl << 62))

    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)

    lines += [f".org {IVT_OFFSET}", "ivt_base:",
              f".org {IVT_OFFSET + BREAK_VECTOR}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT_OFFSET + ILLEGAL_VECTOR}",
              "illegal_vector:", "fault_handler:"]
    if not fault:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        # ISR.ei and saved IPSR.ri identify the faulting B slot.
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        emit(lines, "m", "mov r9=cr.isr")
        compare(lines, 9, hex(slot << 41))
        emit(lines, "m", "mov r9=cr.iip")
        literal(lines, 14, "fault_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.ipsr")
        literal(lines, 14, hex(RI_MASK | (3 << CPL_SHIFT) | IT))
        emit(lines, "i", "and r9=r9,r14")
        compare(lines, 9, hex((slot << 41) | (cpl << CPL_SHIFT) |
                               (IT if it else 0)))
        emit(lines, "m", "mov r9=cr.iim")
        compare(lines, 9, 0)
        emit(lines, "i", "mov r9=ar.pfs")
        compare(lines, 9, hex(ppl << 62))
        # Fault, not trap: skip the offending bundle and resume at
        # its next group without upgrading the saved CPL.
        literal(lines, 14, "after_epc")
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
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-b8-epc-execution"))
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
                               case_spec=(ILLEGAL_VECTOR, PASS, FAIL),
                               source=generate(case))
        results.append(evidence)
    report = {
        "reference": "Intel Itanium SDM Vol 3 rev 2.3 p. 3:53 B8 epc",
        "cases": len(results),
        "coverage": [
            "real RFI-selected CPL3 with and without instruction translation",
            "PFS.ppl pre-promotion check and real Illegal Operation IVT fault",
            "ITC.I physical page mapping with AR7/PL0,PL2,PL3 and AR1/PL3",
            "success, no-promotion and fault-before-commit cases",
            "B-slot 0, 1 and 2 execution with proper ISR.ei/IPSR.ri",
            "fault state restored with RFI, no firmware-specific patches",
        ],
        "results": results,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B8 EPC {mode} PASS: {len(results)}/{len(PROFILES)} profiles")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
