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
    if p.gap:
        return 0x5400
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
        self.assertEqual(len(runner.PROFILES), 42)
        self.assertEqual({p.vrn for p in runner.PROFILES.values()}, set(range(8)))
        self.assertEqual(
            {p.vec for p in runner.PROFILES.values()},
            {0, 0x800, 0x2000, 0x2800, 0x5000, 0x5300, 0x5400})
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
                self.assertEqual(
                    p.expected_isr,
                    (1 << (33 if p.kind == "store" else 34)) |
                    (0x30 if p.gap else 0))
                if not p.present:
                    self.assertEqual(p.pte_flags, 0)

    def test_guest_has_real_itc_d_psr_dt_fault_state_and_rfi(self):
        for name, p in runner.PROFILES.items():
            with self.subTest(name=name):
                asm = runner.generate(name)
                self.assertEqual(asm.count("itc.d r20"),
                                 int(p.mapped and not p.tr))
                self.assertEqual(asm.count("itr.d dtr[r22]=r20"),
                                 int(p.mapped and p.tr))
                self.assertEqual(asm.count("ptc.e r23"),
                                 int(p.mapped and p.tr))
                if p.tr:
                    self.assertTrue(p.mapped)
                    self.assertLess(asm.index("itr.d dtr[r22]=r20"),
                                    asm.index("ptc.e r23"))
                    self.assertLess(asm.index("rsm 0x2000"),
                                    asm.index("itr.d dtr[r22]=r20"))
                    self.assertLess(asm.index("ptc.e r23"),
                                    asm.index("srlz.d"))
                self.assertIn("srlz.d", asm)
                self.assertIn("mov cr.ipsr=r9", asm)
                self.assertIn("source_bundle:", asm)
                if p.vec:
                    if p.gap:
                        # UAD is a General Exception; the architecture
                        # does not promise CR.IFA for this vector.
                        self.assertNotIn("mov r9=cr.ifa", asm)
                    else:
                        self.assertIn("mov r9=cr.ifa", asm)
                    self.assertIn("mov r9=cr.iip", asm)
                    self.assertIn("mov r9=cr.isr", asm)
                    self.assertIn("mov r9=cr.ipsr", asm)
                    checks = asm.split("fault_checks:", 1)[1]
                    # The IVT does not implicitly disable live PSR.dt.
                    # Physical memory comparisons must follow an actual
                    # rsm DT and serialization, never precede them.
                    self.assertLess(checks.index("rsm 0x20000"),
                                    checks.index("srlz.d"))
                    self.assertLess(checks.index("srlz.d"),
                                    checks.index("ld8 r9=[r14]"))
                else:
                    # A clean result must not enter the fault handler;
                    # it has no reason to read interruption registers.
                    self.assertNotIn("mov r9=cr.ifa", asm)
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
        self.assertIn("IA64_VEC_DATA_ACCESS_RIGHTS", fill)
        self.assertIn("IA64_VEC_DATA_ACCESS_BIT", fill)
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


    def test_pinned_translation_registers_preserve_not_present(self):
        # Each pinned TR must match on its own hidden valid/RID/tag,
        # even when the PTE's architectural P bit is zero. Purged TC
        # copies cannot silently substitute for these registers.
        body = self.helper.split("bool ia64_cpu_tlb_fill(", 1)[1].split(
            "void HELPER(check_null_branch)", 1)[0]
        for which in ("itrs", "dtrs"):
            with self.subTest(which=which):
                entry = body.split(
                    f"for (int i = 0; i < ARRAY_SIZE(env->{which}); i++)", 1)[1]
                entry = entry.split("break;\n", 1)[0]
                self.assertIn(f"!env->{which}[i].valid", entry)
                self.assertIn(f"env->{which}[i].rid != rid", entry)
                self.assertNotIn(f"if (!PTE_P(env->{which}[i].pte))", entry)
                self.assertIn(f"tr_match.p  = PTE_P(env->{which}[i].pte)", entry)
                self.assertIn("match = &tr_match", entry)
                self.assertIn("hit = true", entry)

    def test_dtr_p_zero_requires_real_tr_not_cached_copy(self):
        tr_profiles = {n: p for n, p in runner.PROFILES.items() if p.tr}
        self.assertEqual(len(tr_profiles), 9)
        p_zero = {n: p for n, p in tr_profiles.items() if not p.present}
        self.assertEqual(len(p_zero), 3)
        self.assertEqual({p.vrn for p in p_zero.values()}, {5, 6, 7})
        self.assertTrue(all(p.vec == runner.NOT_PRESENT for p in p_zero.values()))
        for name, p in tr_profiles.items():
            with self.subTest(name=name):
                asm = runner.generate(name)
                self.assertEqual(asm.count("itc.d r20"), 0)
                self.assertEqual(asm.count("itr.d dtr[r22]=r20"), 1)
                self.assertEqual(asm.count("ptc.e r23"), 1)
                self.assertIn("mov cr.ifa=r14", asm)
                self.assertIn("mov cr.itir=r14", asm)
                self.assertLess(asm.index("itr.d dtr[r22]=r20"),
                                asm.index("ptc.e r23"))
                self.assertIn(hex(p.va), asm)
        # ptc.e must retain pinned TRs while removing TC entries.
        start = self.helper.index("void HELPER(ptc_e)(")
        end = self.helper.index("void HELPER(ptc_g)(", start)
        helper = self.helper[start:end]
        self.assertIn("env->dtlb[i].valid = 0", helper)
        self.assertIn("env->itlb[i].valid = 0", helper)
        self.assertNotIn("env->dtrs[i].valid = 0", helper)
        self.assertNotIn("env->itrs[i].valid = 0", helper)


if __name__ == "__main__":
    unittest.main()
