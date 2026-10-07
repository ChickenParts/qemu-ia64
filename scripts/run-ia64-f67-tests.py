#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free real F6/F7 reciprocal-approximation execution tests."""
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_f9_runner", ROOT / "scripts/run-ia64-f9-tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

PASS = "r8=0000000066377061"
FAIL = "r8=0000000066376661"
FPSR_MASKED = 0x3f
DFL = 0x40000
NAT = (0, 0x1fffe)
ZERO = (0, 0)
ONE = (1 << 63, 0xffff)
NEG_FOUR = (1 << 63, 0x30001)
UNORM = (1 << 62, 0xffff)
SEED = (0xff80000000000000, 0xfffe)
INDEFINITE = (0xc000000000000000, 0x3ffff)
PACKED_ONE = (0x3f8000003f800000, 0x1003e)
PACKED_NEG_ONE_HIGH = (0xbf8000003f800000, 0x1003e)
PACKED_SEED = (0x3f7f80003f7f8000, 0x1003e)
PACKED_INVALID_HIGH = (0xffc000003f7f8000, 0x1003e)
SENTINEL = (0x123456789abcdef0, 0x23456)


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

    def set_predicate(pred, value):
        other = 62 if pred == 63 else pred + 1
        relation = "eq" if value else "ne"
        emit("i", f"cmp.{relation} p{pred},p{other}=r0,r0")

    def check_predicate(pred, value):
        nonlocal assertion
        assertion += 1
        literal(10, assertion)
        if value:
            label = f"predicate_true_{assertion}"
            emit("b", f"(p{pred}) br.cond.sptk {label}")
            emit("b", "br.cond.sptk fail")
            lines.append(label + ":")
        else:
            emit("b", f"(p{pred}) br.cond.spnt fail")

    cases = [
        dict(mnemonic="frcpa.s0", dst=6, pred=8, f2=7, f3=8,
             a=ONE, b=ONE, expected=SEED, predicate=True, flags=0, flag_sf=0),
        dict(mnemonic="frsqrta.s3", dst=6, pred=8, f2=None, f3=8,
             a=None, b=ONE, expected=SEED, predicate=True, flags=0, flag_sf=3),
        dict(mnemonic="frcpa.s1", dst=6, pred=8, f2=7, f3=8,
             a=ZERO, b=ONE, expected=ZERO, predicate=False, flags=0, flag_sf=1),
        dict(mnemonic="frsqrta.s2", dst=6, pred=8, f2=None, f3=8,
             a=None, b=NEG_FOUR, expected=INDEFINITE, predicate=False,
             flags=1, flag_sf=2),
        dict(mnemonic="frcpa.s0", dst=6, pred=8, f2=7, f3=8,
             a=NAT, b=ONE, expected=NAT, predicate=False, flags=0, flag_sf=0),
        dict(mnemonic="frcpa.s1", dst=126, pred=63, f2=125, f3=124,
             a=ONE, b=ONE, expected=SEED, predicate=True, flags=0, flag_sf=1),
        dict(mnemonic="fprcpa.s0", dst=6, pred=8, f2=7, f3=8,
             a=PACKED_ONE, b=PACKED_ONE, expected=PACKED_SEED,
             predicate=True, flags=0, flag_sf=0),
        dict(mnemonic="fprsqrta.s3", dst=6, pred=8, f2=None, f3=8,
             a=None, b=PACKED_ONE, expected=PACKED_SEED,
             predicate=True, flags=0, flag_sf=0),
        dict(mnemonic="fprsqrta.s2", dst=6, pred=8, f2=None, f3=8,
             a=None, b=PACKED_NEG_ONE_HIGH, expected=PACKED_INVALID_HIGH,
             predicate=False, flags=1, flag_sf=0),
        dict(mnemonic="frcpa.s0", dst=7, pred=8, f2=7, f3=8,
             a=ONE, b=ONE, expected=SEED, predicate=True, flags=0, flag_sf=0),
        dict(mnemonic="frsqrta.s0", dst=8, pred=8, f2=None, f3=8,
             a=None, b=ONE, expected=SEED, predicate=True, flags=0, flag_sf=0),
        dict(mnemonic="frcpa.s0", dst=6, pred=8, f2=7, f3=8,
             a=UNORM, b=ONE, expected=SEED, predicate=True, flags=2, flag_sf=0),
    ]

    for case in cases:
        count += 1
        lines.append(f"case_{count}: /* {case['mnemonic']} */")
        literal(9, count)
        dst = case["dst"]
        f2 = case["f2"]
        f3 = case["f3"]
        if dst not in (f2, f3):
            fill(dst, SENTINEL)
        if f2 is not None:
            fill(f2, case["a"])
        if f3 != f2:
            fill(f3, case["b"])
        emit("m", "rsm 0x30")
        emit("m", "srlz.d")
        literal(20, hex(FPSR_MASKED))
        emit("m", "mov ar.fpsr=r20")
        set_predicate(case["pred"], not case["predicate"])
        if f2 is None:
            insn = f"{case['mnemonic']} f{dst},p{case['pred']}=f{f3}"
        else:
            insn = (f"{case['mnemonic']} f{dst},p{case['pred']}="
                    f"f{f2},f{f3}")
        emit("f", insn)
        emit("m", "mov r24=psr")
        emit("i", "and r24=0x30,r24")
        compare(24, 0x10 if dst < 32 else 0x20)
        emit("m", "mov r24=ar.fpsr")
        expected_fpsr = FPSR_MASKED | (
            case["flags"] << (13 + 13 * case["flag_sf"]))
        compare(24, expected_fpsr)
        check_predicate(case["pred"], case["predicate"])
        spill_check(dst, case["expected"])

    # False qualification still clears p2, while suppressing illegal-target,
    # disabled-register, source-event, destination, FPSR and dirty-state work.
    count += 1
    lines.append(f"case_{count}: /* false qualification */")
    literal(9, count)
    fill(7, NEG_FOUR)
    fill(8, ZERO)
    literal(20, 0)
    emit("m", "mov ar.fpsr=r20")
    emit("m", "rsm 0x30")
    emit("m", f"ssm {DFL:#x}")
    emit("m", "srlz.d")
    emit("i", "cmp.eq p4,p5=r0,r0")
    set_predicate(8, True)
    emit("f", "(p5) frcpa.s0 f1,p8=f7,f8")
    check_predicate(8, False)
    emit("m", "mov r24=ar.fpsr")
    compare(24, 0)
    emit("m", "mov r24=psr")
    emit("i", "and r24=0x30,r24")
    compare(24, 0)
    emit("m", f"rsm {DFL:#x}")
    emit("m", "srlz.d")

    literal(8, "0x66377061")
    emit("m", "break.m 0")
    lines.append("pass_spin:")
    emit("b", "br.cond.sptk pass_spin")
    lines.append("fail:")
    literal(8, "0x66376661")
    emit("m", "break.m 0")
    lines.append("fail_spin:")
    emit("b", "br.cond.sptk fail_spin")
    lines += [".data", ".align 16", "output:", ".quad 0,0"]
    for label, pair in data:
        lines += [".align 16", label + ":",
                  f".quad {pair[0]:#x}, {pair[1]:#x}"]
    return "\n".join(lines) + "\n", count


def main():
    runner.run_generated_guest("F67", generate, 4, PASS, FAIL)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError,
            __import__("subprocess").SubprocessError) as exc:
        sys.exit(str(exc))
