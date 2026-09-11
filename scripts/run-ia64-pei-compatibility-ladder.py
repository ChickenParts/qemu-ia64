#!/usr/bin/env python3
"""Run a cumulative IA-64 PEI compatibility-switch ladder.

This separates the natural firmware frontier from progress enabled by existing
bring-up switches.  The null-call target repair is intentionally unavailable:
every case forces it off.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

STAGES = (
    ("rooster", r"ROOSTER-IA64-EFI|Rooster IA-64 EFI entry reached"),
    ("bds-shell", r"\bBDS\b|Boot Manager|EFI Shell|startup\.nsh"),
    ("dxe-core", r"DXE Core Entry|DxeCore|CoreInitialize"),
    ("dxe-ipl", r"DxeIpl|DXE IPL|PeiFindFile"),
    ("permanent-hob", r"040e[fF][0-9a-fA-F]{3}|permanent.*HOB|HOB.*permanent"),
    ("pei", r"\bPEI\b|\bPei[A-Z]"),
    ("fit", r"\bFIT\b"),
    ("pal-sal", r"\bPAL\b|\bSAL\b"),
    ("null-call", r"(?:target|tgt)=0{8,16}|null[^\n]*call"),
    ("unsupported", r"8000000000000003|EFI_UNSUPPORTED|unsupported"),
)

CASES: tuple[tuple[str, dict[str, str]], ...] = (
    (
        "pristine",
        {
            "IA64_PEI_SYSMEM_HOB_FIX": "0",
            "IA64_PEI_22560_STATUS_FIX": "0",
            "IA64_PEI_279D0_STATUS_FIX": "0",
            "IA64_PEI_279D0_SAFE_MODE": "0",
            "QEMU_IA64_PEI_FV_HOB_RESTORE": "0",
        },
    ),
    (
        "sysmem-hob",
        {
            "IA64_PEI_SYSMEM_HOB_FIX": "1",
            "IA64_PEI_22560_STATUS_FIX": "0",
            "IA64_PEI_279D0_STATUS_FIX": "0",
            "IA64_PEI_279D0_SAFE_MODE": "0",
            "QEMU_IA64_PEI_FV_HOB_RESTORE": "0",
        },
    ),
    (
        "sysmem-plus-22560",
        {
            "IA64_PEI_SYSMEM_HOB_FIX": "1",
            "IA64_PEI_22560_STATUS_FIX": "1",
            "IA64_PEI_279D0_STATUS_FIX": "0",
            "IA64_PEI_279D0_SAFE_MODE": "0",
            "QEMU_IA64_PEI_FV_HOB_RESTORE": "0",
        },
    ),
    (
        "legacy-pei-path",
        {
            "IA64_PEI_SYSMEM_HOB_FIX": "1",
            "IA64_PEI_22560_STATUS_FIX": "1",
            "IA64_PEI_279D0_STATUS_FIX": "1",
            "IA64_PEI_279D0_SAFE_MODE": "0",
            "QEMU_IA64_PEI_FV_HOB_RESTORE": "0",
        },
    ),
    (
        "legacy-plus-fv-causality",
        {
            "IA64_PEI_SYSMEM_HOB_FIX": "1",
            "IA64_PEI_22560_STATUS_FIX": "1",
            "IA64_PEI_279D0_STATUS_FIX": "1",
            "IA64_PEI_279D0_SAFE_MODE": "0",
            "QEMU_IA64_PEI_FV_HOB_RESTORE": "1",
        },
    ),
)


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def classify(text: str) -> list[str]:
    return [name for name, pattern in STAGES if re.search(pattern, text, re.I)]


def read_logs(directory: pathlib.Path) -> str:
    pieces: list[str] = []
    for path in sorted(directory.rglob("*.log")):
        pieces.append(path.read_text(errors="replace"))
    return "\n".join(pieces)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, type=pathlib.Path)
    parser.add_argument("--probe-qemu", type=pathlib.Path)
    parser.add_argument("--firmware", required=True, type=pathlib.Path)
    parser.add_argument("--payload", required=True, type=pathlib.Path)
    parser.add_argument("--runner", type=pathlib.Path,
                        default=pathlib.Path("scripts/run-ia64-firmware.sh"))
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--timeout", type=int, default=120)
    arguments = parser.parse_args(argv)

    for name, path in (
        ("QEMU", arguments.qemu),
        ("firmware", arguments.firmware),
        ("payload", arguments.payload),
        ("runner", arguments.runner),
    ):
        if not path.is_file():
            parser.error(f"{name} is missing: {path}")
    if arguments.probe_qemu is not None and not arguments.probe_qemu.is_file():
        parser.error(f"probe QEMU is missing: {arguments.probe_qemu}")

    output = arguments.out.resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    esp = output / "esp"
    boot = esp / "EFI" / "BOOT"
    boot.mkdir(parents=True)
    shutil.copyfile(arguments.payload, boot / "BOOTIA64.EFI")
    (esp / "startup.nsh").write_bytes(
        b"fs0:\\EFI\\BOOT\\BOOTIA64.EFI\r\n"
    )
    (esp / "rooster_config.json").write_text("{}\n")

    firmware_sha = digest(arguments.firmware)
    payload_sha = digest(arguments.payload)
    results: list[dict[str, object]] = []
    for case_name, switches in CASES:
        qemu = arguments.qemu
        if switches["QEMU_IA64_PEI_FV_HOB_RESTORE"] == "1":
            if arguments.probe_qemu is None:
                results.append(
                    {
                        "case": case_name,
                        "switches": switches,
                        "skipped": "--probe-qemu was not supplied",
                    }
                )
                continue
            qemu = arguments.probe_qemu

        case_dir = output / case_name
        case_dir.mkdir()
        environment = os.environ.copy()
        environment.update(switches)
        environment.update(
            {
                "QEMU_BIN": str(qemu.resolve()),
                "IA64_BIOS": str(arguments.firmware.resolve()),
                "IA64_LOGDIR": str(case_dir),
                "IA64_MEM": "512M",
                "IA64_SMP": "1",
                "IA64_DISPLAY": "none",
                "IA64_GUEST_ERRORS": "1",
                "IA64_CALL_NULL_FIX": "0",
                "IA64_PEI_22560_TRACE": "1",
                "IA64_PEI_279D0_TRACE": "1",
                "IA64_PEI_HOB_FLOW_TRACE": "1",
                "IA64_PEI_HOB_FLOW_TRACE_LIMIT": "8192",
                "IA64_DXE_LOAD_TRACE": "1",
                "IA64_DXE_LOAD_TRACE_LIMIT": "8192",
            }
        )
        command = [
            "timeout", "--signal=TERM", "--kill-after=5s",
            f"{arguments.timeout}s", str(arguments.runner.resolve()), "--",
            "-drive", f"file=fat:rw:{esp},format=raw,media=disk,if=ide",
        ]
        started = time.monotonic()
        completed = subprocess.run(
            command,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            check=False,
        )
        elapsed = time.monotonic() - started
        (case_dir / "runner.log").write_text(completed.stdout)
        text = read_logs(case_dir)
        relevant = [
            line for line in text.splitlines()
            if re.search(
                r"FIT|PEI|Pei|HOB|Dxe|DXE|PAL|SAL|null|unsupported|"
                r"Rooster|BDS|Shell|22560|279D0|FV_HOB",
                line,
                re.I,
            )
        ]
        (case_dir / "frontier.log").write_text(
            "\n".join(relevant[-8000:]) + "\n"
        )
        results.append(
            {
                "case": case_name,
                "qemu": str(qemu),
                "switches": switches,
                "return_code": completed.returncode,
                "seconds": round(elapsed, 3),
                "stages": classify(text),
                "fv_restore_observed": "IA64 FV_HOB_RESTORE source=" in text,
                "null_target_repair_observed": "CALL_NULL_FIX applied" in text,
                "serial_bytes": sum(
                    path.stat().st_size for path in case_dir.glob("serial*.log")
                ),
                "qemu_log_bytes": sum(
                    path.stat().st_size for path in case_dir.glob("qemu*.log")
                ),
            }
        )

    report = {
        "schema": 1,
        "firmware": str(arguments.firmware),
        "firmware_sha256": firmware_sha,
        "payload": str(arguments.payload),
        "payload_sha256": payload_sha,
        "cases": results,
    }
    (output / "ladder.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = [
        "# IA-64 PEI compatibility ladder",
        "",
        f"- Firmware SHA-256: `{firmware_sha}`",
        f"- Payload SHA-256: `{payload_sha}`",
        "- Null-call target repair: **disabled in every case**",
        "",
        "| Case | Return | Stages | FV restoration |",
        "|---|---:|---|---|",
    ]
    for record in results:
        lines.append(
            "| `{}` | {} | {} | {} |".format(
                record["case"],
                record.get("return_code", "skipped"),
                ", ".join(record.get("stages", [])),
                record.get("fv_restore_observed", False),
            )
        )
    (output / "ladder.md").write_text("\n".join(lines) + "\n")
    print(json.dumps(report, indent=2))

    if any(record.get("null_target_repair_observed") for record in results):
        print("compatibility ladder was contaminated by null-target repair",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
