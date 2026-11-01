#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent IA-64 data-fault truth table and fail-closed QEMU source guards."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "ia64_dtlb_fault_guests", ROOT / "scripts/run-ia64-dtlb-fault-priority-tests.py")
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)


def independent_vector(p):
    # Independent literal oracle for the AR={1,2}, CPL=0 test subset.
    # Intel SDM Vol2 §4.1.8: present > permissions > access > dirty.
    if not p.mapped:
        return 0x800
    if not p.present:
        return 0x5000
    if p.kind == "store" and p.ar == 1:
        return 0x5300
    if not p.accessed:
        return 0x2800
    if p.kind == "store" and not p.dirty:
        return 0x2000
    return 0


class DTLBPrioritySource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()
        cls.script = (ROOT / "scripts/run-ia64-dtlb-fault-priority-tests.py").read_text()

    def test_independent_fault_vectors_and_qualifiers(self):
        self.assertEqual(len(runner.PROFILES), 15)
        self.assertEqual({p.vrn for p in runner.PROFILES.values()}, set(range(8)))
        self.assertEqual(
            {p.vec for p in runner.PROFILES.values()},
            {0, 0x800, 0x2000, 0x2800, 0x5000, 0x5300})
        for name, p in runner.PROFILES.items():
            with self.subTest(name=name):
                self.assertIn(p.kind, {"load", "store"})
                self.assertIn(p.ar, {1, 2})
                self.assertEqual(p.vec, independent_vector(p))
                self.assertEqual(p.va >> 61, p.vrn)
                self.assertEqual(p.va & 0x3fff, 0)
                self.assertEqual(p.expected_reg, runner.INIT_DATA if
                                 p.kind == "load" and not p.vec else runner.INIT_REG)
                self.assertEqual(p.expected_memory, runner.STORE_DATA if
                                 p.kind == "store" and not p.vec else runner.INIT_DATA)
                self.assertEqual(p.expected_isr, 1 << (33 if p.kind == "store" else 34))
                if not p.present:
                    self.assertEqual(p.pte_flags, 0)

    def test_guest_has_real_itc_d_psr_dt_fault_state_and_rfi(self):
        for name, p in runner.PROFILES.items():
            with self.subTest(name=name):
                asm = runner.generate(name)
                self.assertEqual(asm.count("itc.d r20"), int(p.mapped))
                self.assertIn("srlz.d", asm)
                self.assertIn("mov cr.ipsr=r9", asm)
                self.assertIn("source_bundle:", asm)
                self.assertIn("mov r9=cr.ifa", asm)
                self.assertIn("mov r9=cr.iip", asm)
                self.assertIn("mov r9=cr.isr", asm)
                self.assertIn("mov r9=cr.ipsr", asm)
                self.assertIn("mov cr.iip=r14", asm)
                self.assertIn("rfi", asm)
                self.assertIn("data_page:", asm)
                self.assertIn(hex(p.va), asm)
                self.assertIn("st8 [r20]=r21" if p.kind == "store"
                              else "ld8 r11=[r20]", asm)
                self.assertIn("break.m 0", asm)
                self.assertIn("fail_spin:", asm)
                self.assertIn("pass_spin:", asm)

    def test_production_priority_is_present_rights_access_dirty(self):
        h = self.helper
        start = h.index("bool ia64_cpu_tlb_fill(")
        end = h.index("void HELPER(check_null_branch)", start)
        fill = h[start:end]
        segments = ("if (match && !match->p)",
                    "!ia64_access_rights(match->ar, match->pl, cpl, access_type)",
                    "if (match && !match->a)", "write && match && !match->d")
        positions = [fill.index(needle) for needle in segments]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("vec = IA64_VEC_DATA_PAGE_NOT_P", fill)
        self.assertIn("vec = IA64_VEC_DATA_ACCESS_RIGHTS", fill)
        self.assertIn("vec = IA64_VEC_DATA_ACCESS_BIT", fill)
        self.assertIn("vec = IA64_VEC_DATA_DIRTY", fill)
        self.assertLess(fill.index("for (int i = 0; i < ARRAY_SIZE(env->dtlb)"),
                        fill.index("Linux uses region 6 as an identity-mapped"))
        self.assertLess(fill.index("Linux uses region 7 addresses (__va())"),
                        fill.index("IA64_VEC_DATA_TLB"))

    def test_p_zero_is_an_inserted_cache_entry_not_tlb_miss(self):
        h = self.helper
        start = h.index("static void ia64_insert_tlb(", h.index("bool ia64_cpu_tlb_fill("))
        end = h.index("void HELPER(itc_d)", start)
        insert = h[start:end]
        self.assertNotIn("if (!p)", insert)
        self.assertIn("env->dtlb[idx].p = p", insert)
        self.assertIn("env->dtlb[idx].valid = 1", insert)
        self.assertIn("env->itlb[idx].p = p", insert)
        self.assertIn("env->itlb[idx].valid = 1", insert)


if __name__ == "__main__":
    unittest.main()
