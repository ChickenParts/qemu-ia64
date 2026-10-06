#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise F10 exception delivery through the real IA-64 IVT.

Each case installs cr.iva, deliberately triggers one F10 interruption, then
checks the saved CR.IIP/IPSR/ISR/IIPA state from the vector handler.  The trap
case additionally proves that architectural result/FPSR/dirty state committed
before entry while fault cases prove the opposite.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
TEXT = 0x5000000
IVT_OFFSET = 0x20000
IVT = TEXT + IVT_OFFSET
CASES = {
    'disabled': (0x5500, '0x66316470', '0x66316466'),
    'invalid': (0x5c00, '0x66316970', '0x66316966'),
    'inexact': (0x5d00, '0x66317470', '0x66317466'),
}


def emit(lines, kind, insn):
    layouts = {
        'm': ['.mii', insn, 'nop.i 0', 'nop.i 0'],
        'i': ['.mii', 'nop.m 0', insn, 'nop.i 0'],
        'f': ['.mmf', 'nop.m 0', 'nop.m 0', insn],
        'b': ['.mib', 'nop.m 0', 'nop.i 0', insn],
        'l': ['.mlx', 'nop.m 0', insn],
    }
    parts = layouts[kind]
    lines.extend(['{ ' + parts[0], *parts[1:], ';;', '}'])


def literal(lines, reg, value):
    emit(lines, 'l', f'movl r{reg}={value}')


def compare(lines, reg, value):
    literal(lines, 14, value)
    emit(lines, 'i', f'cmp.eq p6,p7=r{reg},r14')
    emit(lines, 'b', '(p7) br.cond.spnt fail')


def common_start(lines):
    literal(lines, 14, 'ivt_base')
    emit(lines, 'm', 'mov cr.iva=r14')
    emit(lines, 'm', 'srlz.i')


def check_fault_common(lines, expected_isr, fault_label):
    emit(lines, 'm', 'mov r9=cr.isr')
    compare(lines, 9, hex(expected_isr))
    emit(lines, 'm', 'mov r9=cr.iip')
    literal(lines, 14, fault_label)
    emit(lines, 'i', 'cmp.eq p6,p7=r9,r14')
    emit(lines, 'b', '(p7) br.cond.spnt fail')
    # Saved RI must identify slot 2.  Ignore unrelated interrupted PSR bits.
    emit(lines, 'm', 'mov r9=cr.ipsr')
    literal(lines, 14, hex(3 << 41))
    emit(lines, 'i', 'and r9=r14,r9')
    compare(lines, 9, hex(2 << 41))


def terminal(lines, pass_marker, fail_marker):
    literal(lines, 8, pass_marker)
    emit(lines, 'm', 'break.m 0')
    lines.append('pass_spin:')
    emit(lines, 'b', 'br.cond.sptk pass_spin')
    lines.append('fail:')
    literal(lines, 8, fail_marker)
    emit(lines, 'm', 'break.m 0')
    lines.append('fail_spin:')
    emit(lines, 'b', 'br.cond.sptk fail_spin')


def generate(case):
    vector, passed, failed = CASES[case]
    lines = ['.text', '.explicit', '.align 16', '.global _start', '_start:']
    common_start(lines)
    literal(lines, 15, 0)  # actual IVT-entry count; r15 is unbanked

    literal(lines, 14, 'source')
    emit(lines, 'm', 'ldf.fill f7=[r14]')
    if case == 'disabled':
        literal(lines, 20, '63')
        emit(lines, 'm', 'mov ar.fpsr=r20')
        emit(lines, 'm', 'rsm 0x10')
        emit(lines, 'm', 'ssm 0x40000')
        emit(lines, 'm', 'srlz.d')
        lines.append('fault_bundle:')
        emit(lines, 'f', 'fcvt.fx.s0 f6=f7')
        lines.append('after_retry:')
        compare(lines, 15, 1)
        emit(lines, 'm', 'getf.sig r9=f6')
        compare(lines, 9, '1')
        emit(lines, 'm', 'getf.exp r9=f6')
        compare(lines, 9, '0x1003e')
        emit(lines, 'm', 'mov r9=ar.fpsr')
        compare(lines, 9, '63')
        emit(lines, 'b', 'br.cond.sptk pass')
    elif case == 'invalid':
        literal(lines, 14, 'sentinel')
        emit(lines, 'm', 'ldf.fill f6=[r14]')
        literal(lines, 20, '62')
        emit(lines, 'm', 'mov ar.fpsr=r20')
        emit(lines, 'm', 'rsm 0x10')
        emit(lines, 'm', 'srlz.d')
        lines.append('fault_bundle:')
        emit(lines, 'f', 'fcvt.fx.s0 f6=f7')
        lines.append('after_retry:')
        compare(lines, 15, 1)
        emit(lines, 'm', 'getf.sig r9=f6')
        compare(lines, 9, '0x8000000000000000')
        emit(lines, 'm', 'getf.exp r9=f6')
        compare(lines, 9, '0x1003e')
        emit(lines, 'm', 'mov r9=ar.fpsr')
        compare(lines, 9, '0x203f')
        emit(lines, 'b', 'br.cond.sptk pass')
    else:
        literal(lines, 20, '31')
        emit(lines, 'm', 'mov ar.fpsr=r20')
        emit(lines, 'm', 'rsm 0x10')
        emit(lines, 'm', 'srlz.d')
        lines.append('fault_bundle:')
        emit(lines, 'f', 'fcvt.fx.s0 f6=f7')
        lines.append('after_trap:')
        compare(lines, 15, 1)
        emit(lines, 'b', 'br.cond.sptk pass')

    # Reserve an aligned IVT base and place this case's vector at base+offset.
    lines += [f'.org {IVT_OFFSET}', 'ivt_base:',
              f'.org {IVT_OFFSET + vector}', 'handler:']

    # A correct result alone cannot prove that the IVT handler executed.
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    ei = 2 << 41
    if case == 'disabled':
        # DFL is architectural bit 18. ISR.r/w/x are memory-access bits,
        # so all three are zero for this register-only conversion.
        check_fault_common(lines, ei | 1, 'fault_bundle')
        emit(lines, 'm', 'mov r9=cr.ipsr')
        literal(lines, 14, '0x40000')
        emit(lines, 'i', 'and r10=r14,r9')
        compare(lines, 10, '0x40000')
        # Clear DFL in the saved PSR, then rfi.  The fault must retry the
        # original F10 slot and fall through to after_retry.
        literal(lines, 14, '0x40000')
        emit(lines, 'i', 'andcm r9=r9,r14')
        emit(lines, 'm', 'mov cr.ipsr=r9')
        emit(lines, 'b', 'rfi')
    elif case == 'invalid':
        check_fault_common(lines, ei | 1, 'fault_bundle')
        emit(lines, 'm', 'getf.sig r9=f6')
        compare(lines, 9, '0x1234')
        emit(lines, 'm', 'getf.exp r9=f6')
        compare(lines, 9, '0x23456')
        emit(lines, 'm', 'mov r9=ar.fpsr')
        compare(lines, 9, '62')
        emit(lines, 'm', 'mov r9=cr.ipsr')
        emit(lines, 'i', 'and r10=0x10,r9')
        compare(lines, 10, '0')
        # Mask invalid in FPSR and restart the faulting conversion.  The retry
        # must commit the indefinite integer and sticky V flag.
        literal(lines, 20, '63')
        emit(lines, 'm', 'mov ar.fpsr=r20')
        emit(lines, 'b', 'rfi')
    else:
        # fp + I(high/scalar) + FPA(high/scalar), EI identifies slot 2.
        emit(lines, 'm', 'mov r9=cr.isr')
        compare(lines, 9, hex(ei | 1 | (1 << 13) | (1 << 14)))
        emit(lines, 'm', 'mov r9=cr.iipa')
        literal(lines, 14, 'fault_bundle')
        emit(lines, 'i', 'cmp.eq p6,p7=r9,r14')
        emit(lines, 'b', '(p7) br.cond.spnt fail')
        emit(lines, 'm', 'mov r9=cr.iip')
        literal(lines, 14, 'after_trap')
        emit(lines, 'i', 'cmp.eq p6,p7=r9,r14')
        emit(lines, 'b', '(p7) br.cond.spnt fail')
        emit(lines, 'm', 'mov r9=cr.ipsr')
        literal(lines, 14, hex((3 << 41) | 0x10))
        emit(lines, 'i', 'and r9=r14,r9')
        compare(lines, 9, '0x10')
        emit(lines, 'm', 'getf.sig r9=f6')
        compare(lines, 9, '2')
        emit(lines, 'm', 'getf.exp r9=f6')
        compare(lines, 9, '0x1003e')
        emit(lines, 'm', 'mov r9=ar.fpsr')
        compare(lines, 9, '0x4001f')
        # A trap resumes at after_trap rather than re-executing the F10 slot.
        emit(lines, 'b', 'rfi')

    lines.append('pass:')
    terminal(lines, passed, failed)
    lines += ['.data', '.align 16', 'source:']
    if case == 'disabled':
        lines += ['.quad 0x8000000000000000, 0xffff']
    elif case == 'invalid':
        lines += ['.quad 0x8000000000000000, 0x1ffff',
                  '.align 16', 'sentinel:',
                  '.quad 0x1234, 0x23456']
    else:
        lines += ['.quad 0xc000000000000000, 0xffff']
    return '\n'.join(lines) + '\n'


def run_one(case, args, *, case_spec=None, source=None):
    """Assemble and execute a fixture; shared with F9 register-fault tests."""
    vector, passed, failed = CASES[case] if case_spec is None else case_spec
    pass_text = f"r8={int(passed, 0):016x}"
    fail_text = f"r8={int(failed, 0):016x}"
    out = args.out / case
    out.mkdir(parents=True, exist_ok=True)
    src, obj, elf = [out / ('fp-exception.' + ext) for ext in ('S', 'o', 'elf')]
    src.write_text(generate(case) if source is None else source)
    as_cmd = shlex.split(os.environ.get('IA64_AS', 'ia64-linux-gnu-as'))
    ld_cmd = shlex.split(os.environ.get('IA64_LD', 'ia64-linux-gnu-ld'))
    subprocess.run(as_cmd + ['-o', str(obj), str(src)], check=True, timeout=60)
    subprocess.run(ld_cmd + ['-static', '-nostdlib', '-e', '_start',
                            f'-Ttext={TEXT:#x}', '-Tdata=0x8000000',
                            '-o', str(elf), str(obj)], check=True, timeout=60)
    evidence = {
        'case': case, 'vector': hex(vector),
        'assembly_sha256': hashlib.sha256(src.read_bytes()).hexdigest(),
        'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
        'execution': 'not-run',
    }
    result = out / 'result.json'
    result.write_text(json.dumps(evidence, indent=2) + '\n')
    if args.assemble_only:
        return evidence

    qemu = str(Path(args.qemu).resolve())
    log = out / 'qemu.log'
    log.unlink(missing_ok=True)
    env = dict(os.environ, QEMU_IA64_BREAK_LOG='1', QEMU_IA64_LOG_BREAK_STR='0')
    cmd = [qemu, '-accel', 'tcg', '-M', 'ipf', '-m', '512M', '-smp', '1',
           '-display', 'none', '-monitor', 'none', '-vga', 'none', '-nic', 'none',
           '-serial', 'file:' + str(out / 'serial.log'), '-d', 'guest_errors',
           '-D', str(log), '-kernel', str(elf)]
    limit = 8 * 1024 * 1024
    offset, tail, truncated = 0, '', False
    with (out / 'stderr.txt').open('w') as stderr:
        proc = subprocess.Popen(cmd, env=env, stdout=stderr, stderr=stderr)
        deadline = time.monotonic() + args.timeout
        try:
            while proc.poll() is None and time.monotonic() < deadline:
                if log.exists():
                    with log.open('rb') as stream:
                        stream.seek(offset)
                        chunk = stream.read(512 * 1024)
                    offset += len(chunk)
                    text = tail + chunk.decode(errors='replace')
                    if pass_text in text or fail_text in text or 'IA64 UNIMPL' in text:
                        break
                    tail = text[-128:]
                    if offset >= limit:
                        truncated = True
                        break
                time.sleep(0.02)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2)
    text = log.read_bytes()[:limit].decode(errors='replace') if log.exists() else ''
    if log.exists() and log.stat().st_size > limit:
        truncated = True
        with log.open('r+b') as stream:
            stream.truncate(limit)
    pass_seen = pass_text in text
    fail_seen = fail_text in text
    # Once a terminal marker is observed, log growth caused by the break
    # exception itself is irrelevant; truncate retained evidence but do not
    # turn a witnessed PASS into a log-limit failure.
    ok = pass_seen and not fail_seen and 'IA64 UNIMPL' not in text
    evidence.update(execution='pass' if ok else 'fail',
                    returncode=proc.returncode, pass_seen=pass_seen,
                    fail_seen=fail_seen, log_limit_exceeded=truncated,
                    qemu_sha256=hashlib.sha256(Path(qemu).read_bytes()).hexdigest())
    result.write_text(json.dumps(evidence, indent=2) + '\n')
    if not ok:
        raise RuntimeError(f'{case} FP exception guest failed; see {out}')
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default=os.environ.get('QEMU_BIN', './build/qemu-system-ia64'))
    parser.add_argument('--out', type=Path, default=Path('scratch/ia64-fp-exceptions'))
    parser.add_argument('--timeout', type=float, default=5.0)
    parser.add_argument('--assemble-only', action='store_true')
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error('--timeout must be in (0,60]')
    args.out = args.out.resolve()
    results = [run_one(case, args) for case in CASES]
    print('F10 IVT exception ' + ('assembly' if args.assemble_only else 'execution') +
          ' PASS: ' + ', '.join(r['case'] for r in results))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
