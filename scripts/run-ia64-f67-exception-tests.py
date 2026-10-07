#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-IVT F6/F7 legality, register, FP-event and predication tests."""
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

DFL, DIRTY = 0x40000, 0x30
IVT_OFFSET = 0x400000
V, D, Z, SWA = 1, 2, 4, 8
PASS, FAIL = "0x66363770", "0x66363766"
ZERO = (0, 0)
ONE = (1 << 63, 0xffff)
NEG_FOUR = (1 << 63, 0x30001)
UNORM = (1 << 62, 0xffff)
TINY_NORMAL = (1 << 63, 0x40)
SEED = (0xff80000000000000, 0xfffe)
INF = (1 << 63, 0x1ffff)
INDEFINITE = (0xc000000000000000, 0x3ffff)
PACKED_LOW_NEG = (0x3f800000bf800000, 0x1003e)
PACKED_LOW_INVALID_RESULT = (0x3f7f8000ffc00000, 0x1003e)
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
    literal(lines, 14, hex((3 << 41) | DFL | DIRTY))
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


def set_predicate(lines, pred, value):
    other = pred + 1
    relation = "eq" if value else "ne"
    emit(lines, "i", f"cmp.{relation} p{pred},p{other}=r0,r0")


def check_predicate(lines, pred, value, tag):
    if value:
        label = f"{tag}_predicate_true"
        emit(lines, "b", f"(p{pred}) br.cond.sptk {label}")
        emit(lines, "b", "br.cond.sptk fail")
        lines.append(label + ":")
    else:
        emit(lines, "b", f"(p{pred}) br.cond.spnt fail")


def generate():
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    data = []
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)

    # 1. Disabled source faults before reads; retry succeeds after DFL clear.
    fill(lines, data, 6, SENTINEL, "disabled_dst")
    fill(lines, data, 7, ONE, "disabled_num")
    fill(lines, data, 8, ONE, "disabled_den")
    set_fpsr(lines, 0x3f)
    set_predicate(lines, 8, False)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", f"ssm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "disabled_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("disabled_fault:")
    emit(lines, "f", "frcpa.s0 f6,p8=f7,f8")
    lines.append("disabled_after:")
    compare(lines, 15, 1)
    emit(lines, "m", f"rsm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    spill_check(lines, 6, SEED)
    check_predicate(lines, 8, True, "disabled_after")
    check_fpsr(lines, 0x3f)
    emit(lines, "b", "br.cond.sptk illegal_case")
    lines.append("disabled_handler:")
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    check_saved(lines, 0x5500, 1, "disabled_fault", DFL)
    spill_check(lines, 6, SENTINEL)
    check_predicate(lines, 8, False, "disabled_handler")
    check_fpsr(lines, 0x3f)
    check_clean_dirty(lines)
    clear_saved_and_retry(lines, DFL)

    # 2. Illegal f1 destination wins over disabled-register detection.
    lines.append("illegal_case:")
    set_predicate(lines, 8, True)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", f"ssm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "illegal_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("illegal_fault:")
    emit(lines, "f", "frcpa.s0 f1,p8=f7,f8")
    lines.append("illegal_after:")
    compare(lines, 15, 2)
    check_predicate(lines, 8, True, "illegal_after")
    emit(lines, "m", f"rsm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    emit(lines, "b", "br.cond.sptk invalid_case")
    lines.append("illegal_handler:")
    compare(lines, 15, 1)
    literal(lines, 15, 2)
    check_saved(lines, 0x5400, 0, "illegal_fault", DFL)
    check_predicate(lines, 8, True, "illegal_handler")
    skip_fault(lines, "illegal_after")

    # 3. Enabled V fault preserves result/predicate/FPSR/dirty state; retry
    # after masking V commits the canonical indefinite result and clears p8.
    lines.append("invalid_case:")
    fill(lines, data, 6, SENTINEL, "invalid_dst")
    fill(lines, data, 8, NEG_FOUR, "invalid_src")
    set_fpsr(lines, 0x3e)
    set_predicate(lines, 8, True)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "invalid_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("invalid_fault:")
    emit(lines, "f", "frsqrta.s0 f6,p8=f8")
    lines.append("invalid_after:")
    compare(lines, 15, 3)
    spill_check(lines, 6, INDEFINITE)
    check_predicate(lines, 8, False, "invalid_after")
    check_fpsr(lines, 0x3f | (V << 13))
    emit(lines, "b", "br.cond.sptk zero_case")
    lines.append("invalid_handler:")
    compare(lines, 15, 2)
    literal(lines, 15, 3)
    check_saved(lines, 0x5c00, V, "invalid_fault")
    spill_check(lines, 6, SENTINEL)
    check_predicate(lines, 8, True, "invalid_handler")
    check_fpsr(lines, 0x3e)
    check_clean_dirty(lines)
    emit(lines, "m", "mov r9=ar.fpsr")
    emit(lines, "i", "or r9=1,r9")
    emit(lines, "m", "mov ar.fpsr=r9")
    emit(lines, "b", "rfi")

    # 4. Enabled Z fault for finite/zero frcpa; masked retry returns infinity.
    lines.append("zero_case:")
    fill(lines, data, 6, SENTINEL, "zero_dst")
    fill(lines, data, 7, ONE, "zero_num")
    fill(lines, data, 8, ZERO, "zero_den")
    set_fpsr(lines, 0x3b)
    set_predicate(lines, 8, True)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "zero_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("zero_fault:")
    emit(lines, "f", "frcpa.s0 f6,p8=f7,f8")
    lines.append("zero_after:")
    compare(lines, 15, 4)
    spill_check(lines, 6, INF)
    check_predicate(lines, 8, False, "zero_after")
    check_fpsr(lines, 0x3f | (Z << 13))
    emit(lines, "b", "br.cond.sptk denormal_case")
    lines.append("zero_handler:")
    compare(lines, 15, 3)
    literal(lines, 15, 4)
    check_saved(lines, 0x5c00, Z, "zero_fault")
    spill_check(lines, 6, SENTINEL)
    check_predicate(lines, 8, True, "zero_handler")
    check_fpsr(lines, 0x3b)
    check_clean_dirty(lines)
    emit(lines, "m", "mov r9=ar.fpsr")
    emit(lines, "i", "or r9=4,r9")
    emit(lines, "m", "mov ar.fpsr=r9")
    emit(lines, "b", "rfi")

    # 5. Enabled D fault from an unnormal numerator; retry sets D and p8.
    lines.append("denormal_case:")
    fill(lines, data, 6, SENTINEL, "denormal_dst")
    fill(lines, data, 7, UNORM, "denormal_num")
    fill(lines, data, 8, ONE, "denormal_den")
    set_fpsr(lines, 0x3d)
    set_predicate(lines, 8, False)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "denormal_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("denormal_fault:")
    emit(lines, "f", "frcpa.s0 f6,p8=f7,f8")
    lines.append("denormal_after:")
    compare(lines, 15, 5)
    spill_check(lines, 6, SEED)
    check_predicate(lines, 8, True, "denormal_after")
    check_fpsr(lines, 0x3f | (D << 13))
    emit(lines, "b", "br.cond.sptk swa_case")
    lines.append("denormal_handler:")
    compare(lines, 15, 4)
    literal(lines, 15, 5)
    check_saved(lines, 0x5c00, D, "denormal_fault")
    spill_check(lines, 6, SENTINEL)
    check_predicate(lines, 8, False, "denormal_handler")
    check_fpsr(lines, 0x3d)
    check_clean_dirty(lines)
    emit(lines, "m", "mov r9=ar.fpsr")
    emit(lines, "i", "or r9=2,r9")
    emit(lines, "m", "mov ar.fpsr=r9")
    emit(lines, "b", "rfi")

    # 6. SWA is an unmaskable floating-point fault with ISR.code 8.
    lines.append("swa_case:")
    fill(lines, data, 6, SENTINEL, "swa_dst")
    fill(lines, data, 8, TINY_NORMAL, "swa_src")
    set_fpsr(lines, 0x3f)
    set_predicate(lines, 8, True)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "swa_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("swa_fault:")
    emit(lines, "f", "frsqrta.s0 f6,p8=f8")
    lines.append("swa_after:")
    compare(lines, 15, 6)
    spill_check(lines, 6, SENTINEL)
    check_predicate(lines, 8, True, "swa_after")
    check_fpsr(lines, 0x3f)
    check_clean_dirty(lines)
    emit(lines, "b", "br.cond.sptk packed_case")
    lines.append("swa_handler:")
    compare(lines, 15, 5)
    literal(lines, 15, 6)
    check_saved(lines, 0x5c00, SWA, "swa_fault")
    spill_check(lines, 6, SENTINEL)
    check_predicate(lines, 8, True, "swa_handler")
    check_fpsr(lines, 0x3f)
    check_clean_dirty(lines)
    skip_fault(lines, "swa_after")

    # 7. Packed low-lane V is reported in the lane-shifted ISR code 0x10.
    lines.append("packed_case:")
    fill(lines, data, 6, SENTINEL, "packed_dst")
    fill(lines, data, 8, PACKED_LOW_NEG, "packed_src")
    set_fpsr(lines, 0x3e)
    set_predicate(lines, 8, True)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")
    literal(lines, 14, "packed_handler")
    emit(lines, "i", "mov b6=r14")
    lines.append("packed_fault:")
    emit(lines, "f", "fprsqrta.s0 f6,p8=f8")
    lines.append("packed_after:")
    compare(lines, 15, 7)
    spill_check(lines, 6, PACKED_LOW_INVALID_RESULT)
    check_predicate(lines, 8, False, "packed_after")
    check_fpsr(lines, 0x3f | (V << 13))
    emit(lines, "b", "br.cond.sptk false_predicate")
    lines.append("packed_handler:")
    compare(lines, 15, 6)
    literal(lines, 15, 7)
    check_saved(lines, 0x5c00, V << 4, "packed_fault")
    spill_check(lines, 6, SENTINEL)
    check_predicate(lines, 8, True, "packed_handler")
    check_fpsr(lines, 0x3e)
    check_clean_dirty(lines)
    emit(lines, "m", "mov r9=ar.fpsr")
    emit(lines, "i", "or r9=1,r9")
    emit(lines, "m", "mov ar.fpsr=r9")
    emit(lines, "b", "rfi")

    # 8. False qp suppresses illegal target, DFL, enabled FP events and dirty
    # state, but the F6/F7 predicate destination is architecturally cleared.
    lines.append("false_predicate:")
    set_fpsr(lines, 0)
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", f"ssm {DFL:#x}")
    emit(lines, "m", "srlz.d")
    emit(lines, "i", "cmp.eq p4,p5=r0,r0")
    set_predicate(lines, 8, True)
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b6=r14")
    emit(lines, "f", "(p5) frcpa.s0 f1,p8=f7,f8")
    check_predicate(lines, 8, False, "false_predicate")
    check_fpsr(lines, 0)
    check_clean_dirty(lines)
    emit(lines, "m", f"rsm {DFL:#x}")
    emit(lines, "m", "srlz.d")

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
    parser.add_argument("--out", type=Path,
                        default=Path("scratch/ia64-f67-exceptions"))
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--assemble-only", action="store_true")
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error("--timeout must be in (0,60]")
    args.out = args.out.resolve()
    evidence = ivt.run_one("f67-faults", args,
                           case_spec=(0x5c00, PASS, FAIL), source=generate())
    evidence.update(cases=8, expected_ivt_entries=7,
                    vectors=["0x5400", "0x5500", "0x5c00"],
                    controls=["false qp clears p2 without faults"])
    result = args.out / "f67-faults" / "result.json"
    result.write_text(json.dumps(evidence, indent=2) + "\n")
    print("F6/F7 IVT " +
          ("assembly" if args.assemble_only else "execution") +
          " PASS: 7 real faults, 1 false-predicate control")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
