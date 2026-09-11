#!/usr/bin/env python3
"""Synthetic tests for analyze-ia64-hob-transition.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "analyze-ia64-hob-transition.py"
SPEC = importlib.util.spec_from_file_location("ia64_hob_transition", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def line(hit: int, region: str, pc: int, address: int, size: int = 8) -> str:
    return (
        f"IA64_HOB_DUAL hit={hit} region={region} vcpu=0 "
        f"pc={pc:016x} vaddr={address:016x} size={size}"
    )


def analyze(lines: list[str]) -> dict[str, object]:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        trace = root / "trace.log"
        report = root / "report.json"
        trace.write_text("\n".join(lines) + "\n")
        assert MODULE.main([str(trace), "--json", str(report)]) == 0
        return json.loads(report.read_text())


def test_preclear_does_not_activate_permanent_list() -> None:
    report = analyze(
        [
            line(0, "permanent", 0x9000, 0x040E0000),
            line(1, "temporary", 0x1000, 0xFFFF7000),
            line(2, "permanent", 0x2000, 0x040EF000),
            line(3, "temporary", 0x3000, 0xFFFF7060),
        ]
    )
    assert report["activation"]["hit"] == 2
    assert report["classification"] == (
        "temporary-hob-writes-continue-after-permanent-phit"
    )
    assert report["temporary_hob_writes_after_activation"] == 1


def test_clean_phit_transition() -> None:
    report = analyze(
        [
            line(0, "temporary", 0x1000, 0xFFFF7000),
            line(1, "permanent", 0x2000, 0x040EF000),
            line(2, "permanent", 0x2000, 0x040EF038),
        ]
    )
    assert report["classification"] == (
        "no-temporary-hob-writes-after-permanent-phit"
    )
    assert report["permanent_phit_write_count"] == 1


def test_missing_phit_is_inconclusive() -> None:
    report = analyze(
        [
            line(0, "temporary", 0x1000, 0xFFFF7000),
            line(1, "permanent", 0x9000, 0x040E1000),
        ]
    )
    assert report["classification"] == "permanent-phit-not-observed"


if __name__ == "__main__":
    test_preclear_does_not_activate_permanent_list()
    test_clean_phit_transition()
    test_missing_phit_is_inconclusive()
