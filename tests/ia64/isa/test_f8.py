# SPDX-License-Identifier: GPL-2.0-or-later
"""F8 exact-result oracle plus production-helper state/fault tests."""
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
HELPER = Path("target/ia64/helper.c")
HEADER = Path("target/ia64/fp-f8.h")
MASK64 = (1 << 64) - 1
NAT = (0, 0x1fffe)
ONE = (1 << 63, 0xffff)

OPS = {
    "fmin": 0x14, "fmax": 0x15, "famin": 0x16, "famax": 0x17,
    "fpmin": 0x54, "fpmax": 0x55, "fpamin": 0x56, "fpamax": 0x57,
    "fpcmp.eq": 0x70, "fpcmp.lt": 0x71, "fpcmp.le": 0x72,
    "fpcmp.unord": 0x73, "fpcmp.neq": 0x74, "fpcmp.nlt": 0x75,
    "fpcmp.nle": 0x76, "fpcmp.ord": 0x77,
}


def word(name, f1=6, f2=7, f3=8, sf=0, qp=0):
    op = OPS[name]
    return (((op >> 6) & 1) << 37) | (sf << 34) | ((op & 0x3f) << 27) | \
           (f3 << 20) | (f2 << 13) | (f1 << 6) | qp


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError("production helper shape changed: " + signature)
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


class FR(ctypes.Structure):
    _fields_ = [("significand", ctypes.c_uint64), ("sign_exp", ctypes.c_uint64)]


class State(ctypes.Structure):
    _fields_ = [
        ("f", (ctypes.c_uint64 * 2) * 128),
        ("cfm", ctypes.c_uint64),
        ("psr", ctypes.c_uint64),
        ("ar", ctypes.c_uint64 * 128),
    ]


def compile_harness(root, directory):
    source = (root / HELPER).read_text()
    cpu = (root / "target/ia64/cpu.h").read_text()
    signatures = [
        "static inline uint32_t ia64_fr_phys(",
        "uint64_t HELPER(fr_get_lo)(",
        "uint64_t HELPER(fr_get_hi)(",
        "void HELPER(fr_set_lo)(",
        "void HELPER(fr_set_hi)(",
        "static uint16_t ia64_fp_disabled_code(",
        "void HELPER(f8)(",
    ]
    bodies = "\n".join(function(source, sig) for sig in signatures)
    constants = []
    for name in ("IA64_FR_ROT_BASE", "IA64_FR_ROT_SIZE", "IA64_CFM_RRBF_SHIFT",
                 "IA64_CFM_RRBF_MASK", "IA64_PSR_MFL", "IA64_PSR_MFH",
                 "IA64_PSR_DFL", "IA64_PSR_DFH", "IA64_AR_FPSR"):
        lines = [line for line in (source + "\n" + cpu).splitlines()
                 if line.startswith("#define " + name + " ")]
        if len(set(lines)) != 1:
            raise ValueError("constant shape changed: " + name)
        constants.append(lines[0])
    prelude = """#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>
#include "fp-f8.h"
typedef struct CPUIA64State {
    uint64_t f[128][2], cfm, psr, ar[128];
} CPUIA64State;
#define HELPER(x) helper_##x
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GETPC() 0
#define IA64_VEC_ILLEGAL_OP 0x5400
#define IA64_VEC_DISABLED_FP 0x5500
#define IA64_VEC_FP_FAULT 0x5c00
static jmp_buf escape;
static uint16_t delivered_code;
static void ia64_fp_interrupt(CPUIA64State *env, uint32_t vector,
                              uint16_t code, uint64_t isr_extra,
                              bool trap, uintptr_t pc) {
    (void)env; (void)isr_extra; (void)trap; (void)pc;
    delivered_code = code;
    longjmp(escape, vector);
}
"""
    exports = """
static IA64F8Result evaluated;
void evaluate(int op, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi,
              uint64_t fpsr, unsigned sf) {
    evaluated = ia64_f8_result(op, (IA64FRBits){alo, ahi},
                               (IA64FRBits){blo, bhi}, fpsr, sf);
}
uint64_t eval_lo(void) { return evaluated.value.significand; }
uint64_t eval_hi(void) { return evaluated.value.sign_exp; }
uint32_t eval_flags(void) { return evaluated.flags; }
uint16_t eval_fault(void) { return evaluated.fault_code; }
int decode(uint64_t insn) { return ia64_f8_decode(insn); }
uint16_t last_code(void) { return delivered_code; }
int run(CPUIA64State *env, uint64_t insn) {
    delivered_code = 0;
    int fault = setjmp(escape);
    if (!fault) helper_f8(env, insn);
    return fault;
}
"""
    path = directory / "f8-test.c"
    path.write_text(prelude + "\n".join(constants) + "\n" + bodies + exports)
    library = directory / "f8-test.so"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-shared", "-fPIC",
        "-I" + str(root / "target/ia64"), str(path), "-o", str(library)],
        check=True, capture_output=True, text=True, timeout=60)
    lib = ctypes.CDLL(str(library))
    lib.evaluate.argtypes = [ctypes.c_int] + [ctypes.c_uint64] * 5 + [ctypes.c_uint]
    for name in ("eval_lo", "eval_hi"):
        getattr(lib, name).restype = ctypes.c_uint64
    lib.eval_flags.restype = ctypes.c_uint32
    lib.eval_fault.restype = ctypes.c_uint16
    lib.decode.argtypes, lib.decode.restype = [ctypes.c_uint64], ctypes.c_int
    lib.run.argtypes, lib.run.restype = [ctypes.POINTER(State), ctypes.c_uint64], ctypes.c_int
    lib.last_code.restype = ctypes.c_uint16
    return lib


def ext_class(v):
    sig, se = v
    exp, sign = se & 0x1ffff, bool(se & 0x20000)
    if v == NAT:
        return "nat", sign
    if exp == 0x1ffff:
        if not (sig >> 63):
            return "invalid", sign
        if sig == 1 << 63:
            return "inf", sign
        return ("qnan" if sig & (1 << 62) else "snan"), sign
    if sig == 0:
        return ("zero" if exp == 0 else "unorm"), sign
    if exp == 0 or not (sig >> 63):
        return "unorm", sign
    return "normal", sign


def ext_mag_key(v):
    sig, se = v
    if not sig:
        return (-1, 0)
    exp = se & 0x1ffff
    exp = exp or 1
    return (exp + sig.bit_length(), sig << (64 - sig.bit_length()))


def ext_cmp(a, b, absolute=False):
    ca, sa = ext_class(a)
    cb, sb = ext_class(b)
    if ca == "inf" or cb == "inf":
        mag = 0 if ca == cb == "inf" else (1 if ca == "inf" else -1)
    else:
        ka, kb = ext_mag_key(a), ext_mag_key(b)
        mag = (ka > kb) - (ka < kb)
    if absolute:
        return mag
    if a[0] == 0 and b[0] == 0:
        return 0
    if sa != sb:
        return -1 if sa else 1
    return -mag if sa else mag


def single_cmp(a, b, absolute=False):
    am, bm = a & 0x7fffffff, b & 0x7fffffff
    if absolute:
        return (am > bm) - (am < bm)
    if am == bm == 0:
        return 0
    sa, sb = a >> 31, b >> 31
    if sa != sb:
        return -1 if sa else 1
    cmp = (am > bm) - (am < bm)
    return -cmp if sa else cmp


class F8Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="ia64-f8-")
        cls.lib = compile_harness(ROOT, Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def evaluate(self, name, a, b, fpsr=0x3f, sf=0):
        self.lib.evaluate(OPS[name], *a, *b, fpsr, sf)
        return ((self.lib.eval_lo(), self.lib.eval_hi()),
                self.lib.eval_flags(), self.lib.eval_fault())

    def test_decode_exact_f8_families(self):
        for major, x, x6 in itertools.product(range(16), range(2), range(64)):
            insn = (major << 37) | (x << 33) | (x6 << 27) | (8 << 20) | (7 << 13) | (6 << 6)
            expected = -1
            key = (major << 6) | x6
            if key in OPS.values() and x == 0:
                expected = key
            self.assertEqual(self.lib.decode(insn), expected, (major, x, x6))

    def test_scalar_minmax_order_abs_ties_and_nat(self):
        rng = random.Random(0xf8002026)
        for _ in range(4096):
            # Supported normal extended values only: independent integer oracle.
            a = (rng.getrandbits(63) | (1 << 63), rng.randrange(1, 0x1ffff))
            b = (rng.getrandbits(63) | (1 << 63), rng.randrange(1, 0x1ffff))
            if rng.getrandbits(1):
                a = (a[0], a[1] | 0x20000)
            if rng.getrandbits(1):
                b = (b[0], b[1] | 0x20000)
            for name in ("fmin", "fmax", "famin", "famax"):
                cmp = ext_cmp(a, b, name in ("famin", "famax"))
                want = a if ((name in ("fmin", "famin") and cmp < 0) or
                             (name in ("fmax", "famax") and cmp > 0)) else b
                self.assertEqual(self.evaluate(name, a, b)[0], want)
        a = (0x8000000000000000, 0xffff)
        b = (0x8000000000000000, 0xffff)
        self.assertEqual(self.evaluate("fmin", a, b)[0], b)
        for name in ("fmin", "fmax", "famin", "famax"):
            self.assertEqual(self.evaluate(name, NAT, b), (NAT, 0, 0))

    def test_scalar_special_and_denormal_events(self):
        normal = ONE
        qnan = (0xc000000000000001, 0x1ffff)
        snan = (0x8000000000000001, 0x1ffff)
        unsupported = (0x4000000000000000, 0x1ffff)
        unnormal = (0x4000000000000000, 0xffff)
        for special in (qnan, snan, unsupported):
            value, flags, fault = self.evaluate("fmax", special, normal)
            self.assertEqual((value, flags, fault), (normal, 1, 0))
            self.assertEqual(self.evaluate("fmax", special, normal, fpsr=0)[2], 1)
        value, flags, fault = self.evaluate("fmin", unnormal, normal)
        self.assertEqual(flags, 2)
        self.assertEqual(fault, 0)
        self.assertEqual(self.evaluate("fmin", unnormal, normal, fpsr=0)[2], 2)
        self.assertNotEqual(value, NAT)

    def test_packed_minmax_and_compare_relations(self):
        lanes = [
            (0x3f800000, 0x40000000),  # 1, 2
            (0xbf800000, 0x3f800000),  # -1, 1
            (0x80000000, 0),           # -0, +0 tie
            (0x7f800000, 0x3f800000),  # inf, 1
        ]
        for ah, bh in lanes:
            for al, bl in reversed(lanes):
                a = ((ah << 32) | al, 0x1003e)
                b = ((bh << 32) | bl, 0x1003e)
                for name in ("fpmin", "fpmax", "fpamin", "fpamax"):
                    absolute = name in ("fpamin", "fpamax")
                    is_min = name in ("fpmin", "fpamin")
                    ch, cl = single_cmp(ah, bh, absolute), single_cmp(al, bl, absolute)
                    rh = ah if (ch < 0 if is_min else ch > 0) else bh
                    rl = al if (cl < 0 if is_min else cl > 0) else bl
                    self.assertEqual(self.evaluate(name, a, b)[0],
                                     ((rh << 32) | rl, 0x1003e))
        a = ((0x3f800000 << 32) | 0x7fc00000, 0x1003e)
        b = ((0x40000000 << 32) | 0x3f800000, 0x1003e)
        expected = {
            "fpcmp.eq": (0, 0), "fpcmp.lt": (0xffffffff, 0),
            "fpcmp.le": (0xffffffff, 0), "fpcmp.unord": (0, 0xffffffff),
            "fpcmp.neq": (0xffffffff, 0xffffffff),
            "fpcmp.nlt": (0, 0xffffffff), "fpcmp.nle": (0, 0xffffffff),
            "fpcmp.ord": (0xffffffff, 0),
        }
        for name, (hi, lo) in expected.items():
            value, _, _ = self.evaluate(name, a, b)
            self.assertEqual(value, ((hi << 32) | lo, 0x1003e), name)

    def test_packed_lane_fault_codes_and_masks(self):
        # high lane qNaN ordered compare => V in HI code; low denormal => D in LO code.
        a = ((0x7fc00000 << 32) | 1, 0x1003e)
        b = ((0x3f800000 << 32) | 0x3f800000, 0x1003e)
        value, flags, fault = self.evaluate("fpcmp.lt", a, b, fpsr=0)
        self.assertEqual(flags, 3)
        self.assertEqual(fault, 1 | (2 << 4))
        self.assertEqual(value[0] >> 32, 0)
        # qNaN eq is unordered false without V; low denormal still D.
        _, flags, fault = self.evaluate("fpcmp.eq", a, b, fpsr=0)
        self.assertEqual((flags, fault), (2, 2 << 4))
        # Alternate-field TD masks all FP events.
        _, flags, fault = self.evaluate("fpcmp.lt", a, b, fpsr=(0x40 << (6 + 13)), sf=1)
        self.assertEqual(flags, 3)
        self.assertEqual(fault, 0)

    def test_production_helper_rotation_aliases_fault_order_and_commit(self):
        for rotation in (0, 1, 47, 95):
            state = State()
            state.cfm = rotation << 25
            state.psr = 0x2004
            state.ar[40] = 0x3f  # V/D masked
            for i in range(128):
                state.f[i][:] = (0x8000000000000000 | i, 0xffff + (i & 3))
            f1, f2, f3 = 32, 126, 127
            phys = lambda f: f if f < 32 else 32 + ((f - 32 + rotation) % 96)
            a, b = tuple(state.f[phys(f2)]), tuple(state.f[phys(f3)])
            before = [tuple(x) for x in state.f]
            self.assertEqual(self.lib.run(ctypes.byref(state), word("fmax", f1, f2, f3)), 0)
            want = self.evaluate("fmax", a, b)[0]
            before[phys(f1)] = want
            self.assertEqual([tuple(x) for x in state.f], before)
            self.assertEqual(state.psr, 0x2004 | 0x20)

        state = State()
        state.psr = 0x40000
        state.ar[40] = 0
        state.f[7][:] = (0xc000000000000001, 0x1ffff)
        state.f[8][:] = ONE
        before = bytes(state)
        self.assertEqual(self.lib.run(ctypes.byref(state), word("fmax", 6, 7, 8)), 0x5500)
        self.assertEqual(self.lib.last_code(), 1)
        self.assertEqual(bytes(state), before)

        state = State()
        state.psr = 0x40000
        before = bytes(state)
        self.assertEqual(self.lib.run(ctypes.byref(state), word("fmax", 1, 7, 8)), 0x5400)
        self.assertEqual(bytes(state), before)

        state = State()
        state.ar[40] = 0  # invalid enabled
        state.f[7][:] = (0xc000000000000001, 0x1ffff)
        state.f[8][:] = ONE
        state.f[6][:] = (0x1234, 0x23456)
        before = bytes(state)
        self.assertEqual(self.lib.run(ctypes.byref(state), word("fmax", 6, 7, 8)), 0x5c00)
        self.assertEqual(self.lib.last_code(), 1)
        self.assertEqual(bytes(state), before)

        state = State()
        state.ar[40] = 0x3f
        state.f[7][:] = (0xc000000000000001, 0x1ffff)
        state.f[8][:] = ONE
        self.assertEqual(self.lib.run(ctypes.byref(state), word("fmax", 6, 7, 8)), 0)
        self.assertEqual(tuple(state.f[6]), ONE)
        self.assertTrue(state.ar[40] & (1 << 13))
        self.assertTrue(state.psr & 0x10)


if __name__ == "__main__":
    unittest.main()
