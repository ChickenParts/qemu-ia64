#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-IVT F12 reserved-field and F15 break.f tests."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    'ia64_fp_ivt', ROOT / 'scripts/run-ia64-fp-exception-tests.py')
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT_OFFSET = 0x400000
PASS, FAIL = '0x66313570', '0x66313566'
GENERAL = 0x5400
BREAK = 0x2c00
EI_MASK = 3 << 41
F_EI = 2 << 41


def skip_faulting_bundle(lines, after):
    literal(lines, 14, after)
    emit(lines, 'm', 'mov cr.iip=r14')
    emit(lines, 'm', 'mov r9=cr.ipsr')
    literal(lines, 14, hex(EI_MASK))
    emit(lines, 'i', 'andcm r9=r9,r14')
    emit(lines, 'm', 'mov cr.ipsr=r9')
    emit(lines, 'b', 'rfi')


def check_fault_location(lines, vector, fault_label):
    compare(lines, 12, hex(vector))
    emit(lines, 'm', 'mov r9=cr.iip')
    literal(lines, 14, fault_label)
    emit(lines, 'i', 'cmp.eq p6,p7=r9,r14')
    emit(lines, 'b', '(p7) br.cond.spnt fail')
    emit(lines, 'm', 'mov r9=cr.ipsr')
    literal(lines, 14, hex(EI_MASK))
    emit(lines, 'i', 'and r9=r9,r14')
    compare(lines, 9, hex(F_EI))


def generate():
    lines = ['.text', '.explicit', '.align 16', '.global _start', '_start:']
    literal(lines, 14, 'ivt_base')
    emit(lines, 'm', 'mov cr.iva=r14')
    emit(lines, 'm', 'srlz.i')
    literal(lines, 15, 0)

    # F12: reserved PC=1 must fault before modifying FPSR.
    literal(lines, 20, '0x3f')
    emit(lines, 'm', 'mov ar.fpsr=r20')
    literal(lines, 14, 'fsetc_handler')
    emit(lines, 'i', 'mov b6=r14')
    lines.append('fsetc_fault:')
    emit(lines, 'f', 'fsetc.s1 0x0,0x4')
    lines.append('fsetc_after:')
    compare(lines, 15, 1)
    emit(lines, 'm', 'mov r9=ar.fpsr')
    compare(lines, 9, '0x3f')
    emit(lines, 'b', 'br.cond.sptk break_zero_case')

    lines.append('fsetc_handler:')
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    check_fault_location(lines, GENERAL, 'fsetc_fault')
    emit(lines, 'm', 'mov r9=cr.isr')
    compare(lines, 9, hex(F_EI | 0x30))
    emit(lines, 'm', 'mov r9=ar.fpsr')
    compare(lines, 9, '0x3f')
    skip_faulting_bundle(lines, 'fsetc_after')

    # F15: all-zero break.f 0 must not be treated as an empty F slot.
    lines.append('break_zero_case:')
    literal(lines, 14, 'break_zero_handler')
    emit(lines, 'i', 'mov b6=r14')
    lines.append('break_zero_fault:')
    emit(lines, 'f', 'break.f 0')
    lines.append('break_zero_after:')
    compare(lines, 15, 2)
    emit(lines, 'b', 'br.cond.sptk break_nonzero_case')

    lines.append('break_zero_handler:')
    compare(lines, 15, 1)
    literal(lines, 15, 2)
    check_fault_location(lines, BREAK, 'break_zero_fault')
    emit(lines, 'm', 'mov r9=cr.iim')
    compare(lines, 9, 0)
    skip_faulting_bundle(lines, 'break_zero_after')

    # Nonzero F15 immediate must land in CR.IIM unchanged.
    lines.append('break_nonzero_case:')
    literal(lines, 14, 'break_nonzero_handler')
    emit(lines, 'i', 'mov b6=r14')
    lines.append('break_nonzero_fault:')
    emit(lines, 'f', 'break.f 0x12345')
    lines.append('break_nonzero_after:')
    compare(lines, 15, 3)
    emit(lines, 'b', 'br.cond.sptk break_false_qp_case')

    lines.append('break_nonzero_handler:')
    compare(lines, 15, 2)
    literal(lines, 15, 3)
    check_fault_location(lines, BREAK, 'break_nonzero_fault')
    emit(lines, 'm', 'mov r9=cr.iim')
    compare(lines, 9, '0x12345')
    skip_faulting_bundle(lines, 'break_nonzero_after')

    # False qp suppresses break.f completely.
    lines.append('break_false_qp_case:')
    emit(lines, 'i', 'cmp.eq p4,p5=r0,r0')
    emit(lines, 'f', '(p5) break.f 0x55')
    compare(lines, 15, 3)

    lines.append('pass:')
    ivt.terminal(lines, PASS, FAIL)

    # Real vector stubs dispatch to the case-specific b6 handler.
    lines += [f'.org {IVT_OFFSET}', 'ivt_base:']
    for vector in (BREAK, GENERAL):
        lines.append(f'.org {IVT_OFFSET + vector}')
        literal(lines, 12, hex(vector))
        emit(lines, 'b', 'br.cond.sptk b6')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default='./build/qemu-system-ia64')
    parser.add_argument('--out', type=Path, default=Path('scratch/ia64-f12-f15-exceptions'))
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--assemble-only', action='store_true')
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error('--timeout must be in (0,60]')
    args.out = args.out.resolve()
    evidence = ivt.run_one(
        'f12-f15-ivt', args,
        case_spec=(BREAK, PASS, FAIL), source=generate())
    evidence.update(cases=4, expected_ivt_entries=3,
                    vectors=['0x2c00', '0x5400'],
                    controls=['reserved fsetc non-commit',
                              'break.f 0', 'break.f immediate',
                              'false-qp break.f suppression'])
    result = args.out / 'f12-f15-ivt' / 'result.json'
    result.write_text(json.dumps(evidence, indent=2) + '\n')
    print('F12/F15 IVT ' +
          ('assembly' if args.assemble_only else 'execution') +
          ' PASS: reserved fsetc + break.f zero/nonzero/predication')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
