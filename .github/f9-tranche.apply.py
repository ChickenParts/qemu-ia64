#!/usr/bin/env python3
"""One-use transport for reviewed, locally tested F9 source edits.

The resulting ordinary C/Python files, not this script, are the deliverable.
Both input and output hashes are checked. No network, firmware, or implicit
baseline acceptance. Delete this transport after the validated source commit.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import json5

r = Path.cwd()
def sha(path):
    return hashlib.sha256((r / path).read_bytes()).hexdigest()

before = {
 'target/ia64/translate.c': 'db6916a94f886f07dd27a2d0bb9c22db10e45010e2329d84787d2ca7488059a3',
 'target/ia64/helper.c': '1ee60fff8558433ea4f3cc5f342e12ece31352352e10161b79ad19758edc5aff',
 'target/ia64/helper.h': 'c6d02e62dfda73e002d50dad7053c2c394a944175e09f8796a63e10da66cf51b',
 'target/ia64/cpu.h': 'fd30aa321714036f625e544627436798909beab3168a6f23831c05e9ef50b53c',
 'scripts/ia64-isa-coverage.py': '46b4afd5f5a4e38f94e13b0f8771dc1db2155bbe8a6f574b90d1cfc3208fabb3',
 'tests/ia64/isa/test_coverage.py': '9f32ac3e4abc651a791c52874c10fd8b76211d00fb130e398e2aa86111d58363',
 'tests/ia64/isa/coverage.json5': 'a7ebf2d25b0b3c9b8cc533bd40aa227ab569ba5b0b2a3ecac2eb1d8390a8073b',
 'target/ia64/fp-bitops.h': '6ba2a923b38a6c1e9eaa8ef5da73d6ded9c6de25aff09fc21a3ee775f6dcffe1',
 'tests/ia64/isa/test_f9.py': '99eaa4939f32fdb729d980f768f4cc944770c53088f45e3eed5aa654839932fd',
 'scripts/run-ia64-f9-tests.py': '2a0afc93e514dac012542ff6e11c24a5995ad811af85ebee69e957a5f1efd22b',
}
for path, expected in before.items():
    if sha(path) != expected:
        raise SystemExit('Refusing changed input: ' + path + ' ' + sha(path))

p = r / 'target/ia64/translate.c'
s = p.read_text().replace('#include "cpu.h"', '#include "cpu.h"\n#include "fp-bitops.h"', 1)
a = s.index('            if (!handled && f_major == 0x0 &&\n                extract64(insn, 36, 1) == 0 &&', s.index('    case SLOT_F:'))
b = s.index('            if (!handled && f_major == 0x0) {\n                /*\n                 * F10:', a)
s = s[:a] + '''            if (!handled && ia64_f9_decode(insn) != IA64_F9_INVALID) {
                /* All F9 forms operate on the full 82-bit register value. */
                gen_helper_f9(tcg_env, tcg_constant_i64(insn));
                handled = true;
            }

''' + s[b:]
a = s.index('            if (!handled && f_major == 0x0) {\n                /* F9: fmerge.')
b = s.index('            /*\n             * fnorm.*:', a)
s = s[:a] + s[b:]
p.write_text(s)
p = r / 'target/ia64/helper.h'
s = p.read_text().replace('DEF_HELPER_2(fr_get_lo,', 'DEF_HELPER_2(f9, void, env, i64)\nDEF_HELPER_2(fr_get_lo,', 1)
p.write_text(s)
p = r / 'target/ia64/helper.c'
s = p.read_text().replace('#include "cpu.h"', '#include "cpu.h"\n#include "fp-bitops.h"', 1)
pos = s.index('uint64_t HELPER(gr_nat)')
s = s[:pos] + '''/*
 * F9 bit operations.  Source snapshots precede every destination write so
 * f1=f2, f1=f3 and f2=f3 are safe, including rotating FRs.
 * Disabled-FP fault delivery remains part of the shared FP-state work (#10).
 */
void HELPER(f9)(CPUIA64State *env, uint64_t insn)
{
    unsigned f1 = (insn >> 6) & 127;
    unsigned f2 = (insn >> 13) & 127;
    unsigned f3 = (insn >> 20) & 127;
    int op = ia64_f9_decode(insn);
    IA64FRBits a, b, result;

    if (op == IA64_F9_INVALID || f1 <= 1) {
        ia64_fault(env_cpu(env), env, false, false, IA64_VEC_ILLEGAL_OP,
                   0, GETPC());
        g_assert_not_reached();
    }
    a = (IA64FRBits) { HELPER(fr_get_lo)(env, f2),
                      HELPER(fr_get_hi)(env, f2) };
    b = (IA64FRBits) { HELPER(fr_get_lo)(env, f3),
                      HELPER(fr_get_hi)(env, f3) };
    result = ia64_f9_result(op, a, b);
    HELPER(fr_set_lo)(env, f1, result.significand);
    HELPER(fr_set_hi)(env, f1, result.sign_exp);
    env->psr |= f1 < IA64_FR_ROT_BASE ? IA64_PSR_MFL : IA64_PSR_MFH;
}

''' + s[pos:]
p.write_text(s)
p = r / 'target/ia64/cpu.h'
s = p.read_text().replace('/* Lower FP registers disabled */', '/* Lower FP registers modified */').replace('/* Upper FP registers disabled */', '/* Upper FP registers modified */').replace('/* Lower FP fault disabled */', '/* Lower FP registers disabled */').replace('/* Upper FP fault disabled */', '/* Upper FP registers disabled */')
p.write_text(s)
p = r / 'scripts/ia64-isa-coverage.py'
s = p.read_text()
s = s.replace(" xma_l xma_hu xma_h'''.split()", " xma_l xma_hu xma_h f9'''.split()")
s = s.replace("    path = directory / 'f-unit-probe.c'", "    header = (root / 'target/ia64/fp-bitops.h').read_bytes()\n    prelude += '#include \"fp-bitops.h\"\\n'\n    path = directory / 'f-unit-probe.c'")
s = s.replace("'-O0', str(path), '-o', str(executable)", "'-O0', '-I' + str(root / 'target/ia64'), str(path), '-o', str(executable)")
s = s.replace('return executable, digest(block.encode())', "return executable, digest(block.encode() + b'\\0fp-bitops.h\\0' + header)")
s = s.replace('source-block hash supplies provenance', 'source-block-plus-F9-header hash supplies provenance')
s = s.replace('- F-block SHA-256:', '- F-block plus F9 header SHA-256:')
s = s.replace('No emulator semantics are changed by this audit tranche.', 'The audit itself does not execute instructions; F9 data-path/state tests are separate.')
p.write_text(s)
p = r / 'tests/ia64/isa/test_coverage.py'
s = p.read_text().replace("Path('target/ia64/translate.c')):", "Path('target/ia64/translate.c'), Path('target/ia64/fp-bitops.h')):")
pos = s.index('    def test_known_break_zero_skip')
s = s[:pos] + '''    def test_all_f9_forms_reach_the_bit_operation_helper(self):
        vectors = [v for v in self.vectors if v['format'] == 'F9']
        self.assertEqual(len(vectors), 19 * len(AUDIT.PROFILES))
        for v in vectors:
            self.assertEqual(self.routes[v['id']], 'f9', v['id'])

''' + s[pos:]
p.write_text(s)
p = r / 'tests/ia64/isa/coverage.json5'
s = p.read_text()
data = json5.loads(s)
old = data['limitations']['FP-BITOPS']
new = 'All 19 F9 data paths are integer-only 82-bit/packed operations with NaTVal propagation. Production-helper tests cover aliases, all 96 FR rotations, dirty bits and illegal-destination fault requests. Full QEMU disabled-FP fault delivery remains #10; an accepted encoding is not whole-instruction conformance.'
s = s.replace(json.dumps(old), json.dumps(new))
for row in data['forms']:
    if row['format'] == 'F9' and row['semantics'] == 'MISSING':
        line = next(l for l in s.splitlines() if '"mnemonic": "' + row['mnemonic'] + '"' in l)
        s = s.replace(line, line.replace('"semantics": "MISSING"', '"semantics": "FP-BITOPS"'))
p.write_text(s)
# The generated output must match the previously reviewed local run exactly.
subprocess.run([sys.executable, 'scripts/ia64-isa-coverage.py', '--record'], check=True, timeout=60)
after = {
 'target/ia64/translate.c': '700bf559d5d3ef69ba0f5b2fec8940cf6e96c3600f442584452629a3dc94bc1f',
 'target/ia64/helper.c': 'f199f226396d5ea59bebb82fdd17c94d0d5c8d5fd80818ca0aaea31b6f662347',
 'target/ia64/helper.h': 'e381336f3312d0c3de35ebe8e0440a5f359fca7a8d362d497643f42cae033eda',
 'target/ia64/cpu.h': 'd502ee81c103d8ba84dab101223621c79c8603efdc17cdcc499a4c16fe840c4c',
 'scripts/ia64-isa-coverage.py': 'e362a61fb64615afb0db5ed996dad948ea09f3fefa892dcb42b49469a8cd25de',
 'tests/ia64/isa/test_coverage.py': 'fbbd64a5d3daf9f9aaba51430e143bd194301a2bf30d2f1af923c16121902e27',
 'tests/ia64/isa/coverage.json5': '48e8ccf5c3e1d42089651751ed6635a00326442cc89d0955fe38ef0f034ad214',
 'tests/ia64/isa/f-unit-baseline.json5': '57b2f21735ff9c2873ecec88e37b6dcfde9e1f08c2902281726476e26b6d2a9d',
 'docs/generated/ia64-instruction-coverage.md': '0d7fe7f704aff9d5eadd9ca7e03bc25e4ac403ae518a798ced7b54d6f48d74cf',
}
for path, expected in after.items():
    if sha(path) != expected:
        raise SystemExit('Unexpected output: ' + path + ' ' + sha(path))
print('All source edits match the reviewed local hashes.')
