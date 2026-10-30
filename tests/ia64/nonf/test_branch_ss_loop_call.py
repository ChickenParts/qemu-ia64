#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed source and real-guest coverage guards for loop/call SS."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]

class BranchSingleStepMatrix(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner=(ROOT/"scripts/run-ia64-branch-ss-loop-call-tests.py").read_text()
        cls.tr=(ROOT/"target/ia64/translate.c").read_text()
        cls.helper=(ROOT/"target/ia64/helper.c").read_text()

    def test_required_profiles_and_independent_firmware_free_guests(self):
        for profile in ("cloop-lc2-ss","cloop-lc2-tb-ss","cloop-lc0-tb-ss",
                        "wtop-false-ec2-ss","wtop-false-ec1-tb-ss",
                        "wexit-false-ec0-tb-ss","ctop-lc2-tb-ss",
                        "ctop-ec2-ss","cexit-zero-ss","cexit-lc2-tb-ss",
                        "b3-b0-ss","b3-b0-tb-ss","b3-b3-slot0-ss",
                        "b5-b1-ss","b5-b1-tb-ss","b5-alias-b3-tb-ss",
                        "b3-false-b0-tb-ss","b5-false-b1-slot1-tb-ss"):
            self.assertIn('"'+profile+'"',self.runner)
        self.assertIn('ivt.run_one(case, args,',self.runner)
        self.assertIn('"--assemble-only"',self.runner)
        self.assertIn('"summary.json"',self.runner)

    def test_real_ri_and_single_rfi_resume_gate(self):
        for phrase in ('"mov cr.ipsr=r9"','"mov cr.iip=r14"',
                       "SS | (TB if p.tb else 0) | IC",
                       "p.slot << 41", '"mov r9=cr.isr"',
                       '"mov r9=cr.iipa"', '"mov r9=cr.iip"',
                       '"mov r9=cr.ipsr"', 'check_state(lines, p)',
                       'hex(SS | TB)', '"rfi"'):
            self.assertIn(phrase,self.runner)
        self.assertIn('return TBV if self.taken and self.tb else SSV',self.runner)
        self.assertIn('return 12 if self.taken and self.tb else 8',self.runner)

    def test_state_covered_in_handler_and_after_rfi(self):
        self.assertGreaterEqual(self.runner.count("check_state(lines, p)"),3)
        for phrase in ('"mov r9=ar.ec"','"mov r9=ar.lc"',
                       '"mov r9=ar.pfs"','"mov r9=b{p.dest_br}"',
                       '"br.call.sptk b5=frame_probe_target"',
                       'RRBG | CFM_SHAPE','p.rotates'):
            self.assertIn(phrase,self.runner)

    def test_assembler_and_translator_commit_before_taken_trap(self):
        a=self.tr.index("static void decode_b_unit(")
        b=self.tr.index("static void decode_insn(",a)
        src=self.tr[a:b]
        self.assertIn("gen_helper_call(tcg_env",src)
        self.assertIn("gen_store_ar(65, lc1);",src)
        self.assertIn("gen_store_ar(66, ec_new);",src)
        self.assertIn("gen_record_branch(ctx, insn",src)
        self.assertLess(src.index("tcg_gen_movi_i64(cpu_b[b1]"),
                        src.index("gen_record_branch(ctx, insn, 3, tgt)"))
        self.assertLess(src.index("gen_helper_call(tcg_env"),
                        src.index("gen_record_branch(ctx, insn, 3, tgt)"))

    def test_taken_helper_reports_target_and_both_trap_codes(self):
        a=self.helper.index("void HELPER(taken_branch)(")
        b=self.helper.index("void HELPER(single_step)(",a)
        src=self.helper[a:b]
        for phrase in ("ia64_tb_saved_ipsr(source_psr)",
                       "ia64_tb_target_iip(target)",
                       "env->cr_iipa = source", "ia64_tb_isr(ri, source_psr)",
                       "ia64_ss_isr(ri, source_psr)"):
            self.assertIn(phrase,src)

if __name__=="__main__":
    unittest.main()
