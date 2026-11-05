#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent Merced noncanonical data-VA/UAD real-IVT guest oracles.

Intel Itanium SDM rev2.3 Vol2 §4.3.3 / §5.3 / §8:
UAD is a pre-result General Exception with ISR.code[7:4]=3.
CR.IFA is architecturally unspecified; do not use it as an oracle.
"""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "ia64_uad_guests", ROOT / "scripts/run-ia64-dtlb-fault-priority-tests.py")
guests = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = guests
SPEC.loader.exec_module(guests)


def implemented(va: int) -> bool:
    """Independent Merced truth table for VRN-independent sign extension."""
    msb = (va >> 50) & 1
    bits = (va >> 51) & 0x3ff
    return bits == (0x3ff if msb else 0)


class UnimplementedDataVirtualAddress(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = (ROOT / "target/ia64/helper.c").read_text()

    def test_both_noncanonical_gaps_in_all_eight_regions(self):
        negatives = {name: p for name, p in guests.PROFILES.items() if p.gap}
        self.assertEqual(len(negatives), 16)
        self.assertEqual({p.vrn for p in negatives.values()}, set(range(8)))
        for vrn in range(8):
            matches = {p.gap: p for p in negatives.values() if p.vrn == vrn}
            self.assertEqual(set(matches), {"low", "high"})
            self.assertEqual(matches["low"].va,
                             (vrn << 61) | (1 << 50))
            self.assertEqual(matches["high"].va,
                             (vrn << 61) | (((1 << 61) - (1 << 50)) - 0x4000))
        for name, p in negatives.items():
            with self.subTest(case=name):
                self.assertFalse(implemented(p.va))
                self.assertFalse(p.mapped)
                self.assertFalse(p.tr)
                self.assertEqual(p.vec, guests.GENERAL)
                self.assertEqual(p.expected_isr & 0xff, guests.UAD_CODE)
                self.assertEqual(p.expected_isr >> 41, 0)
                self.assertEqual(p.expected_reg, guests.INIT_REG)
                self.assertEqual(p.expected_memory, guests.INIT_DATA)
                self.assertEqual(p.va & 0x3fff, 0)
                self.assertEqual(p.expected_isr & (
                    guests.READ_ISR | guests.WRITE_ISR),
                    guests.WRITE_ISR if p.kind == "store" else guests.READ_ISR)

    def test_high_canonical_positive_controls_remain_valid(self):
        for name, vrn, kind in (
            ("mapped-r6-high-load", 6, "load"),
            ("mapped-r7-high-store", 7, "store"),
        ):
            with self.subTest(case=name):
                p = guests.PROFILES[name]
                self.assertTrue(p.high and p.mapped and not p.gap)
                self.assertEqual(p.va, (vrn << 61) | (
                    (1 << 61) - (1 << 50) + 0x8000))
                self.assertTrue(implemented(p.va))
                self.assertEqual(p.vec, 0)
                self.assertEqual(p.kind, kind)
                self.assertEqual(p.expected_isr & 0xff, 0)

    def test_real_fault_handlers_verify_only_defined_state(self):
        for name, p in guests.PROFILES.items():
            if not p.gap:
                continue
            with self.subTest(case=name):
                asm = guests.generate(name)
                self.assertIn("general_vector:", asm)
                self.assertIn("br.cond.sptk fault_checks", asm)
                self.assertIn("mov r9=cr.isr", asm)
                self.assertIn("mov r9=cr.iip", asm)
                self.assertIn("mov r9=cr.ipsr", asm)
                self.assertNotIn("mov r9=cr.ifa", asm)
                self.assertIn(hex(p.expected_isr), asm)
                self.assertIn(hex(p.va), asm)
                self.assertIn("rsm 0x20000", asm)
                self.assertIn("srlz.d", asm)
                self.assertIn("rfi", asm)
                self.assertIn("ld8 r9=[r14]", asm)  # unchanged backing data
                self.assertEqual(asm.count("itc.d r20"), 0)
                self.assertEqual(asm.count("itr.d dtr[r22]=r20"), 0)

    def test_uad_fires_before_firmware_and_region_shortcuts(self):
        body = self.helper.split("bool ia64_cpu_tlb_fill(", 1)[1].split(
            "void HELPER(check_null_branch)", 1)[0]
        start = body.index("if (!is_fetch && (env->psr & IA64_PSR_DT) &&")
        path = body[start:body.index("#ifndef CONFIG_USER_ONLY", start)]
        for needle in ("!ia64_uia_virtual_implemented((uint64_t)address)",
                       "access_type == MMU_DATA_STORE",
                       "IA64_VEC_GENERAL_EXCEPTION",
                       "0, 0x30, 0", "true, false, retaddr"):
            self.assertIn(needle, path)
        self.assertLess(start, body.index("ia64_fw_try_patch_efi_hobs"))
        self.assertLess(start, body.index("Canonical per-cpu region mapping"))
        self.assertLess(start, body.index("Physical mode: if translation"))
        self.assertLess(start, body.index("Linux uses region 7 addresses (__va())"))
        self.assertLess(start, body.index("Linux uses region 6 as an identity-mapped"))
        self.assertLess(start, body.index("IA64_VEC_DATA_TLB"))
        self.assertNotIn("env->cr_ifa =", path)
        # This tranche does not pretend to implement speculative loads,
        # non-faulting probes, or mandatory RSE address exceptions.


if __name__ == "__main__":
    unittest.main()
