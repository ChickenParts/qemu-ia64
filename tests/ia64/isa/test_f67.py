#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
C_SOURCE = r"""
#include <assert.h>
#include <stdint.h>
#include "fp-approx.h"
static uint64_t word(unsigned major, unsigned q) {
    return ((uint64_t)major << 37) | ((uint64_t)q << 36) |
           (UINT64_C(1) << 33);
}
int main(void) {
    IA64FRBits one = { UINT64_C(1) << 63, 0xffff };
    IA64FRBits zero = { 0, 0 };
    IA64FRBits neg_four = { UINT64_C(1) << 63, 0x30001 };
    IA64FRBits packed_one = { UINT64_C(0x3f8000003f800000), 0x1003e };
    IA64F67Result r;
    assert(ia64_f67_decode(word(0, 0)) == IA64_F67_FRCPA);
    assert(ia64_f67_decode(word(0, 1)) == IA64_F67_FRSQRTA);
    assert(ia64_f67_decode(word(1, 0)) == IA64_F67_FPRCPA);
    assert(ia64_f67_decode(word(1, 1)) == IA64_F67_FPRSQRTA);
    assert(ia64_f67_decode(0) == IA64_F67_INVALID);
    r = ia64_f67_result(IA64_F67_FRCPA, one, one, 0x3f, 0);
    assert(r.write_value && r.predicate && !r.fault_code && !r.flags);
    assert(r.value.significand == (UINT64_C(0x7fc) << 53));
    assert(r.value.sign_exp == 0xfffe);
    r = ia64_f67_result(IA64_F67_FRSQRTA, zero, zero, 0x3f, 0);
    assert(r.write_value && !r.predicate && r.value.significand == 0);
    r = ia64_f67_result(IA64_F67_FRSQRTA, zero, neg_four, 0x3f, 0);
    assert(r.write_value && r.flags == IA64_F67_V && !r.fault_code);
    assert(r.value.significand == UINT64_C(0xc000000000000000));
    r = ia64_f67_result(IA64_F67_FRSQRTA, zero, neg_four, 0, 0);
    assert(!r.write_value && r.fault_code == IA64_F67_V);
    r = ia64_f67_result(IA64_F67_FPRCPA, packed_one, packed_one, 0x3f, 3);
    assert(r.write_value && r.predicate && !r.fault_code);
    assert(r.value.sign_exp == 0x1003e);
    assert(r.value.significand == UINT64_C(0x3f7f80003f7f8000));
    r = ia64_f67_result(IA64_F67_FPRSQRTA, packed_one, packed_one, 0x3f, 2);
    assert(r.write_value && r.predicate && !r.fault_code);
    assert((uint32_t)r.value.significand ==
           (uint32_t)(r.value.significand >> 32));
    return 0;
}
"""
class F67ModelTests(unittest.TestCase):
    def test_integer_model(self):
        with tempfile.TemporaryDirectory(prefix="ia64-f67-") as tmp:
            src = Path(tmp) / "test.c"
            exe = Path(tmp) / "test"
            src.write_text(textwrap.dedent(C_SOURCE))
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-O2", "-I" + str(ROOT / "target/ia64"),
                            str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
if __name__ == "__main__":
    unittest.main()
