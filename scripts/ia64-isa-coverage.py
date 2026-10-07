#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit actual F-slot translation decisions, not a second decoder.

The projection compiles the production SLOT_F C block with recording TCG
emitters. Guest branches are recorded, never executed: this is L1 decode/
dispatch evidence only, NOT an execution, exception or numerical oracle.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

import json5

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = Path('tests/ia64/isa/coverage.json5')
REPORT = Path('docs/generated/ia64-instruction-coverage.md')
BASELINE = Path('tests/ia64/isa/f-unit-baseline.json5')
PROFILES = ('low', 'high', 'alias', 'unit-multiply', 'high-multiply',
            'predicated', 'p15', 'p16', 'p63')
# Explicit emitter allowlist: source changes using a new API fail to compile
# until reviewed. No guessed helper bodies, regex classification, or eval().
EMITTERS = '''gen_set_label gen_set_predicates gen_fr_load_lo gen_fr_load_hi
 gen_fr_store_lo gen_fr_store_hi tcg_gen_andi_i64 tcg_gen_xori_i64
 tcg_gen_brcondi_i64 tcg_gen_mov_i64 tcg_gen_movi_i64 tcg_gen_sub_i64
 tcg_gen_clzi_i64 tcg_gen_shl_i64 tcg_gen_or_i64 tcg_gen_br
 tcg_gen_shri_i64 tcg_gen_neg_i64 tcg_gen_shli_i64
 gen_fchkf_branch gen_break_common'''.split()
HELPERS = '''fcmp fma_s1 fms_s1 fnma_s1 frcpa_s1
 xma_l xma_hu xma_h f8 f9 f10 fselect fclass fsetc fclrf'''.split()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load_registry(root):
    data = json5.loads((root / REGISTRY).read_text(), allow_duplicate_keys=False)
    if data.get('schema_version') != 1:
        raise ValueError('unsupported registry schema')
    if data['reference']['revision'] != '2.3':
        raise ValueError('reference change requires an explicit new audit')
    names = set()
    formats = set()
    for row in data['forms']:
        if row['mnemonic'] in names:
            raise ValueError('duplicate family: ' + row['mnemonic'])
        names.add(row['mnemonic'])
        formats.add(row['format'])
        if row['semantics'] not in data['limitations']:
            raise ValueError('unclassified semantics: ' + row['mnemonic'])
        if type(row['status_fields']) is not bool or type(row['unconditional']) is not bool:
            raise ValueError('variant selectors must be boolean')
        compose(row['fixed'])
    if formats != {'F' + str(i) for i in range(1, 17)}:
        raise ValueError('F1..F16 inventory is incomplete')
    return data


def compose(fields):
    word = used = 0
    for start, width, value in fields:
        if not (0 <= start < 41 and 1 <= width <= 41 - start):
            raise ValueError('field outside 41-bit instruction')
        if not (0 <= value < 1 << width):
            raise ValueError('value does not fit instruction field')
        mask = ((1 << width) - 1) << start
        if mask & used:
            raise ValueError('overlapping instruction fields')
        used |= mask
        word |= value << start
    return word


def vector(row, sf, unc, profile):
    v = dict(f1=6, f2=7, f3=8, f4=9, p1=6, p2=7, qp=0,
             imm=0, cls=0x1ff, amask=0x7f, omask=0x12)
    if profile == 'high':
        v.update(f1=126, f2=125, f3=124, f4=123, p1=61, p2=62,
                 imm=0x1fffff, cls=0x100, amask=1, omask=0x7f)
    elif profile == 'alias':
        v.update(f1=7, f2=7, f3=7, f4=7, imm=1, cls=1)
    elif profile == 'unit-multiply':
        v.update(f4=1, f2=0, imm=0x80000, cls=0)
    elif profile == 'high-multiply':
        v.update(f4=65, f2=0, imm=0x100000, cls=0x80)
    elif profile == 'predicated':
        v.update(qp=5, imm=0x12345)
    elif profile == 'p15':
        v.update(p2=15, imm=0xabc)
    elif profile == 'p16':
        v.update(p2=16, imm=0x1abc)
    elif profile == 'p63':
        v.update(p2=63, imm=0x1ffffe)
    if profile not in PROFILES:
        raise ValueError('unknown operand profile ' + profile)
    fmt = row['format']
    fields = list(row['fixed']) + [[0, 6, v['qp']]]
    name = row['mnemonic'] + ('.unc' if unc else '')
    if row['status_fields']:
        fields.append([34, 2, sf])
        name += '.s' + str(sf)
    if row['unconditional']:
        fields.append([12, 1, int(unc)])
    if fmt in ('F1', 'F2', 'F3'):
        fields += [[6, 7, v['f1']], [13, 7, v['f2']],
                   [20, 7, v['f3']], [27, 7, v['f4']]]
        operands = 'f{f1}=f{f3},f{f4},f{f2}'
    elif fmt in ('F4', 'F5'):
        fields += [[6, 6, v['p1']], [27, 6, v['p2']], [13, 7, v['f2']]]
        if fmt == 'F4':
            fields.append([20, 7, v['f3']])
            operands = 'p{p1},p{p2}=f{f2},f{f3}'
        else:
            fields += [[20, 7, v['cls'] >> 2], [33, 2, v['cls'] & 3]]
            operands = 'p{p1},p{p2}=f{f2},{cls}'
    elif fmt in ('F6', 'F7'):
        fields += [[6, 7, v['f1']], [27, 6, v['p2']], [20, 7, v['f3']]]
        operands = 'f{f1},p{p2}=f{f3}'
        if fmt == 'F6':
            fields.append([13, 7, v['f2']])
            operands = 'f{f1},p{p2}=f{f2},f{f3}'
    elif fmt in ('F8', 'F9', 'F10', 'F11'):
        fields += [[6, 7, v['f1']], [13, 7, v['f2']]]
        operands = 'f{f1}=f{f2}'
        if fmt in ('F8', 'F9'):
            fields.append([20, 7, v['f3']])
            operands += ',f{f3}'
    elif fmt == 'F12':
        fields += [[13, 7, v['amask']], [20, 7, v['omask']]]
        operands = '{amask},{omask}'
    elif fmt == 'F13':
        operands = ''
    elif fmt == 'F14':
        operands = '{label}'  # self-target: displacement zero
    elif fmt in ('F15', 'F16'):
        fields += [[6, 20, v['imm'] & 0xfffff], [36, 1, v['imm'] >> 20]]
        operands = '{imm}'
    else:
        raise ValueError('unhandled format ' + fmt)
    word = compose(fields)
    # label is substituted when emitting the complete assembly corpus.
    v['label'] = '{label}'
    assembly = f"(p{v['qp']}) {name} " + operands.format(**v)
    return dict(id=name + '/' + profile, family=row['mnemonic'], format=fmt,
                word=word, assembly=assembly.rstrip(), semantics=row['semantics'])


def corpus(data):
    result = []
    for row in data['forms']:
        for sf in (range(4) if row['status_fields'] else (0,)):
            for unc in ((False, True) if row['unconditional'] else (False,)):
                for profile in PROFILES:
                    result.append(vector(row, sf, unc, profile))
    if len({x['id'] for x in result}) != len(result):
        raise ValueError('duplicate expanded vector ID')
    return result


def project(root, directory, cc):
    source = (root / 'target/ia64/translate.c').read_text()
    start, end = '    case SLOT_F:\n', '    case SLOT_B:\n'
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError('production decoder shape changed; review projection boundaries')
    block = source.split(start, 1)[1].split(end, 1)[0]
    if not block.rstrip().endswith('break;'):
        raise ValueError('production F-slot termination changed')
    prelude = '''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t TCGv_i64;
typedef int TCGLabel;
typedef int DisasContext;
static uintptr_t tcg_env;
static int unimplemented;
static const char *route;
#define IA64_FP_EXP_INTEGER 0x1003e
#define TCG_COND_EQ 0
#define TCG_COND_NE 1
#define TCG_COND_GE 2
static uint64_t extract64(uint64_t x, unsigned s, unsigned n)
{ return (x >> s) & ((UINT64_C(1) << n) - 1); }
static uintptr_t tcg_temp_new_i64(void) { return 1; }
static uintptr_t tcg_constant_i32(uint32_t v) { return v; }
static uintptr_t tcg_constant_i64(uint64_t v) { return v; }
static TCGLabel *gen_new_label(void) { static TCGLabel l; return &l; }
static TCGLabel *gen_qp_skip(unsigned qp)
{ return qp ? gen_new_label() : NULL; }
static uintptr_t gen_pr_read_bit(unsigned qp)
{ (void)qp; return 1; }
static void gen_pr_write_bit(unsigned p, uintptr_t value)
{ (void)p; (void)value; }
static void record(const char *name, ...) {
    if (strncmp(name, "gen_helper_", 11) == 0) { route = name + 11; }
    else if ((strncmp(name, "gen_fr_", 7) == 0 ||
              strcmp(name, "gen_fchkf_branch") == 0 ||
              strcmp(name, "gen_break_common") == 0) &&
             strcmp(route, "none") == 0)
        route = "tcg";
}
static void gen_unimpl(DisasContext *ctx, uint64_t insn, const char *msg)
{ (void)ctx; (void)insn; (void)msg; unimplemented = 1; }
'''
    for name in EMITTERS + ['gen_helper_' + x for x in HELPERS]:
        prelude += f'#define {name}(...) record("{name}", __VA_ARGS__)\n'
    body = '\nstatic void probe(uint64_t insn) {\nDisasContext context = 0;\nDisasContext *ctx = &context;\ndo {\n' + block + '\n} while (0);\n}\n'
    main = '''
int main(void) {
    unsigned long long word;
    while (scanf("%llx", &word) == 1) {
        route = "none"; unimplemented = 0;
        probe((uint64_t)word);
        printf("%d %s\\n", unimplemented, route);
    }
    return ferror(stdin) || ferror(stdout);
}
'''
    header = (root / 'target/ia64/fp-bitops.h').read_bytes()
    prelude += '#include "fp-bitops.h"\n#include "fp-convert.h"\n#include "fp-f8.h"\n#include "fp-compare.h"\n'
    path = directory / 'f-unit-probe.c'
    path.write_text(prelude + body + main)
    executable = directory / 'f-unit-probe'
    subprocess.run(shlex.split(cc) + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-O0', '-I' + str(root / 'target/ia64'), str(path), '-o', str(executable)], check=True, timeout=60,
                   capture_output=True, text=True)
    return executable, digest(block.encode() + b'\0fp-bitops.h\0' + header +
                              b'\0fp-convert.h\0' + (root / 'target/ia64/fp-convert.h').read_bytes() +
                              b'\0fp-f8.h\0' + (root / 'target/ia64/fp-f8.h').read_bytes() +
                              b'\0fp-compare.h\0' + (root / 'target/ia64/fp-compare.h').read_bytes())


def probe(executable, words):
    result = subprocess.run([str(executable)], input=''.join(f'{w:x}\n' for w in words),
                            text=True, capture_output=True, check=True, timeout=30)
    rows = [line.split() for line in result.stdout.splitlines()]
    if len(rows) != len(words) or any(len(x) != 2 or x[0] not in ('0', '1') for x in rows):
        raise ValueError('invalid projection response')
    return [('unimplemented' if flag == '1' else route) for flag, route in rows]


def unassigned_vectors():
    # Blank cells in Intel tables 4-59..4-65 are COLOR-CODED: some are
    # ignored/NOP, some fault unconditionally or only with qp=1. Do not infer
    # architectural legality from the absence of an assembler mnemonic.
    # These probes lock observed behavior only; disposition is unadjudicated.
    rows = []
    for major in (2, 3, 6, 7, 15):
        rows.append((f'major-{major:x}', major << 37))
    for x6 in (2, 3, 6, 7, 9, 10, 11, 12, 13, 14, 15, 0x13, 0x1d, 0x1e, 0x1f):
        rows.append((f'extension-0-{x6:02x}', x6 << 27))
    rows.append(('xma-extension-1', (14 << 37) | (1 << 36) | (1 << 34)))
    return rows


def assemble(vectors, directory, commands):
    src = directory / 'f-unit.s'
    lines = ['.text', '.explicit', '.align 16', '.global isa_case_0']
    for i, item in enumerate(vectors):
        label = f'isa_case_{i}'
        asm = item['assembly'].replace('{label}', label)
        lines += [label + ':', '{ .mmf', 'nop.m 0', 'nop.m 0', asm, ';;', '}']
    src.write_text('\n'.join(lines) + '\n')
    obj, elf, raw = [directory / ('f-unit.' + ext) for ext in ('o', 'elf', 'bin')]
    invocations = ((commands[0], ['-o', str(obj), str(src)]),
                   (commands[1], ['-static', '-nostdlib', '-e', 'isa_case_0',
                                  '-Ttext=0x5000000', '-o', str(elf), str(obj)]),
                   (commands[2], ['-O', 'binary', '--only-section=.text', str(elf), str(raw)]))
    for cmd, args in invocations:
        subprocess.run(shlex.split(cmd) + args, check=True, timeout=60)
    payload = raw.read_bytes()
    if len(payload) != 16 * len(vectors):
        raise ValueError('assembler changed explicit bundle layout')
    for i, item in enumerate(vectors):
        bundle = int.from_bytes(payload[16*i:16*i+16], 'little')
        if (bundle & 0x1e) != 0x0e:
            raise ValueError('expected MMF bundle at ' + item['id'])
        observed = (bundle >> 87) & ((1 << 41) - 1)
        if observed != item['word']:
            raise ValueError(f"assembler mismatch {item['id']}: {observed:011x} != {item['word']:011x}")
    return digest(payload)


def measure(data, vectors, executable, source_hash):
    routes = probe(executable, [x['word'] for x in vectors])
    unassigned = unassigned_vectors()
    unassigned_routes = probe(executable, [w for _, w in unassigned])
    unassigned_observations = [dict(id=name, word=f'{word:011x}', route=route)
                              for (name, word), route in zip(unassigned, unassigned_routes)]
    form_routes = {}
    for v, route in zip(vectors, routes):
        form_routes.setdefault(v['id'].split('/')[0], []).append(route)
    disposition = Counter(
        'all_probes_rejected' if all(r == 'unimplemented' for r in rr) else
        'all_probes_accepted' if all(r != 'unimplemented' for r in rr) else
        'operand_dependent' for rr in form_routes.values())
    observations = [{'id': v['id'], 'word': f"{v['word']:011x}", 'route': r}
                    for v, r in zip(vectors, routes)]
    return dict(schema_version=1, source_f_block_sha256=source_hash,
                observations_sha256=digest(json.dumps(observations, sort_keys=True).encode()),
                vectors=len(vectors), forms=len(vectors)//len(PROFILES),
                form_dispositions=dict(sorted(disposition.items())),
                unassigned_vectors=len(unassigned),
                unassigned_observations_sha256=digest(json.dumps(unassigned_observations, sort_keys=True).encode()),
                routes=dict(sorted(Counter(routes).items())))


def render(data, vectors, executable, measurement):
    routes = probe(executable, [v['word'] for v in vectors])
    lines = ['# IA-64 instruction coverage: F-unit audit baseline', '',
             'Generated by `scripts/ia64-isa-coverage.py`; do not hand-edit.', '',
             '**ISA complete: NO. A passing audit locks known gaps; it does not certify instruction correctness.**', '',
             f"Reference: Intel Volume 3, revision 2.3 (May 2010), document 323207, section 4.6, pages 3:356–3:365.", '',
             f"Scope: all F1–F16 families below; {len(data['forms'])} canonical families, {measurement['forms']} precision/status/conditional forms, {len(vectors)} operand-profile vectors and {measurement['unassigned_vectors']} unassigned-encoding observation probes.", '',
             'A, I, M, B and L/X are **not audited** in this tranche. Aliases are linked rather than counted twice. Processor-generation availability and exhaustive ignored/reserved-bit constraints remain to be audited.', '',
             '## Method and limits', '',
             'The host harness compiles the actual production `case SLOT_F` with recording TCG emitters. It observes translation dispatch without copying the decoder into Python. It does not execute TCG, evaluate guest predicates, prove numeric results, or test architectural exception delivery. An accepted word can still be incorrectly implemented.', '',
             f'{len(PROFILES)} profiles cover ordinary/high/aliased FP operands, unit-multiply and high-register multiply operands, predication, and the p15/p16/p63 predicate-destination boundary. All profile encodings can be independently assembled and byte-compared with `--assembler`; that optional check is not implied by the host-only check.', '',
             'The additional unassigned-encoding probes do **not** assert reserved/illegal behavior. Intel’s opcode-table color key distinguishes ignored, reserved, and conditional-reserved cells. Their architectural dispositions are unadjudicated here; no negative-encoding conformance claim is made.', '',
             'The baseline file locks **all vector routes**, not merely this summary. Rebaselining requires reviewing the route changes and corresponding issues. The source-block-plus-FP-support-header hash supplies provenance; moving code also requires refreshing generated evidence.', '',
             '## Observed dispatch by family', '',
             f"Form-level observation (not semantic correctness): {measurement['form_dispositions']}.", '',
             '| Family | Format | Forms | Accepted / probes | Emitted routes | Semantic audit |',
             '|---|---|---:|---:|---|---|']
    for row in data['forms']:
        pairs = [(v, r) for v, r in zip(vectors, routes) if v['family'] == row['mnemonic']]
        counts = Counter(r for _, r in pairs)
        accepted = sum(n for r, n in counts.items() if r != 'unimplemented')
        route_text = ', '.join(f'`{r}`:{n}' for r, n in sorted(counts.items()))
        lines.append(f"| `{row['mnemonic']}` | {row['format']} | {len(pairs)//len(PROFILES)} | {accepted}/{len(pairs)} | {route_text} | {row['semantics']} |")
    lines += ['', '## Semantic findings', '']
    for key, text in data['limitations'].items():
        lines += [f'**{key}.** {text}', '']
    lines += ['## Alias policy', '']
    for alias, canonical in data['aliases'].items():
        lines += [f'- `{alias}` → {canonical}.']
    lines += ['', '## Reproduction', '', '```sh',
              'python3 -m pip install -r tests/ia64/isa/requirements.txt',
              'python3 scripts/ia64-isa-coverage.py --check',
              'python3 -m unittest discover -s tests/ia64/isa -p "test_*.py"',
              '# Independent encoding check; IA64_AS / IA64_LD / IA64_OBJCOPY override paths:',
              'python3 scripts/ia64-isa-coverage.py --check --assembler',
              '# After reviewing intentional source/corpus changes:',
              'python3 scripts/ia64-isa-coverage.py --record', '```', '',
              '## Provenance', '',
              f"- Starting stack: `{data['baseline']}` (PR #6, above indexed-register/RSE work).",
              f"- F-block plus FP support headers SHA-256: `{measurement['source_f_block_sha256']}`.",
              f"- All-vector observation SHA-256: `{measurement['observations_sha256']}`.", '',
              'No firmware, ROM, or private payload is needed or included. The audit itself does not execute instructions; family data-path/state tests are separate.', '']
    return '\n'.join(lines)


def check_snapshot(root, measurement, report):
    expected = json5.loads((root / BASELINE).read_text(), allow_duplicate_keys=False)
    recorded_report = (root / REPORT).read_text()
    if measurement != expected or recorded_report != report:
        details = []
        for key in sorted(set(measurement) | set(expected)):
            if measurement.get(key) != expected.get(key):
                details.append(f"{key}: expected={expected.get(key)!r} observed={measurement.get(key)!r}")
        if recorded_report != report:
            details.append(
                "generated report differs: recorded_sha256=" +
                digest(recorded_report.encode()) + " observed_sha256=" +
                digest(report.encode()))
        raise ValueError('audit drift: ' + '; '.join(details) +
                         '; review before --record')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--check', action='store_true')
    action.add_argument('--record', action='store_true')
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--assembler', action='store_true')
    parser.add_argument('--evidence-dir', type=Path,
                        help='retain assembly corpus and toolchain provenance')
    parser.add_argument('--output', type=Path, help='optional detailed observations (JSON5)')
    args = parser.parse_args()
    root = args.root.resolve()
    data = load_registry(root)
    vectors = corpus(data)
    with tempfile.TemporaryDirectory(prefix='ia64-isa-') as tmp:
        directory = Path(tmp)
        executable, source_hash = project(root, directory, os.environ.get('CC', 'cc'))
        measurement = measure(data, vectors, executable, source_hash)
        report = render(data, vectors, executable, measurement)
        if args.assembler:
            value = assemble(vectors, directory, [os.environ.get('IA64_' + x, 'ia64-linux-gnu-' + y)
                              for x, y in (('AS', 'as'), ('LD', 'ld'), ('OBJCOPY', 'objcopy'))])
            print(f'Assembler byte comparison PASS: {len(vectors)} vectors; text SHA-256 {value}')
            if args.evidence_dir:
                args.evidence_dir.mkdir(parents=True, exist_ok=True)
                (args.evidence_dir / 'f-unit.s').write_bytes((directory / 'f-unit.s').read_bytes())
                command = os.environ.get('IA64_AS', 'ia64-linux-gnu-as')
                version = subprocess.run(shlex.split(command) + ['--version'],
                                         capture_output=True, text=True,
                                         check=True, timeout=30).stdout
                (args.evidence_dir / 'toolchain.txt').write_text(version + '\ntext_sha256=' + value + '\n')
        if args.output:
            args.output.write_text(json.dumps([dict(v, route=r) for v, r in zip(vectors,
                                   probe(executable, [x['word'] for x in vectors]))], indent=2) + '\n')
        if args.record:
            (root / BASELINE).write_text('// Known-gap measurement; NOT an ISA correctness certificate.\n' + json.dumps(measurement, indent=2) + '\n')
            (root / REPORT).write_text(report)
        else:
            check_snapshot(root, measurement, report)
        print(f"AUDIT BASELINE PASS: {measurement['forms']} F forms, {measurement['vectors']} legal vectors, {measurement['unassigned_vectors']} unassigned probes. ISA COMPLETE=NO")


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as exc:
        if isinstance(exc, subprocess.CalledProcessError) and exc.stderr:
            print(exc.stderr, file=sys.stderr)
        sys.exit(f'ia64-isa-coverage: {exc}')
