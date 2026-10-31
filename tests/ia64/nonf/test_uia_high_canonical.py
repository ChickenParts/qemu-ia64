#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent high-canonical VA, instruction-miss and RFI-gap guest oracles."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
MASK61 = (1 << 61) - 1
HIGH_FIRST = (1 << 61) - (1 << 50)


def load_runner(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


sequential = load_runner("ia64_high_half_ss", "run-ia64-uia-sequential-tests.py")
rfi = load_runner("ia64_high_half_rfi", "run-ia64-uia-rfi-tests.py")


def merced_implemented(va):
    """Independent truth table: [60:51] replicate within-region bit 50."""
    sign = (va >> 50) & 1
    return ((va >> 51) & 0x3ff) == (0x3ff if sign else 0)


class HighCanonicalAndFetchPriority(unittest.TestCase):
    def test_real_high_half_ss_in_each_virtual_region(self):
        mapped = [(name, p) for name, p in sequential.PROFILES.items()
                  if p.high and not p.miss]
        self.assertEqual(len(mapped), 8)
        self.assertEqual({p.vrn for _, p in mapped}, set(range(8)))
        for name, p in mapped:
            with self.subTest(name=name):
                self.assertTrue(p.ss)
                self.assertEqual(p.vec, sequential.SSV)
                self.assertEqual(p.source_va, (p.vrn << 61) | (HIGH_FIRST + 0x3ff0))
                self.assertEqual(p.next_va, (p.vrn << 61) | (HIGH_FIRST + 0x4000))
                self.assertTrue(merced_implemented(p.source_va))
                self.assertTrue(merced_implemented(p.next_va))
                self.assertEqual(p.source_va & 0x3fff, 0x3ff0)
                self.assertEqual(p.next_va & 0x3fff, 0)
                self.assertEqual(p.expected_isr & 0xf, 8)  # SS, never TB/LP
                asm = sequential.generate(name)
                self.assertEqual(asm.count("itc.i r20"), 2)
                self.assertIn(hex(p.source_va), asm)
                self.assertIn(hex(p.next_va), asm)
                self.assertIn("mov r9=cr.iipa", asm)
                self.assertIn("mov r9=cr.iip", asm)
                self.assertIn("mov r9=cr.isr", asm)
                self.assertIn("rfi", asm)

    def test_real_unmapped_high_half_faults_before_retirement(self):
        unmapped = [(name, p) for name, p in sequential.PROFILES.items()
                    if p.high and p.miss]
        self.assertEqual({p.vrn for _, p in unmapped}, {0, 3, 6, 7})
        for name, p in unmapped:
            with self.subTest(name=name):
                self.assertTrue(p.ic and p.ss)
                self.assertTrue(merced_implemented(p.source_va))
                self.assertEqual(p.vec, sequential.ITLB)
                self.assertEqual(p.expected_reg, 3)
                self.assertEqual(p.expected_isr & 0xf, 0)  # No SS on fault
                self.assertEqual(p.expected_isr & sequential.ISR_FETCH,
                                 sequential.ISR_FETCH)
                self.assertEqual(p.expected_isr >> 41,
                                 1 if p.kind == "mlx" else 2)
                asm = sequential.generate(name)
                # Only the identity ITC.I for harness/IVT. The high-VA
                # source bundle is physically present but not translated.
                self.assertEqual(asm.count("itc.i r20"), 1)
                self.assertIn("mov r9=cr.ifa", asm)
                self.assertNotIn("mov r9=cr.iipa", asm)
                self.assertIn(hex(p.source_va), asm)
                self.assertIn("instruction_tlb_vector:", asm)
                self.assertIn("rfi", asm)

    def test_both_noncanonical_gaps_generate_rfi_uia_not_itlb(self):
        profiles = [(name, p) for name, p in rfi.PROFILES.items()
                    if p.virtual]
        self.assertEqual(len(profiles), 4)
        for name, p in profiles:
            with self.subTest(name=name):
                self.assertTrue(p.bad)
                self.assertTrue(p.restored_flags & rfi.IT)
                self.assertFalse(merced_implemented(p.pa))
                self.assertIn(p.pa >> 61, {0, 1, 6, 7})
                self.assertEqual(p.expected_isr & 0xf, 0)  # RFI no SS/TB
                asm = rfi.generate(name)
                self.assertIn(hex(p.pa), asm)
                self.assertIn("mov cr.ipsr=r9", asm)
                self.assertIn("uia_vector:", asm)
                self.assertNotIn("itc.i r20", asm)
                self.assertIn("rfi", asm)
                if "low" in name:
                    self.assertEqual(p.pa & MASK61, 1 << 50)
                else:
                    self.assertEqual(p.pa & MASK61, HIGH_FIRST - 16)

    def test_real_fault_and_trap_paths_are_separate(self):
        helper = (ROOT / "target/ia64/helper.c").read_text()
        uia = (ROOT / "target/ia64/uia-address.h").read_text()
        h = helper.split("bool ia64_cpu_tlb_fill(", 1)[1].split(
            "void HELPER(check_null_branch)", 1)[0]
        self.assertLess(h.index("for (int i = 0; i < ARRAY_SIZE(env->itlb)"),
                        h.index("IA64_VEC_INST_TLB"))
        self.assertIn("return ia64_fault(cs, env, is_data, write, vec, 0, retaddr)", h)
        rfi_helper = helper.split("void HELPER(rfi)(", 1)[1].split(
            "static uint64_t ia64_dbg_next_call_pc", 1)[0]
        self.assertIn("ia64_uia_itanium_rfi_unimplemented(", rfi_helper)
        self.assertIn("ia64_uia_virtual_implemented(", uia)
        self.assertIn("source_psr & (UINT64_C(1) << 36)", uia)


if __name__ == "__main__":
    unittest.main()
