#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute generated F10 masked-event cases with an independent Fraction oracle.

Both result words, selected FPSR sticky flags, other FPSR fields, PSR dirty bits,
constant sources, high FRs, aliases and false predication are guest-checked.
This suite does not certify delivery of enabled FP exceptions.
"""
import importlib.util
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/ia64/isa'))
import test_f10 as ORACLE

spec = importlib.util.spec_from_file_location('f9_runner', ROOT / 'scripts/run-ia64-f9-tests.py')
RUNNER = importlib.util.module_from_spec(spec)
spec.loader.exec_module(RUNNER)


def generate():
    lines = ['.text', '.explicit', '.align 16', '.global _start', '_start:']
    data = []
    count = 0

    def emit(kind, insn):
        choices = {'m': ['.mii', insn, 'nop.i 0', 'nop.i 0'],
                   'i': ['.mii', 'nop.m 0', insn, 'nop.i 0'],
                   'f': ['.mmf', 'nop.m 0', 'nop.m 0', insn],
                   'b': ['.mib', 'nop.m 0', 'nop.i 0', insn],
                   'l': ['.mlx', 'nop.m 0', insn]}
        parts = choices[kind]
        lines.extend(['{ ' + parts[0], *parts[1:], ';;', '}'])

    def literal(reg, value):
        emit('l', f'movl r{reg}={value}')

    def fill(reg, value):
        label = 'input_' + str(len(data))
        data.append((label, value))
        literal(14, label)
        emit('m', f'ldf.fill f{reg}=[r14]')

    def compare(reg, expected):
        literal(18, hex(expected))
        emit('i', f'cmp.eq p6,p7=r{reg},r18')
        emit('b', '(p7) br.cond.spnt fail')

    for name, op in ORACLE.OPS.items():
        patterns = ORACLE.PACKED_PATTERNS if op & 4 else ORACLE.SCALAR_PATTERNS
        for sf in range(4):
            for rc in range(4):
                for index, value in enumerate(patterns + [(0, 0), (1 << 63, 0xffff)]):
                    # Every numerical case executes. Alias/high/constant profiles
                    # are distributed independently of rounding/status selection.
                    f1, f2 = ((6, 7), (7, 7), (126, 125), (127, 127))[index % 4]
                    if index == len(patterns):
                        f1, f2 = 6, 0
                    elif index == len(patterns) + 1:
                        f1, f2 = 126, 1
                    for false_predicate in (False, True):
                        count += 1
                        lines.append(f'case_{count}: /* {name}.s{sf}, rc={rc}, source={index}, false={false_predicate} */')
                        literal(9, count)
                        sentinel = (0x456789abcdef0123, 0x24567)
                        if f1 != f2:
                            fill(f1, sentinel)
                        if f2 > 1:
                            fill(f2, value)
                        # Exercise td as well as the global masks, retaining
                        # unrelated fields and pre-existing sticky flags.
                        td = sf != 0 and index % 2 == 1
                        initial = ORACLE.fpsr_for(sf, rc, disabled=0 if td else 63, td=td,
                                                 sticky=4 << (13 + 13 * ((sf + 1) % 4)))
                        literal(25, hex(initial))
                        emit('m', 'mov ar.fpsr=r25')
                        emit('m', 'rsm 0x30')
                        emit('m', 'srlz.d')
                        emit('i', 'cmp.eq p4,p5=r0,r0')
                        emit('f', f'(p{5 if false_predicate else 0}) {name}.s{sf} f{f1}=f{f2}')
                        emit('m', 'mov r24=psr')
                        emit('i', 'and r24=0x30,r24')
                        compare(24, 0 if false_predicate else (16 if f1 < 32 else 32))
                        expected, flags, enabled = ORACLE.oracle(op, value, initial, sf)
                        assert enabled == 0
                        if false_predicate:
                            expected = value if f1 == f2 else sentinel
                            flags = 0
                        emit('m', 'mov r24=ar.fpsr')
                        compare(24, initial | flags << (13 + 13 * sf))
                        literal(14, 'output')
                        emit('m', f'stf.spill [r14]=f{f1}')
                        emit('m', 'ld8 r16=[r14],8')
                        emit('m', 'ld8 r17=[r14]')
                        compare(16, expected[0])
                        compare(17, expected[1])
    literal(8, '0x66313070')
    emit('m', 'break.m 0')
    lines.append('pass_spin:')
    emit('b', 'br.cond.sptk pass_spin')
    lines.append('fail:')
    literal(8, '0x66313066')
    emit('m', 'break.m 0')
    lines.append('fail_spin:')
    emit('b', 'br.cond.sptk fail_spin')
    lines += ['.data', '.align 16', 'output:', '.quad 0,0']
    for label, pair in data:
        lines += ['.align 16', label + ':', f'.quad {pair[0]:#x}, {pair[1]:#x}']
    return '\n'.join(lines) + '\n', count


if __name__ == '__main__':
    try:
        RUNNER.run_generated_guest('F10', generate, len(ORACLE.OPS),
                                   'r8=0000000066313070', 'r8=0000000066313066')
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
