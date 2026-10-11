#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class B5LegalitySourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.translator = (ROOT / "target/ia64/translate.c").read_text()
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()
        cls.policy = (ROOT / "target/ia64/b5.h").read_text()

    def test_validation_precedes_predicate_skip_and_call(self):
        begin = self.translator.index("static void decode_b_unit(")
        end = self.translator.index("static void decode_insn(", begin)
        decoder = self.translator[begin:end]
        validate = decoder.index("gen_helper_b5_validate(tcg_env")
        skip = decoder.index("TCGLabel *skip_label = gen_qp_skip(qp);")
        call = decoder.index("} else if (major == 0x1) {")
        self.assertLess(validate, skip)
        self.assertLess(skip, call)
        self.assertIn("tcg_constant_i64(insn)", decoder[validate:skip])

    def test_policy_assigns_only_odd_wh(self):
        for value in (1, 3, 5, 7):
            self.assertIn(f"(wh & 7) == {value}", self.policy)
        self.assertIn("major == 1", self.policy)
        self.assertIn("!ia64_b5_wh_is_assigned", self.policy)

    def test_fault_is_non_access_and_has_no_call_side_effect(self):
        begin = self.helper.index("void HELPER(b5_validate)(")
        end = self.helper.index("static bool ia64_try_translate(", begin)
        body = self.helper[begin:end]
        self.assertIn("uintptr_t retaddr = GETPC();", body)
        self.assertIn("IA64_VEC_ILLEGAL_OP", body)
        self.assertIn("false, false, retaddr);", body)
        self.assertNotIn("ia64_fault(", body)
        self.assertNotIn("env->psr =", body)
        self.assertNotIn("HELPER(call)", body)

    def test_call_execution_is_labeled_assigned_only(self):
        begin = self.translator.index("} else if (major == 0x1) {")
        body = self.translator[begin:begin + 400]
        self.assertIn("assigned odd-wh br.call", body)


if __name__ == "__main__":
    unittest.main()
