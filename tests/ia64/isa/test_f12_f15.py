# SPDX-License-Identifier: GPL-2.0-or-later
"""F12-F14 FPSR-control production-helper tests."""
import ctypes
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class State(ctypes.Structure):
    _fields_ = [("ar", ctypes.c_uint64 * 128)]


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError("production helper shape changed: " + signature)
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def fsetc_word(sf, amask, omask, qp=0):
    return ((sf & 3) << 34) | (4 << 27) | ((omask & 0x7f) << 20) | \
           ((amask & 0x7f) << 13) | (qp & 63)


def set_controls(fpsr, sf, controls):
    shift = 6 + 13 * sf
    return (fpsr & ~(0x7f << shift)) | ((controls & 0x7f) << shift)


def set_flags(fpsr, sf, flags):
    shift = 13 + 13 * sf
    return (fpsr & ~(0x3f << shift)) | ((flags & 0x3f) << shift)


def compile_harness(directory):
    source = (ROOT / "target/ia64/helper.c").read_text()
    bodies = "\n".join(function(source, sig) for sig in (
        "void HELPER(fsetc)(",
        "void HELPER(fclrf)(",
        "uint64_t HELPER(fchkf_cond)(",
    ))
    prelude = r'''
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
typedef struct CPUIA64State { uint64_t ar[128]; } CPUIA64State;
#define HELPER(x) helper_##x
#define IA64_AR_FPSR 40
#define IA64_VEC_GENERAL_EXCEPTION 0x5400
#define GETPC() 0
static jmp_buf escape;
static uint16_t fault_code;
static void ia64_fp_interrupt(CPUIA64State *env, uint32_t vec,
                              uint16_t code, uint64_t extra,
                              bool trap, uintptr_t pc)
{
    (void)env; (void)extra; (void)trap; (void)pc;
    fault_code = code;
    longjmp(escape, (int)vec);
}
'''
    exports = r'''
int run_fsetc(CPUIA64State *env, uint64_t insn) {
    fault_code = 0;
    int fault = setjmp(escape);
    if (!fault) helper_fsetc(env, insn);
    return fault;
}
uint16_t last_code(void) { return fault_code; }
void run_fclrf(CPUIA64State *env, unsigned sf) { helper_fclrf(env, sf); }
uint64_t run_fchkf(CPUIA64State *env, unsigned sf) {
    return helper_fchkf_cond(env, sf);
}
'''
    src = directory / "f12-f14-test.c"
    src.write_text(prelude + bodies + exports)
    so = directory / "f12-f14-test.so"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-shared", "-fPIC",
        str(src), "-o", str(so)], check=True, capture_output=True, text=True,
        timeout=60)
    lib = ctypes.CDLL(str(so))
    lib.run_fsetc.argtypes = [ctypes.POINTER(State), ctypes.c_uint64]
    lib.run_fsetc.restype = ctypes.c_int
    lib.last_code.restype = ctypes.c_uint16
    lib.run_fclrf.argtypes = [ctypes.POINTER(State), ctypes.c_uint]
    lib.run_fchkf.argtypes = [ctypes.POINTER(State), ctypes.c_uint]
    lib.run_fchkf.restype = ctypes.c_uint64
    return lib


class ControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="ia64-f12-f14-")
        cls.lib = compile_harness(Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def state(self, fpsr):
        s = State()
        s.ar[40] = fpsr
        return s

    def test_fsetc_derives_every_target_from_sf0(self):
        base = 0x15
        base = set_controls(base, 0, 0x22)
        for sf in range(4):
            base = set_controls(base, sf, 0x10 + sf * 2)
            base = set_flags(base, sf, 0x21 + sf)
        # Restore the architectural source after initializing other fields.
        base = set_controls(base, 0, 0x22)
        expected_controls = (0x22 & 0x3f) | 0x10
        for sf in range(4):
            with self.subTest(sf=sf):
                s = self.state(base)
                self.assertEqual(self.lib.run_fsetc(
                    ctypes.byref(s), fsetc_word(sf, 0x3f, 0x10)), 0)
                expected = set_controls(base, sf, expected_controls)
                self.assertEqual(s.ar[40], expected)

    def test_fsetc_reserved_pc_and_sf0_td_fault_without_commit(self):
        for sf in range(4):
            base = set_controls(0x3f, 0, 0)
            s = self.state(base)
            self.assertEqual(self.lib.run_fsetc(
                ctypes.byref(s), fsetc_word(sf, 0, 0x04)), 0x5400)
            self.assertEqual(self.lib.last_code(), 0x30)
            self.assertEqual(s.ar[40], base)

        base = set_controls(0x3f, 0, 0)
        s = self.state(base)
        self.assertEqual(self.lib.run_fsetc(
            ctypes.byref(s), fsetc_word(0, 0, 0x40)), 0x5400)
        self.assertEqual(s.ar[40], base)

        # TD is only reserved in sf0; the same bit is legal in sf1..sf3.
        for sf in (1, 2, 3):
            s = self.state(base)
            self.assertEqual(self.lib.run_fsetc(
                ctypes.byref(s), fsetc_word(sf, 0, 0x40)), 0)
            self.assertEqual((s.ar[40] >> (6 + 13 * sf)) & 0x7f, 0x40)

    def test_fclrf_clears_only_selected_flags(self):
        base = 0x2d
        for sf in range(4):
            base = set_controls(base, sf, 0x22)
            base = set_flags(base, sf, 0x3f - sf)
        for sf in range(4):
            with self.subTest(sf=sf):
                s = self.state(base)
                self.lib.run_fclrf(ctypes.byref(s), sf)
                self.assertEqual(s.ar[40], set_flags(base, sf, 0))

    def test_fchkf_enabled_trap_or_new_flag(self):
        # All traps disabled and selected flags already represented in sf0:
        # no branch.
        fpsr = 0x3f
        fpsr = set_flags(fpsr, 0, 0x02)
        fpsr = set_flags(fpsr, 1, 0x02)
        s = self.state(fpsr)
        self.assertEqual(self.lib.run_fchkf(ctypes.byref(s), 1), 0)

        # Clear trap-disable bit 1: selected flag is now enabled => branch.
        s = self.state(fpsr & ~0x02)
        self.assertEqual(self.lib.run_fchkf(ctypes.byref(s), 1), 1)

        # Keep all traps disabled, but selected sf has a flag absent from sf0.
        fpsr2 = set_flags(fpsr, 1, 0x06)
        s = self.state(fpsr2)
        self.assertEqual(self.lib.run_fchkf(ctypes.byref(s), 1), 1)

        # sf0 can only trigger the first condition because it is compared to
        # itself for the second.
        s = self.state(set_flags(0x3f, 0, 0x08))
        self.assertEqual(self.lib.run_fchkf(ctypes.byref(s), 0), 0)
        s.ar[40] &= ~0x08
        self.assertEqual(self.lib.run_fchkf(ctypes.byref(s), 0), 1)


if __name__ == "__main__":
    unittest.main()
