# SPDX-License-Identifier: GPL-2.0-or-later
"""F10 exact rational oracle, production header/helper/accessor tests.

The oracle rounds Python Fractions, not host floats or copied C algorithms.
The native exception hook records requests; it does not emulate QEMU delivery.
"""
import ctypes
from fractions import Fraction
import itertools
import os
from pathlib import Path
import random
import shlex
import shutil
import subprocess
import tempfile
import unittest

from test_f9 import FR, function

ROOT = Path(__file__).resolve().parents[3]
MASK = (1 << 64) - 1
NAT = (0, 0x1fffe)
OPS = {'fcvt.fx': 0, 'fcvt.fxu': 1, 'fcvt.fx.trunc': 2,
       'fcvt.fxu.trunc': 3, 'fpcvt.fx': 4, 'fpcvt.fxu': 5,
       'fpcvt.fx.trunc': 6, 'fpcvt.fxu.trunc': 7}


def word(op, sf=0, f1=6, f2=7, qp=0):
    return ((op >> 2) << 37 | sf << 34 | (0x18 + (op & 3)) << 27 |
            f2 << 13 | f1 << 6 | qp)


def scalar(value):
    """Exactly encode a small dyadic Fraction (also handles integer limits)."""
    value = Fraction(value)
    if not value:
        return 0, 0
    negative = value < 0
    value = abs(value)
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    significand = value * Fraction(2) ** (63 - exponent)
    assert significand.denominator == 1 and significand < 1 << 64
    return int(significand), (0x20000 if negative else 0) | (0xffff + exponent)


SCALAR_PATTERNS = [scalar(x) for x in (
    0, 1, -1, Fraction(1, 4), Fraction(-1, 4), Fraction(1, 2), Fraction(-1, 2),
    Fraction(3, 2), Fraction(-3, 2), Fraction(5, 2), Fraction(-5, 2),
    Fraction(7, 4), Fraction(-7, 4), (1 << 63) - 1, -(1 << 63),
    1 << 63, -(1 << 63) - 1, (1 << 64) - 1, 1 << 64,
    Fraction(2 * (1 << 63) - 1, 2), Fraction(-2 * (1 << 63) + 1, 2),
    (1 << 32) - 1, 1 << 32, (1 << 31) - 1, -(1 << 31),
)] + [
    NAT, (0, 0x3fffe), (0, 0x20000), (0, 1),
    (1, 0), (1 << 63, 0), (1, 1), (1 << 63, 1),
    (1, 0x1003e), (MASK, 0x1fffe), (MASK, 0x3fffe),
    (0, 0x1ffff), (1 << 63, 0x1ffff), (1 << 63, 0x3ffff),
    (0xc123456789abcdef, 0x1ffff), (0x8123456789abcdef, 0x3ffff),
    (1, 0x1ffff), (1 << 40, 0xff81), (1 << 40, 0x2ff81),
]
LANES = [0, 0x80000000, 0x3e800000, 0xbe800000, 0x3f000000, 0xbf000000,
         0x3fc00000, 0xbfc00000, 0x40200000, 0xc0200000,
         0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001,
         0x4f7fffff, 0x4f800000, 1, 0x80000001, 0x007fffff,
         0x00800000, 0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345]
PACKED_PATTERNS = [(x << 32 | LANES[(i * 7 + 3) % len(LANES)], 0x1003e)
                   for i, x in enumerate(LANES)] + [NAT, (0, 0x3fffe)]


def integer_oracle(value, width, unsigned, rounding, unnormal):
    if value is None:
        return 1 << (width - 1), 1
    if rounding == 0:
        rounded = round(value)  # Fraction's nearest/even, including negatives.
    elif rounding == 1:
        rounded = value.numerator // value.denominator
    elif rounding == 2:
        rounded = -((-value.numerator) // value.denominator)
    else:
        rounded = int(value)
    lower = 0 if unsigned else -(1 << (width - 1))
    upper = (1 << (width if unsigned else width - 1)) - 1
    if not lower <= rounded <= upper:
        return 1 << (width - 1), 1
    return rounded % (1 << width), (2 if unnormal else 0) | (32 if value != rounded else 0)


def source_value(source, packed=False):
    if packed:
        exponent = (source >> 23) & 255
        fraction = source & 0x7fffff
        sign = -1 if source >> 31 else 1
        if exponent == 255:
            return None, False
        value = sign * Fraction((1 << 23) + fraction if exponent else fraction)
        value *= Fraction(2) ** ((exponent or 1) - 127 - 23)
        return value, exponent == 0 and fraction != 0
    sig, hi = source
    exponent = hi & 0x1ffff
    if exponent == 0x1ffff:
        return None, False
    unnormal = (exponent == 0 and sig != 0) or (exponent != 0 and sig < 1 << 63)
    effective_exponent = exponent or 0xc001
    value = Fraction(sig) * Fraction(2) ** (effective_exponent - 0xffff - 63)
    return (-value if hi & 0x20000 else value), unnormal


def fpsr_for(sf, rc, disabled=63, td=False, sticky=0):
    # All other fields have different RC values to catch an incorrect selector.
    fpsr = disabled
    for field in range(4):
        mode = rc if field == sf else (rc + field + 1) & 3
        fpsr |= (mode << 4 | 0x0c) << (6 + 13 * field)
    if td:
        fpsr |= 1 << (12 + 13 * sf)
    return fpsr | sticky


def oracle(op, source, fpsr, sf):
    if source[0] == 0 and source[1] & 0x3ffff == 0x1fffe:
        return NAT, 0, 0
    controls = fpsr >> (6 + 13 * sf) & 127
    mode = 3 if op & 2 else (controls >> 4) & 3
    if op & 4:
        values = [source_value(source[0] >> shift & 0xffffffff, True) for shift in (0, 32)]
        lo, hi = [integer_oracle(v, 32, op & 1, mode, d) for v, d in values]
        sig, flags = hi[0] << 32 | lo[0], hi[1] | lo[1]
    else:
        value, unnormal = source_value(source)
        sig, flags = integer_oracle(value, 64, op & 1, mode, unnormal)
    disabled = 63 if sf and controls & 64 else fpsr & 63
    return (sig, 0x1003e), flags, flags & ~disabled


class Result(ctypes.Structure):
    _fields_ = [('value', FR), ('flags', ctypes.c_uint32), ('enabled', ctypes.c_uint32)]


class State(ctypes.Structure):
    _fields_ = [('f', (ctypes.c_uint64 * 2) * 128), ('cfm', ctypes.c_uint64),
                ('psr', ctypes.c_uint64), ('ip', ctypes.c_uint64),
                ('ar', ctypes.c_uint64 * 128)]


def compile_harness(root, directory):
    source = (root / 'target/ia64/helper.c').read_text()
    cpu = (root / 'target/ia64/cpu.h').read_text()
    signatures = ['static inline uint32_t ia64_fr_phys(',
                  'uint64_t HELPER(fr_get_lo)(', 'uint64_t HELPER(fr_get_hi)(',
                  'void HELPER(fr_set_lo)(', 'void HELPER(fr_set_hi)(',
                  'void HELPER(f10)(']
    constants = []
    for name in ('IA64_FR_ROT_BASE', 'IA64_FR_ROT_SIZE', 'IA64_CFM_RRBF_SHIFT',
                 'IA64_CFM_RRBF_MASK', 'IA64_PSR_MFL', 'IA64_PSR_MFH',
                 'IA64_PSR_DFL', 'IA64_PSR_DFH', 'IA64_AR_FPSR'):
        lines = [line for line in (source + '\n' + cpu).splitlines()
                 if line.startswith('#define ' + name + ' ')]
        if len(set(lines)) != 1:
            raise ValueError('production constant changed: ' + name)
        constants.append(lines[0])
    prelude = '''#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "fp-convert.h"
typedef struct CPUIA64State {
    uint64_t f[128][2], cfm, psr, ip, ar[128];
} CPUIA64State;
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
static void helper_unimpl(CPUIA64State *env, uint64_t pc, uint32_t ri,
                          uint64_t insn, uint64_t why) {
    (void)env; (void)pc; (void)ri; (void)insn;
    const char *message = (const char *)(uintptr_t)why;
    if (strcmp(message, "F10 disabled-FP delivery unsupported") == 0) {
        longjmp(escape, 0xf10d);
    }
    if (strcmp(message, "F10 enabled-FP exception delivery unsupported") == 0) {
        longjmp(escape, 0xf10e);
    }
    abort();
}
'''
    exports = '''
int decode(uint64_t insn) { return ia64_f10_decode(insn); }
IA64F10Result result(int op, IA64FRBits source, uint64_t fpsr, unsigned sf) {
    return ia64_f10_result(op, source, fpsr, sf);
}
int run(CPUIA64State *env, uint64_t insn) {
    int fault = setjmp(escape);
    if (!fault) { helper_f10(env, insn); }
    return fault;
}
'''
    path = directory / 'f10-test.c'
    path.write_text(prelude + '\n'.join(constants) + '\n' +
                    '\n'.join(function(source, s) for s in signatures) + exports)
    library = directory / 'f10-test.so'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-shared', '-fPIC',
        '-fsanitize=undefined', '-fno-sanitize-recover=undefined',
        '-I' + str(root / 'target/ia64'), str(path), '-o', str(library)],
        check=True, capture_output=True, text=True, timeout=60)
    lib = ctypes.CDLL(str(library))
    lib.result.argtypes, lib.result.restype = [ctypes.c_int, FR, ctypes.c_uint64, ctypes.c_uint], Result
    lib.decode.argtypes, lib.decode.restype = [ctypes.c_uint64], ctypes.c_int
    lib.run.argtypes, lib.run.restype = [ctypes.POINTER(State), ctypes.c_uint64], ctypes.c_int
    return lib


class F10Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='ia64-f10-')
        cls.lib = compile_harness(ROOT, Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def result(self, op, source, fpsr, sf):
        r = self.lib.result(op, FR(*source), fpsr, sf)
        return (r.value.significand, r.value.sign_exp), r.flags, r.enabled

    def test_opcode_fields_and_all_status_operands(self):
        for major, x, x6 in itertools.product(range(16), range(2), range(64)):
            w = major << 37 | x << 33 | x6 << 27 | 7 << 13 | 6 << 6
            expected = (major << 2) | (x6 & 3) if major < 2 and not x and 0x18 <= x6 <= 0x1b else -1
            self.assertEqual(self.lib.decode(w), expected)
        for op, sf, f1, f2 in itertools.product(range(8), range(4), (0, 1, 6, 31, 32, 63, 64, 127), (0, 1, 7, 31, 32, 127)):
            self.assertEqual(self.lib.decode(word(op, sf, f1, f2, 63)), op)

    def test_golden_rounding_ties_and_negative_unsigned_zero(self):
        for op in range(4):
            for rc in range(4):
                for value in (Fraction(1, 2), Fraction(-1, 2), Fraction(3, 2), Fraction(-3, 2), Fraction(5, 2), Fraction(-5, 2)):
                    s, f = scalar(value), fpsr_for(0, rc)
                    self.assertEqual(self.result(op, s, f, 0), oracle(op, s, f, 0))
        self.assertEqual(self.result(1, scalar(Fraction(-1, 4)), fpsr_for(0, 0), 0), ((0, 0x1003e), 32, 0))
        self.assertEqual(self.result(1, scalar(Fraction(-1, 4)), fpsr_for(0, 1), 0), ((1 << 63, 0x1003e), 1, 0))

    def test_directed_patterns_rounding_and_status_fields(self):
        for op, rc, sf in itertools.product(range(8), range(4), range(4)):
            for source in (PACKED_PATTERNS if op & 4 else SCALAR_PATTERNS):
                fpsr = fpsr_for(sf, rc)
                self.assertEqual(self.result(op, source, fpsr, sf), oracle(op, source, fpsr, sf), (op, rc, sf, source))

    def test_random_scalar_and_packed_against_fraction_oracle(self):
        rng = random.Random(0xf102026)
        for op in range(8):
            for _ in range(2048):
                source = (rng.getrandbits(64), rng.getrandbits(18))
                # Mix extreme exponent coverage with the precision/range boundaries.
                if not op & 4 and rng.randrange(2):
                    source = (source[0], (rng.randrange(2) << 17) | (0xffff + rng.randrange(-70, 70)))
                sf, rc = rng.randrange(4), rng.randrange(4)
                fpsr = fpsr_for(sf, rc)
                self.assertEqual(self.result(op, source, fpsr, sf), oracle(op, source, fpsr, sf), (op, source, sf, rc))

    def test_event_masks_td_and_sticky_flags(self):
        for op, sf, disabled, td in itertools.product(range(8), range(4), range(64), (False, True)):
            source = PACKED_PATTERNS[16] if op & 4 else scalar(Fraction(3, 2))
            fpsr = fpsr_for(sf, 0, disabled, td, 0x20 << (13 + 13 * sf))
            self.assertEqual(self.result(op, source, fpsr, sf), oracle(op, source, fpsr, sf))
        # Pre-existing I with exact input must not cause an enabled event.
        f = fpsr_for(0, 0, 0, False, 32 << 13)
        self.assertEqual(self.result(0, scalar(1), f, 0)[2], 0)

    def test_special_encodings_invalid_priority_and_natval(self):
        for op, sf in itertools.product(range(8), range(4)):
            f = fpsr_for(sf, 0, 0)
            self.assertEqual(self.result(op, NAT, f, sf), (NAT, 0, 0))
        # Invalid suppresses lower-priority D/I within the same lane.
        self.assertEqual(self.result(0, (1, 0x1ffff), 63, 0), ((1 << 63, 0x1003e), 1, 0))
        self.assertEqual(self.result(0, (MASK >> 1, 0x1fffe), 63, 0)[1], 1)
        # Exp-zero nonzero operand is tiny, not integer-form 1.
        self.assertEqual(self.result(0, (1, 0), 63, 0), ((0, 0x1003e), 34, 0))
        self.assertEqual(self.result(0, (0, 0x3fffe), 63, 0), ((0, 0x1003e), 2, 0))

    def test_helper_rotation_aliases_dirty_and_selected_sticky_field(self):
        profiles = [(6, 7), (7, 7), (31, 32), (32, 127), (127, 127), (127, 0), (6, 1)]
        for op, sf, rotation, (f1, f2) in itertools.product(range(8), range(4), range(96), profiles):
            state = State()
            state.cfm = rotation << 25
            state.psr = 0x2004
            state.ip = 0x5000000
            state.f[1][:] = (1 << 63, 0xffff)
            phys = lambda f: f if f < 32 else 32 + (f - 32 + rotation) % 96
            source = PACKED_PATTERNS[6] if op & 4 else scalar(Fraction(-7, 4))
            if f2 <= 1:
                source = tuple(state.f[f2])
            else:
                state.f[phys(f2)][:] = source
            fpsr = fpsr_for(sf, sf, sticky=4 << (13 + 13 * ((sf + 1) % 4)))
            state.ar[40] = fpsr
            expected = oracle(op, source, fpsr, sf)
            before = [tuple(f) for f in state.f]
            self.assertEqual(self.lib.run(ctypes.byref(state), word(op, sf, f1, f2)), 0)
            self.assertEqual(tuple(state.f[phys(f1)]), expected[0])
            self.assertEqual(state.ar[40], fpsr | expected[1] << (13 + 13 * sf))
            self.assertEqual(state.psr, 0x2004 | (16 if f1 < 32 else 32))
            for i in {0, 1, phys(f2)} - {phys(f1)}:
                self.assertEqual(tuple(state.f[i]), before[i])

    def test_explicit_exception_frontiers_do_not_commit_state(self):
        for destination, source, psr, fpsr, reason in (
                (0, scalar(1), 0, 63, 0x5400), (1, scalar(1), 0, 63, 0x5400),
                (6, scalar(1), 64, 63, 0xf10d), (127, NAT, 128, 63, 0xf10d),
                (6, scalar(Fraction(3, 2)), 0, 31, 0xf10e),
                (6, (1 << 63, 0x1ffff), 0, 62, 0xf10e)):
            state = State()
            state.f[7][:] = source
            state.psr, state.ar[40] = psr, fpsr
            before = bytes(state)
            self.assertEqual(self.lib.run(ctypes.byref(state), word(0, 0, destination, 7)), reason)
            self.assertEqual(bytes(state), before)

    def test_rounding_mutation_is_detected(self):
        with tempfile.TemporaryDirectory(prefix='f10-mutation-') as tmp:
            root = Path(tmp)
            (root / 'target/ia64').mkdir(parents=True)
            for name in ('helper.c', 'cpu.h', 'fp-bitops.h', 'fp-convert.h'):
                shutil.copyfile(ROOT / 'target/ia64' / name, root / 'target/ia64' / name)
            path = root / 'target/ia64/fp-convert.h'
            text = path.read_text()
            self.assertIn('(magnitude & 1)', text)
            path.write_text(text.replace('(magnitude & 1)', '1'))
            lib = compile_harness(root, root)
            r = lib.result(0, FR(*scalar(Fraction(5, 2))), 63, 0)
            self.assertNotEqual(r.value.significand, 2)


if __name__ == '__main__':
    unittest.main()
