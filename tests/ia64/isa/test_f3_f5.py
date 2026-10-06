# SPDX-License-Identifier: GPL-2.0-or-later
"""F3 fselect and F5 fclass tests against production helper bodies."""
import ctypes
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HELPER = ROOT / 'target/ia64/helper.c'
CPU = ROOT / 'target/ia64/cpu.h'
NAT = (0, 0x1fffe)
ONE = (1 << 63, 0xffff)

def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError('production helper shape changed: ' + signature)
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

class State(ctypes.Structure):
    _fields_ = [('f', (ctypes.c_uint64 * 2) * 128),
                ('cfm', ctypes.c_uint64), ('psr', ctypes.c_uint64),
                ('pr', ctypes.c_uint64)]

def fselect_word(f1=6, f2=7, f3=8, f4=9):
    return (14 << 37) | (f4 << 27) | (f3 << 20) | (f2 << 13) | (f1 << 6)

def fclass_word(p1=6, p2=7, f2=8, cls=0x1ff, unc=False, qp=0):
    return ((5 << 37) | ((cls & 3) << 33) | (p2 << 27) |
            ((cls >> 2) << 20) | (f2 << 13) | (int(unc) << 12) |
            (p1 << 6) | qp)

def compile_harness(directory):
    source, cpu = HELPER.read_text(), CPU.read_text()
    sigs = ['static inline uint32_t ia64_fr_phys(',
            'uint64_t HELPER(fr_get_lo)(', 'uint64_t HELPER(fr_get_hi)(',
            'void HELPER(fr_set_lo)(', 'void HELPER(fr_set_hi)(',
            'static uint16_t ia64_fp_disabled_code(',
            'static inline IA64FRBits ia64_fr_bits(',
            'static inline bool ia64_fr_is_natval(',
            'static uint16_t ia64_fr_class(',
            'static inline void ia64_pr_write(',
            'void HELPER(fselect)(', 'void HELPER(fclass)(']
    bodies = '\n'.join(function(source, s) for s in sigs)
    constants = []
    for name in ('IA64_FR_ROT_BASE','IA64_FR_ROT_SIZE','IA64_CFM_RRBF_SHIFT',
                 'IA64_CFM_RRBF_MASK','IA64_PSR_MFL','IA64_PSR_MFH',
                 'IA64_PSR_DFL','IA64_PSR_DFH','IA64_FP_EXP_INTEGER'):
        lines = [l for l in (source + '\n' + cpu).splitlines()
                 if l.startswith('#define ' + name + ' ')]
        if len(set(lines)) != 1:
            raise ValueError('constant shape changed: ' + name)
        constants.append(lines[0])
    prelude = r'''#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>
#include "fp-bitops.h"
typedef struct CPUIA64State { uint64_t f[128][2], cfm, psr, pr; } CPUIA64State;
#define HELPER(x) helper_##x
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define env_cpu(e) (e)
#define g_assert_not_reached() abort()
#define GETPC() 0
#define IA64_VEC_ILLEGAL_OP 0x5400
#define IA64_VEC_DISABLED_FP 0x5500
static jmp_buf escape;
static uint16_t delivered_code;
static _Noreturn void ia64_fp_interrupt(CPUIA64State *env, uint32_t vector,
                                        uint16_t code, uint64_t extra,
                                        bool trap, uintptr_t pc)
{
    (void)env; (void)extra; (void)trap; (void)pc;
    delivered_code = code; longjmp(escape, vector);
}
'''
    exports = r'''
int run_select(CPUIA64State *env, uint64_t insn) {
    delivered_code = 0; int fault = setjmp(escape);
    if (!fault) helper_fselect(env, insn); return fault;
}
int run_class(CPUIA64State *env, uint64_t insn) {
    delivered_code = 0; int fault = setjmp(escape);
    if (!fault) helper_fclass(env, insn); return fault;
}
uint16_t last_code(void) { return delivered_code; }
uint16_t classify(uint64_t sig, uint64_t se) {
    return ia64_fr_class((IA64FRBits){sig, se});
}
'''
    path = directory / 'f3-f5-test.c'
    path.write_text(prelude + '\n'.join(constants) + '\n' + bodies + exports)
    libpath = directory / 'f3-f5-test.so'
    result = subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11','-Wall','-Wextra','-Werror','-O2','-fsanitize=undefined',
        '-shared','-fPIC','-I' + str(ROOT / 'target/ia64'),
        str(path),'-o',str(libpath)], capture_output=True,
        text=True, timeout=60)
    if result.returncode:
        raise RuntimeError(
            'F3/F5 production-helper harness compilation failed:\n'
            + result.stdout + result.stderr)
    lib = ctypes.CDLL(str(libpath))
    for name in ('run_select','run_class'):
        fn = getattr(lib, name)
        fn.argtypes = [ctypes.POINTER(State), ctypes.c_uint64]
        fn.restype = ctypes.c_int
    lib.last_code.restype = ctypes.c_uint16
    lib.classify.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
    lib.classify.restype = ctypes.c_uint16
    return lib

class F3F5Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='ia64-f3-f5-')
        cls.lib = compile_harness(Path(cls.tmp.name))
    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()
    def state(self):
        s = State(); s.pr = 1
        s.f[0][:], s.f[1][:] = (0, 0), ONE
        return s
    def test_fselect_exact_bits_nat_and_dirty(self):
        s=self.state()
        s.f[7][:]=(0xff00ff00ff00ff00,0x12345)
        s.f[8][:]=(0xaaaaaaaa55555555,0x22222)
        s.f[9][:]=(0x0123456789abcdef,0x33333)
        expected=(s.f[8][0]&s.f[7][0])|(s.f[9][0]&(~s.f[7][0]&((1<<64)-1)))
        self.assertEqual(self.lib.run_select(ctypes.byref(s),fselect_word()),0)
        self.assertEqual(tuple(s.f[6]),(expected,0x1003e))
        self.assertEqual(s.psr&0x30,0x10)
        s=self.state(); s.f[7][:],s.f[8][:],s.f[9][:]=NAT,ONE,ONE
        self.assertEqual(self.lib.run_select(ctypes.byref(s),fselect_word()),0)
        self.assertEqual(tuple(s.f[6]),NAT)
    def test_fselect_rotation_and_fault_priority(self):
        for rotation in (0,1,47,95):
            s=self.state(); s.cfm=rotation<<25
            phys=lambda f: f if f<32 else 32+((f-32+rotation)%96)
            for f,v in ((32,(0xffff0000ffff0000,0x1003e)),
                        (33,(0xaaaaaaaaaaaaaaaa,0x1003e)),
                        (34,(0x5555555555555555,0x1003e)),
                        (35,(0,0))):
                s.f[phys(f)][:]=v
            self.assertEqual(self.lib.run_select(ctypes.byref(s),
                             fselect_word(35,32,33,34)),0)
            self.assertEqual(s.psr&0x30,0x20)
        s=self.state(); s.psr=0x40000|0x80000
        self.assertEqual(self.lib.run_select(ctypes.byref(s),
                         fselect_word(0,31,32,33)),0x5400)
        self.assertEqual(self.lib.run_select(ctypes.byref(s),
                         fselect_word(6,31,32,33)),0x5500)
        self.assertEqual(self.lib.last_code(),3)
    def test_fclass_reference_classes_and_pseudos(self):
        cases=[(NAT,0x100),((0xc000000000000001,0x1ffff),0x080),
               ((0x8000000000000001,0x1ffff),0x040),
               ((1<<63,0x1ffff),0x020),((0,0),0x004),((1,0),0x008),
               ((0,1),0x008),((1<<63,1),0x010),((1,1),0),
               ((1,0x1ffff),0)]
        for pair,expected in cases:
            self.assertEqual(self.lib.classify(*pair),expected,pair)
    def test_fclass_membership_nat_and_predication(self):
        s=self.state(); s.f[8][:]=(0xc000000000000001,0x1ffff)
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(cls=0x080)),0)
        self.assertTrue(s.pr&(1<<6)); self.assertFalse(s.pr&(1<<7))
        s=self.state(); s.f[8][:]=NAT
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(cls=0x100)),0)
        self.assertTrue(s.pr&(1<<6)); self.assertFalse(s.pr&(1<<7))
        s.pr|=(1<<6)|(1<<7)
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(cls=0x010)),0)
        self.assertFalse(s.pr&(1<<6)); self.assertFalse(s.pr&(1<<7))
        s=self.state(); s.pr|=(1<<6)|(1<<7)
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(cls=0x1ff,qp=5)),0)
        self.assertTrue(s.pr&(1<<6)); self.assertTrue(s.pr&(1<<7))
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(cls=0x1ff,qp=5,unc=True)),0)
        self.assertFalse(s.pr&(1<<6)); self.assertFalse(s.pr&(1<<7))
    def test_fclass_illegal_and_disabled_order(self):
        s=self.state(); s.psr=0x40000; s.f[8][:]=ONE
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(p1=6,p2=6,f2=8)),0x5400)
        s=self.state(); s.psr=0x40000; s.f[8][:]=ONE
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(f2=8)),0x5500)
        self.assertEqual(self.lib.last_code(),1)
        s=self.state(); s.pr&=~(1<<5)
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(p1=6,p2=6,f2=8,qp=5)),0)
        self.assertEqual(self.lib.run_class(ctypes.byref(s),
                         fclass_word(p1=6,p2=6,f2=8,qp=5,unc=True)),0x5400)

if __name__ == '__main__':
    unittest.main()
