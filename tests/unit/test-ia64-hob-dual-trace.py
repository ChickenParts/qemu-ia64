#!/usr/bin/env python3
"""Synthetic tests for analyze-ia64-hob-dual-trace.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "analyze-ia64-hob-dual-trace.py"
SPEC = importlib.util.spec_from_file_location("ia64_hob_dual", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def line(hit: int, region: str, pc: int, address: int) -> str:
    return (
        f"IA64_HOB_DUAL hit={hit} region={region} vcpu=0 "
        f"pc={pc:016x} vaddr={address:016x} size=8"
    )


def analyze(lines: list[str]) -> dict[str, object]:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        trace = root / "trace.log"
        report = root / "report.json"
        markdown = root / "report.md"
        trace.write_text("\n".join(lines) + "\n")
        rc = MODULE.main(
            [str(trace), "--json", str(report), "--markdown", str(markdown)]
        )
        assert rc == 0
        assert markdown.is_file()
        return json.loads(report.read_text())


def test_stale_temporary_writer() -> None:
    report = analyze(
        [
            line(0, "temporary", 0x1000, 0xFFFF7000),
            line(1, "permanent", 0x2000, 0x040EF000),
            line(2, "permanent", 0x2000, 0x040EF008),
            line(3, "temporary", 0x3000, 0xFFFF7040),
        ]
    )
    assert report["classification"] == (
        "temporary-writes-continue-after-permanent-activation"
    )
    assert report["temporary_events_after_permanent_activation"] == 1


def test_clean_transition() -> None:
    report = analyze(
        [
            line(0, "temporary", 0x1000, 0xFFFF7000),
            line(1, "temporary", 0x1000, 0xFFFF7008),
            line(2, "permanent", 0x1000, 0x040EF000),
            line(3, "permanent", 0x1000, 0x040EF008),
        ]
    )
    assert report["classification"] == "one-way-temporary-to-permanent-transition"
    assert report["cross_region_writer_pcs"] == ["0x1000"]


def test_missing_permanent_side() -> None:
    report = analyze([line(0, "temporary", 0x1000, 0xFFFF7000)])
    assert report["classification"] == "permanent-arena-never-written"


if __name__ == "__main__":
    test_stale_temporary_writer()
    test_clean_transition()
    test_missing_permanent_side()
