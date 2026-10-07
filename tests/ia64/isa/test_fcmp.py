# SPDX-License-Identifier: GPL-2.0-or-later
"""F4 exact-comparison oracle and production-helper fault/state tests."""
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
NAT = (0, 0x1fffe)
ONE = (1 << 63, 0xffff)
TWO = (1 << 63, 0x10000)
QNAN = (0xc000000000000001, 0x1ffff)
SNAN = (0x8000000000000001, 0x1ffff)
UNSUPPORTED = (0x4000000000000000, 0x1ffff)
UNORM = (0x4000000000000000, 0xffff)
RELS = {"eq": 0, "lt": 1, "le": 2, "unord": 3}


def word(rel, p1=6, p2=7, f2=8, f3=9, sf=0, unc=False, qp=0):
    r = RELS[rel]
    return ((4 << 37) | ((r & 1) << 36) | (sf << 34) |
            ((r >> 1) << 33) | (p2 << 27) | (f3 << 20) |
            (f2 << 13) | (int(unc) << 12) | (p1 << 6) | qp)


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError("production helper shape changed: " + signature)
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


class State(ctypes.Structure):
    _fields_ = [("f", (ctypes.c_uint64 * 2) * 128),
                ("cfm", ctypes.c_uint64), ("psr", ctypes.c_uint64),
                ("ar", ctypes.c_uint64 * 128)]


def classify(v):
    sig, se = v
    exp = se & 0x1ffff
    if v == NAT:
        return "nat"
    if exp == 0x1ffff:
        if not sig >> 63:
            return "unsupported"
        if sig == 1 << 63:
            return "inf"
        return "qnan" if sig & (1 << 62) else "snan"
    if not sig:
        return "zero" if not exp else "unorm"
    if not exp or not sig >> 63:
        return "unorm"
    return "normal"


def mag_cmp(a, b):
    if not a[0] or not b[0]:
        return (a[0] > b[0]) - (a[0] < b[0])
    def key(v):
        sig, se = v
        exp = (se & 0x1ffff) or 0xc001
        lz = 64 - sig.bit_length()
        return exp - lz, (sig << lz) & ((1 << 64) - 1)
    return (key(a) > key(b)) - (key(a) < key(b))


def exact_cmp(a, b):
    ca, cb = classify(a), classify(b)
    sa, sb = bool(a[1] & 0x20000), bool(b[1] & 0x20000)
    if ca == "inf" or cb == "inf":
        mag = 0 if ca == cb == "inf" else (1 if ca == "inf" else -1)
    else:
        mag = mag_cmp(a, b)
    if not a[0] and not b[0]:
        return 0
    if sa != sb:
        return -1 if sa else 1
    return -mag if sa else mag


def oracle(rel, a, b, fpsr=0x3f, sf=0, qual=True):
    if not qual or a == NAT or b == NAT:
        return 0, 0, 0
    ca, cb = classify(a), classify(b)
    unordered = ca in ("qnan", "snan", "unsupported") or cb in ("qnan", "snan", "unsupported")
    invalid = (ca in ("snan", "unsupported") or cb in ("snan", "unsupported") or
               (rel in ("lt", "le") and (ca == "qnan" or cb == "qnan")))
    flags = int(invalid)
    if not unordered and (ca == "unorm" or cb == "unorm"):
        flags |= 2
    cmp = exact_cmp(a, b) if not unordered else 0
    relation = {"eq": not unordered and cmp == 0,
                "lt": not unordered and cmp < 0,
                "le": not unordered and cmp <= 0,
                "unord": unordered}[rel]
    controls = (fpsr >> (6 + 13 * sf)) & 0x7f
    disabled = 0x3f if sf and controls & 0x40 else fpsr & 0x3f
    return 1 if relation else 2, flags, flags & ~disabled & 3


def compile_harness(directory):
    source = (ROOT / "target/ia64/helper.c").read_text()
    cpu = (ROOT / "target/ia64/cpu.h").read_text()
    sigs = ["static inline uint32_t ia64_fr_phys(",
            "uint64_t HELPER(fr_get_lo)(", "uint64_t HELPER(fr_get_hi)(",
            "static uint16_t ia64_fp_disabled_code(", "uint64_t HELPER(fcmp)("]
    bodies = "\n".join(function(source, s) for s in sigs)
    defs = []
    for name in ("IA64_FR_ROT_BASE", "IA64_FR_ROT_SIZE", "IA64_CFM_RRBF_SHIFT",
                 "IA64_CFM_RRBF_MASK", "IA64_PSR_DFL", "IA64_PSR_DFH",
                 "IA64_AR_FPSR"):
        rows = [x for x in (source + "\n" + cpu).splitlines()
                if x.startswith("#define " + name + " ")]
        if len(set(rows)) != 1:
            raise ValueError("constant shape changed: " + name)
        defs.append(rows[0])
    c = """#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include "fp-compare.h"
typedef struct CPUIA64State { uint64_t f[128][2], cfm, psr, ar[128]; } CPUIA64State;
#define HELPER(x) helper_##x
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GETPC() 0
#define IA64_VEC_ILLEGAL_OP 0x5400
#define IA64_VEC_DISABLED_FP 0x5500
#define IA64_VEC_FP_FAULT 0x5c00
static jmp_buf escape; static uint16_t code; static uint64_t preds;
static void ia64_fp_interrupt(CPUIA64State *e, uint32_t v, uint16_t c,
 uint64_t x, bool t, uintptr_t p) {(void)e;(void)x;(void)t;(void)p;code=c;longjmp(escape,v);}
""" + "\n".join(defs) + "\n" + bodies + """
static IA64FCmpResult ev;
void evaluate(int r,uint64_t al,uint64_t ah,uint64_t bl,uint64_t bh,uint64_t f,unsigned sf,int q)
{ ev=ia64_fcmp_result(r,(IA64FRBits){al,ah},(IA64FRBits){bl,bh},f,sf,q); }
uint32_t ep(void){return ev.predicates;} uint32_t ef(void){return ev.flags;}
uint16_t ex(void){return ev.fault_code;} uint16_t lc(void){return code;}
uint64_t lp(void){return preds;}
int run(CPUIA64State *e,uint64_t i,int q){code=preds=0;int v=setjmp(escape);if(!v)preds=helper_fcmp(e,i,q);return v;}
"""
    src = directory / "fcmp.c"; so = directory / "fcmp.so"; src.write_text(c)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11","-Wall","-Wextra","-Werror","-O2","-shared","-fPIC",
        "-fsanitize=undefined","-I"+str(ROOT/"target/ia64"),str(src),"-o",str(so)],
        check=True,capture_output=True,text=True,timeout=60)
    lib=ctypes.CDLL(str(so))
    lib.evaluate.argtypes=[ctypes.c_int]+[ctypes.c_uint64]*5+[ctypes.c_uint,ctypes.c_int]
    for n in ("ep","ef","ex"): getattr(lib,n).restype=ctypes.c_uint32
    lib.run.argtypes=[ctypes.POINTER(State),ctypes.c_uint64,ctypes.c_int];lib.run.restype=ctypes.c_int
    lib.lc.restype=ctypes.c_uint16;lib.lp.restype=ctypes.c_uint64
    return lib


class FCmpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix="ia64-fcmp-")
        cls.lib=compile_harness(Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def ev(self,rel,a,b,fpsr=0x3f,sf=0,q=True):
        self.lib.evaluate(RELS[rel],*a,*b,fpsr,sf,int(q))
        return self.lib.ep(),self.lib.ef(),self.lib.ex()

    def test_exact_random_finite_all_relations_and_status_fields(self):
        rng=random.Random(0xf4002026)
        for _ in range(256):
            a=(rng.getrandbits(63)|(1<<63),rng.randrange(1,0x1ffff))
            b=(rng.getrandbits(63)|(1<<63),rng.randrange(1,0x1ffff))
            a=(a[0],a[1]|(0x20000 if rng.getrandbits(1) else 0))
            b=(b[0],b[1]|(0x20000 if rng.getrandbits(1) else 0))
            for rel,sf in itertools.product(RELS,range(4)):
                self.assertEqual(self.ev(rel,a,b,sf=sf),oracle(rel,a,b,sf=sf))

    def test_special_values_and_masks(self):
        for rel,a,b in [("eq",NAT,ONE),("eq",QNAN,ONE),("lt",QNAN,ONE),
                        ("unord",QNAN,ONE),("eq",SNAN,ONE),
                        ("unord",UNSUPPORTED,ONE),("lt",UNORM,TWO),
                        ("eq",(0,0),(0,0x20000))]:
            self.assertEqual(self.ev(rel,a,b),oracle(rel,a,b),(rel,a,b))
        self.assertEqual(self.ev("lt",QNAN,ONE,fpsr=0)[2],1)
        self.assertEqual(self.ev("eq",UNORM,ONE,fpsr=0)[2],2)
        td=0x40 << (6+13*2)
        self.assertEqual(self.ev("lt",QNAN,ONE,fpsr=td,sf=2)[2],0)

    def test_helper_ordering_qualification_and_commit(self):
        s=State();s.f[8][:],s.f[9][:]=ONE,TWO;s.ar[40]=0x3f
        self.assertEqual(self.lib.run(ctypes.byref(s),word("lt"),1),0)
        self.assertEqual(self.lib.lp(),1)

        s=State();s.psr=0x40000;before=bytes(s)
        self.assertEqual(self.lib.run(ctypes.byref(s),word("eq",p1=6,p2=6,unc=True),0),0x5400)
        self.assertEqual(bytes(s),before)

        s=State();s.psr=0x40000;s.f[8][:],s.f[9][:]=ONE,TWO;before=bytes(s)
        self.assertEqual(self.lib.run(ctypes.byref(s),word("lt"),1),0x5500)
        self.assertEqual(self.lib.lc(),1);self.assertEqual(bytes(s),before)

        s=State();s.psr=0x40000;s.f[8][:],s.f[9][:]=QNAN,UNORM;before=bytes(s)
        self.assertEqual(self.lib.run(ctypes.byref(s),word("lt",unc=True),0),0)
        self.assertEqual(self.lib.lp(),0);self.assertEqual(bytes(s),before)

    def test_fault_does_not_commit_and_masked_flags_do(self):
        s=State();s.f[8][:],s.f[9][:]=QNAN,ONE;before=bytes(s)
        self.assertEqual(self.lib.run(ctypes.byref(s),word("lt"),1),0x5c00)
        self.assertEqual(self.lib.lc(),1);self.assertEqual(bytes(s),before)
        s=State();s.f[8][:],s.f[9][:]=QNAN,ONE;s.ar[40]=0x3f
        self.assertEqual(self.lib.run(ctypes.byref(s),word("lt",sf=2),1),0)
        self.assertEqual(self.lib.lp(),2)
        self.assertTrue(s.ar[40] & (1 << (13+26)))


if __name__ == "__main__": unittest.main()
