#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove FP+SS only changes the post-result FP trap delivery path."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class FloatingPointSingleStepSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()
        cls.policy = (ROOT / "target/ia64/fp-trap-priority.h").read_text()
        cls.guest = (ROOT / "scripts/run-ia64-fp-ss-priority-tests.py").read_text()

    def test_fp_priority_is_gated_on_actual_post_result_trap(self):
        a = self.helper.index("static G_NORETURN void ia64_fp_interrupt(")
        b = self.helper.index("void ia64_cpu_do_unaligned_access(", a)
        source = self.helper[a:b]
        self.assertIn("vec == IA64_VEC_FP_TRAP && trap", source)
        self.assertIn("code = ia64_fp_trap_isr_code(code, env->psr)", source)
        self.assertLess(source.index("ia64_fp_trap_isr_code("),
                        source.index("ia64_exception("))
        self.assertNotIn("vec == IA64_VEC_FP_FAULT && trap", source)
        self.assertIn('"fp-trap-priority.h"', self.helper)

    def test_runtime_post_result_commit_still_precedes_interrupt(self):
        a = self.helper.index("void HELPER(f10)(")
        b = self.helper.index("uint64_t HELPER(gr_nat)", a)
        f10 = self.helper[a:b]
        self.assertLess(f10.index("HELPER(fr_set_lo)(env, f1"),
                        f10.index("if (result.trap_code)"))
        self.assertIn("IA64_VEC_FP_FAULT", f10)
        self.assertIn("IA64_VEC_FP_TRAP", f10)
        self.assertIn("ia64_ss_enabled(psr)", self.policy)

    def test_real_guest_cases_cover_fp_trap_fault_and_ss(self):
        for needle in ("fp-inexact-ss", "fp-inexact-no-ss",
                       "fp-masked-ss", "fp-invalid-ss",
                       "fp-false-qp-ss"):
            self.assertIn('"' + needle + '"', self.guest)
        for needle in ("0x5c00", "0x5d00", "0x6000",
                       "mov cr.ipsr=r9", "rfi", "cr.isr",
                       "cr.iip", "cr.iipa", "ar.fpsr",
                       "getf.sig", "getf.exp"):
            self.assertIn(needle, self.guest)


if __name__ == "__main__":
    unittest.main()
