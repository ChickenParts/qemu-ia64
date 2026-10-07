#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-IVT F8 legality, disabled-register, V/D fault and predication tests."""
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
V, D = 1, 2
PASS, FAIL = "0x66383870", "0x66383866"
ONE = (1 << 63, 0xffff)
TWO = (1 << 63, 0x10000)
QNaN = (0xc000000000000001, 0x1ffff)
UNORM = (0x4000000000000000, 0xffff)
SENTINEL = (0x123456789abcdef0, 0x23456)


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


def check_saved(lines, vector, code, fault_label, saved_mask=0, slot=2):
    compare(lines, 12, hex(vector))
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex((slot << 41) | code))
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, fault_label)
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex((3 << 41) | DFL | DFH | DIRTY))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex((slot << 41) | saved_mask))


def clear_saved_and_retry(lines, mask):
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


def set_fpsr(lines, value):
    literal(lines, 20, hex(value))
    emit(lines, "m", "mov ar.fpsr=r20")


def check_fpsr(lines, value):
    emit(lines, "m", "mov r9=ar.fpsr")
    compare(lines, 9, hex(value))


def check_clean_dirty(lines):
    emit(lines, "m", "mov r9=psr")
    emit(lines, "i", "and r9=0x30,r9")
    compare(lines, 9, 0)


def generate():
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    data = []
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)  # actual handler entries

    # 1. Disabled source: fault before reads, clear DFL in saved PSR, retry.
    fill(lines, data, 6, SENTINEL, "disabled_dst")
    fill(lines, data, 7, ONE, "disabled_a")
    fill(lines, data, 8, TWO, "disabled_b")
    set_fpsr(lines, 0x3f)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", f"ssm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "disabled_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("disabled_fault:")
    emit(lines, "f", "fmax.s0 f6=f7,f8")
    lines.append("disabled_after:")
    compare(lines, 15, 1)
    emit(lines, "m", f"rsm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    spill_check(lines, 6, TWO)
    check_fpsr(lines, 0x3f)
    emit(lines, "b", "br.cond.sptk illegal_case")
    lines.append("disabled_handler:")
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    check_saved(lines, 0x5500, 1, "disabled_fault", DFL)
    spill_check(lines, 6, SENTINEL)
    check_fpsr(lines, 0x3f)
    clear_saved_and_retry(lines, DFL)

    # 2. Illegal destination wins even when the source bank is disabled.
    lines.append("illegal_case:")
    emit(lines, "m", f"ssm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "illegal_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("illegal_fault:")
    emit(lines, "f", "fmax.s0 f1=f7,f8")
    lines.append("illegal_after:")
    compare(lines, 15, 2)
    emit(lines, "m", f"rsm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    emit(lines, "b", "br.cond.sptk invalid_case")
    lines.append("illegal_handler:")
    compare(lines, 15, 1)
    literal(lines, 15, 2)
    check_saved(lines, 0x5400, 0, "illegal_fault", DFL)
    skip_fault(lines, "illegal_after")

    # 3. Enabled invalid-operation fault: no result/FPSR/dirty commit.
    lines.append("invalid_case:")
    fill(lines, data, 6, SENTINEL, "invalid_dst")
    fill(lines, data, 7, QNaN, "invalid_a")
    fill(lines, data, 8, TWO, "invalid_b")
    set_fpsr(lines, 0x3e)  # V enabled, other global exceptions masked
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "invalid_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("invalid_fault:")
    emit(lines, "f", "fmax.s0 f6=f7,f8")
    lines.append("invalid_after:")
    compare(lines, 15, 3)
    spill_check(lines, 6, TWO)
    check_fpsr(lines, 0x3f | (V << 13))
    emit(lines, "b", "br.cond.sptk denormal_case")
    lines.append("invalid_handler:")
    compare(lines, 15, 2)
    literal(lines, 15, 3)
    check_saved(lines, 0x5c00, V, "invalid_fault")
    spill_check(lines, 6, SENTINEL)
    check_fpsr(lines, 0x3e)
    check_clean_dirty(lines)
    emit(lines, "m", "mov r9=ar.fpsr")
    emit(lines, "i", "or r9=1,r9")
    emit(lines, "m", "mov ar.fpsr=r9")
    emit(lines, "b", "rfi")

    # 4. Enabled denormal fault and retry with D masked.
    lines.append("denormal_case:")
    fill(lines, data, 6, SENTINEL, "denorm_dst")
    fill(lines, data, 7, UNORM, "denorm_a")
    fill(lines, data, 8, TWO, "denorm_b")
    set_fpsr(lines, 0x3d)  # D enabled
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "denorm_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("denorm_fault:")
    emit(lines, "f", "fmin.s0 f6=f7,f8")
    lines.append("denorm_after:")
    compare(lines, 15, 4)
    spill_check(lines, 6, UNORM)
    check_fpsr(lines, 0x3f | (D << 13))
    emit(lines, "b", "br.cond.sptk packed_low_invalid")
    lines.append("denorm_handler:")
    compare(lines, 15, 3)
    literal(lines, 15, 4)
    check_saved(lines, 0x5c00, D, "denorm_fault")
    spill_check(lines, 6, SENTINEL)
    check_fpsr(lines, 0x3d)
    check_clean_dirty(lines)
    emit(lines, "m", "mov r9=ar.fpsr")
    emit(lines, "i", "or r9=2,r9")
    emit(lines, "m", "mov ar.fpsr=r9")
    emit(lines, "b", "rfi")

    # 5. Packed low-lane V uses the lane-shifted ISR code 0x10.
    lines.append("packed_low_invalid:")
    fill(lines, data, 6, SENTINEL, "packed_dst")
    fill(lines, data, 7, (0x3f8000007fc00000, 0x1003e), "packed_a")
    fill(lines, data, 8, (0x3f8000003f800000, 0x1003e), "packed_b")
    set_fpsr(lines, 0x3e)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "packed_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("packed_fault:")
    emit(lines, "f", "fpcmp.lt.s0 f6=f7,f8")
    lines.append("packed_after:")
    compare(lines, 15, 5)
    spill_check(lines, 6, (0x0000000000000000, 0x1003e))
    check_fpsr(lines, 0x3f | (V << 13))
    emit(lines, "b", "br.cond.sptk false_predicate")
    lines.append("packed_handler:")
    compare(lines, 15, 4)
    literal(lines, 15, 5)
    check_saved(lines, 0x5c00, V << 4, "packed_fault")
    spill_check(lines, 6, SENTINEL)
    check_fpsr(lines, 0x3e)
    check_clean_dirty(lines)
    emit(lines, "m", "mov r9=ar.fpsr")
    emit(lines, "i", "or r9=1,r9")
    emit(lines, "m", "mov ar.fpsr=r9")
    emit(lines, "b", "rfi")

    # 6. False qp suppresses illegal target, DFL, and enabled invalid input.
    lines.append("false_predicate:")
    emit(lines, "i", "cmp.eq p4,p5=r0,r0")
    set_fpsr(lines, 0x3e)
    emit(lines, "m", f"ssm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b6=r14")
    emit(lines, "f", "(p5) fmax.s0 f1=f7,f8")
    compare(lines, 15, 5)
    emit(lines, "m", f"rsm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    check_fpsr(lines, 0x3e)

    lines.append("pass:")
    ivt.terminal(lines, PASS, FAIL)

    lines += [f".org {IVT_OFFSET}", "ivt_base:"]
    for vector in (0x5400, 0x5500, 0x5c00):
        lines.append(f".org {IVT_OFFSET + vector}")
        literal(lines, 12, hex(vector))
        emit(lines, "b", "br.cond.sptk b6")

    lines += [".data", ".align 16", "output:", ".quad 0,0", *data]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="./build/qemu-system-ia64")
    parser.add_argument("--out", type=Path, default=Path("scratch/ia64-f8-exceptions"))
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--assemble-only", action="store_true")
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error("--timeout must be in (0,60]")
    args.out = args.out.resolve()
    evidence = ivt.run_one("f8-faults", args,
                           case_spec=(0x5c00, PASS, FAIL), source=generate())
    evidence.update(cases=6, expected_ivt_entries=5,
                    vectors=["0x5400", "0x5500", "0x5c00"],
                    controls=["false qp suppresses target/disabled/V"])
    result = args.out / "f8-faults" / "result.json"
    result.write_text(json.dumps(evidence, indent=2) + "\n")
    print("F8 IVT " + ("assembly" if args.assemble_only else "execution") +
          " PASS: 5 real faults, 1 false-predicate control")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
