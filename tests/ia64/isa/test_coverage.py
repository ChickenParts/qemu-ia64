# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit-harness tests, including deliberate mutations of production C.

Known-gap assertions reproduce observations, NOT desired architectural behavior.
They must be updated together with the baseline when an implementation is fixed.
"""
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    'isa_coverage', ROOT / 'scripts/ia64-isa-coverage.py')
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class CoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = AUDIT.load_registry(ROOT)
        cls.vectors = AUDIT.corpus(cls.data)
        cls.temporary = tempfile.TemporaryDirectory(prefix='isa-tests-')
        cls.work = Path(cls.temporary.name)
        cls.executable, cls.source_hash = AUDIT.project(
            ROOT, cls.work, os.environ.get('CC', 'cc'))
        cls.routes = dict(zip(
            (v['id'] for v in cls.vectors),
            AUDIT.probe(cls.executable, [v['word'] for v in cls.vectors])))
        cls.measurement = AUDIT.measure(
            cls.data, cls.vectors, cls.executable, cls.source_hash)
        cls.report = AUDIT.render(
            cls.data, cls.vectors, cls.executable, cls.measurement)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def row(self, name):
        return next(r for r in self.data['forms'] if r['mnemonic'] == name)

    def modified_root(self):
        temporary = tempfile.TemporaryDirectory(prefix='isa-mutation-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        for relative in (AUDIT.REGISTRY, AUDIT.BASELINE, AUDIT.REPORT,
                         Path('target/ia64/translate.c'), Path('target/ia64/fp-bitops.h'),
                         Path('target/ia64/fp-convert.h'), Path('target/ia64/fp-f8.h'),
                         Path('target/ia64/fp-approx.h'),
                         Path('target/ia64/fp-compare.h')):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        return root

    def assert_registry_rejected(self, mutate):
        root = self.modified_root()
        data = copy.deepcopy(self.data)
        mutate(data)
        (root / AUDIT.REGISTRY).write_text(json.dumps(data))
        with self.assertRaises((ValueError, KeyError)):
            AUDIT.load_registry(root)

    def test_inventory_size_and_scope(self):
        self.assertEqual(len(self.data['forms']), 75)
        self.assertEqual(len(self.vectors), 2097)
        self.assertEqual(self.measurement['forms'], 233)
        self.assertEqual(self.data['units']['A'], 'not-audited')
        self.assertEqual(self.data['units']['F'], 'inventoried')

    def test_vector_identity_is_unique(self):
        self.assertEqual(len({v['id'] for v in self.vectors}), 2097)

    def test_all_words_fit_41_bits(self):
        self.assertTrue(all(0 <= v['word'] < (1 << 41) for v in self.vectors))

    def test_fixed_fields_survive_operand_expansion(self):
        for v in self.vectors:
            for start, width, value in self.row(v['family'])['fixed']:
                self.assertEqual((v['word'] >> start) & ((1 << width)-1), value)

    def test_exact_firmware_fcvt_vector(self):
        v = AUDIT.vector(self.row('fcvt.xf'), 0, False, 'alias')
        self.assertEqual(v['word'], 0x000e000e1c0)
        self.assertIn('fcvt.xf f7=f7', v['assembly'])
        self.assertEqual(self.routes[v['id']], 'tcg')

    def test_fclass_split_immediate(self):
        # Intel Table 4-74: fclass9 = (fclass7c << 2) | fc2.
        for profile, expected in [('alias', 1), ('high', 0x100),
                                  ('high-multiply', 0x80), ('low', 0x1ff)]:
            v = AUDIT.vector(self.row('fclass.m'), 0, False, profile)
            reconstructed = (((v['word'] >> 20) & 127) << 2) | ((v['word'] >> 33) & 3)
            self.assertEqual(reconstructed, expected)

    def test_compose_rejects_overlap(self):
        with self.assertRaises(ValueError):
            AUDIT.compose([[27, 6, 1], [31, 2, 0]])

    def test_compose_rejects_overflow(self):
        for fields in ([[40, 2, 0]], [[0, 6, 64]], [[0, 6, -1]]):
            with self.assertRaises(ValueError):
                AUDIT.compose(fields)

    def test_unknown_profile_rejected(self):
        with self.assertRaises(ValueError):
            AUDIT.vector(self.row('fcvt.xf'), 0, False, 'typo')

    def test_duplicate_family_rejected(self):
        self.assert_registry_rejected(lambda d: d['forms'].append(d['forms'][0]))

    def test_missing_format_rejected(self):
        self.assert_registry_rejected(
            lambda d: d.update(forms=[r for r in d['forms'] if r['format'] != 'F11']))

    def test_unclassified_semantics_rejected(self):
        self.assert_registry_rejected(lambda d: d['forms'][0].update(semantics='unknown'))

    def test_nonboolean_variant_rejected(self):
        self.assert_registry_rejected(lambda d: d['forms'][0].update(status_fields='yes'))

    def test_unknown_schema_rejected(self):
        self.assert_registry_rejected(lambda d: d.update(schema_version=99))

    def test_reference_revision_change_requires_review(self):
        self.assert_registry_rejected(lambda d: d['reference'].update(revision='9.9'))

    def test_duplicate_json5_key_rejected(self):
        root = self.modified_root()
        with (root / AUDIT.REGISTRY).open('w') as stream:
            stream.write('{schema_version:1, schema_version:2}')
        with self.assertRaises(ValueError):
            AUDIT.load_registry(root)

    def test_recorded_snapshot_matches(self):
        AUDIT.check_snapshot(ROOT, self.measurement, self.report)

    def test_stale_documentation_fails_closed(self):
        root = self.modified_root()
        with (root / AUDIT.REPORT).open('a') as stream:
            stream.write('stale manual edit\n')
        with self.assertRaisesRegex(ValueError, 'audit drift'):
            AUDIT.check_snapshot(root, self.measurement, self.report)

    def test_same_counts_different_routes_fail(self):
        changed = dict(self.measurement, observations_sha256='0' * 64)
        with self.assertRaisesRegex(ValueError, 'audit drift'):
            AUDIT.check_snapshot(ROOT, changed, self.report)

    def test_unassigned_is_not_a_reserved_conformance_claim(self):
        self.assertEqual(len(AUDIT.unassigned_vectors()), 21)
        self.assertIn('architectural dispositions are unadjudicated', self.report)
        self.assertNotIn('reserved_vectors', self.measurement)

    def test_all_f67_forms_reach_approximation_helper(self):
        selected = [v for v in self.vectors if v['format'] in ('F6', 'F7')]
        self.assertEqual(len(selected), 4 * 4 * len(AUDIT.PROFILES))
        for v in selected:
            self.assertEqual(self.routes[v['id']], 'f67', v['id'])

    def test_scalar_f1_decode_is_operand_independent(self):
        families = {
            'fma': 'fma_s1',
            'fma.s': 'fma_s1',
            'fma.d': 'fma_s1',
            'fms': 'fms_s1',
            'fms.s': 'fms_s1',
            'fms.d': 'fms_s1',
            'fnma': 'fnma_s1',
            'fnma.s': 'fnma_s1',
            'fnma.d': 'fnma_s1',
        }
        for family, helper in families.items():
            for sf in range(4):
                for profile in AUDIT.PROFILES:
                    route = self.routes[f'{family}.s{sf}/{profile}']
                    if family in ('fma', 'fma.s', 'fma.d') and profile == 'unit-multiply':
                        self.assertEqual(route, 'tcg')
                    else:
                        self.assertEqual(route, helper)

    def test_parallel_f1_stays_explicitly_unimplemented(self):
        for family in ('fpma', 'fpms', 'fpnma'):
            for sf in range(4):
                for profile in AUDIT.PROFILES:
                    self.assertEqual(
                        self.routes[f'{family}.s{sf}/{profile}'],
                        'unimplemented')

    def test_negative_multiply_is_not_normalization(self):
        self.assertEqual(self.routes['fnma.s0/unit-multiply'], 'fnma_s1')
        self.assertEqual(self.routes['fnma.s.s0/unit-multiply'], 'fnma_s1')
        self.assertEqual(self.routes['fms.s0/unit-multiply'], 'fms_s1')

    def test_all_f8_forms_reach_f8_helper(self):
        selected = [v for v in self.vectors if v['format'] == 'F8']
        self.assertEqual(len(selected), 576)
        for v in selected:
            self.assertEqual(self.routes[v['id']], 'f8', v['id'])

    def test_all_f4_forms_reach_fcmp_helper(self):
        selected = [v for v in self.vectors if v['format'] == 'F4']
        self.assertEqual(len(selected), 288)
        for v in selected:
            self.assertEqual(self.routes[v['id']], 'fcmp', v['id'])

    def test_all_f9_forms_reach_the_bit_operation_helper(self):
        vectors = [v for v in self.vectors if v['format'] == 'F9']
        self.assertEqual(len(vectors), 19 * len(AUDIT.PROFILES))
        for v in vectors:
            self.assertEqual(self.routes[v['id']], 'f9', v['id'])

    def test_all_f10_forms_reach_conversion_helper(self):
        selected = [v for v in self.vectors if v['format'] == 'F10']
        self.assertEqual(len(selected), 288)
        for v in selected:
            self.assertEqual(self.routes[v['id']], 'f10', v['id'])

    def test_break_f_zero_and_nonzero_reach_architectural_path(self):
        # Word zero is the valid encoding of break.f 0, not an empty F slot.
        self.assertEqual(AUDIT.probe(self.executable, [0]), ['tcg'])
        for profile in AUDIT.PROFILES:
            self.assertEqual(self.routes[f'break.f/{profile}'], 'tcg')

    def test_production_decoder_mutation_is_detected(self):
        root = self.modified_root()
        path = root / 'target/ia64/translate.c'
        source = path.read_text()
        old = 'if (x == 0 && sf == 0 && x6 == 0x1c && f3 == 0)'
        self.assertEqual(source.count(old), 1)
        path.write_text(source.replace(old, old.replace('0x1c', '0x1d')))
        executable, source_hash = AUDIT.project(root, root, os.environ.get('CC', 'cc'))
        self.assertEqual(AUDIT.probe(executable, [0x000e000e1c0]), ['unimplemented'])
        changed = AUDIT.measure(self.data, self.vectors, executable, source_hash)
        self.assertNotEqual(changed['observations_sha256'], self.measurement['observations_sha256'])
        with self.assertRaisesRegex(ValueError, 'audit drift'):
            AUDIT.check_snapshot(root, changed, self.report)

    def test_projection_boundaries_fail_closed(self):
        root = self.modified_root()
        path = root / 'target/ia64/translate.c'
        path.write_text(path.read_text().replace('    case SLOT_F:\n', '    case SLOT_OTHER:\n'))
        with self.assertRaises(ValueError):
            AUDIT.project(root, root, os.environ.get('CC', 'cc'))

    def test_new_emitter_requires_explicit_review(self):
        root = self.modified_root()
        path = root / 'target/ia64/translate.c'
        source = path.read_text().replace('gen_helper_fcmp(', 'gen_helper_unreviewed(')
        path.write_text(source)
        with self.assertRaises(subprocess.CalledProcessError):
            AUDIT.project(root, root, os.environ.get('CC', 'cc'))


if __name__ == '__main__':
    unittest.main()
