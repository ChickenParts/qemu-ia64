# SPDX-License-Identifier: GPL-2.0-or-later
"""F9 bit-result oracle and tests of the actual production helper.

Expected values use bit strings/lane concatenation, not host floats or the C
implementation. The helper harness runs the production C accessor and helper
bodies; its fault hook only observes delivery requests, not QEMU exceptions.
"""
import ctypes
import itertools
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HEADER = Path('target/ia64/fp-bitops.h')
HELPER = Path('target/ia64/helper.c')
OPS = {
    'fmerge.s': 0x10, 'fmerge.ns': 0x11, 'fmerge.se': 0x12,
    'fpack': 0x28, 'fand': 0x2c, 'fandcm': 0x2d, 'for': 0x2e,
    'fxor': 0x2f, 'fswap': 0x34, 'fswap.nl': 0x35, 'fswap.nr': 0x36,
    'fmix.lr': 0x39, 'fmix.r': 0x3a, 'fmix.l': 0x3b,
    'fsxt.r': 0x3c, 'fsxt.l': 0x3d,
    'fpmerge.s': 0x50, 'fpmerge.ns': 0x51, 'fpmerge.se': 0x52,
}
MASK = (1 << 64) - 1
NAT = (0, 0x1fffe)
PATTERNS = (
    (0x0123456789abcdef, 0x255aa),
    (0xfedcba9876543210, 0x12345),
    (0x800000007fffffff, 0x3003e),
    (0x7fffffff80000000, 0x1003e),
    (0, 0), (0, 0x20000), (0x8000000000000000, 0xffff),
    (0x8000000000000000, 0x1ffff),  # infinity
    (0xc123456789abcdef, 0x1ffff),  # qNaN, payload must not be rounded
    (0x8123456789abcdef, 0x3ffff),  # negative sNaN
    NAT, (0, 0x3fffe),             # sign distinguishes NaT from pseudo-zero
    (1 << 40, 0xff81),             # minimum SP denormal
    (0x7fffff << 40, 0xff81),       # maximum SP denormal
    (0x8000000000000000, 0xff81),  # minimum SP normal
    (MASK, 0x3fffe),
)


def word(name, f1=6, f2=7, f3=8, qp=0):
    op = OPS[name]
    return ((op >> 6) << 37) | ((op & 63) << 27) | (f3 << 20) | (f2 << 13) | (f1 << 6) | qp


def oracle(name, a, b):
    """Independent Intel F9 operation expressed as bit/lane selection."""
    if a == NAT or b == NAT:
        return NAT
    sa, sb = f'{a[0]:064b}', f'{b[0]:064b}'
    ha, hb = f'{a[1] & 0x3ffff:018b}', f'{b[1] & 0x3ffff:018b}'
    ah, al, bh, bl = sa[:32], sa[32:], sb[:32], sb[32:]
    invert = lambda s: ''.join('1' if c == '0' else '0' for c in s)
    hi = 0x1003e
    if name.startswith('fmerge.'):
        sign = invert(ha[0]) if name == 'fmerge.ns' else ha[0]
        hi = int(ha if name == 'fmerge.se' else sign + hb[1:], 2)
        bits = sb
    elif name == 'fpack':
        def single(sig, head):
            exponent = head[1] + head[-7:] if sig[0] == '1' else '0' * 8
            return head[0] + exponent + sig[1:24]
        bits = single(sa, ha) + single(sb, hb)
    elif name in ('fand', 'fandcm', 'for', 'fxor'):
        if name == 'fandcm':
            sb = invert(sb)
        fun = {'fand': lambda x, y: x and y,
               'fandcm': lambda x, y: x and y,
               'for': lambda x, y: x or y,
               'fxor': lambda x, y: x != y}[name]
        bits = ''.join('1' if fun(x == '1', y == '1') else '0'
                       for x, y in zip(sa, sb))
    elif name.startswith('fswap'):
        left, right = bl, ah
        if name == 'fswap.nl':
            left = invert(left[0]) + left[1:]
        if name == 'fswap.nr':
            right = invert(right[0]) + right[1:]
        bits = left + right
    elif name.startswith('fmix.'):
        bits = {'fmix.lr': ah + bl, 'fmix.r': al + bl, 'fmix.l': ah + bh}[name]
    elif name.startswith('fsxt.'):
        sign, payload = (al[0], bl) if name == 'fsxt.r' else (ah[0], bh)
        bits = sign * 32 + payload
    elif name.startswith('fpmerge.'):
        n = 9 if name == 'fpmerge.se' else 1
        def lane(x, y):
            top = invert(x[:n]) if name == 'fpmerge.ns' else x[:n]
            return top + y[n:]
        bits = lane(ah, bh) + lane(al, bl)
    else:
        raise ValueError(name)
    return int(bits, 2), hi


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError('production helper shape changed: ' + signature)
    start = source.index(signature)
    # Production functions use an unindented closing brace; fail closed if moved.
    end = source.index('\n}\n', start) + 3
    return source[start:end]


class FR(ctypes.Structure):
    _fields_ = [('significand', ctypes.c_uint64), ('sign_exp', ctypes.c_uint64)]


class State(ctypes.Structure):
    _fields_ = [('f', (ctypes.c_uint64 * 2) * 128),
                ('cfm', ctypes.c_uint64), ('psr', ctypes.c_uint64)]


def compile_harness(root, directory):
    source = (root / HELPER).read_text()
    cpu = (root / 'target/ia64/cpu.h').read_text()
    signatures = ['static inline uint32_t ia64_fr_phys(',
                  'uint64_t HELPER(fr_get_lo)(', 'uint64_t HELPER(fr_get_hi)(',
                  'void HELPER(fr_set_lo)(', 'void HELPER(fr_set_hi)(',
                  'void HELPER(f9)(']
    bodies = '\n'.join(function(source, s) for s in signatures)
    # Use production constants as well as bodies, rather than testing a mirror.
    constants = []
    for name in ('IA64_FR_ROT_BASE', 'IA64_FR_ROT_SIZE', 'IA64_CFM_RRBF_SHIFT',
                 'IA64_CFM_RRBF_MASK', 'IA64_PSR_MFL', 'IA64_PSR_MFH'):
        lines = [l for l in (source + '\n' + cpu).splitlines()
                 if l.startswith('#define ' + name + ' ')]
        if len(set(lines)) != 1:
            raise ValueError('constant shape changed: ' + name)
        constants.append(lines[0])
    prelude = '''#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>
#include "fp-bitops.h"
typedef struct CPUIA64State { uint64_t f[128][2], cfm, psr; } CPUIA64State;
#define HELPER(x) helper_##x
#define env_cpu(e) (e)
#define GETPC() 0
#define IA64_VEC_ILLEGAL_OP 0x5400
#define g_assert_not_reached() abort()
static jmp_buf escape;
static bool ia64_fault(void *cpu, CPUIA64State *env, bool write, bool data,
                       int vector, uint64_t iim, uintptr_t pc) {
    (void)cpu; (void)env; (void)write; (void)data; (void)iim; (void)pc;
    longjmp(escape, vector);
    return false;
}
'''
    exports = '''
int decode(uint64_t insn) { return ia64_f9_decode(insn); }
IA64FRBits result(int op, IA64FRBits a, IA64FRBits b) {
    return ia64_f9_result(op, a, b);
}
int run(CPUIA64State *env, uint64_t insn) {
    int fault = setjmp(escape);
    if (!fault) { helper_f9(env, insn); }
    return fault;
}
'''
    path = directory / 'f9-test.c'
    path.write_text(prelude + '\n'.join(constants) + '\n' + bodies + exports)
    library = directory / 'f9-test.so'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-shared', '-fPIC',
        '-I' + str(root / 'target/ia64'), str(path), '-o', str(library)],
        check=True, capture_output=True, text=True, timeout=60)
    lib = ctypes.CDLL(str(library))
    lib.result.argtypes, lib.result.restype = [ctypes.c_int, FR, FR], FR
    lib.decode.argtypes, lib.decode.restype = [ctypes.c_uint64], ctypes.c_int
    lib.run.argtypes, lib.run.restype = [ctypes.POINTER(State), ctypes.c_uint64], ctypes.c_int
    return lib


class F9Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='ia64-f9-')
        cls.lib = compile_harness(ROOT, Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def result(self, name, a, b):
        r = self.lib.result(OPS[name], FR(*a), FR(*b))
        return r.significand, r.sign_exp

    def test_all_f9_opcode_keys(self):
        for major, x, x6 in itertools.product(range(16), range(2), range(64)):
            insn = major << 37 | x << 33 | x6 << 27 | 8 << 20 | 7 << 13 | 6 << 6
            expected = ((major << 6) | x6) if (major, x, x6) in {
                (op >> 6, 0, op & 63) for op in OPS.values()} else -1
            self.assertEqual(self.lib.decode(insn), expected)

    def test_golden_merge_uses_both_sources(self):
        a, b = PATTERNS[0], PATTERNS[1]
        self.assertEqual(self.result('fmerge.s', a, b), (b[0], 0x32345))
        self.assertEqual(self.result('fmerge.ns', a, b), (b[0], 0x12345))
        self.assertEqual(self.result('fmerge.se', a, b), (b[0], a[1]))

    def test_golden_fsxt_uses_independent_sign(self):
        a, b = PATTERNS[2], PATTERNS[1]
        self.assertEqual(self.result('fsxt.r', a, b), (0x76543210, 0x1003e))
        self.assertEqual(self.result('fsxt.l', a, b), (0xfffffffffedcba98, 0x1003e))

    def test_pack_denormal_normal_infinity_and_zero(self):
        one = (1 << 63, 0xffff)
        denorm = (1 << 40, 0xff81)
        self.assertEqual(self.result('fpack', one, denorm), (0x3f80000000000001, 0x1003e))
        self.assertEqual(self.result('fpack', (1 << 63, 0x1ffff), (0, 0x20000)),
                         (0x7f80000080000000, 0x1003e))
        self.assertEqual(self.result('fpack', (0x7fffff << 40, 0xff81),
                                     (1 << 63, 0xff81)), (0x007fffff00800000, 0x1003e))

    def test_all_pattern_pairs(self):
        for name, a, b in itertools.product(OPS, PATTERNS, PATTERNS):
            self.assertEqual(self.result(name, a, b), oracle(name, a, b),
                             (name, a, b))

    def test_deterministic_random_bit_patterns(self):
        rng = random.Random(0xf9002026)
        for name in OPS:
            for _ in range(1024):
                a = (rng.getrandbits(64), rng.getrandbits(18))
                b = (rng.getrandbits(64), rng.getrandbits(18))
                self.assertEqual(self.result(name, a, b), oracle(name, a, b),
                                 (name, a, b))

    def test_natval_sign_is_significant(self):
        for name in OPS:
            self.assertEqual(self.result(name, NAT, PATTERNS[0]), NAT)
            self.assertEqual(self.result(name, PATTERNS[0], NAT), NAT)
            self.assertEqual(self.result(name, (0, 0x3fffe), PATTERNS[0]),
                             oracle(name, (0, 0x3fffe), PATTERNS[0]))

    def test_runtime_aliases_rotation_dirty_state(self):
        profiles = [(6, 7, 8), (7, 7, 8), (8, 7, 8), (7, 7, 7),
                    (31, 32, 127), (32, 127, 31), (127, 126, 32), (127, 127, 32)]
        for name, rotation, regs in itertools.product(OPS, range(96), profiles):
            state = State()
            state.cfm = rotation << 25
            state.psr = 0x2004  # unrelated flags must be preserved
            for i in range(128):
                state.f[i][:] = (0x1234567000000000 + i, 0x10000 + i)
            state.f[0][:], state.f[1][:] = (0, 0), (1 << 63, 0xffff)
            f1, f2, f3 = regs
            phys = lambda f: f if f < 32 else 32 + ((f - 32 + rotation) % 96)
            p1, p2, p3 = map(phys, regs)
            a, b = tuple(state.f[p2]), tuple(state.f[p3])
            before = [tuple(f) for f in state.f]
            self.assertEqual(self.lib.run(ctypes.byref(state), word(name, f1, f2, f3)), 0)
            before[p1] = oracle(name, a, b)
            self.assertEqual([tuple(f) for f in state.f], before, (name, rotation, regs))
            self.assertEqual(state.psr, 0x2004 | (16 if f1 < 32 else 32))
            self.assertEqual(state.cfm, rotation << 25)

    def test_illegal_destinations_request_fault_before_writes(self):
        for name, f1 in itertools.product(OPS, (0, 1)):
            state = State()
            state.f[0][:], state.f[1][:] = (0, 0), (1 << 63, 0xffff)
            before = bytes(state)
            self.assertEqual(self.lib.run(ctypes.byref(state), word(name, f1)), 0x5400)
            self.assertEqual(bytes(state), before)

    def test_bit_operation_mutation_is_detected(self):
        with tempfile.TemporaryDirectory(prefix='f9-mutant-') as tmp:
            root = Path(tmp)
            (root / 'target/ia64').mkdir(parents=True)
            for p in (HEADER, HELPER, Path('target/ia64/cpu.h')):
                (root / p).write_bytes((ROOT / p).read_bytes())
            h = root / HEADER
            s = h.read_text()
            old = 'result.sign_exp = a.sign_exp & 0x3ffff;'
            self.assertEqual(s.count(old), 1)
            h.write_text(s.replace(old, 'result.sign_exp = b.sign_exp & 0x3ffff;'))
            lib = compile_harness(root, root)
            a, b = PATTERNS[:2]
            result = lib.result(OPS['fmerge.se'], FR(*a), FR(*b))
            self.assertNotEqual((result.significand, result.sign_exp), oracle('fmerge.se', a, b))


if __name__ == '__main__':
    unittest.main()
