#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed source contracts for target-wide post-commit taken-branch trap."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[3]

class TakenBranchSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t=(ROOT/"target/ia64/translate.c").read_text()
        cls.h=(ROOT/"target/ia64/helper.c").read_text()
        cls.policy=(ROOT/"target/ia64/taken-branch.h").read_text()
        cls.decl=(ROOT/"target/ia64/helper.h").read_text()

    def test_helper_uses_source_slot_but_target_resume(self):
        a=self.h.index("void HELPER(taken_branch)(")
        b=self.h.index("static bool ia64_fault(",a)
        s=self.h[a:b]
        for must in ("uintptr_t retaddr = GETPC()", "cpu_restore_state(cs, retaddr)",
                     "ia64_intr_push_window(env)", "ia64_tb_saved_ipsr(source_psr)",
                     "ia64_tb_target_iip(target)", "env->cr_iipa = source",
                     "ia64_tb_isr(ri, source_psr)",
                     "ia64_ifs_on_interruption(", "IA64_VEC_TAKEN_BRANCH",
                     "cpu_loop_exit_restore(cs, retaddr)"):
            self.assertIn(must,s)
        self.assertNotIn("gen_helper_rfi",s)
        self.assertNotIn("env->ar[IA64_AR_EC] =",s)
        self.assertNotIn("IA64_VEC_FP_TRAP",s)

    def test_all_taken_branches_check_uia_without_psr_debug_flags(self):
        a=self.t.index("static void gen_record_branch(")
        b=self.t.index("static void gen_record_b0_write(",a)
        s=self.t[a:b]
        for must in ("Every taken branch must check target validity",
                     "gen_helper_taken_branch(tcg_env", "tcg_constant_i32(ctx->ri)"):
            self.assertIn(must,s)
        self.assertNotIn("no_branch_trap",s)
        self.assertNotIn("TCG_COND_EQ, tb_bit, 0",s)
        self.assertEqual(s.count("gen_helper_taken_branch("),1)
        # RFI does not report a taken-branch trap, regardless of flags.
        a=self.t.index("/* B8: rfi")
        b=self.t.index("} else if (major == 0x0",a)
        self.assertNotIn("gen_record_branch(",self.t[a:b])

    def test_b1_b2_all_loop_taken_paths_recorded(self):
        a=self.t.index("} else if (btype == 2 || btype == 3) {",
                       self.t.index("static void decode_b_unit("))
        b=self.t.index("} else if (major == 0x5) {",a)
        s=self.t[a:b]
        self.assertIn("gen_record_branch(ctx, insn, 4",s)
        for needle in ("} else if (btype == 5) {",
                       "} else if (btype == 6 || btype == 7) {"):
            self.assertIn(needle,s)
        self.assertGreaterEqual(s.count("gen_record_branch(ctx, insn, 4"),3)

    def test_f14_and_lx_reuse_unified_taken_path(self):
        a=self.t.index("static void gen_fchkf_branch(")
        b=self.t.index("static void gen_a_unit_nat1(",a)
        self.assertIn("gen_record_branch(ctx, insn, 4",self.t[a:b])
        a=self.t.index("/* X3/X4: brl.cond / brl.call")
        b=self.t.index("gen_unimpl(ctx, insn, \"X-slot\")",a)
        self.assertGreaterEqual(self.t[a:b].count("gen_record_branch("),2)

    def test_priority_source_not_claimed(self):
        self.assertIn("IA64_TB_SS_PSR_MASK",self.policy)
        self.assertIn("IA64_TB_ISR_NI_BIT",self.policy)
        self.assertIn("DEF_HELPER_4(taken_branch, void, env, i64, i32, i64)",self.decl)

if __name__=="__main__":
    unittest.main()
