#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Full-system IFS.v, cover, interruption and rfi/AR.EC preservation tests."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_ifs_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT = 0x400000
BREAK_VECTOR = 0x2c00
GENERAL_VECTOR = 0x5400
PASS, FAIL = "0x1f560061", "0x1f560066"
V = 1 << 63
IFM_MASK = (1 << 38) - 1
BASE_CFM = 8 | (1 << 14)
EC_VALUES = (0, 1, 2, 63)
PROFILES = {
    **{f"rfi-v{v}-ec{ec}": ("rfi", v, ec)
       for v in (0, 1) for ec in EC_VALUES},
    "cover-ic0": ("cover", 0, 2),
    "cover-ic1": ("cover", 1, 2),
    "interrupt-ic0": ("interrupt", 0, 2),
    "interrupt-ic1": ("interrupt", 1, 63),
}

def setup(lines: list[str], ec: int) -> None:
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b6=r14")
    literal(lines, 15, 0)
    # CR.IFS accesses require PSR.ic=0; set explicit initial mode.
    emit(lines, "m", "rsm 0x2000")
    emit(lines, "m", "srlz.i")
    literal(lines, 20, ec)
    emit(lines, "i", "mov ar.ec=r20")
    # Preserve the actual guest-visible value; do not assume QEMU's
    # internal shifted representation is the architectural read format.
    emit(lines, "i", "mov r10=ar.ec")

def check_ec(lines: list[str]) -> None:
    emit(lines, "i", "mov r9=ar.ec")
    emit(lines, "i", "cmp.eq p6,p7=r9,r10")
    emit(lines, "b", "(p7) br.cond.spnt fail")

def check_cfm_through_call(lines: list[str], expected: int) -> None:
    # Existing verified br.call machinery records previous CFM in ar.pfs.
    emit(lines, "b", "br.call.sptk b0=cfm_snapshot")
    emit(lines, "b", "br.cond.sptk fail")
    lines.append("cfm_snapshot:")
    emit(lines, "i", "mov r9=ar.pfs")
    literal(lines, 14, hex(IFM_MASK))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex(expected))

def terminal(lines: list[str], handler: list[str] | None = None) -> None:
    lines += ["pass:"]
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [f".org {IVT}", "ivt_base:",
              f".org {IVT + BREAK_VECTOR}", "break_vector:"]
    if handler is None:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        lines += handler
    lines += [f".org {IVT + GENERAL_VECTOR}", "general_vector:"]
    emit(lines, "b", "br.cond.sptk fail")

def manual_rfi(valid: int, ec: int) -> str:
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, ec)
    # A nonzero CFM distinguishes valid from invalid IFS restoration.
    emit(lines, "m", "alloc r20=ar.pfs,0,0,8,8")
    literal(lines, 20, hex(V if valid else 0))
    emit(lines, "m", "mov cr.ifs=r20")
    literal(lines, 20, "after_rfi")
    emit(lines, "m", "mov cr.iip=r20")
    emit(lines, "m", "mov r9=psr")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")
    emit(lines, "b", "br.cond.sptk fail")
    lines += ["after_rfi:"]
    check_ec(lines)
    check_cfm_through_call(lines, 0 if valid else BASE_CFM)
    compare(lines, 15, 0)
    emit(lines, "b", "br.cond.sptk pass")
    terminal(lines)
    return "\n".join(lines) + "\n"

def cover_case(ic: int, ec: int) -> str:
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, ec)
    emit(lines, "m", "alloc r20=ar.pfs,0,0,8,8")
    # A distinct, legal IFS value must survive cover with PSR.ic=1.
    sentinel = V if ic else 0
    literal(lines, 20, hex(sentinel))
    emit(lines, "m", "mov cr.ifs=r20")
    if ic:
        emit(lines, "m", "ssm 0x2000")
        emit(lines, "m", "srlz.i")
    emit(lines, "b", "cover")
    if ic:
        emit(lines, "m", "rsm 0x2000")
        emit(lines, "m", "srlz.i")
    emit(lines, "m", "mov r9=cr.ifs")
    compare(lines, 9, hex(sentinel if ic else V | BASE_CFM))
    check_ec(lines)
    compare(lines, 15, 0)
    emit(lines, "b", "br.cond.sptk pass")
    terminal(lines)
    return "\n".join(lines) + "\n"

def interruption_case(ic: int, ec: int) -> str:
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, ec)
    emit(lines, "m", "alloc r20=ar.pfs,0,0,8,8")
    # Valid IFS at entry: collected interruptions invalidate V, whereas
    # uncollected interruptions must preserve this for software cover.
    literal(lines, 20, hex(V | BASE_CFM))
    emit(lines, "m", "mov cr.ifs=r20")
    if ic:
        emit(lines, "m", "ssm 0x2000")
        emit(lines, "m", "srlz.i")
    lines += ["fault_bundle:"]
    emit(lines, "b", "break.b 0")
    lines += ["after_fault:"]
    compare(lines, 15, 1)
    check_ec(lines)
    check_cfm_through_call(lines, BASE_CFM)
    emit(lines, "b", "br.cond.sptk pass")

    handler = []
    compare(handler, 15, 0)
    literal(handler, 15, 1)
    emit(handler, "m", "mov r9=cr.ifs")
    if ic:
        literal(handler, 14, hex(V))
        emit(handler, "i", "and r9=r9,r14")
        compare(handler, 9, 0)
        # Interrupt delivery cleared PSR.ic. cover must save only CFM.
        emit(handler, "b", "cover")
        emit(handler, "m", "mov r9=cr.ifs")
        compare(handler, 9, hex(V | BASE_CFM))
    else:
        compare(handler, 9, hex(V | BASE_CFM))
    check_ec(handler)
    emit(handler, "m", "mov r9=cr.iip")
    literal(handler, 14, "fault_bundle")
    emit(handler, "i", "cmp.eq p6,p7=r9,r14")
    emit(handler, "b", "(p7) br.cond.spnt fail")
    literal(handler, 14, "after_fault")
    emit(handler, "m", "mov cr.iip=r14")
    emit(handler, "m", "mov r9=cr.ipsr")
    literal(handler, 14, hex(3 << 41))
    emit(handler, "i", "andcm r9=r9,r14")
    emit(handler, "m", "mov cr.ipsr=r9")
    emit(handler, "b", "rfi")
    terminal(lines, handler)
    return "\n".join(lines) + "\n"

def generate(case: str) -> str:
    kind, flag, ec = PROFILES[case]
    if kind == "rfi":
        return manual_rfi(flag, ec)
    if kind == "cover":
        return cover_case(flag, ec)
    return interruption_case(flag, ec)

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="./build/qemu-system-ia64")
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-ifs-rfi"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--assemble-only", action="store_true")
    ap.add_argument("--only", choices=list(PROFILES))
    args = ap.parse_args()
    if not 0 < args.timeout <= 60:
        ap.error("timeout must be in (0,60]")
    args.out = args.out.resolve()
    cases = [args.only] if args.only else list(PROFILES)
    results = [ivt.run_one(c, args, case_spec=(BREAK_VECTOR, PASS, FAIL),
                           source=generate(c)) for c in cases]
    report = {"reference": "Intel SDM Vol 2 §3.3.5.7; Vol 3 cover/rfi",
              "scope": "IFS.ifm/v, AR.EC preservation and PSR.ic",
              "cases": len(cases), "results": results}
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 IFS/RFI {mode} PASS: {len(cases)}/{len(PROFILES)} profiles")

if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
