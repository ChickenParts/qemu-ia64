#!/usr/bin/env python3
"""Self-test the IA-64 firmware matrix without building or running QEMU."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
MATRIX = ROOT / "scripts" / "run-ia64-firmware-matrix.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run_matrix(
    temp: Path,
    runner: Path,
    firmware: Path,
    name: str,
    mode: str,
    timeout: float,
) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
    output = temp / f"output-{name}"
    env = os.environ.copy()
    env["MATRIX_FAKE_MODE"] = mode
    env.pop("MATRIX_FAKE_SLEEP", None)
    if mode == "frontier":
        env["MATRIX_FAKE_SLEEP"] = "5"

    completed = subprocess.run(
        [
            sys.executable,
            str(MATRIX),
            "--no-defaults",
            "--firmware",
            f"xen-gfw={firmware}",
            "--runner",
            str(runner),
            "--qemu",
            "/nonexistent/qemu-system-ia64",
            "--output",
            str(output),
            "--timeout",
            str(timeout),
        ],
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    summary_path = output / "summary.json"
    require(summary_path.is_file(), f"{name}: matrix omitted summary.json")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    require(len(summary) == 1, f"{name}: expected one result")
    return completed, summary[0]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="ia64-firmware-matrix-") as raw:
        temp = Path(raw)
        firmware = temp / "Flash.fd"
        firmware.write_bytes(b"test firmware")

        runner = temp / "fake-firmware-runner.sh"
        runner.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$IA64_LOGDIR"
case "${MATRIX_FAKE_MODE:-pass}" in
  pass)
    printf 'reset vector\nPeiCore\nDxeCore\nEFI Shell version 2.0\nShell>\n' \
      > "$IA64_LOGDIR/serial.fw.fake.log"
    ;;
  fail|frontier)
    printf 'reset vector\nPeiCore\nASSERT Universal\\\\DxeIpl\\\\Pei\\\\DxeLoad.c Line 536\n' \
      > "$IA64_LOGDIR/serial.fw.fake.log"
    ;;
  *)
    echo "unknown MATRIX_FAKE_MODE" >&2
    exit 2
    ;;
esac
sleep "${MATRIX_FAKE_SLEEP:-0}"
""",
            encoding="utf-8",
        )
        runner.chmod(0o755)

        completed, result = run_matrix(
            temp, runner, firmware, "pass", "pass", 2.0
        )
        require(completed.returncode == 0, "pass: expected zero exit status")
        require(result["status"] == "PASS", "pass: expected PASS")
        require(result["phase"] == "shell", "pass: expected shell phase")
        require(result["success_text"] is not None, "pass: missing oracle text")

        completed, result = run_matrix(
            temp, runner, firmware, "fail", "fail", 2.0
        )
        require(completed.returncode == 1, "fail: expected nonzero exit status")
        require(result["status"] == "FAIL", "fail: expected FAIL")
        require(result["phase"] == "dxe", "fail: expected dxe phase")
        require(result["blocker"] == "dxe-load", "fail: expected DxeLoad blocker")

        completed, result = run_matrix(
            temp, runner, firmware, "frontier", "frontier", 0.1
        )
        require(
            completed.returncode == 1,
            "frontier: expected nonzero exit status",
        )
        require(result["status"] == "FRONTIER", "frontier: expected FRONTIER")
        require(result["timed_out"] is True, "frontier: timeout not recorded")
        require(result["blocker"] == "dxe-load", "frontier: blocker lost")

        missing = subprocess.run(
            [
                sys.executable,
                str(MATRIX),
                "--no-defaults",
                "--firmware",
                f"missing={temp / 'missing.fd'}",
                "--runner",
                str(runner),
                "--qemu",
                "/nonexistent/qemu-system-ia64",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(missing.returncode != 0, "missing: expected configuration failure")
        require(
            "missing or empty firmware images" in missing.stderr,
            "missing: expected strict inventory error",
        )

    print("IA-64 firmware matrix self-tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
