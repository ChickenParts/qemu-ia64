#!/usr/bin/env python3
"""Replay IA-64 firmware images against one Rooster EFI payload.

Firmware images are supplied by path and are never copied into the repository.
The matrix records cryptographic identity, duplicate inputs, runner status,
serial/log sizes and the deepest recognisable boot stage.
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

STAGES: tuple[tuple[str, str], ...] = (
    ("rooster", r"Rooster IA-64 EFI entry reached|ROOSTER-IA64-EFI"),
    ("bds-shell", r"\bBDS\b|Boot Manager|EFI Shell|startup\.nsh"),
    ("dxe-core", r"DXE Core Entry|DxeCore|CoreInitialize"),
    ("dxe-ipl", r"DxeIpl|DXE IPL"),
    ("pei", r"\bPEI\b|\bPei[A-Z]"),
    ("fit", r"\bFIT\b"),
    ("pal-sal", r"\bPAL\b|\bSAL\b"),
    ("null-call", r"(?:target|tgt)=0{8,16}|null[^\n]*call"),
    ("unsupported", r"8000000000000003|EFI_UNSUPPORTED|unsupported"),
)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def safe_name(path: pathlib.Path) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", path.name)


def read_logs(directory: pathlib.Path) -> str:
    pieces: list[str] = []
    for path in sorted(directory.glob("*.log")):
        try:
            pieces.append(path.read_text(errors="replace"))
        except OSError:
            continue
    return "\n".join(pieces)


def classify(text: str) -> list[str]:
    return [name for name, pattern in STAGES if re.search(pattern, text, re.I)]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", type=pathlib.Path, required=True)
    parser.add_argument("--payload", type=pathlib.Path, required=True)
    parser.add_argument("--runner", type=pathlib.Path,
                        default=pathlib.Path("scripts/run-ia64-firmware.sh"))
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--memory", default="512M")
    parser.add_argument(
        "--vga",
        default="none",
        help="QEMU -vga device (default: none for the diagnostic control lane)",
    )
    parser.add_argument(
        "--display",
        default="none",
        help="QEMU host display backend (default: none; VGA may still be present)",
    )
    parser.add_argument(
        "--qemu-data-dir",
        type=pathlib.Path,
        help="QEMU data directory containing VGA option ROMs",
    )
    parser.add_argument("--config", type=pathlib.Path)
    parser.add_argument("firmware", nargs="+", type=pathlib.Path)
    arguments = parser.parse_args(argv)

    for label, path in (
        ("QEMU", arguments.qemu),
        ("Rooster payload", arguments.payload),
        ("firmware runner", arguments.runner),
    ):
        if not path.is_file():
            parser.error(f"{label} is missing: {path}")
    if not os.access(arguments.qemu, os.X_OK):
        parser.error(f"QEMU is not executable: {arguments.qemu}")

    qemu_data_dir = arguments.qemu_data_dir
    if qemu_data_dir is None:
        candidate = arguments.qemu.resolve().parent / "pc-bios"
        if candidate.is_dir():
            qemu_data_dir = candidate
    if qemu_data_dir is not None:
        qemu_data_dir = qemu_data_dir.resolve()
        if not qemu_data_dir.is_dir():
            parser.error(f"QEMU data directory is missing: {qemu_data_dir}")
    elif arguments.vga != "none":
        parser.error(
            "a display-enabled run requires --qemu-data-dir or a pc-bios "
            "directory beside qemu-system-ia64"
        )

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
    if arguments.config is not None:
        shutil.copyfile(arguments.config, esp / "rooster_config.json")
    else:
        (esp / "rooster_config.json").write_text("{}\n")

    seen: dict[str, str] = {}
    results: list[dict[str, object]] = []
    for firmware in arguments.firmware:
        if not firmware.is_file():
            results.append({"firmware": str(firmware), "error": "missing"})
            continue
        identity = sha256(firmware)
        duplicate = seen.get(identity)
        seen.setdefault(identity, str(firmware))
        record: dict[str, object] = {
            "firmware": str(firmware),
            "sha256": identity,
            "bytes": firmware.stat().st_size,
            "vga": arguments.vga,
            "display": arguments.display,
            "qemu_data_dir": str(qemu_data_dir) if qemu_data_dir else None,
        }
        if duplicate is not None:
            record["duplicate_of"] = duplicate
            results.append(record)
            continue

        run_directory = output / "runs" / safe_name(firmware)
        run_directory.mkdir(parents=True)
        environment = os.environ.copy()
        environment.update(
            {
                "QEMU_BIN": str(arguments.qemu.resolve()),
                "IA64_BIOS": str(firmware.resolve()),
                "IA64_LOGDIR": str(run_directory),
                "IA64_MEM": arguments.memory,
                "IA64_SMP": "1",
                "IA64_DISPLAY": arguments.display,
                "IA64_GUEST_ERRORS": "1",
                "IA64_CALL_NULL_FIX": "0",
                "IA64_PEI_SYSMEM_HOB_FIX": "1",
                "IA64_PEI_HOB_FLOW_TRACE": "1",
                "IA64_PEI_HOB_FLOW_TRACE_LIMIT": "4096",
                "IA64_DXE_LOAD_TRACE": "1",
                "IA64_DXE_LOAD_TRACE_LIMIT": "4096",
            }
        )
        if qemu_data_dir is not None:
            environment["IA64_QEMU_DATA_DIR"] = str(qemu_data_dir)

        command = [
            "timeout",
            "--signal=TERM",
            "--kill-after=5s",
            f"{arguments.timeout}s",
            str(arguments.runner.resolve()),
            "--",
            # The default is the headless CPU/firmware control lane.  Passing
            # --vga std (or another device) exercises the packaged ROM and
            # framebuffer path without requiring a host window.
            "-vga",
            arguments.vga,
            "-nic",
            "none",
            "-drive",
            f"file=fat:rw:{esp},format=raw,media=disk,if=ide",
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
        (run_directory / "runner.log").write_text(completed.stdout)
        text = read_logs(run_directory)
        relevant = [
            line
            for line in text.splitlines()
            if re.search(
                r"FIT|PEI|Dxe|DXE|HOB|PAL|SAL|null|unsupported|Rooster|BDS|Shell",
                line,
                re.I,
            )
        ]
        (run_directory / "frontier.log").write_text(
            "\n".join(relevant[-4000:]) + "\n"
        )
        record.update(
            {
                "return_code": completed.returncode,
                "seconds": round(elapsed, 3),
                "stages": classify(text),
                "serial_bytes": sum(
                    path.stat().st_size
                    for path in run_directory.glob("serial*.log")
                ),
                "qemu_log_bytes": sum(
                    path.stat().st_size
                    for path in run_directory.glob("qemu*.log")
                ),
            }
        )
        results.append(record)

    (output / "matrix.json").write_text(json.dumps(results, indent=2) + "\n")
    markdown = [
        "# IA-64 Rooster firmware matrix",
        "",
        "| Firmware | VGA | SHA-256 | Bytes | Return | Observed stages |",
        "|---|---|---|---:|---:|---|",
    ]
    for record in results:
        markdown.append(
            "| `{}` | `{}` | `{}` | {} | {} | {} |".format(
                pathlib.Path(str(record["firmware"])).name,
                record.get("vga", ""),
                record.get("sha256", ""),
                record.get("bytes", ""),
                record.get("return_code", record.get("error", "duplicate")),
                ", ".join(record.get("stages", [])),
            )
        )
    (output / "matrix.md").write_text("\n".join(markdown) + "\n")
    print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
