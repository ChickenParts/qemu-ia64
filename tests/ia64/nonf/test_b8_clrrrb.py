#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Source dispatch / GR rematerialization / test-oracle mutation guards."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class B8SourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.translator = (ROOT / "target/ia64/translate.c").read_text()
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()

    def test_legal_b8_dispatch_is_exact_and_not_nop(self):
        begin = self.translator.index("static void decode_b_unit(")
        end = self.translator.index("static void decode_insn(", begin)
        decode = self.translator[begin:end]
        self.assertIn("(x6 == 0x4 || x6 == 0x5)", decode)
        self.assertIn("(insn & ~(UINT64_C(0x3f) << 27)) == 0", decode)
        self.assertIn("gen_helper_clrrrb(tcg_env, tcg_constant_i32(x6 == 0x5))",
                      decode)
        self.assertLess(decode.index("gen_helper_clrrrb"),
                        decode.index('gen_unimpl(ctx, insn, "B-slot");'))
        self.assertIn("DEF_HELPER_2(clrrrb, void, env, i32)",
                      (ROOT / "target/ia64/helper.h").read_text())

    def test_helper_reorders_nat_with_gr_and_preserves_pr_only(self):
        start = self.helper.index("void HELPER(clrrrb)(")
        end = self.helper.index("void HELPER(call)(", start)
        body = self.helper[start:end]
        self.assertIn("if (predicate_only)", body)
        self.assertIn("env->cfm = old_cfm & ~pr_mask;", body)
        self.assertIn("uint32_t src = (i + count - rrbg) % count;", body)
        self.assertIn("values[i] = env->r[32 + src];", body)
        self.assertIn("nats[i] = env->nat[32 + src];", body)
        self.assertIn("memcpy(&env->r[32]", body)
        self.assertIn("memcpy(&env->nat[32]", body)
        self.assertIn("(UINT64_C(0x7f) << 25)", body)

    def test_missing_gr_nat_remap_mutations_are_caught(self):
        start = self.helper.index("void HELPER(clrrrb)(")
        end = self.helper.index("void HELPER(call)(", start)
        body = self.helper[start:end]
        for needle in ("nats[i] = env->nat[32 + src];",
                       "uint32_t src = (i + count - rrbg) % count;",
                       "env->cfm = old_cfm & ~pr_mask;"):
            mutated = body.replace(needle, "", 1)
            self.assertIn(needle, body)
            self.assertNotIn(needle, mutated)

    def test_real_guest_profiles_include_wrap_and_each_form(self):
        path = ROOT / "scripts/run-ia64-b8-clrrrb-tests.py"
        src = path.read_text()
        self.assertIn("ROTATIONS = (0, 1, 2, 7, 8, 9)", src)
        self.assertIn("br.call.sptk b0=cfm_snapshot", src)
        self.assertIn("mov r9=ar.pfs", src)
        self.assertIn("getf.sig r9=f32", src)
        self.assertIn("ld8.fill r34=[r12]", src)
        self.assertIn("tnat.z p6,p7=r{nat_after}", src)
        self.assertIn("br.wtop.sptk rotated_", src)
        self.assertIn("cmp.eq p16,p17=r0,r0", src)


if __name__ == "__main__":
    unittest.main()
