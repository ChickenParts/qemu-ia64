#!/usr/bin/env python3
"""Synthetic tests for analyze-ia64-hob-register-trace.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "analyze-ia64-hob-register-trace.py"
SPEC = importlib.util.spec_from_file_location("ia64_hob_registers", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def le64(value: int) -> str:
    return value.to_bytes(8, "little").hex()


def test_role_inference() -> None:
    lines = []
    for hit in range(4):
        destination = 0x040EF000 + hit * 8
        source = 0xFFFF7000 + hit * 8
        remaining = 0x40 - hit * 8
        lines.append(
            "IA64_HOB_REGS hit={hit} vcpu=0 pc=0000000012345000 "
            "vaddr={destination:016x} size=8 r16={r16} r17={r17} "
            "r18={r18} r19={r19}".format(
                hit=hit,
                destination=destination,
                r16=le64(destination),
                r17=le64(source),
                r18=le64(remaining),
                r19=le64(0xDEADBEEF),
            )
        )

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        trace = root / "trace.log"
        report_path = root / "report.json"
        markdown_path = root / "report.md"
        trace.write_text("\n".join(lines) + "\n")
        rc = MODULE.main(
            [
                str(trace),
                "--json", str(report_path),
                "--markdown", str(markdown_path),
            ]
        )
        assert rc == 0
        report = json.loads(report_path.read_text())
        assert report["sample_count"] == 4
        assert report["destination_pointer_candidates"][0] == ["r16", 4]
        assert report["source_pointer_candidates"][0] == ["r17", 4]
        counters = {
            item["register"]: item
            for item in report["monotonic_counter_candidates"]
        }
        assert counters["r16"]["common_delta"] == 8
        assert counters["r17"]["common_delta"] == 8
        assert counters["r18"]["common_delta"] == -8
        assert markdown_path.is_file()


if __name__ == "__main__":
    test_role_inference()
