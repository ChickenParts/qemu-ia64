#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mutation-oriented B1/B2 last-slot and while-operand routing assertions."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]

class LoopStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = (ROOT / "target/ia64/translate.c").read_text()
        cls.help = (ROOT / "target/ia64/helper.c").read_text()
        cls.policy = (ROOT / "target/ia64/b12.h").read_text()
        cls.decl = (ROOT / "target/ia64/helper.h").read_text()

    def test_while_qp_is_operand_not_skip_guard(self):
        a = self.src.index("static void decode_b_unit(")
        b = self.src.index("static void decode_insn(",a)
        branch = self.src[a:b]
        validate = branch.index("gen_helper_b12_validate(tcg_env")
        skip = branch.index("TCGLabel *skip_label = gen_qp_skip(predicate_for_skip);")
        execute = branch.index("} else if (major == 0x4) {")
        self.assertLess(validate, skip)
        self.assertLess(skip, execute)
        self.assertIn("b12_type == 2 || b12_type == 3", branch[validate:skip])
        self.assertIn("predicate_for_skip = 0;", branch[validate:skip])
        while_block = branch[execute:branch.index("} else if (major == 0x5)",execute)]
        self.assertIn("gen_pr_read_bit(qp)",while_block)
        self.assertIn("tcg_gen_brcondi_i64(TCG_COND_EQ, qp_val, 0, check_ec)",while_block)

    def test_slot_validation_precedes_execution_and_preserves_return_address(self):
        self.assertIn("DEF_HELPER_4(b12_validate, void, env, i64, i64, i32)",
                      self.decl)
        a = self.help.index("void HELPER(b12_validate)(")
        b = self.help.index("static bool ia64_try_translate(",a)
        code=self.help[a:b]
        self.assertIn("uintptr_t retaddr = GETPC();", code)
        self.assertIn("ia64_b12_loop_wrong_slot(kind, ri)",code)
        self.assertIn("IA64_VEC_ILLEGAL_OP",code)
        self.assertIn("false, false, retaddr);",code)
        self.assertNotIn("env->ar[",code)
        self.assertNotIn("env->psr =",code)

    def test_all_five_loop_families_required_last_slot(self):
        for fam in ("IA64_B12_WEXIT", "IA64_B12_WTOP", "IA64_B12_CLOOP",
                    "IA64_B12_CEXIT", "IA64_B12_CTOP"):
            self.assertIn("kind == "+fam,self.policy)
        self.assertIn("ri != 2",self.policy)
        self.assertIn("while_uses_qp_as_operand",self.policy)

    def test_existing_parent_no_vm_and_fault_guards_retained(self):
        self.assertIn("gen_helper_b4_validate(tcg_env",self.src)
        self.assertIn("gen_helper_b5_validate(tcg_env",self.src)
        self.assertIn("gen_helper_epc(tcg_env",self.src)
if __name__ == "__main__":
    unittest.main()
