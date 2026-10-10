#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lock vmsw's unpredicated decode, no-VM fault and PAL contract."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class VMSWSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.trans = (ROOT / "target/ia64/translate.c").read_text()
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()
        cls.helper_h = (ROOT / "target/ia64/helper.h").read_text()
        cls.pal = (ROOT / "target/ia64/pal.c").read_text()
        cls.policy = (ROOT / "target/ia64/vmsw.h").read_text()

    def test_decode_precedes_predicate_qualification(self):
        start = self.trans.index("static void decode_b_unit(")
        end = self.trans.index("static void decode_insn(", start)
        decoder = self.trans[start:end]
        vmsw = decoder.index(
            "if (major == 0 && (x6 == 0x18 || x6 == 0x19))"
        )
        skip = decoder.index("TCGLabel *skip_label = gen_qp_skip(qp);")
        self.assertLess(vmsw, skip)
        self.assertIn("gen_helper_vmsw(tcg_env", decoder[vmsw:skip])
        self.assertIn("ctx->base.is_jmp = DISAS_TOO_MANY;",
                      decoder[vmsw:skip])
        self.assertIn("DEF_HELPER_3(vmsw, void, env, i64, i64)",
                      self.helper_h)

    def test_current_model_delivers_illegal_without_vm_commit(self):
        begin = self.helper.index("void HELPER(vmsw)(")
        end = self.helper.index("static bool ia64_try_translate(", begin)
        body = self.helper[begin:end]
        validate = body.index("ia64_vmsw_encoding_valid(insn)")
        absent = body.rindex("ia64_vmsw_raise_illegal(env)")
        self.assertLess(validate, absent)
        self.assertNotIn("IA64_PSR_VM", body)
        self.assertNotIn("env->psr =", body)
        fault_begin = self.helper.index(
            "static void ia64_vmsw_raise_illegal(")
        fault_end = self.helper.index("void HELPER(vmsw)(", fault_begin)
        fault = self.helper[fault_begin:fault_end]
        self.assertIn("IA64_VEC_ILLEGAL_OP", fault)
        self.assertNotIn("ia64_fault(", fault)

    def test_encoding_requires_zero_qp_and_fixed_fields(self):
        self.assertIn("(insn & ~(UINT64_C(0x3f) << 27)) == 0",
                      self.policy)
        self.assertIn("IA64_VMSW_X6_CLEAR 0x18U", self.policy)
        self.assertIn("IA64_VMSW_X6_SET   0x19U", self.policy)

    def test_pal_reports_read_only_no_vm_and_disabled_vmsw(self):
        self.assertIn("ia64_vmsw_no_vm_pal_features(feature_set", self.pal)
        self.assertIn("IA64_PAL_STATUS_SUCCESS", self.pal)
        self.assertIn("UINT64_C(1) << 40", self.policy)
        self.assertIn("UINT64_C(1) << 54", self.policy)
        feature = self.policy.index("*implemented = IA64_PAL_PROC_FEATURE_NO_VM")
        result = self.policy[feature:]
        self.assertIn("*current = IA64_PAL_PROC_FEATURE_NO_VM", result)
        self.assertNotIn("*current = IA64_PAL_PROC_FEATURE_ENABLE_VMSW", result)


if __name__ == "__main__":
    unittest.main()
