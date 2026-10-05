#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate and execute raw-FR F9 regression cases without proprietary firmware.

Oracle: tests/ia64/isa/test_f9.py. Every result compares both spill words;
FPSR is preserved, PSR dirty bits checked, and false predication tested.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('f9_oracle', ROOT / 'tests/ia64/isa/test_f9.py')
ORACLE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ORACLE)
PASS = 'r8=0000000066397061'
FAIL = 'r8=0000000066396661'


def generate():
    lines = ['.text', '.explicit', '.align 16', '.global _start', '_start:']
    data = []
    count = 0
    def emit(kind, insn):
        choices = {
            'm': ['.mii', insn, 'nop.i 0', 'nop.i 0'],
            'i': ['.mii', 'nop.m 0', insn, 'nop.i 0'],
            'f': ['.mmf', 'nop.m 0', 'nop.m 0', insn],
            'b': ['.mib', 'nop.m 0', 'nop.i 0', insn],
            'l': ['.mlx', 'nop.m 0', insn],
        }
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
    profiles = [(6, 7, 8), (7, 7, 8), (8, 7, 8), (7, 7, 7),
                (126, 125, 124), (126, 126, 124), (6, 0, 1)]
    for name in ORACLE.OPS:
        for index, original_a in enumerate(ORACLE.PATTERNS):
            original_b = ORACLE.PATTERNS[(index * 5 + 3) % len(ORACLE.PATTERNS)]
            for f1, f2, f3 in profiles:
                for false_predicate in (False, True):
                    a, b = original_a, original_b
                    if f2 == 0:
                        a = (0, 0)
                    if f3 == 1:
                        b = (1 << 63, 0xffff)
                    if f2 == f3:
                        b = a
                    count += 1
                    lines.append(f'case_{count}: /* {name}, profile {f1}/{f2}/{f3}, false={false_predicate} */')
                    literal(9, count)
                    sentinel = (0x456789abcdef0123, 0x24567)
                    if f1 not in (f2, f3):
                        fill(f1, sentinel)
                    if f2 > 1:
                        fill(f2, a)
                    if f3 > 1 and f3 != f2:
                        fill(f3, b)
                    # All setup precedes dirty-bit reset/status snapshot.
                    emit('m', 'rsm 0x30')
                    emit('m', 'srlz.d')
                    emit('m', 'mov r25=ar.fpsr')
                    emit('i', 'cmp.eq p4,p5=r0,r0')
                    emit('f', f'(p{5 if false_predicate else 0}) {name} f{f1}=f{f2},f{f3}')
                    emit('m', 'mov r24=psr')
                    emit('i', 'and r24=0x30,r24')
                    compare(24, 0 if false_predicate else (16 if f1 < 32 else 32))
                    emit('m', 'mov r24=ar.fpsr')
                    emit('i', 'cmp.eq p6,p7=r24,r25')
                    emit('b', '(p7) br.cond.spnt fail')
                    if false_predicate:
                        expected = a if f1 == f2 else b if f1 == f3 else sentinel
                    else:
                        expected = ORACLE.oracle(name, a, b)
                    literal(14, 'output')
                    emit('m', f'stf.spill [r14]=f{f1}')
                    emit('m', 'ld8 r16=[r14],8')
                    emit('m', 'ld8 r17=[r14]')
                    compare(16, expected[0])
                    compare(17, expected[1])
    literal(8, '0x66397061')
    emit('m', 'break.m 0')
    lines.append('pass_spin:')
    emit('b', 'br.cond.sptk pass_spin')
    lines.append('fail:')
    literal(8, '0x66396661')
    emit('m', 'break.m 0')
    lines.append('fail_spin:')
    emit('b', 'br.cond.sptk fail_spin')
    lines += ['.data', '.align 16', 'output:', '.quad 0,0']
    for label, pair in data:
        lines += ['.align 16', label + ':', f'.quad {pair[0]:#x}, {pair[1]:#x}']
    return '\n'.join(lines) + '\n', count


def run_generated_guest(suite, generator, family_count, pass_marker, fail_marker):
    """Shared bounded runner. Each suite supplies independent cases and markers."""
    p = argparse.ArgumentParser(description=f'Generate and execute {suite} raw-FR tests')
    p.add_argument('--qemu', default=os.environ.get('QEMU_BIN', './build/qemu-system-ia64'))
    p.add_argument('--out', type=Path, default=Path('scratch/ia64-' + suite.lower()))
    p.add_argument('--assemble-only', action='store_true')
    p.add_argument('--timeout', type=float, default=5.0)
    args = p.parse_args()
    if not 0 < args.timeout <= 60:
        p.error('--timeout must be in (0,60]')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    assembly, count = generator()
    src, obj, elf = [out / (suite.lower() + '-selftest.' + ext) for ext in ('S', 'o', 'elf')]
    src.write_text(assembly)
    commands = [('AS', 'as', ['-o', str(obj), str(src)]),
                ('LD', 'ld', ['-static', '-nostdlib', '-e', '_start', '-Ttext=0x5000000', '-Tdata=0x8000000',
                              '-o', str(elf), str(obj)])]
    for var, name, arguments in commands:
        subprocess.run(shlex.split(os.environ.get('IA64_' + var, 'ia64-linux-gnu-' + name)) + arguments,
                       check=True, timeout=60)
    evidence = dict(cases=count, families=family_count,
                    assembly_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),
                    elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),
                    execution='not-run')
    evidence_path = out / 'result.json5'
    evidence_path.write_text(json.dumps(evidence, indent=2) + '\n')
    if args.assemble_only:
        print(f'{suite} assembly PASS: {count} cases across {family_count} families')
        return
    qemu = str(Path(args.qemu).resolve())
    log = out / 'qemu.log'
    # Fresh evidence only; a stale PASS log must never satisfy a later run.
    log.unlink(missing_ok=True)
    env = dict(os.environ, QEMU_IA64_BREAK_LOG='1', QEMU_IA64_LOG_BREAK_STR='0')
    cmd = [qemu, '-accel', 'tcg', '-M', 'ipf', '-m', '512M', '-smp', '1',
           '-display', 'none', '-monitor', 'none', '-vga', 'none', '-nic', 'none',
           '-serial', 'file:' + str(out / 'serial.log'), '-d', 'guest_errors',
           '-D', str(log), '-kernel', str(elf)]
    # Terminate on the first terminal marker. BREAK enters an exception
    # handler rather than necessarily returning to the guest spin loop.
    # Bound both runtime and retained log memory even on a broken handler.
    limit = 16 * 1024 * 1024
    truncated = False
    with (out / 'stderr.txt').open('w') as stderr:
        proc = subprocess.Popen(cmd, env=env, stdout=stderr, stderr=stderr)
        deadline = time.monotonic() + args.timeout
        offset, tail = 0, ''
        try:
            while proc.poll() is None and time.monotonic() < deadline:
                if log.exists():
                    with log.open('rb') as stream:
                        stream.seek(offset)
                        chunk = stream.read(1024 * 1024)
                    offset += len(chunk)
                    text = tail + chunk.decode(errors='replace')
                    if pass_marker in text or fail_marker in text or 'IA64 UNIMPL' in text:
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
        rc = proc.returncode
    if log.exists():
        size = log.stat().st_size
        with log.open('rb') as stream:
            text = stream.read(limit).decode(errors='replace')
        if size > limit:
            truncated = True
            with log.open('r+b') as stream:
                stream.truncate(limit)
    else:
        text = ''
    passed = (pass_marker in text and fail_marker not in text and 'IA64 UNIMPL' not in text
              and not truncated and rc in (0, -15, -9))
    evidence.update(execution='pass' if passed else 'fail', returncode=rc,
                    log_limit_exceeded=truncated,
                    qemu_sha256=hashlib.sha256(Path(qemu).read_bytes()).hexdigest())
    evidence_path.write_text(json.dumps(evidence, indent=2) + '\n')
    if not passed:
        raise RuntimeError(f'{suite} guest FAILED; see {out} (rc={rc})')
    print(f'{suite} execution PASS: {count} cases; raw results, NaTVal, aliases, predication, PSR/FPSR')


def main():
    run_generated_guest('F9', generate, len(ORACLE.OPS), PASS, FAIL)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
