#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free execution tests for F1 fpma/fpms/fpnma."""
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_f9_runner", ROOT / "scripts/run-ia64-f9-tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

PASS = "r8=0000000066317061"
FAIL = "r8=0000000066316661"
FPSR_MASKED = 0x3f
NAT = (0, 0x1fffe)
I, U = 32, 16

def generate():
    lines = [".text", ".explicit", ".align 16", ".global _start",
             "_start:"]
    data = []
    count = 0
    assertion = 0

    def emit(kind, insn):
        table = {
            "m": [".mii", insn, "nop.i 0", "nop.i 0"],
            "i": [".mii", "nop.m 0", insn, "nop.i 0"],
            "f": [".mmf", "nop.m 0", "nop.m 0", insn],
            "b": [".mib", "nop.m 0", "nop.i 0", insn],
            "l": [".mlx", "nop.m 0", insn],
        }
        parts = table[kind]
        lines.extend(["{ " + parts[0], *parts[1:], ";;", "}"])

    def literal(reg, value):
        emit("l", f"movl r{reg}={value}")

    def compare(reg, value):
        nonlocal assertion
        assertion += 1
        literal(10, assertion)
        literal(18, hex(value) if isinstance(value, int) else value)
        emit("i", f"cmp.eq p6,p7=r{reg},r18")
        emit("b", "(p7) br.cond.spnt fail")

    def fill(reg, value):
        label = f"input_{len(data)}"
        data.append((label, value))
        literal(14, label)
        emit("m", f"ldf.fill f{reg}=[r14]")

    def spill_check(reg, expected):
        literal(14, "output")
        emit("m", f"stf.spill [r14]=f{reg}")
        emit("m", "ld8 r16=[r14],8")
        emit("m", "ld8 r17=[r14]")
        compare(16, expected[0])
        compare(17, expected[1])

    def check_dirty(value=0x10):
        emit("m", "mov r24=psr")
        emit("i", "and r24=0x30,r24")
        compare(24, value)

    def check_fpsr(value):
        emit("m", "mov r24=ar.fpsr")
        compare(24, value)

    def setup(left, right, addend, dst=6, fpsr=FPSR_MASKED):
        nonlocal count
        count += 1
        literal(9, count)
        fill(dst, (0x123456789abcdef0, 0x23456))
        fill(7, left)
        fill(8, right)
        fill(9, addend)
        emit("m", "rsm 0x30")
        emit("m", "srlz.d")
        literal(20, hex(fpsr))
        emit("m", "mov ar.fpsr=r20")

    left = (0x3f80000040000000, 0x1003e)
    right = (0x4040000040800000, 0x1003e)
    addend = (0x40a0000040c00000, 0x1003e)
    cases = [
        ("fpma.s0", (0x4100000041600000, 0x1003e)),
        ("fpms.s1", (0xc000000040000000, 0x1003e)),
        ("fpnma.s2", (0x40000000c0000000, 0x1003e)),
    ]
    for mnemonic, expected in cases:
        setup(left, right, addend)
        emit("f", f"{mnemonic} f6=f7,f8,f9")
        spill_check(6, expected)
        check_dirty()
        check_fpsr(FPSR_MASKED)

    # Destination/source alias must use source snapshots.
    setup(left, right, addend, dst=7)
    emit("f", "fpma.s0 f7=f7,f8,f9")
    spill_check(7, (0x4100000041600000, 0x1003e))
    check_dirty()

    # NaT propagation is lane-independent at the register level.
    setup(NAT, right, addend)
    emit("f", "fpma.s0 f6=f7,f8,f9")
    spill_check(6, NAT)
    check_dirty()

    # f2=f0 is the architectural packed pure-multiply form.
    setup(left, right, addend)
    emit("f", "fpma.s0 f6=f7,f8,f0")
    spill_check(6, (0x4040000041000000, 0x1003e))
    check_dirty()

    # RC lives in status-field controls[5:4].  Use a positive exact
    # sum 0.75 ulp above one to distinguish the four modes.
    one = (0x3f8000003f800000, 0x1003e)
    round_addend = (0x0000000033c00000, 0x1003e)
    round_cases = [
        (0, 0x3f800001),
        (1, 0x3f800000),
        (2, 0x3f800001),
        (3, 0x3f800000),
    ]
    for rc, low in round_cases:
        fpsr = FPSR_MASKED | (rc << 10)
        setup(one, one, round_addend, fpsr=fpsr)
        emit("f", "fpma.s0 f6=f7,f8,f9")
        spill_check(6, (0x3f80000000000000 | low, 0x1003e))
        check_dirty()
        check_fpsr(fpsr | (I << 13))

    # FTZ is controls bit 0.  With FTZ clear, an exact subnormal is
    # retained; with FTZ set it becomes zero and reports U and I.
    tiny_left = (0x3f80000000800000, 0x1003e)
    half_right = (0x3f8000003f000000, 0x1003e)
    packed_zero = (0, 0x1003e)
    setup(tiny_left, half_right, packed_zero)
    emit("f", "fpma.s0 f6=f7,f8,f9")
    spill_check(6, (0x3f80000000400000, 0x1003e))
    check_dirty()
    check_fpsr(FPSR_MASKED)

    ftz_fpsr = FPSR_MASKED | (1 << 6)
    setup(tiny_left, half_right, packed_zero, fpsr=ftz_fpsr)
    emit("f", "fpma.s0 f6=f7,f8,f9")
    spill_check(6, (0x3f80000000000000, 0x1003e))
    check_dirty()
    check_fpsr(ftz_fpsr | ((U | I) << 13))

    literal(8, "0x66317061")
    emit("m", "break.m 0")
    lines.append("pass_spin:")
    emit("b", "br.cond.sptk pass_spin")
    lines.append("fail:")
    literal(8, "0x66316661")
    emit("m", "break.m 0")
    lines.append("fail_spin:")
    emit("b", "br.cond.sptk fail_spin")
    lines += [".data", ".align 16", "output:", ".quad 0,0"]
    for label, pair in data:
        lines += [".align 16", label + ":",
                  f".quad {pair[0]:#x}, {pair[1]:#x}"]
    return "\n".join(lines) + "\n", count

def main():
    runner.run_generated_guest("F1 parallel", generate, 8, PASS, FAIL)

if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError,
            __import__("subprocess").SubprocessError) as exc:
        sys.exit(str(exc))
