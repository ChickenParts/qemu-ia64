# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent architectural register-fault controls for production F9/F10.

Masks and codes below are literal Intel Vol. 2 PSR/ISR definitions, not copied
from cpu.h. Native hooks establish ordering/non-commit, not IVT delivery; see
scripts/run-ia64-f9-exception-tests.py for the actual system-mode gate.
"""
import ctypes
import itertools
from pathlib import Path
import tempfile
import unittest

from test_f9 import OPS, NAT, PATTERNS, ROOT, State, compile_harness, oracle, word
import test_f10

DFL, DFH, AC = 0x40000, 0x80000, 0x8


class RegisterFaultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='ia64-register-faults-')
        directory = Path(cls.tmp.name)
        cls.f9 = compile_harness(ROOT, directory)
        cls.f10 = test_f10.compile_harness(ROOT, directory)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_f9_banks_constants_aliases_rotation_and_nat_priority(self):
        # Source-only, destination-only, both banks, f0/f1 exemption, aliases,
        # and both sides of the f31/f32 architectural bank boundary.
        profiles = [(6, 7, 8), (32, 33, 34), (6, 32, 33), (32, 6, 7),
                    (32, 0, 1), (6, 0, 1), (31, 0, 32), (32, 31, 1),
                    (127, 0, 31), (31, 1, 127), (32, 32, 32),
                    (31, 31, 32), (127, 32, 127), (32, 0, 127)]
        for name, regs, mask, rotation, nat in itertools.product(
                OPS, profiles, (0, DFL, DFH, DFL | DFH, AC),
                (0, 1, 31, 63, 95), (False, True)):
            with self.subTest(name=name, regs=regs, mask=mask,
                              rotation=rotation, nat=nat):
                state = State()
                state.cfm = rotation << 25
                state.psr = mask | 0x2004
                state.f[1][:] = (1 << 63, 0xffff)
                physical = lambda f: f if f < 32 else 32 + (f - 32 + rotation) % 96
                for f in set(regs) - {0, 1}:
                    state.f[physical(f)][:] = NAT if nat else PATTERNS[f % 2]
                before = bytes(state)
                f1, f2, f3 = regs
                a, b = tuple(state.f[physical(f2)]), tuple(state.f[physical(f3)])
                code = int(bool(mask & DFL and any(2 <= f <= 31 for f in regs)))
                code |= 2 * int(bool(mask & DFH and any(f >= 32 for f in regs)))
                vector = self.f9.run(ctypes.byref(state), word(name, *regs))
                self.assertEqual(vector, 0x5500 if code else 0)
                if code:
                    self.assertEqual(bytes(state), before)
                    self.assertEqual(self.f9.last_code(), code)
                    self.assertEqual(self.f9.last_isr_extra(), 0)
                    self.assertFalse(self.f9.last_trap())
                else:
                    self.assertEqual(tuple(state.f[physical(f1)]), oracle(name, a, b))
                    self.assertEqual(state.psr, mask | 0x2004 | (16 if f1 < 32 else 32))

    def test_f9_illegal_target_precedes_disabled_and_nat(self):
        for name, destination, mask in itertools.product(OPS, (0, 1), (0, DFL | DFH)):
            state = State()
            state.f[1][:] = (1 << 63, 0xffff)
            state.f[31][:] = state.f[32][:] = NAT
            state.psr = mask
            before = bytes(state)
            self.assertEqual(self.f9.run(ctypes.byref(state),
                                        word(name, destination, 31, 32)), 0x5400)
            self.assertEqual(bytes(state), before)
            self.assertEqual(self.f9.last_code(), 0)
            self.assertEqual(self.f9.last_isr_extra(), 0)
            self.assertFalse(self.f9.last_trap())

    def test_f10_architectural_masks_and_access_bits(self):
        for op, sf, regs, mask in itertools.product(
                range(8), range(4), ((6, 7), (32, 7), (6, 32),
                                    (32, 127), (32, 0), (6, 1)),
                (0, DFL, DFH, DFL | DFH, AC)):
            state = test_f10.State()
            state.f[1][:] = (1 << 63, 0xffff)
            state.f[7][:] = state.f[32][:] = state.f[127][:] = (1 << 63, 0xffff)
            state.psr, state.ar[40] = mask, 63
            before = bytes(state)
            code = int(bool(mask & DFL and any(2 <= f <= 31 for f in regs)))
            code |= 2 * int(bool(mask & DFH and any(f >= 32 for f in regs)))
            vector = self.f10.run(ctypes.byref(state), test_f10.word(op, sf, *regs))
            self.assertEqual(vector, 0x5500 if code else 0)
            if code:
                self.assertEqual(bytes(state), before)
                self.assertEqual(self.f10.last_code(), code)
                self.assertEqual(self.f10.last_isr_extra(), 0)
                self.assertFalse(self.f10.last_trap())

    def test_f10_illegal_target_precedes_disabled_and_nat(self):
        for op, sf, destination in itertools.product(range(8), range(4), (0, 1)):
            state = test_f10.State()
            state.f[32][:] = NAT
            state.psr, state.ar[40] = DFL | DFH, 0
            before = bytes(state)
            self.assertEqual(self.f10.run(ctypes.byref(state),
                                         test_f10.word(op, sf, destination, 32)), 0x5400)
            self.assertEqual(bytes(state), before)
            self.assertEqual(self.f10.last_code(), 0)
            self.assertEqual(self.f10.last_isr_extra(), 0)


if __name__ == '__main__':
    unittest.main()
