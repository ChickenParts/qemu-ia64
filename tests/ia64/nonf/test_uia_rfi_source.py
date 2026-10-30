#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed source guards for native rfi UIA; not ISA completeness."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class RFIIUATests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()
        cls.translate = (ROOT / "target/ia64/translate.c").read_text()
        cls.helper_h = (ROOT / "target/ia64/helper.h").read_text()
        cls.policy = (ROOT / "target/ia64/uia-address.h").read_text()
        cls.script = (ROOT / "scripts/run-ia64-uia-rfi-tests.py").read_text()

    def test_rfi_source_and_slot_passed_explicitly(self):
        self.assertIn("DEF_HELPER_3(rfi, void, env, i64, i32)", self.helper_h)
        self.assertIn("gen_helper_rfi(tcg_env,", self.translate)
        self.assertIn("tcg_constant_i64(ctx->base.pc_next)", self.translate)
        self.assertIn("tcg_constant_i32(ctx->ri)", self.translate)

    def test_restored_psr_controls_native_target_check(self):
        a = self.helper.index("void HELPER(rfi)(")
        b = self.helper.index("static uint64_t ia64_dbg_next_call_pc;", a)
        rfi = self.helper[a:b]
        for required in ("uint64_t new_psr = env->cr_ipsr;",
                         "uint64_t new_ip = env->cr_iip",
                         "ia64_uia_itanium_rfi_unimplemented(",
                         "ia64_phys_mode_addr(new_ip)",
                         "env->psr = new_psr;", "env->ip = new_ip;",
                         "ia64_intr_push_window(env);",
                         "env->cr_ipsr = new_psr;",
                         "env->cr_iip = new_ip;",
                         "env->cr_iipa = source",
                         "ia64_ifs_on_interruption(",
                         "ia64_uia_rfi_isr(source_ri, new_psr)",
                         "IA64_VEC_LOWER_PRIVILEGE",
                         "cpu_loop_exit_restore(cs, retaddr);"):
            self.assertIn(required, rfi)
        self.assertLess(rfi.index("ia64_uia_itanium_rfi_unimplemented("),
                        rfi.index("ia64_rse_cancel_stack_switch(env);"))
        self.assertLess(rfi.index("env->psr = new_psr;"),
                        rfi.index("ia64_intr_push_window(env);"))
        self.assertNotIn("ia64_tb_isr(", rfi)
        self.assertNotIn("ia64_lp_isr(", rfi)
        self.assertNotIn("ia64_ss_isr(", rfi)

    def test_rfi_status_cannot_invent_concurrent_trap_codes(self):
        a = self.policy.index("static inline uint64_t ia64_uia_rfi_isr(")
        rfi = self.policy[a:]
        self.assertIn("IA64_TB_ISR_EI_SHIFT", rfi)
        self.assertIn("IA64_TB_ISR_NI_BIT", rfi)
        for token in ("IA64_TB_CODE", "IA64_SS_CODE", "IA64_LP_CODE"):
            self.assertNotIn(token, rfi)
        self.assertIn("UINT64_C(1) << 34", self.policy)

    def test_real_assembly_cases_and_genuine_rfi(self):
        for name in ("rfi-uia-plain", "rfi-uia-ss", "rfi-uia-tb",
                     "rfi-uia-lp-tb-ss", "rfi-uia-second-pa",
                     "rfi-uia-restored-ic", "rfi-uia-restored-ic-lp-tb-ss",
                     "rfi-uia-restored-ri1", "rfi-uia-restored-ri2-ss",
                     "rfi-valid-return", "rfi-valid-collected"):
            self.assertIn('"' + name + '"', self.script)
        for needle in ('"rfi"', '"mov cr.ipsr=r9"', '"mov cr.iip=r14"',
                       '"mov r9=cr.isr"', '"mov r9=cr.iipa"',
                       '"mov r9=cr.iip"', '"mov r9=cr.ipsr"',
                       "p.target_ri << 41", "ivt.run_one(", "NI, EI_RFI"):
            self.assertIn(needle, self.script)


if __name__ == "__main__":
    unittest.main()
