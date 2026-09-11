#!/usr/bin/env python3
"""Unit tests for include-ia64-hob-causality.py."""

from __future__ import annotations

import importlib.util
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "include-ia64-hob-causality.py"
SPEC = importlib.util.spec_from_file_location("ia64_causality_meson", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_hw_arch_binding() -> None:
    original = """ia64_ss = ss.source_set()
ia64_ss.add(files('ipf.c'))
hw_arch += {'ia64': ia64_ss}
"""
    updated = MODULE.include_source(original)
    assert updated == """ia64_ss = ss.source_set()
ia64_ss.add(files('ipf.c'))
ia64_ss.add(files('hob-migration-causality.c'))
hw_arch += {'ia64': ia64_ss}
"""
    assert MODULE.include_source(updated) == updated


def test_single_source_set_without_binding() -> None:
    original = """ipf_ss = ss.source_set()
ipf_ss.add(when: 'CONFIG_IA64', if_true: files('ipf.c'))
"""
    updated = MODULE.include_source(original)
    assert "ipf_ss.add(files('hob-migration-causality.c'))" in updated
    assert updated.count("hob-migration-causality.c") == 1


def test_ambiguous_source_sets_uses_ipf_owner() -> None:
    original = """common_ss = ss.source_set()
ia64_ss = ss.source_set()
ia64_ss.add(files('ipf.c'))
"""
    updated = MODULE.include_source(original)
    assert "ia64_ss.add(files('hob-migration-causality.c'))" in updated
    assert "common_ss.add(files('hob-migration-causality.c'))" not in updated


def test_missing_owner_is_rejected() -> None:
    original = """one_ss = ss.source_set()
two_ss = ss.source_set()
"""
    try:
        MODULE.include_source(original)
    except ValueError as exc:
        assert "could not identify" in str(exc)
    else:
        raise AssertionError("ambiguous source sets were accepted")


if __name__ == "__main__":
    test_hw_arch_binding()
    test_single_source_set_without_binding()
    test_ambiguous_source_sets_uses_ipf_owner()
    test_missing_owner_is_rejected()
