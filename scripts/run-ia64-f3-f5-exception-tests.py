#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-IVT F3/F5 register-fault, priority and predication tests.

Covers F3 fselect target legality and four-operand disabled-bank checks, plus
F5 fclass disabled-source and equal-predicate-target faults.  The .unc false
predicate control proves that a disabled source is not checked when the
instruction only clears predicate destinations.
"""
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

DFL, DFH, DIRTY = 0x40000, 0x80000, 0x30
IVT_OFFSET = 0x400000
FPSR = 63 | (1 << 13) | (1 << 26) | (1 << 39) | (1 << 52)
PASS, FAIL = '0x66333570', '0x66333566'
SENTINEL = (0x123456789abcdef0, 0x23456)
A = (0xaaaaaaaa55555555, 0x1003e)
B = (0x55555555aaaaaaaa, 0x1003e)
SEL = (0xffff0000ffff0000, 0x1003e)
EXPECTED = ((A[0] & SEL[0]) | (B[0] & ~SEL[0] & ((1 << 64) - 1)), 0x1003e)


def fill(lines, data, reg, bits, label):
    data += ['.align 16', label + ':', f'.quad {bits[0]:#x}, {bits[1]:#x}']
    literal(lines, 14, label)
    emit(lines, 'm', f'ldf.fill f{reg}=[r14]')


def spill_check(lines, reg, bits):
    literal(lines, 14, 'output')
    emit(lines, 'm', f'stf.spill [r14]=f{reg}')
    emit(lines, 'm', 'ld8 r9=[r14],8')
    emit(lines, 'm', 'ld8 r10=[r14]')
    compare(lines, 9, hex(bits[0]))
    compare(lines, 10, hex(bits[1]))


def check_saved(lines, vector, code, fault_label, saved_mask=0, slot=2):
    compare(lines, 12, hex(vector))
    emit(lines, 'm', 'mov r9=cr.isr')
    compare(lines, 9, hex((slot << 41) | code))
    emit(lines, 'm', 'mov r9=cr.iip')
    literal(lines, 14, fault_label)
    emit(lines, 'i', 'cmp.eq p6,p7=r9,r14')
    emit(lines, 'b', '(p7) br.cond.spnt fail')
    emit(lines, 'm', 'mov r9=cr.ipsr')
    literal(lines, 14, hex((3 << 41) | DFL | DFH | DIRTY))
    emit(lines, 'i', 'and r9=r9,r14')
    compare(lines, 9, hex((slot << 41) | saved_mask))
    return slot


def clear_saved_disable_and_retry(lines, mask):
    emit(lines, 'm', 'mov r9=cr.ipsr')
    literal(lines, 14, hex(mask))
    emit(lines, 'i', 'andcm r9=r9,r14')
    emit(lines, 'm', 'mov cr.ipsr=r9')
    emit(lines, 'b', 'rfi')


def skip_faulting_bundle(lines, after):
    literal(lines, 14, after)
    emit(lines, 'm', 'mov cr.iip=r14')
    emit(lines, 'm', 'mov r9=cr.ipsr')
    literal(lines, 14, hex(3 << 41))
    emit(lines, 'i', 'andcm r9=r9,r14')
    emit(lines, 'm', 'mov cr.ipsr=r9')
    emit(lines, 'b', 'rfi')


def generate():
    lines = ['.text', '.explicit', '.align 16', '.global _start', '_start:']
    data = []
    literal(lines, 14, 'ivt_base')
    emit(lines, 'm', 'mov cr.iva=r14')
    emit(lines, 'm', 'srlz.i')
    literal(lines, 15, 0)  # total actual IVT entries, unbanked GR

    # F3: disabled low source among otherwise high operands; retry after DFL clear.
    fill(lines, data, 32, SENTINEL, 'sel_dst')
    fill(lines, data, 31, SEL, 'sel_mask')
    fill(lines, data, 33, A, 'sel_a')
    fill(lines, data, 34, B, 'sel_b')
    literal(lines, 20, hex(FPSR))
    emit(lines, 'm', 'mov ar.fpsr=r20')
    emit(lines, 'm', 'rsm 0x30')
    emit(lines, 'm', f'ssm {DFL:#x}')
    emit(lines, 'm', 'srlz.d')
    literal(lines, 14, 'fselect_disabled_handler')
    emit(lines, 'i', 'mov b6=r14')
    lines.append('fselect_disabled_fault:')
    emit(lines, 'f', 'fselect f32=f33,f34,f31')
    lines.append('fselect_disabled_after:')
    compare(lines, 15, 1)
    emit(lines, 'm', f'rsm {DFL:#x}')
    emit(lines, 'm', 'srlz.d')
    spill_check(lines, 32, EXPECTED)
    emit(lines, 'm', 'mov r9=psr')
    emit(lines, 'i', 'and r9=0x30,r9')
    compare(lines, 9, 0x20)
    emit(lines, 'm', 'mov r9=ar.fpsr')
    compare(lines, 9, hex(FPSR))
    emit(lines, 'b', 'br.cond.sptk case_fselect_illegal')

    lines.append('fselect_disabled_handler:')
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    check_saved(lines, 0x5500, 1, 'fselect_disabled_fault', DFL)
    spill_check(lines, 32, SENTINEL)
    emit(lines, 'm', 'mov r9=ar.fpsr')
    compare(lines, 9, hex(FPSR))
    clear_saved_disable_and_retry(lines, DFL)

    # F3: illegal destination f1 must precede any source evaluation.
    lines.append('case_fselect_illegal:')
    emit(lines, 'm', 'rsm 0x30')
    emit(lines, 'm', 'srlz.d')
    literal(lines, 14, 'fselect_illegal_handler')
    emit(lines, 'i', 'mov b6=r14')
    lines.append('fselect_illegal_fault:')
    emit(lines, 'f', 'fselect f1=f33,f34,f31')
    lines.append('fselect_illegal_after:')
    compare(lines, 15, 2)
    emit(lines, 'b', 'br.cond.sptk case_fclass_disabled')
    lines.append('fselect_illegal_handler:')
    compare(lines, 15, 1)
    literal(lines, 15, 2)
    check_saved(lines, 0x5400, 0, 'fselect_illegal_fault')
    skip_faulting_bundle(lines, 'fselect_illegal_after')

    # F5: disabled low source faults, retry writes predicate result.
    lines.append('case_fclass_disabled:')
    fill(lines, data, 8, (1 << 63, 0xffff), 'class_one')
    emit(lines, 'i', 'cmp.ne p6,p7=r0,r0')  # p6=0,p7=1 initially
    emit(lines, 'm', f'ssm {DFL:#x}')
    emit(lines, 'm', 'srlz.d')
    literal(lines, 14, 'fclass_disabled_handler')
    emit(lines, 'i', 'mov b6=r14')
    lines.append('fclass_disabled_fault:')
    emit(lines, 'f', 'fclass.m p6,p7=f8,0x11')
    lines.append('fclass_disabled_after:')
    compare(lines, 15, 3)
    emit(lines, 'm', f'rsm {DFL:#x}')
    emit(lines, 'm', 'srlz.d')
    emit(lines, 'i', 'mov r9=pr')
    emit(lines, 'i', 'shr.u r9=r9,6')
    emit(lines, 'i', 'and r9=3,r9')
    compare(lines, 9, 1)
    emit(lines, 'b', 'br.cond.sptk case_fclass_equal')
    lines.append('fclass_disabled_handler:')
    compare(lines, 15, 2)
    literal(lines, 15, 3)
    check_saved(lines, 0x5500, 1, 'fclass_disabled_fault', DFL)
    clear_saved_disable_and_retry(lines, DFL)

    # F5: equal predicate destinations are illegal when qualified.
    lines.append('case_fclass_equal:')
    literal(lines, 14, 'fclass_equal_handler')
    emit(lines, 'i', 'mov b6=r14')
    lines.append('fclass_equal_fault:')
    emit(lines, 'f', 'fclass.m p6,p6=f8,0x11')
    lines.append('fclass_equal_after:')
    compare(lines, 15, 4)
    emit(lines, 'b', 'br.cond.sptk case_unc_false')
    lines.append('fclass_equal_handler:')
    compare(lines, 15, 3)
    literal(lines, 15, 4)
    check_saved(lines, 0x5400, 0, 'fclass_equal_fault')
    skip_faulting_bundle(lines, 'fclass_equal_after')

    # .unc false qp: clear destinations but do not consult disabled f8.
    lines.append('case_unc_false:')
    emit(lines, 'i', 'cmp.eq p4,p5=r0,r0')  # p5=false
    emit(lines, 'i', 'cmp.eq p6,p7=r0,r0')  # p6=1,p7=0
    emit(lines, 'm', f'ssm {DFL:#x}')
    emit(lines, 'm', 'srlz.d')
    emit(lines, 'f', '(p5) fclass.m.unc p6,p7=f8,0x11')
    # Snapshot p6/p7 before compare(), whose cmp.eq scratch predicates are
    # themselves p6/p7 and would otherwise overwrite the value under test.
    emit(lines, 'i', 'mov r20=pr')
    emit(lines, 'i', 'shr.u r20=r20,6')
    emit(lines, 'i', 'and r20=3,r20')
    compare(lines, 15, 4)
    emit(lines, 'm', f'rsm {DFL:#x}')
    emit(lines, 'm', 'srlz.d')
    compare(lines, 20, 0)

    # Ordinary false predication suppresses even equal-target legality checks.
    emit(lines, 'i', 'cmp.eq p4,p5=r0,r0')
    emit(lines, 'i', 'cmp.eq p6,p7=r0,r0')
    emit(lines, 'm', f'ssm {DFL:#x}')
    emit(lines, 'm', 'srlz.d')
    emit(lines, 'f', '(p5) fclass.m p6,p6=f8,0x11')
    # Same snapshot rule: verify suppression before scratch compares touch p6/p7.
    emit(lines, 'i', 'mov r20=pr')
    emit(lines, 'i', 'shr.u r20=r20,6')
    emit(lines, 'i', 'and r20=3,r20')
    compare(lines, 15, 4)
    emit(lines, 'm', f'rsm {DFL:#x}')
    emit(lines, 'm', 'srlz.d')
    compare(lines, 20, 1)

    lines.append('pass:')
    ivt.terminal(lines, PASS, FAIL)

    # Real IVT dispatches both architectural vectors to the per-case b6 target.
    lines += [f'.org {IVT_OFFSET}', 'ivt_base:']
    for vector in (0x5400, 0x5500):
        lines.append(f'.org {IVT_OFFSET + vector}')
        literal(lines, 12, hex(vector))
        emit(lines, 'b', 'br.cond.sptk b6')

    lines += ['.data', '.align 16', 'output:', '.quad 0,0', *data]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default='./build/qemu-system-ia64')
    parser.add_argument('--out', type=Path, default=Path('scratch/ia64-f3-f5-exceptions'))
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--assemble-only', action='store_true')
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error('--timeout must be in (0,60]')
    args.out = args.out.resolve()
    evidence = ivt.run_one(
        'f3-f5-register-faults', args,
        case_spec=(0x5500, PASS, FAIL), source=generate())
    evidence.update(cases=6, expected_ivt_entries=4,
                    vectors=['0x5400', '0x5500'],
                    controls=['fclass.unc false qp', 'normal false qp equal target'])
    result = args.out / 'f3-f5-register-faults' / 'result.json'
    result.write_text(json.dumps(evidence, indent=2) + '\n')
    print('F3/F5 IVT register-fault ' +
          ('assembly' if args.assemble_only else 'execution') +
          ' PASS: 4 real faults, 2 suppressed controls')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
