# SPDX-License-Identifier: GPL-2.0-or-later
"""F3/F5 raw-register semantics and production-helper tests."""
import ctypes
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HELPER = Path("target/ia64/helper.c")
HEADER = Path("target/ia64/fp-bitops.h")
NAT = (0, 0x1fffe)
ONE = (1 << 63, 0xffff)


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError("production helper shape changed: " + signature)
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


class FR(ctypes.Structure):
    _fields_ = [("significand", ctypes.c_uint64), ("sign_exp", ctypes.c_uint64)]


class State(ctypes.Structure):
    _fields_ = [("f", (ctypes.c_uint64 * 2) * 128),
                ("cfm", ctypes.c_uint64), ("psr", ctypes.c_uint64),
                ("pr", ctypes.c_uint64)]


def fselect_word(f1=6, f2=7, f3=8, f4=9, qp=0):
    return (14 << 37) | (f4 << 27) | (f3 << 20) | (f2 << 13) | (f1 << 6) | qp


def fclass_word(p1=6, p2=7, f2=8, mask=0x15, unc=False, qp=0):
    return ((5 << 37) | ((mask & 3) << 33) | (p2 << 27) |
            ((mask >> 2) << 20) | (f2 << 13) | (int(unc) << 12) |
            (p1 << 6) | qp)


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
        "static void ia64_pr_write(",
        "void HELPER(fselect)(",
        "void HELPER(fclass)(",
    ]
    bodies = "\n".join(function(source, s) for s in signatures)
    constants = []
    for name in ("IA64_FR_ROT_BASE", "IA64_FR_ROT_SIZE", "IA64_CFM_RRBF_SHIFT",
                 "IA64_CFM_RRBF_MASK", "IA64_PSR_MFL", "IA64_PSR_MFH",
                 "IA64_PSR_DFL", "IA64_PSR_DFH"):
        lines = [l for l in (source + "\n" + cpu).splitlines()
                 if l.startswith("#define " + name + " ")]
        if len(set(lines)) != 1:
            raise ValueError("constant shape changed: " + name)
        constants.append(lines[0])
    prelude = """#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>
#include "fp-bitops.h"
typedef struct CPUIA64State { uint64_t f[128][2], cfm, psr, pr; } CPUIA64State;
#define HELPER(x) helper_##x
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GETPC() 0
#define IA64_VEC_ILLEGAL_OP 0x5400
#define IA64_VEC_DISABLED_FP 0x5500
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
IA64FRBits select_result(IA64FRBits a, IA64FRBits b, IA64FRBits s) {
    return ia64_fselect_result(a, b, s);
}
int class_result(IA64FRBits v, unsigned mask, int *clears) {
    bool c = false;
    bool r = ia64_fclass_relation(v, mask, &c);
    *clears = c;
    return r;
}
uint16_t last_code(void) { return delivered_code; }
int run_select(CPUIA64State *env, uint64_t insn) {
    delivered_code = 0;
    int fault = setjmp(escape);
    if (!fault) helper_fselect(env, insn);
    return fault;
}
int run_class(CPUIA64State *env, uint64_t insn) {
    delivered_code = 0;
    int fault = setjmp(escape);
    if (!fault) helper_fclass(env, insn);
    return fault;
}
"""
    path = directory / "f3-f5-test.c"
    path.write_text(prelude + "\n".join(constants) + "\n" + bodies + exports)
    library = directory / "f3-f5-test.so"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-shared", "-fPIC",
        "-I" + str(root / "target/ia64"), str(path), "-o", str(library)],
        check=True, capture_output=True, text=True, timeout=60)
    lib = ctypes.CDLL(str(library))
    lib.select_result.argtypes = [FR, FR, FR]
    lib.select_result.restype = FR
    lib.class_result.argtypes = [FR, ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
    lib.class_result.restype = ctypes.c_int
    lib.run_select.argtypes = [ctypes.POINTER(State), ctypes.c_uint64]
    lib.run_select.restype = ctypes.c_int
    lib.run_class.argtypes = [ctypes.POINTER(State), ctypes.c_uint64]
    lib.run_class.restype = ctypes.c_int
    lib.last_code.argtypes, lib.last_code.restype = [], ctypes.c_uint16
    return lib


class F3F5Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="ia64-f3-f5-")
        cls.lib = compile_harness(ROOT, Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def select(self, a, b, s):
        r = self.lib.select_result(FR(*a), FR(*b), FR(*s))
        return r.significand, r.sign_exp

    def classify(self, value, mask):
        clears = ctypes.c_int()
        result = self.lib.class_result(FR(*value), mask, ctypes.byref(clears))
        return bool(result), bool(clears.value)

    def test_fselect_integer_mux_and_natval(self):
        rng = random.Random(0xf3052026)
        for _ in range(4096):
            a = (rng.getrandbits(64), rng.getrandbits(18))
            b = (rng.getrandbits(64), rng.getrandbits(18))
            s = (rng.getrandbits(64), rng.getrandbits(18))
            self.assertEqual(self.select(a, b, s),
                             ((a[0] & s[0]) | (b[0] & ~s[0] & ((1 << 64) - 1)),
                              0x1003e))
        for pos in range(3):
            vals = [(0x1234, 0x12345), (0x5678, 0x23456), (0x9abc, 0x34567)]
            vals[pos] = NAT
            self.assertEqual(self.select(*vals), NAT)

    def test_fselect_production_rotation_dirty_and_fault_priority(self):
        for rotation in range(96):
            state = State()
            state.cfm = rotation << 25
            state.psr = 0x2004
            for i in range(128):
                state.f[i][:] = (0x1111000000000000 + i, 0x10000 + i)
            state.f[0][:], state.f[1][:] = (0, 0), ONE
            f1, f2, f3, f4 = (32, 31, 126, 127)
            phys = lambda f: f if f < 32 else 32 + ((f - 32 + rotation) % 96)
            a, b, s = tuple(state.f[phys(f3)]), tuple(state.f[phys(f4)]), tuple(state.f[phys(f2)])
            self.assertEqual(self.lib.run_select(ctypes.byref(state),
                             fselect_word(f1, f2, f3, f4)), 0)
            self.assertEqual(tuple(state.f[phys(f1)]), self.select(a, b, s))
            self.assertEqual(state.psr, 0x2004 | 0x20)
        state = State()
        self.assertEqual(self.lib.run_select(ctypes.byref(state), fselect_word(f1=1)), 0x5400)
        state = State()
        state.psr = 0x40000
        self.assertEqual(self.lib.run_select(ctypes.byref(state),
                         fselect_word(f1=32, f2=31, f3=33, f4=34)), 0x5500)
        self.assertEqual(self.lib.last_code(), 1)

    def test_fclass_all_architectural_classes(self):
        vectors = {
            0x005: (0, 0),
            0x006: (0, 0x20000),
            0x009: (0x4000000000000000, 0xffff),
            0x011: (0x8000000000000000, 0xffff),
            0x021: (0x8000000000000000, 0x1ffff),
            0x040: (0x8000000000000001, 0x1ffff),
            0x080: (0xc000000000000001, 0x1ffff),
            0x100: NAT,
        }
        for mask, value in vectors.items():
            self.assertEqual(self.classify(value, mask), (True, False), (mask, value))
        self.assertEqual(self.classify(NAT, 0x005), (False, True))
        self.assertEqual(self.classify((0x4000000000000000, 0x1ffff), 0x1ff),
                         (False, False))
        self.assertEqual(self.classify((0x8000000000000000, 0xffff), 0x012),
                         (False, False))

    def test_fclass_predication_unc_legality_and_disabled_priority(self):
        state = State()
        state.pr = 1
        state.f[8][:] = ONE
        before = bytes(state)
        self.assertEqual(self.lib.run_class(ctypes.byref(state),
                         fclass_word(p1=6, p2=6, qp=5)), 0)
        self.assertEqual(bytes(state), before)

        state = State()
        state.pr = 1 | (1 << 6) | (1 << 7)
        state.f[8][:] = ONE
        self.assertEqual(self.lib.run_class(ctypes.byref(state),
                         fclass_word(p1=6, p2=7, unc=True, qp=5)), 0)
        self.assertEqual((state.pr >> 6) & 3, 0)
        state = State()
        state.pr = 1
        self.assertEqual(self.lib.run_class(ctypes.byref(state),
                         fclass_word(p1=6, p2=6, unc=True, qp=5)), 0x5400)

        state = State()
        state.pr = 1
        state.psr = 0x40000
        self.assertEqual(self.lib.run_class(ctypes.byref(state),
                         fclass_word(p1=6, p2=6, f2=8)), 0x5400)
        state = State()
        state.pr = 1
        state.psr = 0x40000
        self.assertEqual(self.lib.run_class(ctypes.byref(state),
                         fclass_word(p1=6, p2=7, f2=8)), 0x5500)
        self.assertEqual(self.lib.last_code(), 1)

    def test_fclass_natval_predicate_clearing_and_member(self):
        state = State()
        state.pr = 1 | (1 << 6) | (1 << 7)
        state.f[8][:] = NAT
        self.assertEqual(self.lib.run_class(ctypes.byref(state),
                         fclass_word(mask=0x005, f2=8)), 0)
        self.assertEqual((state.pr >> 6) & 3, 0)
        state = State()
        state.pr = 1
        state.f[8][:] = NAT
        self.assertEqual(self.lib.run_class(ctypes.byref(state),
                         fclass_word(mask=0x100, f2=8)), 0)
        self.assertEqual((state.pr >> 6) & 3, 1)


if __name__ == "__main__":
    unittest.main()
