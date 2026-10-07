#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed tests for the non-F source fallback inventory."""
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "nonf_audit", ROOT / "scripts/ia64-nonf-fallback-audit.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)
SOURCE = (ROOT / "target/ia64/translate.c").read_text()


class NonFFallbackTests(unittest.TestCase):
    def test_deterministic_source_site_census(self):
        one = audit.scan(SOURCE)
        self.assertEqual(one, audit.scan(SOURCE))
        self.assertEqual(len(one["sites"]), 21)
        self.assertEqual(sum(one["counts_by_unit"][u] for u in audit.NON_F),
                         16)
        self.assertEqual(one["counts_by_unit"]["BREAK_HELPER"], 2)
        self.assertEqual(one["counts_by_unit"]["F"], 1)

    def test_distinct_non_f_units(self):
        counts = audit.scan(SOURCE)["counts_by_unit"]
        self.assertEqual(
            [counts[u] for u in audit.NON_F], [1, 5, 6, 2, 2])

    def test_no_architectural_completeness_claim(self):
        record = audit.scan(SOURCE)
        self.assertFalse(record["isa_complete"])
        self.assertEqual(record["scope"],
                         "translator-source-fallback-sites-only")
        self.assertTrue(
            all(s["classification"] == "not-adjudicated"
                for s in record["sites"] if s["unit"] in audit.NON_F))

    def test_relabeling_detected_even_with_same_site_count(self):
        needle = 'gen_unimpl(ctx, insn, "B-slot");'
        self.assertEqual(SOURCE.count(needle), 1)
        changed = SOURCE.replace(needle,
                                 'gen_unimpl(ctx, insn, "I-slot");', 1)
        old = audit.scan(SOURCE)
        new = audit.scan(changed)
        self.assertEqual(len(old["sites"]), len(new["sites"]))
        self.assertNotEqual(old, new)

    def test_unknown_call_is_fail_closed(self):
        changed = SOURCE.replace(
            'gen_unimpl(ctx, insn, "A-slot");',
            'gen_unimpl(ctx, insn, "unknown-slot");', 1)
        self.assertNotEqual(SOURCE, changed)
        with self.assertRaisesRegex(ValueError, "unknown gen_unimpl"):
            audit.scan(changed)

    def test_comment_does_not_invent_site(self):
        changed = SOURCE + '\n// gen_unimpl(ctx, insn, "A-slot");\n'
        self.assertEqual(len(audit.scan(SOURCE)["sites"]),
                         len(audit.scan(changed)["sites"]))

    def test_source_line_shift_is_reported(self):
        left = audit.scan(SOURCE)
        right = audit.scan("\n" + SOURCE)
        self.assertEqual(
            [s["id"] for s in left["sites"]],
            [s["id"] for s in right["sites"]])
        self.assertNotEqual(
            [s["line"] for s in left["sites"]],
            [s["line"] for s in right["sites"]])

    def test_recorded_baseline_and_markdown_check(self):
        self.assertEqual(audit.run(ROOT, "check"), 0)


if __name__ == "__main__":
    unittest.main()
