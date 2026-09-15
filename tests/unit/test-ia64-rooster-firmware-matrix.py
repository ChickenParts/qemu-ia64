#!/usr/bin/env python3
"""Synthetic tests for run-ia64-rooster-firmware-matrix.py."""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import stat
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "run-ia64-rooster-firmware-matrix.py"
SPEC = importlib.util.spec_from_file_location("ia64_rooster_matrix", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def executable(path: pathlib.Path, source: str) -> None:
    path.write_text(source)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def test_classifier() -> None:
    text = """
FIT table accepted
PEI dispatcher running
DxeIpl: DXE Core Entry
EFI Shell
ROOSTER-IA64-EFI-ENTRY: PASS
"""
    stages = MODULE.classify(text)
    assert stages[:5] == ["rooster", "bds-shell", "dxe-core", "dxe-ipl", "pei"]


def test_matrix_and_duplicate_identity() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        qemu = root / "qemu-system-ia64"
        runner = root / "runner.sh"
        payload = root / "BOOTIA64.EFI"
        xen = root / "Flash.fd"
        xen_duplicate = root / "Flash-copy.fd"
        sdv = root / "SDV.bin"
        output = root / "out"

        executable(qemu, "#!/bin/sh\nexit 0\n")
        executable(
            runner,
            """#!/bin/sh
set -eu
mkdir -p "$IA64_LOGDIR"
case "$IA64_BIOS" in
  *Flash.fd) printf '%s\n' 'PEI DxeIpl DXE Core Entry EFI Shell ROOSTER-IA64-EFI-ENTRY: PASS' ;;
  *) printf '%s\n' 'FIT PEI EFI_UNSUPPORTED target=0000000000000000' ;;
esac > "$IA64_LOGDIR/serial.synthetic.log"
exit 124
""",
        )
        payload.write_bytes(b"MZ" + bytes(4094))
        xen.write_bytes(b"canonical-xen")
        xen_duplicate.write_bytes(xen.read_bytes())
        sdv.write_bytes(b"sdv")

        rc = MODULE.main(
            [
                "--qemu", str(qemu),
                "--payload", str(payload),
                "--runner", str(runner),
                "--out", str(output),
                "--timeout", "1",
                str(xen), str(xen_duplicate), str(sdv),
            ]
        )
        assert rc == 0
        records = json.loads((output / "matrix.json").read_text())
        assert len(records) == 3
        assert records[0]["sha256"] == records[1]["sha256"]
        assert records[1]["duplicate_of"] == str(xen)
        assert records[0]["stages"][0] == "rooster"
        assert records[0]["return_code"] == 124
        assert records[2]["stages"] == [
            "pei", "fit", "null-call", "unsupported"
        ]
        assert (output / "matrix.md").is_file()


if __name__ == "__main__":
    test_classifier()
    test_matrix_and_duplicate_identity()
