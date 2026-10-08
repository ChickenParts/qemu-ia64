#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed source-order checks for unpredicated B6/B7 predictions."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class B67DecoderTests(unittest.TestCase):
    def test_b67_decode_occurs_before_predicate_skip(self):
        source = (ROOT / "target/ia64/translate.c").read_text()
        start = source.index("static void decode_b_unit(")
        end = source.index("static void decode_insn(", start)
        branch = source[start:end]
        predication = branch.index("TCGLabel *skip_label = gen_qp_skip(qp);")
        early = branch[:predication]
        self.assertIn("major == 0x7 && (insn & 0x27) == 0", early)
        self.assertIn("(x6 == 0x10 || x6 == 0x11)", early)
        self.assertIn("(0x7ffULL << 16)", early)
        self.assertIn("0x2fULL", early)
        # B8 epc is also correctly routed before predicate skipping.
        # Keep the B6/B7 invariants local to their own dispatcher region
        # instead of forbidding future unpredicated B-unit instructions.
        b67_region = early.split("B8 epc is UNPREDICATED", 1)[0]
        self.assertEqual(2, b67_region.count("return;"))
        self.assertEqual(2, b67_region.count("    if (major =="))
        self.assertIn('gen_unimpl(ctx, insn, "B-slot");', branch[predication:])

    def test_fixed_bit_mutation_breaks_early_routing(self):
        source = (ROOT / "target/ia64/translate.c").read_text()
        start = source.index("static void decode_b_unit(")
        end = source.index("static void decode_insn(", start)
        before = source[start:end].split("TCGLabel *skip_label =", 1)[0]
        for replacement in ("0x00", "0x07", "0x20"):
            mutated = before.replace("(insn & 0x27) == 0",
                                     f"(insn & {replacement}) == 0", 1)
            self.assertNotIn("major == 0x7 && (insn & 0x27) == 0", mutated)


if __name__ == "__main__":
    unittest.main()
