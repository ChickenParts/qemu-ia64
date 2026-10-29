#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed checks for sequential SS retirement and trap precedence."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class SingleStepSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tr = (ROOT / "target/ia64/translate.c").read_text()
        cls.he = (ROOT / "target/ia64/helper.c").read_text()
        cls.decl = (ROOT / "target/ia64/helper.h").read_text()
        cls.cpu = (ROOT / "target/ia64/cpu.h").read_text()

    def test_sequential_helper_after_decode_before_next_slot(self):
        block = self.tr[self.tr.index("static void ia64_tr_translate_insn("):
                        self.tr.index("static void ia64_tr_tb_stop(")]
        self.assertLess(block.index("decode_insn(ctx, insn, type);"),
                        block.index("gen_helper_single_step(tcg_env"))
        self.assertLess(block.index("gen_helper_single_step(tcg_env"),
                        block.index("ctx->ri++;"))
        self.assertIn("ctx->base.is_jmp != DISAS_NORETURN", block)
        self.assertIn("type != SLOT_L", block)
        self.assertIn("mlx_pair ? 1 : cur_ri", block)
        self.assertIn("ia64_ss_next_ri(cur_ri, mlx_pair)", block)
        self.assertIn("ia64_ss_next_iip(ctx->base.pc_next, cur_ri", block)
        self.assertIn("extract64(insn, 27, 6) == 0x8", block)
        self.assertIn("tcg_gen_andi_i64(ss_flag, cpu_psr, IA64_PSR_SS)", block)
        self.assertEqual(block.count("gen_helper_single_step("), 1)

    def test_branch_priority_after_lp_helper_and_runtime(self):
        branch = self.tr[self.tr.index("static void gen_record_branch("):
                         self.tr.index("static void gen_record_b0_write(")]
        self.assertIn("IA64_PSR_TB | IA64_PSR_SS", branch)
        self.assertEqual(branch.count("gen_helper_taken_branch("), 1)
        b4 = self.tr[self.tr.index("static void decode_b_unit("):
                     self.tr.index("static void decode_insn(")]
        self.assertLess(b4.index("gen_helper_ret_privilege("),
                        b4.index("gen_record_branch(ctx, insn, (x6 == 0x21)"))
        helper = self.he[self.he.index("void HELPER(taken_branch)("):
                         self.he.index("void HELPER(single_step)(")]
        self.assertIn("tb_enabled ? IA64_VEC_TAKEN_BRANCH", helper)
        self.assertIn("tb_enabled ? ia64_tb_isr(ri, source_psr)", helper)
        self.assertIn("ia64_ss_isr(ri, source_psr)", helper)

    def test_post_success_snapshot_not_fault_path(self):
        ss = self.he[self.he.index("void HELPER(single_step)("):
                     self.he.index("static bool ia64_fault(")]
        for needle in ("cpu_restore_state(cs, retaddr)",
                       "ia64_intr_push_window(env)",
                       "ia64_ss_saved_ipsr(source_psr, next_ri)",
                       "env->cr_iip = next_iip",
                       "env->cr_iipa = source",
                       "ia64_ss_isr(source_ri, source_psr)",
                       "ia64_ifs_on_interruption(",
                       "IA64_VEC_SINGLE_STEP",
                       "cpu_loop_exit_restore(cs, retaddr)"):
            self.assertIn(needle, ss)
        self.assertIn("DEF_HELPER_5(single_step, void, env, i64, i32, i64, i32)",
                      self.decl)
        self.assertIn("#define IA64_VEC_SINGLE_STEP          0x6000", self.cpu)


if __name__ == "__main__":
    unittest.main()
