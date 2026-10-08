#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lock epc's early decoder selection, fetch-side metadata and fault order."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class EPCSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.trans = (ROOT / "target/ia64/translate.c").read_text()
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()
        cls.spec = (ROOT / "target/ia64/epc.h").read_text()

    def test_epc_is_before_qualification_and_ends_translation_block(self):
        start = self.trans.index("static void decode_b_unit(")
        end = self.trans.index("static void decode_insn(", start)
        decoder = self.trans[start:end]
        epc = decoder.index("if (major == 0 && x6 == 0x10)")
        skip = decoder.index("TCGLabel *skip_label = gen_qp_skip(qp);")
        self.assertLess(epc, skip)
        self.assertIn("gen_helper_epc(tcg_env", decoder[:skip])
        self.assertIn("ctx->base.is_jmp = DISAS_TOO_MANY;", decoder[epc:skip])
        self.assertIn("DEF_HELPER_3(epc, void, env, i64, i64)",
                      (ROOT / "target/ia64/helper.h").read_text())

    def test_invalid_fixed_fields_fault_before_pfs_and_psr_mutations(self):
        begin = self.helper.index("void HELPER(epc)(")
        end = self.helper.index("static bool ia64_try_translate(", begin)
        body = self.helper[begin:end]
        bad = body.index("(insn & ~(UINT64_C(0x3f) << 27)) != 0")
        check = body.index("ia64_epc_next_cpl(")
        fault = body.index("if (target_cpl == IA64_EPC_ILLEGAL)")
        commit = body.index("env->psr =")
        self.assertLess(bad, check)
        self.assertLess(check, fault)
        self.assertLess(fault, commit)
        self.assertIn("IA64_VEC_ILLEGAL_OP", body)
        self.assertIn("GETPC()", body)

    def test_fetch_uses_only_instruction_entries_and_correct_pc(self):
        start = self.helper.index("static bool ia64_epc_fetch_rights(")
        end = self.helper.index("void HELPER(epc)(", start)
        body = self.helper[start:end]
        self.assertIn("env->itrs", body)
        self.assertIn("env->itlb", body)
        self.assertIn("RR_RID(env->rr[extract64(pc, 61, 3)])", body)
        self.assertIn("env->itlb[i].ar", body)
        self.assertIn("env->itlb[i].pl", body)
        self.assertNotIn("env->dtlb", body)
        self.assertNotIn("env->dtrs", body)

    def test_decision_requires_execute_only_and_non_degrading_pl(self):
        self.assertIn("fetch_ar == 7 && fetch_pl < cpl", self.spec)
        self.assertIn("if (ppl < cpl)", self.spec)
        self.assertIn("if (!instruction_translation)", self.spec)
        self.assertIn("fetch_translation_valid", self.spec)


if __name__ == "__main__":
    unittest.main()
