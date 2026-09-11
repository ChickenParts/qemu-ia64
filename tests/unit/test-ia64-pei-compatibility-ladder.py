#!/usr/bin/env python3
"""Synthetic tests for run-ia64-pei-compatibility-ladder.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import stat
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "run-ia64-pei-compatibility-ladder.py"
SPEC = importlib.util.spec_from_file_location("ia64_pei_ladder", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def executable(path: pathlib.Path, text: str) -> None:
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def test_ladder_forces_null_fix_off_and_uses_probe_binary() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        qemu = root / "qemu"
        probe = root / "qemu-probe"
        runner = root / "runner.sh"
        firmware = root / "Flash.fd"
        payload = root / "BOOTIA64.EFI"
        output = root / "out"

        executable(qemu, "#!/bin/sh\nexit 0\n")
        executable(probe, "#!/bin/sh\nexit 0\n")
        executable(
            runner,
            """#!/bin/sh
set -eu
mkdir -p "$IA64_LOGDIR"
case "${QEMU_IA64_PEI_FV_HOB_RESTORE:-0}" in
  1) text='PEI DxeIpl DXE Core Entry IA64 FV_HOB_RESTORE source=1 target=2' ;;
  *) text='PEI EFI_UNSUPPORTED' ;;
esac
printf '%s nullfix=%s qemu=%s\n' "$text" "${IA64_CALL_NULL_FIX:-unset}" "$QEMU_BIN" \
  > "$IA64_LOGDIR/serial.synthetic.log"
exit 124
""",
        )
        firmware.write_bytes(b"firmware")
        payload.write_bytes(b"payload")

        rc = MODULE.main(
            [
                "--qemu", str(qemu),
                "--probe-qemu", str(probe),
                "--firmware", str(firmware),
                "--payload", str(payload),
                "--runner", str(runner),
                "--out", str(output),
                "--timeout", "1",
            ]
        )
        assert rc == 0
        report = json.loads((output / "ladder.json").read_text())
        cases = report["cases"]
        assert len(cases) == len(MODULE.CASES)
        assert all(case["null_target_repair_observed"] is False for case in cases)
        assert all(case["return_code"] == 124 for case in cases)
        assert cases[-1]["qemu"] == str(probe)
        assert cases[-1]["fv_restore_observed"] is True
        assert "dxe-core" in cases[-1]["stages"]
        for case in cases:
            log = (output / case["case"] / "serial.synthetic.log").read_text()
            assert "nullfix=0" in log


if __name__ == "__main__":
    test_ladder_forces_null_fix_off_and_uses_probe_binary()
