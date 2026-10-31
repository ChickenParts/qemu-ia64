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
            if not profile.near:
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


if __name__ == "__main__":
    unittest.main()
