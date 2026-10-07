#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free real F8 execution tests for scalar/packed min/max and compare."""
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_f9_runner", ROOT / "scripts/run-ia64-f9-tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

PASS = "r8=0000000066387061"
FAIL = "r8=0000000066386661"
NAT = (0, 0x1fffe)
ONE = (1 << 63, 0xffff)
TWO = (1 << 63, 0x10000)
NEG_ONE = (1 << 63, 0x2ffff)
NEG_TWO = (1 << 63, 0x30000)
QNaN = (0xc000000000000001, 0x1ffff)
UNORM = (0x4000000000000000, 0xffff)
DE0 = (1 << 63, 0)
FPSR_MASKED = 0x3f


def generate():
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
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

    def setup_case(dst, a, b):
        nonlocal count
        count += 1
        lines.append(f"case_{count}:")
        literal(9, count)
        fill(dst, (0x123456789abcdef0, 0x23456))
        fill(7, a)
        fill(8, b)
        emit("m", "rsm 0x30")
        emit("m", "srlz.d")
        literal(20, hex(FPSR_MASKED))
        emit("m", "mov ar.fpsr=r20")

    scalar = [
        ("fmin.s0", ONE, TWO, ONE, 0),
        ("fmax.s0", ONE, TWO, TWO, 0),
        ("fmin.s1", NEG_TWO, NEG_ONE, NEG_TWO, 0),
        ("fmax.s2", NEG_TWO, NEG_ONE, NEG_ONE, 0),
        ("famin.s0", NEG_TWO, ONE, ONE, 0),
        ("famax.s3", NEG_TWO, ONE, NEG_TWO, 0),
        ("fmin.s0", (0, 0), (0, 0x20000), (0, 0x20000), 0),
        ("fmax.s0", NAT, TWO, NAT, 0),
        ("fmax.s0", QNaN, TWO, TWO, 1),
        ("fmin.s0", UNORM, TWO, UNORM, 2),
        ("fmin.s0", DE0, ONE, DE0, 2),
        ("fmax.s0", DE0, ONE, ONE, 2),
    ]
    for mnemonic, a, b, expected, flags in scalar:
        setup_case(6, a, b)
        emit("f", f"{mnemonic} f6=f7,f8")
        spill_check(6, expected)
        emit("m", "mov r24=psr")
        emit("i", "and r24=0x30,r24")
        compare(24, 0x10)
        emit("m", "mov r24=ar.fpsr")
        sf = int(mnemonic.rsplit("s", 1)[1])
        compare(24, FPSR_MASKED | (flags << (13 + 13 * sf)))

    packed = [
        ("fpmin.s0", 0x3f800000bf800000, 0x400000003f000000,
         0x3f800000bf800000, 0),
        ("fpmax.s1", 0x3f800000bf800000, 0x400000003f000000,
         0x400000003f000000, 0),
        ("fpamin.s2", 0xc00000003f800000, 0x3f800000bf000000,
         0x3f800000bf000000, 0),
        ("fpamax.s3", 0xc00000003f800000, 0x3f800000bf000000,
         0xc00000003f800000, 0),
        ("fpcmp.eq.s0", 0x3f8000007fc00000, 0x3f8000003f800000,
         0xffffffff00000000, 0),
        ("fpcmp.lt.s0", 0x3f800000bf800000, 0x400000003f800000,
         0xffffffffffffffff, 0),
        ("fpcmp.le.s1", 0x3f8000003f800000, 0x3f80000040000000,
         0xffffffffffffffff, 0),
        ("fpcmp.unord.s2", 0x7fc000003f800000, 0x3f8000003f800000,
         0xffffffff00000000, 0),
        ("fpcmp.neq.s3", 0x7fc000003f800000, 0x3f8000003f800000,
         0xffffffff00000000, 0),
        ("fpcmp.nlt.s0", 0x7fc0000040000000, 0x3f8000003f800000,
         0xffffffff00000000, 1),
        ("fpcmp.nle.s0", 0x7fc0000040000000, 0x3f8000003f800000,
         0xffffffffffffffff, 1),
        ("fpcmp.ord.s0", 0x7fc000003f800000, 0x3f8000003f800000,
         0x00000000ffffffff, 0),
        # Ordered qNaN in high lane raises masked V; denormal low lane is
        # short-circuited independently and raises D.
        ("fpcmp.lt.s0", 0x7fc0000000000001, 0x3f8000003f800000,
         0x0000000000000000, 3),
    ]
    for mnemonic, av, bv, expected_sig, flags in packed:
        setup_case(6, (av, 0x1003e), (bv, 0x1003e))
        emit("f", f"{mnemonic} f6=f7,f8")
        spill_check(6, (expected_sig, 0x1003e))
        emit("m", "mov r24=psr")
        emit("i", "and r24=0x30,r24")
        compare(24, 0x10)
        emit("m", "mov r24=ar.fpsr")
        sf = int(mnemonic.rsplit("s", 1)[1])
        compare(24, FPSR_MASKED | (flags << (13 + 13 * sf)))

    # False qualification suppresses illegal destination and all source events.
    count += 1
    literal(9, count)
    fill(7, QNaN)
    fill(8, UNORM)
    fill(6, (0x1234, 0x23456))
    literal(20, 0)
    emit("m", "mov ar.fpsr=r20")
    emit("i", "cmp.eq p4,p5=r0,r0")
    emit("f", "(p5) fmax.s0 f1=f7,f8")
    spill_check(6, (0x1234, 0x23456))
    emit("m", "mov r24=ar.fpsr")
    compare(24, 0)

    literal(8, "0x66387061")
    emit("m", "break.m 0")
    lines.append("pass_spin:")
    emit("b", "br.cond.sptk pass_spin")
    lines.append("fail:")
    literal(8, "0x66386661")
    emit("m", "break.m 0")
    lines.append("fail_spin:")
    emit("b", "br.cond.sptk fail_spin")
    lines += [".data", ".align 16", "output:", ".quad 0,0"]
    for label, pair in data:
        lines += [".align 16", label + ":", f".quad {pair[0]:#x}, {pair[1]:#x}"]
    return "\n".join(lines) + "\n", count


def main():
    runner.run_generated_guest("F8", generate, 16, PASS, FAIL)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError, __import__("subprocess").SubprocessError) as exc:
        sys.exit(str(exc))
