#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed source + real IVT coverage guards for sequential UIA."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]

class MercedSequentialUIA(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tr = (ROOT / "target/ia64/translate.c").read_text()
        cls.he = (ROOT / "target/ia64/helper.c").read_text()
        cls.policy = (ROOT / "target/ia64/uia-address.h").read_text()
        cls.guest = (ROOT / "scripts/run-ia64-uia-sequential-tests.py").read_text()

    def test_only_successful_last_slot_exercises_edge_without_ss(self):
        block = self.tr[self.tr.index("static void ia64_tr_translate_insn("):
                        self.tr.index("static void ia64_tr_tb_stop(")]
        for phrase in ("ctx->base.is_jmp != DISAS_NORETURN",
                       "type != SLOT_L", "step_next_ri == 0",
                       "ia64_uia_sequential_edge_candidate(",
                       "if (!uia_edge)", "gen_helper_single_step(tcg_env",
                       "ctx->base.is_jmp = DISAS_TOO_MANY;", "ctx->ri++;"):
            self.assertIn(phrase, block)
        self.assertEqual(block.count("gen_helper_single_step("), 1)
        self.assertLess(block.index("decode_insn(ctx, insn, type);"),
                        block.index("ia64_uia_sequential_edge_candidate("))
        self.assertLess(block.index("gen_helper_single_step(tcg_env"),
                        block.index("ctx->ri++;"))
        self.assertLess(block.index("ctx->base.is_jmp = DISAS_TOO_MANY;"),
                        block.index("ctx->ri++;"))

    def test_sequential_trap_priority_and_postcommit_state(self):
        h = self.he[self.he.index("void HELPER(single_step)("):
                    self.he.index("static bool ia64_fault(")]
        for needle in ("next_ri == 0 && ia64_uia_target_unimplemented(",
                       "ia64_phys_mode_addr(next_iip)",
                       "!uia && !ia64_ss_enabled(env->psr)",
                       "uia ? IA64_VEC_LOWER_PRIVILEGE",
                       "ia64_ss_saved_ipsr(source_psr, next_ri)",
                       "env->cr_iipa = source", "env->cr_iip = next_iip",
                       "ia64_ifs_on_interruption(",
                       "ia64_uia_sequential_isr(source_ri, source_psr)",
                       "cpu_loop_exit_restore(cs, retaddr)"):
            self.assertIn(needle, h)
        self.assertLess(h.index("ia64_uia_target_unimplemented("),
                        h.index("if (!uia && !ia64_ss_enabled"))
        self.assertLess(h.index("env->cr_isr = uia ?"),
                        h.index("cs->exception_index"))

    def test_sequential_isr_never_invents_tb_or_lp(self):
        s = self.policy[self.policy.index("static inline uint64_t ia64_uia_sequential_isr("):]
        self.assertIn("IA64_TB_SS_PSR_MASK", s)
        for needle in ("IA64_TB_CODE", "IA64_LP_CODE", "IA64_TB_PSR_MASK"):
            self.assertNotIn(needle, s)

    def test_real_16k_itc_mapping_and_negative_controls(self):
        for case in ("edge-adds-no-flags", "edge-adds-ss",
                     "edge-adds-tb", "edge-adds-tb-ss",
                     "edge-adds-ic-ss", "edge-nop-b-tb-ss",
                     "edge-nop-f-ss", "edge-long-mov-ss",
                     "edge-false-branch-tb-ss",
                     "near-adds-ss", "near-nop-b-ss"):
            self.assertIn('"' + case + '"', self.guest)
        for needle in ('"itc.i r20"', "install_itc(",
                       '"edge_page", 14', '".balign 16384"',
                       '".space 16368, 0"',
                       '"mov cr.ipsr=r9"', '"rfi"',
                       '"mov r9=cr.isr"', '"mov r9=cr.iipa"',
                       '"mov r9=cr.iip"', '"mov r9=cr.ipsr"',
                       '"mov cr.iip=r14"', "p.source_va",
                       "ivt.run_one("):
            self.assertIn(needle, self.guest)

if __name__ == "__main__":
    unittest.main()
