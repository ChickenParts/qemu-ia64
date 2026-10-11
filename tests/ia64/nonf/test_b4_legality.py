#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Source-level invariants for B4 indirect-branch legality."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class B4LegalitySourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.translator = (ROOT / "target/ia64/translate.c").read_text()
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()
        cls.policy = (ROOT / "target/ia64/b4.h").read_text()

    def test_validation_precedes_predicate_skip(self):
        begin = self.translator.index("static void decode_b_unit(")
        end = self.translator.index("static void decode_insn(", begin)
        decoder = self.translator[begin:end]
        validate = decoder.index("gen_helper_b4_validate(tcg_env")
        skip = decoder.index("TCGLabel *skip_label = gen_qp_skip(qp);")
        execute = decoder.index(
            "} else if (major == 0x0 && (x6 == 0x20 || x6 == 0x21)) {")
        self.assertLess(validate, skip)
        self.assertLess(skip, execute)
        self.assertIn("tcg_constant_i64(insn)", decoder[validate:skip])

    def test_policy_matches_only_assigned_b4_cells(self):
        self.assertIn("btype == 0", self.policy)
        self.assertIn("IA64_B4_COND", self.policy)
        self.assertIn("btype == 1", self.policy)
        self.assertIn("IA64_B4_IA32", self.policy)
        self.assertIn("btype == 4 ? IA64_B4_RET", self.policy)
        self.assertIn("IA64_B4_RESERVED", self.policy)
        self.assertIn("kind == IA64_B4_RESERVED", self.policy)
        self.assertIn("kind == IA64_B4_IA32 && (qp & 0x3f) != 0", self.policy)

    def test_fault_helper_preserves_translated_return_address(self):
        begin = self.helper.index("void HELPER(b4_validate)(")
        end = self.helper.index("static bool ia64_try_translate(", begin)
        body = self.helper[begin:end]
        self.assertIn("uintptr_t retaddr = GETPC();", body)
        self.assertIn("IA64_VEC_ILLEGAL_OP", body)
        self.assertIn("false, false, retaddr);", body)
        self.assertNotIn("ia64_fault(", body)
        self.assertNotIn("env->psr =", body)
        self.assertNotIn("cpu_pc", body)

    def test_successful_br_ia_is_not_claimed_complete(self):
        begin = self.translator.index(
            "} else if (major == 0x0 && (x6 == 0x20 || x6 == 0x21)) {")
        end = self.translator.index("} else if (major == 0x1)", begin)
        body = self.translator[begin:end]
        self.assertIn("Successful br.ia transition semantics remain", body)
        self.assertNotIn("IA64_PSR_IS", body)
        self.assertNotIn("IA64_PSR_DI", body)


if __name__ == "__main__":
    unittest.main()
