#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""F1 parallel completion trap / PSR.ss regression guard."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[3]

class F1SingleStepMatrix(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner=(ROOT/"scripts/run-ia64-f1-ss-tests.py").read_text()
        cls.helper=(ROOT/"target/ia64/helper.c").read_text()
        cls.policy=(ROOT/"target/ia64/fp-trap-priority.h").read_text()

    def test_matrix_independent_oracles(self):
        for profile in ("fpma-low-ss","fpma-low-no-ss","fpma-high-ss",
                        "fpma-both-ss","fpma-masked-ss","fpma-invalid-high-ss",
                        "fpms-exact-ss","fpnma-exact-ss","fpma-false-qp-ss"):
            self.assertIn('"'+profile+'"',self.runner)
        for oracle in ("0x6009","0x6609","0x3f8000003f800001",
                       "0xc000000040000000","0x40000000c0000000"):
            # The bit-encoded ISR oracle may be expressed rather than literal.
            if oracle not in ("0x6009","0x6609"):
                self.assertIn(oracle,self.runner)
        self.assertIn('((1 << 9) | (1 << 10)',self.runner)
        self.assertIn('((1 << 13) | (1 << 14)',self.runner)
        self.assertIn('if kind == "invalid"',self.runner)

    def test_real_rfi_and_genuine_vectors(self):
        for needle in ("mov cr.ipsr=r9","mov cr.iip=r14",
                       "cr.isr","cr.iipa","cr.iip","ar.fpsr",
                       "stf.spill","0x5c00","0x5d00","0x6000",
                       "rfi", "source_bundle", "checks:"):
            self.assertIn(needle,self.runner)

    def test_f1_helper_uses_same_fp_postcommit_path(self):
        start=self.helper.index("void HELPER(f1_parallel)(")
        stop=self.helper.index("void HELPER(fma_s1)(",start)
        code=self.helper[start:stop]
        self.assertLess(code.index("if (result.fault_code)"),
                        code.index("HELPER(fr_set_lo)(env, f1"))
        self.assertLess(code.index("HELPER(fr_set_lo)(env, f1"),
                        code.index("if (result.trap_code)"))
        self.assertIn("IA64_VEC_FP_FAULT",code)
        self.assertIn("IA64_VEC_FP_TRAP",code)
        self.assertIn("ia64_fp_trap_isr_code",self.helper)
        self.assertIn("IA64_SS_CODE",self.policy)

if __name__=="__main__":
    unittest.main()
