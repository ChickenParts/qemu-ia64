#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-IVT tests for F1 packed multiply-add faults and traps."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_fp_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

DFL, DFH, DIRTY = 0x40000, 0x80000, 0x30
IVT_OFFSET = 0x400000
V, D, I = 1, 2, 32
SENTINEL = (0x123456789abcdef0, 0x23456)
PACKED_ZERO = (0x0000000000000000, 0x1003e)
PACKED_ONE = (0x3f8000003f800000, 0x1003e)
PACKED_TWO = (0x4000000040000000, 0x1003e)
CASES = {
    "disabled": (0x5500, "0x66316470", "0x66316466"),
    "illegal": (0x5400, "0x66316970", "0x66316966"),
    "invalid-high": (0x5c00, "0x66317670", "0x66317666"),
    "denormal-low": (0x5c00, "0x66316470", "0x66316466"),
    "combined-fault": (0x5c00, "0x66316370", "0x66316366"),
    "inexact-low": (0x5d00, "0x66316970", "0x66316966"),
    "false-predicate": (0x5c00, "0x66316670", "0x66316666"),
}


def fill(lines, data, reg, bits, label):
    data += [".align 16", label + ":", f".quad {bits[0]:#x}, {bits[1]:#x}"]
    literal(lines, 14, label)
    emit(lines, "m", f"ldf.fill f{reg}=[r14]")


def spill_check(lines, reg, bits):
    literal(lines, 14, "output")
    emit(lines, "m", f"stf.spill [r14]=f{reg}")
    emit(lines, "m", "ld8 r9=[r14],8")
    emit(lines, "m", "ld8 r10=[r14]")
    compare(lines, 9, hex(bits[0]))
    compare(lines, 10, hex(bits[1]))


def set_fpsr(lines, value):
    literal(lines, 20, hex(value))
    emit(lines, "m", "mov ar.fpsr=r20")


def check_fpsr(lines, value):
    emit(lines, "m", "mov r9=ar.fpsr")
    compare(lines, 9, hex(value))


def check_dirty(lines, value):
    emit(lines, "m", "mov r9=psr")
    emit(lines, "i", "and r9=0x30,r9")
    compare(lines, 9, hex(value))


def check_fault_common(lines, vector, code, fault_label, saved_mask=0):
    compare(lines, 12, hex(vector))
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex((2 << 41) | code))
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, fault_label)
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex((3 << 41) | DFL | DFH | DIRTY))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex((2 << 41) | saved_mask))


def retry_with_saved_mask_cleared(lines, mask):
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(mask))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")


def skip_fault(lines, after):
    literal(lines, 14, after)
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(3 << 41))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")


def generate(case):
    vector, passed, failed = CASES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    data = []
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)

    if case == "disabled":
        fill(lines, data, 6, SENTINEL, "dst")
        fill(lines, data, 7, PACKED_ONE, "addend")
        fill(lines, data, 8, PACKED_ONE, "left")
        fill(lines, data, 9, PACKED_ONE, "right")
        set_fpsr(lines, 0x3f)
        emit(lines, "m", "rsm 0x30")
        emit(lines, "m", f"ssm {DFL:#x}")
        emit(lines, "m", "srlz.d")
        lines.append("fault_bundle:")
        emit(lines, "f", "fpma.s0 f6=f7,f8,f9")
        lines.append("after_retry:")
        compare(lines, 15, 1)
        emit(lines, "m", f"rsm {DFL:#x}")
        emit(lines, "m", "srlz.d")
        spill_check(lines, 6, PACKED_TWO)
        check_fpsr(lines, 0x3f)
        check_dirty(lines, 0x10)
        emit(lines, "b", "br.cond.sptk pass")
    elif case == "illegal":
        fill(lines, data, 7, PACKED_ONE, "addend")
        fill(lines, data, 8, PACKED_ONE, "left")
        fill(lines, data, 9, PACKED_ONE, "right")
        set_fpsr(lines, 0x3f)
        emit(lines, "m", "rsm 0x30")
        emit(lines, "m", f"ssm {DFL:#x}")
        emit(lines, "m", "srlz.d")
        lines.append("fault_bundle:")
        emit(lines, "f", "fpma.s0 f1=f7,f8,f9")
        lines.append("after_fault:")
        compare(lines, 15, 1)
        emit(lines, "m", f"rsm {DFL:#x}")
        emit(lines, "m", "srlz.d")
        check_fpsr(lines, 0x3f)
        check_dirty(lines, 0)
        emit(lines, "b", "br.cond.sptk pass")
    elif case == "invalid-high":
        fill(lines, data, 6, SENTINEL, "dst")
        fill(lines, data, 7, (0x000000003f800000, 0x1003e), "left")
        fill(lines, data, 8, (0x7f8000003f800000, 0x1003e), "right")
        fill(lines, data, 9, PACKED_ONE, "addend")
        set_fpsr(lines, 0x3e)
        emit(lines, "m", "rsm 0x30")
        emit(lines, "m", "srlz.d")
        lines.append("fault_bundle:")
        emit(lines, "f", "fpma.s0 f6=f7,f8,f9")
        lines.append("after_retry:")
        compare(lines, 15, 1)
        spill_check(lines, 6, (0xffc0000040000000, 0x1003e))
        check_fpsr(lines, 0x3f | (V << 13))
        check_dirty(lines, 0x10)
        emit(lines, "b", "br.cond.sptk pass")
    elif case == "denormal-low":
        fill(lines, data, 6, SENTINEL, "dst")
        fill(lines, data, 7, (0x3f80000000000001, 0x1003e), "left")
        fill(lines, data, 8, PACKED_ONE, "right")
        fill(lines, data, 9, PACKED_ZERO, "addend")
        set_fpsr(lines, 0x3d)
        emit(lines, "m", "rsm 0x30")
        emit(lines, "m", "srlz.d")
        lines.append("fault_bundle:")
        emit(lines, "f", "fpma.s0 f6=f7,f8,f9")
        lines.append("after_retry:")
        compare(lines, 15, 1)
        spill_check(lines, 6, (0x3f80000000000001, 0x1003e))
        check_fpsr(lines, 0x3f | (D << 13))
        check_dirty(lines, 0x10)
        emit(lines, "b", "br.cond.sptk pass")
    elif case == "combined-fault":
        fill(lines, data, 6, SENTINEL, "dst")
        fill(lines, data, 7, (0x0000000000000001, 0x1003e), "left")
        fill(lines, data, 8, (0x7f8000003f800000, 0x1003e), "right")
        fill(lines, data, 9, PACKED_ZERO, "addend")
        set_fpsr(lines, 0x3c)
        emit(lines, "m", "rsm 0x30")
        emit(lines, "m", "srlz.d")
        lines.append("fault_bundle:")
        emit(lines, "f", "fpma.s0 f6=f7,f8,f9")
        lines.append("after_fault:")
        compare(lines, 15, 1)
        spill_check(lines, 6, SENTINEL)
        check_fpsr(lines, 0x3c)
        check_dirty(lines, 0)
        emit(lines, "b", "br.cond.sptk pass")
    elif case == "inexact-low":
        fill(lines, data, 6, SENTINEL, "dst")
        fill(lines, data, 7, PACKED_ONE, "left")
        fill(lines, data, 8, PACKED_ONE, "right")
        fill(lines, data, 9, (0x0000000033c00000, 0x1003e), "addend")
        set_fpsr(lines, 0x1f)
        emit(lines, "m", "rsm 0x30")
        emit(lines, "m", "srlz.d")
        lines.append("fault_bundle:")
        emit(lines, "f", "fpma.s0 f6=f7,f8,f9")
        lines.append("after_trap:")
        compare(lines, 15, 1)
        spill_check(lines, 6, (0x3f8000003f800001, 0x1003e))
        check_fpsr(lines, 0x1f | (I << 13))
        check_dirty(lines, 0x10)
        emit(lines, "b", "br.cond.sptk pass")
    else:
        fill(lines, data, 7, (0x000000003f800000, 0x1003e), "left")
        fill(lines, data, 8, (0x7f8000003f800000, 0x1003e), "right")
        fill(lines, data, 9, PACKED_ONE, "addend")
        set_fpsr(lines, 0x3e)
        emit(lines, "i", "cmp.eq p4,p5=r0,r0")
        emit(lines, "m", "rsm 0x30")
        emit(lines, "m", f"ssm {DFL:#x}")
        emit(lines, "m", "srlz.d")
        emit(lines, "f", "(p5) fpma.s0 f1=f7,f8,f9")
        compare(lines, 15, 0)
        emit(lines, "m", f"rsm {DFL:#x}")
        emit(lines, "m", "srlz.d")
        check_fpsr(lines, 0x3e)
        check_dirty(lines, 0)
        emit(lines, "b", "br.cond.sptk pass")

    lines += [f".org {IVT_OFFSET}", "ivt_base:",
              f".org {IVT_OFFSET + vector}", "handler:"]
    literal(lines, 12, hex(vector))
    if case == "disabled":
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        check_fault_common(lines, vector, 1, "fault_bundle", DFL)
        spill_check(lines, 6, SENTINEL)
        check_fpsr(lines, 0x3f)
        check_dirty(lines, 0)
        retry_with_saved_mask_cleared(lines, DFL)
    elif case == "illegal":
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        check_fault_common(lines, vector, 0, "fault_bundle", DFL)
        check_fpsr(lines, 0x3f)
        check_dirty(lines, 0)
        skip_fault(lines, "after_fault")
    elif case == "invalid-high":
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        check_fault_common(lines, vector, V, "fault_bundle")
        spill_check(lines, 6, SENTINEL)
        check_fpsr(lines, 0x3e)
        check_dirty(lines, 0)
        emit(lines, "m", "mov r9=ar.fpsr")
        emit(lines, "i", "or r9=1,r9")
        emit(lines, "m", "mov ar.fpsr=r9")
        emit(lines, "b", "rfi")
    elif case == "denormal-low":
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        check_fault_common(lines, vector, D << 4, "fault_bundle")
        spill_check(lines, 6, SENTINEL)
        check_fpsr(lines, 0x3d)
        check_dirty(lines, 0)
        emit(lines, "m", "mov r9=ar.fpsr")
        emit(lines, "i", "or r9=2,r9")
        emit(lines, "m", "mov ar.fpsr=r9")
        emit(lines, "b", "rfi")
    elif case == "combined-fault":
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        check_fault_common(lines, vector, V | (D << 4), "fault_bundle")
        spill_check(lines, 6, SENTINEL)
        check_fpsr(lines, 0x3c)
        check_dirty(lines, 0)
        skip_fault(lines, "after_fault")
    elif case == "inexact-low":
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        compare(lines, 12, hex(vector))
        emit(lines, "m", "mov r9=cr.isr")
        compare(lines, 9, hex((2 << 41) | 1 | (1 << 9) | (1 << 10)))
        emit(lines, "m", "mov r9=cr.iipa")
        literal(lines, 14, "fault_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.iip")
        literal(lines, 14, "after_trap")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.ipsr")
        literal(lines, 14, hex((3 << 41) | DIRTY))
        emit(lines, "i", "and r9=r9,r14")
        compare(lines, 9, 0x10)
        spill_check(lines, 6, (0x3f8000003f800001, 0x1003e))
        check_fpsr(lines, 0x1f | (I << 13))
        emit(lines, "b", "rfi")
    else:
        emit(lines, "b", "br.cond.sptk fail")

    lines.append("pass:")
    ivt.terminal(lines, passed, failed)
    lines += [".data", ".align 16", "output:", ".quad 0,0", *data]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="./build/qemu-system-ia64")
    parser.add_argument("--out", type=Path,
                        default=Path("scratch/ia64-f1-exceptions"))
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--assemble-only", action="store_true")
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error("--timeout must be in (0,60]")
    args.out = args.out.resolve()

    evidence = []
    for case, spec in CASES.items():
        evidence.append(ivt.run_one(case, args, case_spec=spec,
                                    source=generate(case)))
    summary = {
        "cases": len(CASES),
        "vectors": ["0x5400", "0x5500", "0x5c00", "0x5d00"],
        "controls": [
            "illegal destination precedes disabled-bank detection",
            "high/low packed fault-code placement",
            "fault-before-commit and trap-after-commit",
            "low-lane inexact and FPA completion bits",
            "false qp suppresses legality, banking, and FP events",
        ],
        "results": evidence,
    }
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    mode = "assembly" if args.assemble_only else "execution"
    print(f"F1 IVT {mode} PASS: {len(CASES)} cases")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
