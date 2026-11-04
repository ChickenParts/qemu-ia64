#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent pinned ITR.I instruction-fetch P/AR/A/fault-priority oracle."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "ia64_itr_guest_matrix", ROOT / "scripts/run-ia64-itr-not-present-tests.py")
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)

SOURCE = ROOT / "target/ia64/helper.c"


def independent_vector(p):
    if p.mode == "miss":
        return 0x0400
    if p.mode == "not-present":
        return 0x5000
    if p.mode == "rights":
        return 0x5200
    if p.mode == "access-bit":
        return 0x2400
    return 0x6000  # SS after successful source instruction


class PinnedITRRealGuests(unittest.TestCase):
    def test_independent_region_and_priority_matrix(self):
        cases = runner.PROFILES
        self.assertEqual(len(cases), 12)
        self.assertEqual({p.vrn for p in cases.values()}, set(range(8)))
        self.assertEqual({p.vec for p in cases.values()},
                         {0x0400, 0x2400, 0x5000, 0x5200, 0x6000})
        p_zero = [p for p in cases.values() if p.mode == "not-present"]
        self.assertEqual({p.vrn for p in p_zero}, {2, 5, 6, 7})
        for name, p in cases.items():
            with self.subTest(case=name):
                self.assertEqual(p.source_va, (p.vrn << 61) |
                                 ((1 << 61) - (1 << 50) + 0x3ff0))
                self.assertEqual(p.source_va & 0x3fff, 0x3ff0)
                self.assertEqual(p.next_va, p.source_va + 16)
                self.assertEqual(p.vec, independent_vector(p))
                self.assertEqual(p.start_ri, 1 if p.kind == "mlx" else 2)
                self.assertEqual(p.expected_reg, 4 if p.mode == "ok" else 3)
                if p.mode != "miss":
                    self.assertEqual(p.pte_flags & 1,
                                     int(p.mode != "not-present"))
                self.assertEqual(p.expected_isr & 0xf,
                                 8 if p.mode == "ok" else 0)
                self.assertEqual(p.expected_isr >> 41,
                                 2 if p.mode == "ok" else p.start_ri)
                if p.mode != "ok":
                    self.assertEqual(p.expected_isr & runner.FETCH_ISR,
                                     runner.FETCH_ISR)
                if p.mode == "not-present":
                    self.assertEqual(p.pte_flags, 0)
                    self.assertEqual(p.vec, runner.PAGE_NOT_PRESENT)

    def test_real_itr_then_purge_then_ivt_identity_mapping(self):
        for name, p in runner.PROFILES.items():
            with self.subTest(case=name):
                asm = runner.generate(name)
                self.assertIn("mov cr.ipsr=r9", asm)
                self.assertIn("mov cr.iip=r14", asm)
                self.assertIn("rfi", asm)
                self.assertIn(hex(p.source_va), asm)
                self.assertIn(hex(p.next_va if p.mode == "ok" else p.source_va), asm)
                self.assertEqual(asm.count("itc.i r20"), 1)
                self.assertEqual(asm.count("itr.i itr[r22]=r20"),
                                 int(p.mode != "miss"))
                self.assertEqual(asm.count("ptc.e r23"),
                                 int(p.mode != "miss"))
                if p.mode != "miss":
                    self.assertLess(asm.index("mov cr.ifa=r14"),
                                    asm.index("itr.i itr[r22]=r20"))
                    self.assertLess(asm.index("mov cr.itir=r14"),
                                    asm.index("itr.i itr[r22]=r20"))
                    self.assertLess(asm.index("rsm 0x2000"),
                                    asm.index("itr.i itr[r22]=r20"))
                    self.assertLess(asm.index("itr.i itr[r22]=r20"),
                                    asm.index("ptc.e r23"))
                    self.assertLess(asm.index("ptc.e r23"),
                                    asm.index("itc.i r20"))
                self.assertIn("mov r9=cr.isr", asm)
                self.assertIn("mov r9=cr.iip", asm)
                self.assertIn("mov r9=cr.ipsr", asm)
                self.assertIn("mov r9=cr.ifa" if p.mode != "ok" else
                              "mov r9=cr.iipa", asm)
                self.assertIn("checks:", asm)
                self.assertIn("fail_spin:", asm)
                self.assertIn("pass_spin:", asm)

    def test_itr_validity_is_not_page_present(self):
        helper = SOURCE.read_text()
        body = helper.split("bool ia64_cpu_tlb_fill(", 1)[1].split(
            "void HELPER(check_null_branch)", 1)[0]
        lookup = body.split(
            "for (int i = 0; i < ARRAY_SIZE(env->itrs); i++)", 1)[1]
        lookup = lookup.split(
            "for (int i = 0; i < ARRAY_SIZE(env->dtrs); i++)", 1)[0]
        self.assertIn("!env->itrs[i].valid", lookup)
        self.assertIn("env->itrs[i].rid != rid", lookup)
        self.assertNotIn("if (!PTE_P(env->itrs[i].pte))", lookup)
        self.assertIn("tr_match.p  = PTE_P(env->itrs[i].pte)", lookup)
        self.assertIn("match = &tr_match", lookup)
        self.assertIn("hit = true", lookup)
        self.assertLess(body.index("for (int i = 0; i < ARRAY_SIZE(env->itrs)"),
                        body.index("for (int i = 0; i < ARRAY_SIZE(env->itlb)"))
        self.assertLess(body.index("if (match && !match->p)"),
                        body.index("!ia64_access_rights(match->ar"))
        self.assertIn("IA64_VEC_INST_ACCESS_RIGHTS", body)
        self.assertIn("IA64_VEC_INST_ACCESS_BIT", body)
        self.assertIn("IA64_VEC_DATA_PAGE_NOT_P", body)

    def test_ptc_e_purges_only_caches_not_itr(self):
        helper = SOURCE.read_text()
        s = helper.split("void HELPER(ptc_e)(", 1)[1].split(
            "void HELPER(ptc_g)(", 1)[0]
        self.assertIn("env->itlb[i].valid = 0", s)
        self.assertIn("env->itcs[i].valid = 0", s)
        self.assertNotIn("env->itrs[i].valid = 0", s)
        self.assertNotIn("env->dtrs[i].valid = 0", s)


if __name__ == "__main__":
    unittest.main()
