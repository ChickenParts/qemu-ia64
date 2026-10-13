#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed source contracts for IFS validity, frame marker and AR.EC."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]

class IFSStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = (ROOT / "target/ia64/helper.c").read_text()
        cls.cpu = (ROOT / "target/ia64/cpu.c").read_text()
        cls.policy = (ROOT / "target/ia64/ifs.h").read_text()

    def test_real_ifs_has_no_ec_field(self):
        self.assertIn("IA64_IFS_IFM_MASK", self.policy)
        self.assertIn("UINT64_C(1) << 38", self.policy)
        self.assertIn("UINT64_C(1) << 63", self.policy)
        self.assertNotIn("IA64_IFS_EC", self.policy)

    def test_interruption_never_packs_ar_ec_into_ifs(self):
        a = self.h.index("static bool ia64_exception(")
        b = self.h.index("static bool ia64_fault(", a)
        entry = self.h[a:b]
        self.assertIn("ia64_ifs_on_interruption(", entry)
        self.assertIn("interrupted_psr & IA64_PSR_IC", entry)
        self.assertNotIn("env->cr_ifs = env->cfm", entry)
        self.assertNotIn("env->ar[IA64_AR_EC]", entry)
        a = self.cpu.index("static bool ia64_cpu_exec_interrupt(")
        b = self.cpu.index("static TCGTBCPUState ia64_get_tb_cpu_state(", a)
        external = self.cpu[a:b]
        self.assertIn("ia64_ifs_on_interruption(", external)
        self.assertNotIn("env->cr_ifs = env->cfm", external)

    def test_rfi_restores_only_cfm_not_ar_ec(self):
        a = self.h.index("void HELPER(rfi)(")
        b = self.h.index("static uint64_t ia64_dbg_next_call_pc;", a)
        rfi = self.h[a:b]
        self.assertIn("ia64_ifs_rfi_cfm(", rfi)
        self.assertNotIn("env->ar[IA64_AR_EC] =", rfi)
        self.assertNotIn("ifs >> 52", rfi)
        self.assertNotIn("ifs & ~(1ULL << 63)", rfi)

    def test_cover_guarded_by_psr_ic(self):
        a = self.h.index("void HELPER(cover)(")
        b = self.h.index("void HELPER(set_bspstore)(", a)
        cover = self.h[a:b]
        self.assertIn("ia64_ifs_on_cover(", cover)
        self.assertIn("env->psr & IA64_PSR_IC", cover)
        self.assertNotIn("env->cr_ifs = old_cfm", cover)

    def test_policy_exercises_both_ifs_v_branches(self):
        self.assertIn("psr_ic ? ifs :", self.policy)
        self.assertIn("return (ifs & IA64_IFS_V) ?", self.policy)

if __name__ == "__main__":
    unittest.main()
