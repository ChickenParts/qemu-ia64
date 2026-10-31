#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent host checks of assembled-real-guest virtual-region coverage."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "ia64_uia_sequential_vrn_guests",
    ROOT / "scripts/run-ia64-uia-sequential-tests.py")
guests = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = guests
SPEC.loader.exec_module(guests)


class MercedSequentialVirtualRegionGuests(unittest.TestCase):
    # Intel Merced: bit[63:61] virtual region, VA[50] sign bit, with
    # VA[60:51] sign extension. The *low-half* final legal bundle in
    # each VRN is followed by an unimplemented sequential address.
    def test_all_eight_regions_have_real_edge_profiles(self):
        by_region = {}
        for name, profile in guests.PROFILES.items():
            if not profile.near and not profile.high:
                by_region.setdefault(profile.vrn, []).append(name)
        self.assertEqual(set(by_region), set(range(8)))
        for vrn in range(8):
            for name in by_region[vrn]:
                with self.subTest(case=name):
                    p = guests.PROFILES[name]
                    self.assertEqual(p.source_va, (vrn << 61) | ((1 << 50) - 16))
                    self.assertEqual(p.next_va, (vrn << 61) | (1 << 50))
                    self.assertEqual(p.vec, guests.UIA)
                    asm = guests.generate(name)
                    # A full 64-bit CR.IFA, actual ITC.I and native RFI
                    # are generated in every guest. Runtime checks below
                    # verify the *actual* IIPA/IIP/ISR and committed r10.
                    self.assertIn(hex(p.source_va), asm)
                    self.assertIn(hex(p.next_va), asm)
                    self.assertIn("itc.i r20", asm)
                    self.assertIn("mov cr.ipsr=r9", asm)
                    self.assertIn("mov r9=cr.iipa", asm)
                    self.assertIn("mov r9=cr.iip", asm)
                    self.assertIn("mov r9=cr.isr", asm)
                    self.assertIn("rfi", asm)

    def test_high_region_near_controls_are_ss_not_uia(self):
        for vrn in (6, 7):
            case = f"near-vrn{vrn}-adds-ss"
            with self.subTest(case=case):
                p = guests.PROFILES[case]
                self.assertTrue(p.near and p.ss)
                self.assertEqual(p.source_va, (vrn << 61) | ((1 << 50) - 16 - 0x4000))
                self.assertEqual(p.next_va, (vrn << 61) | ((1 << 50) - 0x4000))
                self.assertEqual(p.vec, guests.SSV)
                self.assertEqual(p.expected_isr & 0xf, 8)

    def test_ic_and_tb_do_not_invent_sequential_trap_causes(self):
        for case in ("edge-vrn3-adds-tb", "edge-vrn4-adds-tb-ss",
                     "edge-vrn5-adds-ic-ss",
                     "edge-vrn6-nop-b-tb-ss"):
            with self.subTest(case=case):
                p = guests.PROFILES[case]
                self.assertEqual(p.vec, guests.UIA)
                self.assertEqual(p.expected_isr & 0xf, 8 if p.ss else 0)
                self.assertEqual(bool(p.expected_isr & guests.NI), not p.ic)


    def test_explicit_itlb_tr_precedes_region_6_7_compat_mapping(self):
        # A regression in ia64_cpu_tlb_fill used to short-circuit
        # low-half region 6/7 before looking up a real ITC.I mapping.
        # That made these guests loop in the 0x400 ITLB vector.
        source = (ROOT / "target/ia64/helper.c").read_text()
        body = source.split("bool ia64_cpu_tlb_fill(", 1)[1].split(
            "void HELPER(check_null_branch)", 1)[0]
        itr = body.index("for (int i = 0; i < ARRAY_SIZE(env->itrs); i++)")
        itlb = body.index("for (int i = 0; i < ARRAY_SIZE(env->itlb); i++)")
        dtr = body.index("for (int i = 0; i < ARRAY_SIZE(env->dtrs); i++)")
        dtlb = body.index("for (int i = 0; i < ARRAY_SIZE(env->dtlb); i++)")
        miss = body.index("if (!hit) {", itlb)
        direct7 = body.index("Linux uses region 7 addresses (__va())")
        direct6 = body.index("Linux uses region 6 as an identity-mapped")
        fault = body.index("IA64_VEC_INST_TLB", direct6)
        self.assertLess(itr, itlb)
        self.assertLess(dtr, dtlb)
        self.assertLess(itlb, miss)
        self.assertLess(dtlb, miss)
        self.assertLess(miss, direct7)
        self.assertLess(direct7, direct6)
        self.assertLess(direct6, fault)


if __name__ == "__main__":
    unittest.main()
