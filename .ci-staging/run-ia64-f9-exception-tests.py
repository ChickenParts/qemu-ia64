#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free F9/F10 register-fault IVT, priority and predication tests.

The guest checks architectural PSR bits 18/19 independently of cpu.h. Each
faulting operation has its own handler; the real IVT dispatches to it through
b6. A handler checks the complete ISR, saved IIP/RI, unchanged destination,
FPSR and dirty state, then retries a disabled operation exactly once or skips
an illegal target. Both legal F-slot positions are exercised.

The existing F9 bit-string oracle supplies results only, never bank masks,
exception codes or restart expectations. No firmware or ROM is used.
"""
import argparse
from dataclasses import dataclass
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/ia64/isa'))
from test_f9 import OPS, NAT, oracle  # noqa: E402

spec = importlib.util.spec_from_file_location(
    'ia64_fp_ivt', ROOT / 'scripts/run-ia64-fp-exception-tests.py')
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

# Intel SDM Vol. 2 PSR/ISR tables, deliberately not imported from cpu.h.
DFL, DFH, AC, DIRTY = 0x40000, 0x80000, 0x8, 0x30
IVT_OFFSET = 0x400000
FPSR = 63 | (1 << 13) | (1 << 26) | (1 << 39) | (1 << 52)
ONE = (1 << 63, 0xffff)
SENTINEL = (0x123456789abcdef0, 0x23456)
A, B = (0x89abcdef01234567, 0x255aa), (0x76543210fedcba98, 0x12345)
PASS, FAIL = '0x66396570', '0x66396566'


@dataclass(frozen=True)
class Case:
    form: str
    scenario: str
    regs: tuple
    mask: int
    enabled: bool = True
    nat: bool = False
    slot: int = 2

    @property
    def code(self):
        if not self.enabled or self.regs[0] < 2:
            return 0
        low = bool(self.mask & DFL and any(2 <= f < 32 for f in self.regs))
        high = bool(self.mask & DFH and any(f >= 32 for f in self.regs))
        return int(low) | (int(high) << 1)

    @property
    def vector(self):
        if not self.enabled:
            return 0
        if self.regs[0] < 2:
            return 0x5400
        return 0x5500 if self.code else 0


def cases():
    # No full numeric cross-family claim: F10 cases below exercise its shared
    # register-fault boundary, with an exact scalar conversion as the payload.
    profiles = [
        ('low-all', (6, 7, 8), DFL, True, False),
        ('high-all', (125, 126, 127), DFH, True, False),
        ('low-source-2', (32, 31, 33), DFL, True, False),
        ('low-source-3', (32, 33, 31), DFL, True, False),
        ('high-source-2', (31, 32, 7), DFH, True, False),
        ('high-source-3', (31, 7, 32), DFH, True, False),
        ('low-target', (31, 32, 33), DFL, True, False),
        ('high-target', (32, 30, 31), DFH, True, False),
        ('mixed', (32, 31, 127), DFL | DFH, True, False),
        ('constants-exempt-low', (32, 0, 1), DFL, True, False),
        ('constants-exempt-high', (31, 0, 1), DFH, True, False),
        ('nat-after-disable', (32, 31, 127), DFL | DFH, True, True),
        ('false-disabled', (32, 31, 127), DFL | DFH, False, True),
        ('illegal-f0-priority', (0, 31, 32), DFL | DFH, True, True),
        ('illegal-f1-priority', (1, 31, 32), DFL | DFH, True, True),
        ('false-illegal-f0', (0, 31, 32), DFL | DFH, False, True),
        ('false-illegal-f1', (1, 31, 32), DFL | DFH, False, True),
        ('ac-is-not-dfl', (6, 7, 8), AC, True, False),
        ('all-alias', (32, 32, 32), DFH, True, False),
    ]
    result = []
    for form in OPS:
        for scenario, regs, mask, enabled, nat in profiles:
            result.append(Case(form, scenario, regs, mask, enabled, nat,
                               1 + len(result) % 2))
    for scenario, regs, mask, enabled, nat in profiles:
        # F10 has only one source; skip tests whose sole disabled operand would
        # be f3, and use finite exact sources (NaT still tests early priority).
        if scenario in ('low-source-3', 'high-source-3'):
            continue
        result.append(Case('fcvt.fx.s0', scenario, regs[:2], mask, enabled,
                           nat, 1 + len(result) % 2))
    return result


def initial_registers(case):
    regs = {0: (0, 0), 1: ONE}
    destination, *sources = case.regs
    if destination > 1:
        regs[destination] = SENTINEL
    for i, source in enumerate(sources):
        if source > 1:
            regs[source] = NAT if case.nat else ONE if len(case.regs) == 2 else (A, B)[i]
    return regs


def check_fr(lines, register, bits):
    emit(lines, 'm', f'getf.sig r9=f{register}')
    compare(lines, 9, hex(bits[0]))
    emit(lines, 'm', f'getf.exp r9=f{register}')
    compare(lines, 9, hex(bits[1]))


def check_fpsr(lines):
    emit(lines, 'm', 'mov r9=ar.fpsr')
    compare(lines, 9, hex(FPSR))


def clear_live_disables(lines):
    emit(lines, 'm', f'rsm {DFL | DFH | AC:#x}')
    emit(lines, 'm', 'srlz.d')


def generate(selected):
    lines = ['.text', '.explicit', '.align 16', '.global _start', '_start:']
    data = []
    literal(lines, 14, 'ivt_base')
    emit(lines, 'm', 'mov cr.iva=r14')
    emit(lines, 'm', 'srlz.i')
    for index, case in enumerate(selected):
        prefix = f'case_{index}'
        initial = initial_registers(case)
        destination = case.regs[0]
        lines += [prefix + ':', f'// {case.form} {case.scenario} slot {case.slot}']
        clear_live_disables(lines)
        for reg, bits in sorted(initial.items()):
            if reg < 2:
                continue
            label = f'{prefix}_f{reg}'
            literal(lines, 14, label)
            emit(lines, 'm', f'ldf.fill f{reg}=[r14]')
            data += ['.align 16', label + ':', f'.quad {bits[0]:#x}, {bits[1]:#x}']
        literal(lines, 14, hex(FPSR))
        emit(lines, 'm', 'mov ar.fpsr=r14')
        literal(lines, 14, prefix + '_handler')
        emit(lines, 'i', 'mov b6=r14')
        literal(lines, 11, index)  # diagnostic case index, unbanked GR
        literal(lines, 15, 0)     # actual fault count, unbanked GR
        emit(lines, 'm', 'rsm 0x30')
        if case.mask:
            emit(lines, 'm', f'ssm {case.mask:#x}')
        emit(lines, 'm', 'srlz.d')
        emit(lines, 'i', f'cmp.{"eq" if case.enabled else "ne"} p6,p7=r0,r0')
        args = ','.join(f'f{f}' for f in case.regs[1:])
        instruction = f'(p6) {case.form} f{destination}={args}'
        lines.append(prefix + '_fault:')
        if case.slot == 2:
            emit(lines, 'f', instruction)
        else:
            lines += ['{ .mfi', 'nop.m 0', instruction, 'nop.i 0', ';;', '}']
        lines.append(prefix + '_after:')
        compare(lines, 15, int(bool(case.vector)))
        clear_live_disables(lines)
        expected = initial[destination]
        commits = case.enabled and destination > 1
        if commits:
            if len(case.regs) == 3:
                expected = oracle(case.form, initial[case.regs[1]], initial[case.regs[2]])
            else:
                source = initial[case.regs[1]]
                expected = NAT if source == NAT else (0 if case.regs[1] == 0 else 1, 0x1003e)
        check_fr(lines, destination, expected)
        check_fr(lines, 0, (0, 0))
        check_fr(lines, 1, ONE)
        check_fpsr(lines)
        emit(lines, 'm', 'mov r9=psr')
        emit(lines, 'i', 'and r9=0x30,r9')
        compare(lines, 9, (16 if destination < 32 else 32) if commits else 0)
        emit(lines, 'b', f'br.cond.sptk {prefix}_done')
        lines.append(prefix + '_handler:')
        if not case.vector:
            emit(lines, 'b', 'br.cond.sptk fail')
        else:
            compare(lines, 12, hex(case.vector))
            compare(lines, 15, 0)
            emit(lines, 'i', 'adds r15=1,r15')
            emit(lines, 'm', 'mov r9=cr.isr')
            compare(lines, 9, hex((case.slot << 41) | case.code))
            emit(lines, 'm', 'mov r9=cr.iip')
            compare(lines, 9, prefix + '_fault')
            emit(lines, 'm', 'mov r9=cr.ipsr')
            literal(lines, 14, hex((3 << 41) | DFL | DFH | AC | DIRTY))
            emit(lines, 'i', 'and r9=r9,r14')
            compare(lines, 9, hex((case.slot << 41) | case.mask))
            # This is live state only: saved IPSR above still describes the
            # faulting operation. No FP write is made by the handler.
            clear_live_disables(lines)
            for reg, bits in sorted(initial.items()):
                check_fr(lines, reg, bits)
            check_fpsr(lines)
            emit(lines, 'm', 'mov r9=cr.ipsr')
            clear = DFL | DFH
            if case.vector == 0x5400:
                # Illegal output is unrepairable; skip to a bundle boundary.
                literal(lines, 14, prefix + '_after')
                emit(lines, 'm', 'mov cr.iip=r14')
                clear |= 3 << 41
            literal(lines, 14, hex(clear))
            emit(lines, 'i', 'andcm r9=r9,r14')
            emit(lines, 'm', 'mov cr.ipsr=r9')
            # The handler's comparisons changed p6; restore the qualifying
            # predicate before retrying the original instruction.
            emit(lines, 'i', 'cmp.eq p6,p7=r0,r0')
            emit(lines, 'b', 'rfi')
        lines.append(prefix + '_done:')
    literal(lines, 11, len(selected))
    ivt.terminal(lines, PASS, FAIL)
    lines += [f'.org {IVT_OFFSET}', 'ivt_base:']
    for vector in (0x5400, 0x5500):
        lines.append(f'.org {IVT_OFFSET + vector}')
        literal(lines, 12, hex(vector))
        emit(lines, 'b', 'br.cond.sptk b6')
    lines += ['.data', *data]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default=os.environ.get('QEMU_BIN', './build/qemu-system-ia64'))
    parser.add_argument('--out', type=Path, default=Path('scratch/ia64-f9-exceptions'))
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--assemble-only', action='store_true')
    parser.add_argument('--family', choices=['f9', 'f10', 'all'], default='all')
    parser.add_argument('--scenario', help='Run one named scenario, including negative controls')
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error('--timeout must be in (0,60]')
    args.out = args.out.resolve()
    selected = [c for c in cases() if (args.family == 'all' or
                (args.family == 'f10') == (len(c.regs) == 2)) and
                (args.scenario is None or c.scenario == args.scenario)]
    if not selected:
        parser.error('no cases match the selected family/scenario')
    evidence = ivt.run_one('register-faults', args,
                          case_spec=(0x5500, PASS, FAIL), source=generate(selected))
    evidence.update(cases=len(selected),
                    disabled=sum(c.vector == 0x5500 for c in selected),
                    illegal=sum(c.vector == 0x5400 for c in selected),
                    suppressed=sum(not c.enabled for c in selected),
                    families=sorted({c.form for c in selected}),
                    expectations=[vars(c) | {'vector': c.vector, 'code': c.code}
                                  for c in selected])
    (args.out / 'register-faults' / 'result.json5').write_text(json.dumps(evidence, indent=2) + '\n')
    print(f'F9/F10 IVT register-fault {"assembly" if args.assemble_only else "execution"} PASS: '
          f'{len(selected)} cases, {evidence["disabled"]} disabled, {evidence["illegal"]} illegal, '
          f'{evidence["suppressed"]} false-predicate controls')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
