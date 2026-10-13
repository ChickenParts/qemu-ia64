#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Source-level B1/B2 table legality and exception noncommit guards."""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[3]

class B12SourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.translator = (ROOT / "target/ia64/translate.c").read_text()
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()
        cls.policy = (ROOT / "target/ia64/b12.h").read_text()

    def test_fault_is_before_ordinary_predication(self):
        start = self.translator.index("static void decode_b_unit(")
        end = self.translator.index("static void decode_insn(", start)
        body = self.translator[start:end]
        validate = body.index("gen_helper_b12_validate(tcg_env")
        skip = body.index("TCGLabel *skip_label = gen_qp_skip(predicate_for_skip);")
        execute = body.index("} else if (major == 0x4) {")
        self.assertLess(validate, skip)
        self.assertLess(skip, execute)
        self.assertIn("major == 0x4", body[validate - 150:skip])
        self.assertIn("tcg_constant_i64(insn)", body[validate:skip])

    def test_policy(self):
        for name in ("IA64_B12_COND", "IA64_B12_WEXIT", "IA64_B12_WTOP",
                     "IA64_B12_CLOOP", "IA64_B12_CEXIT", "IA64_B12_CTOP",
                     "IA64_B12_RESERVED"):
            self.assertIn(name, self.policy)
        self.assertIn("(qp & 0x3f) != 0", self.policy)

    def test_fault_helper_preserves_source_slot(self):
        a = self.helper.index("void HELPER(b12_validate)(")
        b = self.helper.index("static bool ia64_try_translate(", a)
        code = self.helper[a:b]
        self.assertIn("uintptr_t retaddr = GETPC();", code)
        self.assertIn("IA64_VEC_ILLEGAL_OP", code)
        self.assertIn("false, false, retaddr);", code)
        for bad in ("env->psr =", "cpu_pc", "env->ar[", "ia64_fault("):
            self.assertNotIn(bad, code)

    def test_existing_loop_behavior_not_rewritten(self):
        a = self.translator.index("} else if (major == 0x4) {")
        b = self.translator.index("} else if (major == 0x5) {", a)
        code = self.translator[a:b]
        for branch in ("br.cond target25", "br.wexit/br.wtop target25",
                       "br.cloop target25", "br.cexit/br.ctop target25"):
            self.assertIn(branch, code)
if __name__ == "__main__":
    unittest.main()
