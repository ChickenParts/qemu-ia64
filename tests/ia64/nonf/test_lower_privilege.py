#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed source ordering for IA64 br.ret privilege/PSR.lp traps."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[3]

class LowerPrivilegeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tr=(ROOT/"target/ia64/translate.c").read_text()
        cls.he=(ROOT/"target/ia64/helper.c").read_text()
        cls.cpu=(ROOT/"target/ia64/cpu.h").read_text()
        cls.decl=(ROOT/"target/ia64/helper.h").read_text()
        cls.policy=(ROOT/"target/ia64/lower-privilege.h").read_text()

    def test_ret_snapshots_ppl_before_rse_and_traps_after(self):
        b=self.tr[self.tr.index("static void decode_b_unit("):
                  self.tr.index("static void decode_insn(")]
        start=b.index("/* Snapshot the *caller's* saved PPL")
        snap=b.index("tcg_gen_shri_i64(ret_saved_ppl, pfs_before, 62)",start)
        restore=b.index("gen_helper_ret_restore(tcg_env)",snap)
        check=b.index("gen_helper_check_null_branch(tcg_env",restore)
        privilege=b.index("gen_helper_ret_privilege(tcg_env",check)
        record=b.index("gen_record_branch(ctx, insn, (x6 == 0x21)",privilege)
        self.assertLess(snap,restore)
        self.assertLess(restore,privilege)
        self.assertLess(privilege,record)
        self.assertIn("if (x6 == 0x21)",b[check:record])
        self.assertEqual(b.count("gen_helper_ret_privilege("),1)

    def test_post_ret_helper_commits_cpl_before_lp_interrupt(self):
        a=self.he.index("void HELPER(ret_privilege)(")
        b=self.he.index("void HELPER(taken_branch)(",a)
        code=self.he[a:b]
        for must in ("ia64_lp_return_cpl(current_cpl, saved_ppl)",
                     "ia64_lp_should_trap(source_psr", "env->psr =", 
                     "if (!lp_trap)", "uintptr_t retaddr = GETPC()",
                     "cpu_restore_state(cs, retaddr)", "ia64_intr_push_window(env)",
                     "ia64_tb_target_iip(target)", "env->cr_iipa = source",
                     "ia64_lp_isr(ri, interrupted_psr)",
                     "IA64_VEC_LOWER_PRIVILEGE", "cpu_loop_exit_restore(cs, retaddr)"):
            self.assertIn(must,code)
        self.assertLess(code.index("env->psr ="),code.index("if (!lp_trap)"))
        self.assertNotIn("gen_helper_taken_branch(",code)

    def test_architectural_policy_and_vector(self):
        self.assertIn("#define IA64_VEC_LOWER_PRIVILEGE      0x5E00", self.cpu)
        self.assertIn("DEF_HELPER_5(ret_privilege, void, env, i64, i32, i64, i64)",
                      self.decl)
        for v in ("IA64_LP_CODE","IA64_TB_CODE","IA64_SS_CODE","IA64_TB_ISR_NI_BIT",
                  "ia64_lp_should_trap","ia64_lp_return_cpl"):
            self.assertIn(v,self.policy)
        self.assertIn("cpl >= ppl ? cpl : ppl",self.policy)
        self.assertIn("return (source_psr & IA64_LP_MASK)",self.policy)

if __name__=="__main__":
    unittest.main()
