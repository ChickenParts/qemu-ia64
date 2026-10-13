#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real QEMU IVT tests for reserved B1/B2 btypes and counted-branch qp."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
def load(filename, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    obj = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(obj)
    return obj

ivt = load("run-ia64-fp-exception-tests.py", "b12_ivt")
oracle = load("ia64-b12-byte-oracle.py", "b12_bytes")
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare
IVT, BREAK, GENERAL = 0x400000, 0x2c00, 0x5400
PASS, FAIL = "0x62317061", "0x62316661"
RI_MASK = 3 << 41
EC = 2 << 58
# GNU source spelling, B slot, mutated btype and encoded qp.
FAULTS = {
    "reserved-btype1-b0": ("br.cond.sptk.few _start", 0, 1, 0),
    "reserved-btype4-falseqp-b1": ("(p6) br.cond.spnt.many _start", 1, 4, 6),
    "cloop-nonzeroqp6-b2": ("br.cloop.sptk.few _start", 2, 5, 6),
    "cexit-nonzeroqp1-b2": ("br.cexit.dptk.few _start", 2, 6, 1),
}
# slot, branch family, should branch
POSITIVES = {
    "cond-taken-b0": (0, "cond", True),
    "cond-false-b1": (1, "cond", False),
    "cloop-taken-b2": (2, "cloop", True),
    "cloop-untaken-b2": (2, "cloop", False),
}
PROFILES = {**FAULTS, **POSITIVES}

def setup(lines, lc, *, p6_false=False, p1_false=False):
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b6=r14")
    literal(lines, 15, 0)
    literal(lines, 20, lc)
    emit(lines, "m", "mov ar.lc=r20")
    literal(lines, 20, hex(EC))
    emit(lines, "m", "mov ar.ec=r20")
    if p6_false:
        emit(lines, "i", "cmp.ne p6,p7=r0,r0")
    if p1_false:
        emit(lines, "i", "cmp.ne p1,p2=r0,r0")

def terminal_and_ivt(lines, slot):
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [f".org {IVT}", "ivt_base:", f".org {IVT + BREAK}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT + GENERAL}", "general_vector:"]
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
    emit(lines, "m", "mov r9=ar.lc")
    compare(lines, 9, 5)
    emit(lines, "m", "mov r9=ar.ec")
    compare(lines, 9, hex(EC))
    literal(lines, 14, "after_b12")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(RI_MASK))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

def fault_bundle(case, out):
    spelling, slot, btype, qp = FAULTS[case]
    original = oracle.assemble_bundle(spelling, slot, out / "gnu-base", case)
    raw = oracle.change_fields(original, slot, btype=btype, qp=qp)
    got = oracle.fields(oracle.extract_word(raw, slot))
    if got["major"] != 4 or got["btype"] != btype or got["qp"] != qp:
        raise ValueError(f"incorrect fault fixture: {case}: {got}")
    return raw

def fault_source(case, raw):
    _, slot, _, qp = FAULTS[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, 5, p6_false=(qp == 6), p1_false=(qp == 1))
    lines += [".align 16", "fault_bundle:",
              ".byte " + ", ".join(f"0x{x:02x}" for x in raw),
              ".align 16", "after_b12:"]
    compare(lines, 15, 1)
    emit(lines, "m", "mov r9=ar.lc")
    compare(lines, 9, 5)
    emit(lines, "m", "mov r9=ar.ec")
    compare(lines, 9, hex(EC))
    emit(lines, "b", "br.cond.sptk pass")
    lines += ["pass:"]
    terminal_and_ivt(lines, slot)
    return "\n".join(lines) + "\n"

def branch_bundle(lines, slot, spelling):
    if slot == 0:
        lines += ["{ .bbb", spelling, "nop.b 0", "nop.b 0", ";;", "}"]
    elif slot == 1:
        lines += ["{ .mbb", "nop.m 0", spelling, "nop.b 0", ";;", "}"]
    elif slot == 2:
        lines += ["{ .mib", "nop.m 0", "nop.i 0", spelling, ";;", "}"]
    else:
        raise ValueError("bad branch slot")

def positive_source(case):
    slot, kind, taken = POSITIVES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, (2 if taken else 0) if kind == "cloop" else 5,
          p6_false=(kind == "cond" and not taken))
    lines += ["branch_bundle:"]
    if kind == "cond":
        spelling = ("br.cond.sptk.few branch_target" if taken else
                    "(p6) br.cond.sptk.few branch_target")
    else:
        spelling = "br.cloop.sptk.few branch_target"
    branch_bundle(lines, slot, spelling)
    lines += ["after_b12:"]
    if taken:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        compare(lines, 15, 0)
        if kind == "cloop":
            emit(lines, "m", "mov r9=ar.lc")
            compare(lines, 9, 0)
        emit(lines, "b", "br.cond.sptk pass")
    lines += ["branch_target:"]
    if taken:
        compare(lines, 15, 0)
        if kind == "cloop":
            emit(lines, "m", "mov r9=ar.lc")
            compare(lines, 9, 1)
        emit(lines, "b", "br.cond.sptk pass")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    lines += ["pass:"]
    terminal_and_ivt(lines, None)
    return "\n".join(lines) + "\n"

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="./build/qemu-system-ia64")
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-b12-test"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--assemble-only", action="store_true")
    ap.add_argument("--only", choices=list(PROFILES))
    args = ap.parse_args()
    if not 0 < args.timeout <= 60:
        ap.error("timeout must be in (0,60]")
    args.out = args.out.resolve()
    selected = [args.only] if args.only else list(PROFILES)
    results = []
    for case in selected:
        source = (fault_source(case, fault_bundle(case, args.out))
                  if case in FAULTS else positive_source(case))
        results.append(ivt.run_one(case, args,
                                   case_spec=(GENERAL, PASS, FAIL),
                                   source=source))
    results_data = {"reference": "Intel IA-64 SDM rev 2.3 Table 4-47/B1/B2",
                    "cases": len(results), "results": results,
                    "scope": "B1/B2 legality only, PSR.tb and loop semantics deferred"}
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(results_data, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B1/B2 LEGALITY {mode} PASS: {len(results)}/{len(PROFILES)} profiles")

if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
