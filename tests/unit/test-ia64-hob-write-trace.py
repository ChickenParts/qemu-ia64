#!/usr/bin/env python3
"""Synthetic tests for analyze-ia64-hob-write-trace.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "analyze-ia64-hob-write-trace.py"
SPEC = importlib.util.spec_from_file_location("ia64_hob_writes", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def test_parse_and_field_classification() -> None:
    trace = """
noise
IA64_HOB_WRITE hit=0 vcpu=0 pc=0000000012340000 vaddr=00000000040ef000 size=8
IA64_HOB_WRITE hit=1 vcpu=0 pc=0000000012340010 vaddr=00000000040ef028 size=8
IA64_HOB_WRITE hit=2 vcpu=0 pc=0000000012340010 vaddr=00000000040ef030 size=8
IA64_HOB_WRITE hit=3 vcpu=1 pc=0000000012340020 vaddr=00000000040ef080 size=4
IA64_HOB_WRITE summary hits=4 base=00000000040e0000 size=0000000000020000 limit=8192
"""
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        trace_path = root / "trace.log"
        json_path = root / "report.json"
        markdown_path = root / "report.md"
        trace_path.write_text(trace)
        rc = MODULE.main(
            [
                str(trace_path),
                "--hob-list", "0x040ef000",
                "--json", str(json_path),
                "--markdown", str(markdown_path),
            ]
        )
        assert rc == 0
        report = json.loads(json_path.read_text())
        assert report["write_count"] == 4
        assert report["first_write"]["pc"] == 0x12340000
        assert report["writer_pcs"][0]["pc"] == "0x12340010"
        assert report["writer_pcs"][0]["count"] == 2
        assert len(report["field_writers"]["free-memory-bottom"]) == 1
        assert len(report["field_writers"]["end-of-hob-list"]) == 1
        assert markdown_path.is_file()


if __name__ == "__main__":
    test_parse_and_field_classification()
