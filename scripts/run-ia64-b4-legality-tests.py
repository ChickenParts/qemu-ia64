#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free real-IVT B4 reserved and predication validation.

Reserved words are one-field mutations of complete GNU-assembled B4 bundles;
valid conditional branches are assembled normally.  Successful br.ia mode
transition is deliberately not claimed by this legality tranche.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def load(filename: str, module_name: str):
    spec = importlib.util.spec_from_file_location(
        module_name, ROOT / "scripts" / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


ivt = load("run-ia64-fp-exception-tests.py", "ia64_ivt_for_b4")
oracle = load("ia64-b4-byte-oracle.py", "ia64_b4_byte_oracle")
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT_OFFSET = 0x400000
BREAK_VECTOR = 0x2c00
GENERAL_VECTOR = 0x5400
PASS, FAIL = "0x62347061", "0x62346661"
RI_MASK = 3 << 41

# kind, base spelling, slot, mutated btype, mutated qp
FAULT_PROFILES = {
    "reserved-cond-btype2-b0":
        ("br.cond.sptk.few b2", 0, 2, 0),
    "reserved-cond-btype7-falseqp-b1":
        ("br.cond.sptk.few b2", 1, 7, 6),
    "reserved-ret-btype0-b2":
        ("br.ret.sptk.few b2", 2, 0, 0),
    "qualified-ia-falseqp-b1":
        ("br.ia.sptk.few b2", 1, 1, 6),
}
POSITIVE_PROFILES = {
    "legal-cond-taken-b0": (0, True),
    "legal-cond-false-b1": (1, False),
}
PROFILES = {**FAULT_PROFILES, **POSITIVE_PROFILES}


def byte_directive(bundle: bytes) -> str:
    return ".byte " + ", ".join(f"0x{byte:02x}" for byte in bundle)


def setup(lines: list[str], *, false_p6: bool) -> None:
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b6=r14")
    literal(lines, 15, 0)
    if false_p6:
        emit(lines, "i", "cmp.ne p6,p7=r0,r0")


def terminal_and_ivt(lines: list[str], *, fault_slot: int | None) -> None:
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [f".org {IVT_OFFSET}", "ivt_base:",
              f".org {IVT_OFFSET + BREAK_VECTOR}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT_OFFSET + GENERAL_VECTOR}",
              "general_vector:", "fault_handler:"]
    if fault_slot is None:
        emit(lines, "b", "br.cond.sptk fail")
        return

    compare(lines, 15, 0)
    literal(lines, 15, 1)
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex(fault_slot << 41))
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, "fault_bundle")
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(RI_MASK))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex(fault_slot << 41))
    emit(lines, "m", "mov r9=cr.iim")
    compare(lines, 9, 0)

    # Skip the reserved source bundle and return without changing its state.
    literal(lines, 14, "after_b4")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(RI_MASK))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")


def generate_fault(case: str, bundle: bytes) -> str:
    _, slot, _, qp = FAULT_PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, false_p6=(qp == 6))
    literal(lines, 20, "must_not_branch")
    emit(lines, "i", "mov b2=r20")
    lines += [".align 16", "fault_bundle:", byte_directive(bundle),
              ".align 16", "after_b4:"]
    compare(lines, 15, 1)
    emit(lines, "b", "br.cond.sptk pass")
    lines += ["must_not_branch:"]
    literal(lines, 15, "0xbad")
    emit(lines, "b", "br.cond.sptk fail")
    lines += ["pass:"]
    terminal_and_ivt(lines, fault_slot=slot)
    return "\n".join(lines) + "\n"


def cond_bundle(lines: list[str], slot: int, spelling: str) -> None:
    if slot == 0:
        lines.extend(["{ .bbb", spelling, "nop.b 0", "nop.b 0", ";;", "}"])
    elif slot == 1:
        lines.extend(["{ .mbb", "nop.m 0", spelling, "nop.b 0", ";;", "}"])
    else:
        raise ValueError(f"unsupported positive slot {slot}")


def generate_positive(case: str) -> str:
    slot, taken = POSITIVE_PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, false_p6=not taken)
    literal(lines, 20, "branch_target")
    emit(lines, "i", "mov b2=r20")
    lines += ["branch_bundle:"]
    spelling = "br.cond.sptk.few b2" if taken else \
               "(p6) br.cond.sptk.few b2"
    cond_bundle(lines, slot, spelling)
    if taken:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        compare(lines, 15, 0)
        emit(lines, "b", "br.cond.sptk pass")
    lines += ["branch_target:"]
    if taken:
        compare(lines, 15, 0)
        emit(lines, "b", "br.cond.sptk pass")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    lines += ["pass:"]
    terminal_and_ivt(lines, fault_slot=None)
    return "\n".join(lines) + "\n"


def prepare_fault_bundles(out: Path) -> dict[str, bytes]:
    bundles: dict[str, bytes] = {}
    raw = out / "gnu-derived-raw"
    for case, (spelling, slot, btype, qp) in FAULT_PROFILES.items():
        base = oracle.assemble_bundle(spelling, slot, raw, case + "-base")
        mutated = oracle.mutate_b4(base, slot, btype=btype, qp=qp)
        word = oracle.extract_word(mutated, slot)
        got = oracle.fields(word)
        if got["major"] != 0 or got["qp"] != qp or got["btype"] != btype:
            raise ValueError(f"{case}: mutation did not produce requested B4 word")
        bundles[case] = mutated
    return bundles


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="./build/qemu-system-ia64")
    parser.add_argument("--out", type=Path,
                        default=Path("scratch/ia64-b4-legality-execution"))
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--assemble-only", action="store_true")
    parser.add_argument("--only", choices=list(PROFILES))
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error("--timeout must be in (0, 60]")
    args.out = args.out.resolve()
    bundles = prepare_fault_bundles(args.out)
    selected = [args.only] if args.only else list(PROFILES)
    results = []
    for case in selected:
        if case in FAULT_PROFILES:
            source = generate_fault(case, bundles[case])
        else:
            source = generate_positive(case)
        result = ivt.run_one(case, args,
                             case_spec=(GENERAL_VECTOR, PASS, FAIL),
                             source=source)
        results.append(result)
    report = {
        "reference": "Intel Itanium SDM Vol 3 rev 2.3 B4 tables 4-49/4-50",
        "cases": len(results),
        "coverage": [
            "reserved x6=0x20 btype 2 and 7",
            "reserved x6=0x21 btype 0",
            "unpredicated br.ia qp=6 fault despite p6=false",
            "B slots 0/1/2 and exact ISR.ei/IPSR.ri",
            "legal br.cond taken and false-predicate fallthrough",
            "no reserved-branch side effects and real IVT/RFI recovery",
            "successful br.ia transition explicitly out of scope",
        ],
        "results": results,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B4 LEGALITY {mode} PASS: {len(results)}/{len(PROFILES)} profiles")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
