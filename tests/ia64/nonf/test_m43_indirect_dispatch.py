#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed Intel SDM Vol3 M42/M43 opcode separation source oracle.

M42 itr.d/i x6=0e/0f and ptr.d/i x6=0c/0d must not be intercepted
by M43 mov-FROM indirect register instructions (x6=10..15).
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]


class IndirectRegisterOpcodeTable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = (ROOT / "target/ia64/translate.c").read_text()
        cls.source = src[src.index("        case 0x1: {", src.index("static void decode_insn(")):
                         src.index("        case 0x2:", src.index("        case 0x1: {", src.index("static void decode_insn(")))]

    def test_m43_read_codes_match_intel_table_4_45(self):
        # Table 4-45: all these require x3=0, major 1, x6=0x10..0x15.
        checks = {
            "rr": 0x10,
            "dbr": 0x11,
            "ibr": 0x12,
            "pkr": 0x13,
            "pmc": 0x14,
            "pmd": 0x15,
        }
        for register, code in checks.items():
            with self.subTest(register=register):
                block = re.search(
                    r"(?:if|else if) \(x3 == 0 && x6 == 0x([0-9a-f]+)\)"
                    r" \{\s*/\* (?:M43: )?mov r1 = "
                    + re.escape(register) + r"\[r3\] \*/",
                    self.source)
                self.assertIsNotNone(block, register)
                self.assertEqual(int(block.group(1), 16), code)

    def test_m42_tr_and_purge_codes_are_not_stolen(self):
        for name, code in {"ptr.d": 0xc, "ptr.i": 0xd,
                           "itr.d": 0xe, "itr.i": 0xf}.items():
            with self.subTest(instruction=name):
                self.assertRegex(
                    self.source,
                    rf"x3 == 0 && x6 == 0x{code:x}\) \{{\s*/\* {re.escape(name)} \*/")
                self.assertNotRegex(
                    self.source,
                    rf"x3 == 0 && x6 == 0x{code:x}\) \{{\s*/\* M43:")

    def test_critical_privileged_opcodes_reach_real_helpers(self):
        for helper in ("ptr_d", "ptr_i", "itr_d", "itr_i"):
            self.assertIn(f"gen_helper_{helper}(tcg_env,", self.source)
        self.assertIn("gen_helper_ptc_e(tcg_env,", self.source)


if __name__ == "__main__":
    unittest.main()
