#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free system-mode F12/F13/F14 execution tests."""
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    'ia64_f9_runner', ROOT / 'scripts/run-ia64-f9-tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

PASS = 'r8=0000000066313570'
FAIL = 'r8=0000000066313566'


def set_controls(fpsr, sf, value):
    shift = 6 + 13 * sf
    return (fpsr & ~(0x7f << shift)) | ((value & 0x7f) << shift)


def set_flags(fpsr, sf, value):
    shift = 13 + 13 * sf
    return (fpsr & ~(0x3f << shift)) | ((value & 0x3f) << shift)


def base_fpsr():
    v = 0x3f
    controls = (0x22, 0x10, 0x20, 0x30)
    flags = (0x02, 0x02, 0x06, 0x18)
    for sf in range(4):
        v = set_controls(v, sf, controls[sf])
        v = set_flags(v, sf, flags[sf])
    return v


def generate():
    lines = ['.text', '.explicit', '.align 16', '.global _start', '_start:']
    count = 0
    assertion = 0

    def emit(kind, insn):
        layouts = {
            'm': ['.mii', insn, 'nop.i 0', 'nop.i 0'],
            'i': ['.mii', 'nop.m 0', insn, 'nop.i 0'],
            'f': ['.mmf', 'nop.m 0', 'nop.m 0', insn],
            'b': ['.mib', 'nop.m 0', 'nop.i 0', insn],
            'l': ['.mlx', 'nop.m 0', insn],
        }
        parts = layouts[kind]
        lines.extend(['{ ' + parts[0], *parts[1:], ';;', '}'])

    def literal(reg, value):
        emit('l', f'movl r{reg}={value}')

    def compare(reg, value):
        nonlocal assertion
        assertion += 1
        literal(10, assertion)
        literal(18, hex(value) if isinstance(value, int) else value)
        emit('i', f'cmp.eq p6,p7=r{reg},r18')
        emit('b', '(p7) br.cond.spnt fail')

    def set_fpsr(value):
        literal(20, hex(value))
        emit('m', 'mov ar.fpsr=r20')

    base = base_fpsr()
    source = (base >> 6) & 0x7f
    result_controls = (source & 0x3f) | 0x10

    # F12: each target field must derive from sf0 controls.
    for sf in range(4):
        count += 1
        literal(9, count)
        set_fpsr(base)
        emit('f', f'fsetc.s{sf} 0x3f,0x10')
        emit('m', 'mov r24=ar.fpsr')
        compare(24, set_controls(base, sf, result_controls))

    # False qualification suppresses F12 completely.
    count += 1
    literal(9, count)
    set_fpsr(base)
    emit('i', 'cmp.eq p4,p5=r0,r0')
    emit('f', '(p5) fsetc.s2 0x0,0x0')
    emit('m', 'mov r24=ar.fpsr')
    compare(24, base)

    # F13: only selected flags clear.
    for sf in range(4):
        count += 1
        literal(9, count)
        set_fpsr(base)
        emit('f', f'fclrf.s{sf}')
        emit('m', 'mov r24=ar.fpsr')
        compare(24, set_flags(base, sf, 0))

    # False qualification suppresses F13.
    count += 1
    literal(9, count)
    set_fpsr(base)
    emit('i', 'cmp.eq p4,p5=r0,r0')
    emit('f', '(p5) fclrf.s1')
    emit('m', 'mov r24=ar.fpsr')
    compare(24, base)

    # F14 no-branch: all traps disabled and sf1 flags are already in sf0.
    count += 1
    literal(9, count)
    v = set_flags(set_flags(base, 0, 0x02), 1, 0x02) | 0x3f
    set_fpsr(v)
    emit('f', 'fchkf.s1 fail')

    # F14 taken because flag bit 1 is trap-enabled (disable bit cleared).
    count += 1
    literal(9, count)
    set_fpsr(v & ~0x02)
    emit('f', 'fchkf.s1 fchkf_enabled_taken')
    emit('b', 'br.cond.sptk fail')
    lines.append('fchkf_enabled_taken:')

    # F14 taken because sf1 has a flag absent from sf0, even with all traps disabled.
    count += 1
    literal(9, count)
    v2 = set_flags(set_flags(base | 0x3f, 0, 0x02), 1, 0x06)
    set_fpsr(v2)
    emit('f', 'fchkf.s1 fchkf_newflag_taken')
    emit('b', 'br.cond.sptk fail')
    lines.append('fchkf_newflag_taken:')

    # False qualification suppresses a branch whose FPSR condition is true.
    count += 1
    literal(9, count)
    set_fpsr(v & ~0x02)
    emit('i', 'cmp.eq p4,p5=r0,r0')
    emit('f', '(p5) fchkf.s1 fail')

    literal(8, '0x66313570')
    emit('m', 'break.m 0')
    lines.append('pass_spin:')
    emit('b', 'br.cond.sptk pass_spin')
    lines.append('fail:')
    literal(8, '0x66313566')
    emit('m', 'break.m 0')
    lines.append('fail_spin:')
    emit('b', 'br.cond.sptk fail_spin')
    return '\n'.join(lines) + '\n', count


def main():
    runner.run_generated_guest('F12F14', generate, 3, PASS, FAIL)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError, __import__('subprocess').SubprocessError) as exc:
        sys.exit(str(exc))
