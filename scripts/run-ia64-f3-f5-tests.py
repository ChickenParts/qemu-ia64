#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free system-mode F3 fselect and F5 fclass execution tests.

Checks real decoded instructions, raw FR results, rotating/high operands,
predication, predicate destinations, dirty state and unchanged FPSR.
Exception delivery is covered by the companion register-fault IVT suites.
"""
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    'ia64_f9_runner', ROOT / 'scripts/run-ia64-f9-tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

PASS = 'r8=0000000066357061'
FAIL = 'r8=0000000066356661'
NAT = (0, 0x1fffe)
ONE = (1 << 63, 0xffff)
MASK64 = (1 << 64) - 1


def select(a, b, selector):
    if NAT in (a, b, selector):
        return NAT
    return ((a[0] & selector[0]) | (b[0] & ~selector[0] & MASK64), 0x1003e)


def generate():
    lines = ['.text', '.explicit', '.align 16', '.global _start', '_start:']
    data = []
    count = 0
    assertion = 0

    def emit(kind, insn):
        runner_emit = {
            'm': ['.mii', insn, 'nop.i 0', 'nop.i 0'],
            'i': ['.mii', 'nop.m 0', insn, 'nop.i 0'],
            'f': ['.mmf', 'nop.m 0', 'nop.m 0', insn],
            'b': ['.mib', 'nop.m 0', 'nop.i 0', insn],
            'l': ['.mlx', 'nop.m 0', insn],
        }
        parts = runner_emit[kind]
        lines.extend(['{ ' + parts[0], *parts[1:], ';;', '}'])

    def literal(reg, value):
        emit('l', f'movl r{reg}={value}')

    def compare(reg, value):
        nonlocal assertion
        assertion += 1
        literal(10, assertion)  # persistent assertion ID in break log
        literal(18, hex(value) if isinstance(value, int) else value)
        emit('i', f'cmp.eq p6,p7=r{reg},r18')
        emit('b', '(p7) br.cond.spnt fail')

    def fill(reg, value):
        label = f'input_{len(data)}'
        data.append((label, value))
        literal(14, label)
        emit('m', f'ldf.fill f{reg}=[r14]')

    def spill_check(reg, expected):
        literal(14, 'output')
        emit('m', f'stf.spill [r14]=f{reg}')
        emit('m', 'ld8 r16=[r14],8')
        emit('m', 'ld8 r17=[r14]')
        compare(16, expected[0])
        compare(17, expected[1])

    patterns = [
        ((0x0123456789abcdef, 0x255aa),
         (0xfedcba9876543210, 0x12345),
         (0xaa55aa55aa55aa55, 0x1003e)),
        ((0xffff0000ffff0000, 0x2003e),
         (0x0000ffff0000ffff, 0x1003e),
         (0x0f0ff0f00f0ff0f0, 0x3003e)),
        (NAT, (0x1234, 0x1003e), (0x5678, 0x1003e)),
        ((0x1234, 0x1003e), NAT, (0x5678, 0x1003e)),
        ((0x1234, 0x1003e), (0x5678, 0x1003e), NAT),
    ]
    profiles = [
        (6, 7, 8, 9),
        (32, 31, 126, 127),
        (6, 0, 8, 9),
        (6, 7, 0, 1),
        (8, 7, 8, 9),
    ]

    for a0, b0, selector0 in patterns:
        for f1, f2, f3, f4 in profiles:
            a = (0, 0) if f3 == 0 else ONE if f3 == 1 else a0
            b = (0, 0) if f4 == 0 else ONE if f4 == 1 else b0
            selector = (0, 0) if f2 == 0 else ONE if f2 == 1 else selector0
            for false_predicate in (False, True):
                count += 1
                lines.append(f'case_select_{count}:')
                literal(9, count)  # persistent case ID in break log
                sentinel = (0x456789abcdef0123, 0x24567)
                if f1 not in (f2, f3, f4):
                    fill(f1, sentinel)
                for reg, value in ((f2, selector), (f3, a), (f4, b)):
                    # Sources must be initialized even when the destination
                    # aliases one of them; the helper snapshots before write.
                    if reg > 1:
                        fill(reg, value)
                emit('m', 'rsm 0x30')
                emit('m', 'srlz.d')
                emit('m', 'mov r25=ar.fpsr')
                emit('i', 'cmp.eq p4,p5=r0,r0')
                emit('f', f'(p{5 if false_predicate else 0}) fselect f{f1}=f{f3},f{f4},f{f2}')
                emit('m', 'mov r24=psr')
                emit('i', 'and r24=0x30,r24')
                compare(24, 0 if false_predicate else (0x10 if f1 < 32 else 0x20))
                emit('m', 'mov r24=ar.fpsr')
                emit('i', 'cmp.eq p6,p7=r24,r25')
                emit('b', '(p7) br.cond.spnt fail')
                expected = sentinel if false_predicate and f1 not in (f2, f3, f4) else (
                    a if false_predicate and f1 == f3 else
                    b if false_predicate and f1 == f4 else
                    selector if false_predicate and f1 == f2 else
                    select(a, b, selector))
                spill_check(f1, expected)

    classes = [
        ((0, 0), 0x005, 1),                    # +zero
        ((0, 0x20000), 0x006, 1),              # -zero
        ((0x4000000000000000, 0xffff), 0x009, 1), # +unnormal
        ((0, 0x1003e), 0x009, 1),                 # +pseudo-zero => unnormal
        (ONE, 0x011, 1),                       # +normal
        ((1 << 63, 0x1ffff), 0x021, 1),        # +infinity
        ((0x8000000000000001, 0x1ffff), 0x040, 1), # sNaN
        ((0xc000000000000001, 0x1ffff), 0x080, 1), # qNaN
        (NAT, 0x100, 1),                       # NaT member
        (NAT, 0x005, 0),                       # NaT nonmember clears both
        ((0x4000000000000000, 0x1ffff), 0x1ff, 2), # unsupported -> false
        (ONE, 0x012, 2),                       # +normal, negative only -> false
    ]

    for idx, (value, mask, pred_bits) in enumerate(classes):
        for reg in (8, 126):
            count += 1
            lines.append(f'case_class_{idx}_{reg}:')
            literal(9, count)  # persistent case ID in break log
            fill(reg, value)
            emit('m', 'rsm 0x30')
            emit('m', 'srlz.d')
            emit('m', 'mov r25=ar.fpsr')
            emit('i', 'cmp.eq p6,p7=r0,r0')
            emit('f', f'fclass.m p6,p7=f{reg},{mask}')
            emit('i', 'mov r24=pr')
            emit('i', 'shr.u r24=r24,6')
            emit('i', 'and r24=3,r24')
            compare(24, pred_bits)
            emit('m', 'mov r24=psr')
            emit('i', 'and r24=0x30,r24')
            compare(24, 0)
            emit('m', 'mov r24=ar.fpsr')
            emit('i', 'cmp.eq p6,p7=r24,r25')
            emit('b', '(p7) br.cond.spnt fail')

    # Normal false predicate leaves destinations unchanged; .unc clears both.
    fill(8, ONE)
    for unc, expected in ((False, 1), (True, 0)):
        count += 1
        literal(9, count)  # persistent case ID in break log
        emit('i', 'cmp.eq p4,p5=r0,r0')       # p5=false
        emit('i', 'cmp.eq p6,p7=r0,r0')       # p6=1,p7=0
        suffix = '.unc' if unc else ''
        emit('f', f'(p5) fclass.m{suffix} p6,p7=f8,0x11')
        emit('i', 'mov r24=pr')
        emit('i', 'shr.u r24=r24,6')
        emit('i', 'and r24=3,r24')
        compare(24, expected)

    literal(8, '0x66357061')
    emit('m', 'break.m 0')
    lines.append('pass_spin:')
    emit('b', 'br.cond.sptk pass_spin')
    lines.append('fail:')
    literal(8, '0x66356661')
    emit('m', 'break.m 0')
    lines.append('fail_spin:')
    emit('b', 'br.cond.sptk fail_spin')
    lines += ['.data', '.align 16', 'output:', '.quad 0,0']
    for label, pair in data:
        lines += ['.align 16', label + ':', f'.quad {pair[0]:#x}, {pair[1]:#x}']
    return '\n'.join(lines) + '\n', count


def main():
    runner.run_generated_guest('F3F5', generate, 2, PASS, FAIL)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError, __import__('subprocess').SubprocessError) as exc:
        sys.exit(str(exc))
