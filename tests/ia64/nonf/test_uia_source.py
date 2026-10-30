#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed Merced UIA trap source and independent guest coverage gates."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[3]

class MercedUIASource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t=(ROOT/"target/ia64/translate.c").read_text()
        cls.h=(ROOT/"target/ia64/helper.c").read_text()
        cls.policy=(ROOT/"target/ia64/uia-address.h").read_text()
        cls.guest=(ROOT/"scripts/run-ia64-uia-branch-tests.py").read_text()

    def test_uia_always_checked_for_genuine_taken_branches(self):
        a=self.t.index("static void gen_record_branch(")
        b=self.t.index("static void gen_record_b0_write(",a)
        path=self.t[a:b]
        self.assertIn("Every taken branch must check target validity",path)
        self.assertEqual(path.count("gen_helper_taken_branch("),1)
        self.assertNotIn("no_branch_trap",path)
        self.assertNotIn("TCG_COND_EQ, tb_bit, 0",path)

    def test_uia_precedes_tb_ss_and_uses_shared_0x5e00(self):
        a=self.h.index("void HELPER(taken_branch)(")
        b=self.h.index("void HELPER(single_step)(",a)
        src=self.h[a:b]
        for needle in ("ia64_uia_target_unimplemented(",
                       "ia64_phys_mode_addr(target)",
                       "!tb_enabled && !ss_enabled && !uia",
                       "uia ? IA64_VEC_LOWER_PRIVILEGE",
                       "tb_enabled ? IA64_VEC_TAKEN_BRANCH",
                       "ia64_uia_trap_isr(ri, source_psr, false)",
                       "ia64_tb_saved_ipsr(source_psr)",
                       "ia64_tb_target_iip(target)",
                       "env->cr_iipa = source"):
            self.assertIn(needle,src)
        self.assertLess(src.index("ia64_uia_target_unimplemented("),
                        src.index("if (!tb_enabled && !ss_enabled && !uia)"))
        self.assertLess(src.index("env->cr_isr = uia ?"),
                        src.index("cs->exception_index"))

    def test_model_declares_implemented_pa_va_not_synthetic_flags(self):
        for needle in ("IA64_MERCED_IMPL_VA_MSB 50",
                       "IA64_MERCED_IMPL_PA_MSB 43",
                       "ia64_uia_virtual_implemented",
                       "ia64_uia_physical_implemented",
                       "(UINT64_C(1) << 36)",
                       "IA64_TB_PSR_MASK) ? IA64_TB_CODE : 0"):
            self.assertIn(needle,self.policy)
        self.assertNotIn("IA64_UIA_CODE",self.policy)

    def test_real_guest_negative_positive_and_predicated_controls(self):
        for case in ("cond-uia-no-flags-b2","cond-uia-ss-b2",
                     "cond-uia-tb-b2","cond-uia-tb-ss-b2",
                     "cond-uia-ss-b0","cond-uia-tb-ss-b1",
                     "cond-uia-second-pa-ss","call-uia-no-flags",
                     "call-uia-tb-ss","ret-uia-no-flags",
                     "ret-uia-tb-ss","false-cond-no-flags",
                     "false-cond-ss","false-call-ss",
                     "valid-cond-tb-ss"):
            self.assertIn('"'+case+'"',self.guest)
        for phrase in ('"mov cr.ipsr=r9"','"mov cr.iip=r14"',
                       '"mov r9=cr.iipa"','"mov r9=cr.isr"',
                       '"mov r9=cr.iip"','"mov r9=cr.ipsr"',
                       "check_state(lines,p)", "ivt.run_one(",
                       "BAD_PA=0x100000000000"):
            self.assertIn(phrase,self.guest)

if __name__=="__main__":
    unittest.main()
