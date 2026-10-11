#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free B5 reserved-wh and legal indirect-call validation."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def load(filename: str, name: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ivt = load("run-ia64-fp-exception-tests.py", "ia64_ivt_b5")
oracle = load("ia64-b5-byte-oracle.py", "ia64_b5_oracle")
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare
IVT_OFFSET, BREAK_VECTOR, GENERAL_VECTOR = 0x400000, 0x2c00, 0x5400
PASS, FAIL = "0x62357061", "0x62356661"
RI_MASK = 3 << 41
PFS_SENTINEL = 3 << 62

# spelling, slot, reserved wh, qp
FAULTS = {
    "reserved-wh0-b0": ("br.call.sptk.many b1=b2", 0, 0, 0),
    "reserved-wh2-falseqp-b1": ("br.call.spnt.few b1=b2", 1, 2, 6),
    "reserved-wh4-b2": ("br.call.dptk.many b1=b2", 2, 4, 0),
    "reserved-wh6-b2": ("br.call.dpnt.few b1=b2", 2, 6, 0),
}
POSITIVES = {
    "legal-call-taken-b0": (0, True),
    "legal-call-false-b1": (1, False),
}
PROFILES = {**FAULTS, **POSITIVES}


def bytes_line(data: bytes) -> str:
    return ".byte " + ", ".join(f"0x{x:02x}" for x in data)


def setup(lines: list[str], false_p6: bool = False) -> None:
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b6=r14")
    literal(lines, 15, 0)
    if false_p6:
        emit(lines, "i", "cmp.ne p6,p7=r0,r0")


def terminal(lines: list[str], slot: int | None) -> None:
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [f".org {IVT_OFFSET}", "ivt_base:",
              f".org {IVT_OFFSET + BREAK_VECTOR}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT_OFFSET + GENERAL_VECTOR}", "general_vector:"]
    if slot is None:
        emit(lines, "b", "br.cond.sptk fail")
        return
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex(slot << 41))
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, "fault_bundle")
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(RI_MASK))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex(slot << 41))
    emit(lines, "m", "mov r9=cr.iim")
    compare(lines, 9, 0)
    emit(lines, "i", "mov r9=b1")
    compare(lines, 9, "0x1110")
    emit(lines, "i", "mov r9=ar.pfs")
    compare(lines, 9, hex(PFS_SENTINEL))
    literal(lines, 14, "after_b5")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(RI_MASK))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")


def prepare_faults(out: Path) -> dict[str, bytes]:
    result = {}
    raw = out / "gnu-derived"
    for case, (spelling, slot, wh, qp) in FAULTS.items():
        data = oracle.assemble(spelling, slot, raw, case + "-base")
        data = oracle.mutate_wh(data, slot, wh)
        word = oracle.extract_word(data, slot)
        if qp:
            word = (word & ~0x3f) | qp
            data = oracle.replace_word(data, slot, word)
        got = oracle.fields(oracle.extract_word(data, slot))
        if got["major"] != 1 or got["wh"] != wh or got["qp"] != qp:
            raise ValueError(f"{case}: malformed fault fixture {got}")
        result[case] = data
    return result


def fault_source(case: str, data: bytes) -> str:
    _, slot, _, qp = FAULTS[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, false_p6=qp == 6)
    literal(lines, 20, "0x1110")
    emit(lines, "i", "mov b1=r20")
    literal(lines, 20, "must_not_call")
    emit(lines, "i", "mov b2=r20")
    literal(lines, 20, hex(PFS_SENTINEL))
    emit(lines, "i", "mov ar.pfs=r20")
    lines += [".align 16", "fault_bundle:", bytes_line(data),
              ".align 16", "after_b5:"]
    compare(lines, 15, 1)
    emit(lines, "i", "mov r9=b1")
    compare(lines, 9, "0x1110")
    emit(lines, "i", "mov r9=ar.pfs")
    compare(lines, 9, hex(PFS_SENTINEL))
    emit(lines, "b", "br.cond.sptk pass")
    lines += ["must_not_call:"]
    emit(lines, "b", "br.cond.sptk fail")
    lines += ["pass:"]
    terminal(lines, slot)
    return "\n".join(lines) + "\n"


def call_bundle(lines: list[str], slot: int, spelling: str) -> None:
    if slot == 0:
        lines += ["{ .bbb", spelling, "nop.b 0", "nop.b 0", ";;", "}"]
    elif slot == 1:
        lines += ["{ .mbb", "nop.m 0", spelling, "nop.b 0", ";;", "}"]
    else:
        raise ValueError("unsupported call slot")


def positive_source(case: str) -> str:
    slot, taken = POSITIVES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, false_p6=not taken)
    literal(lines, 20, "0x1110")
    emit(lines, "i", "mov b1=r20")
    literal(lines, 20, "call_target")
    emit(lines, "i", "mov b2=r20")
    lines += ["call_bundle:"]
    spelling = "br.call.sptk.many b1=b2" if taken else \
               "(p6) br.call.sptk.many b1=b2"
    call_bundle(lines, slot, spelling)
    lines += ["return_bundle:"]
    if taken:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        emit(lines, "i", "mov r9=b1")
        compare(lines, 9, "0x1110")
        emit(lines, "b", "br.cond.sptk pass")
    lines += ["call_target:"]
    if taken:
        emit(lines, "i", "mov r9=b1")
        literal(lines, 14, "return_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "b", "br.cond.sptk pass")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    lines += ["pass:"]
    terminal(lines, None)
    return "\n".join(lines) + "\n"


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="./build/qemu-system-ia64")
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-b5-legality"))
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("--assemble-only", action="store_true")
    p.add_argument("--only", choices=list(PROFILES))
    args = p.parse_args()
    args.out = args.out.resolve()
    faults = prepare_faults(args.out)
    selected = [args.only] if args.only else list(PROFILES)
    results = []
    for case in selected:
        source = fault_source(case, faults[case]) if case in FAULTS else positive_source(case)
        results.append(ivt.run_one(case, args,
                                   case_spec=(GENERAL_VECTOR, PASS, FAIL),
                                   source=source))
    report = {"reference": "Intel Itanium SDM Vol 3 rev 2.3 B5/Table 4-53",
              "cases": len(results),
              "coverage": ["reserved wh 0/2/4/6", "false predicate reserved fault",
                           "B slots 0/1/2", "no b1/PFS/call-target side effects",
                           "legal taken call return pointer", "legal false call fallthrough"],
              "results": results}
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B5 LEGALITY {mode} PASS: {len(results)}/{len(PROFILES)} profiles")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
