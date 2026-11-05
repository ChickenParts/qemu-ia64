#!/usr/bin/env python3
"""
Run the IA-64 Xen and development-hardware firmware boot matrix.

The existing single-firmware wrapper remains authoritative for QEMU arguments
and environment-variable compatibility. This driver gives every firmware an
isolated log directory, applies one hard success oracle, records the furthest
observed boot phase and terminal blocker, and emits machine-readable results.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Iterable, Pattern


DEFAULT_FIRMWARES = (
    ("xen-gfw", "stuff/Flash.fd"),
    ("sdv-debug-0.99", "stuff/EFI 0.99 Debug SDV.bin"),
    ("sdv-unknown", "stuff/Unknown Ver EFI.bin"),
    ("sdv-117c", "stuff/bios117c.BIN"),
    ("sdv-130", "stuff/bios130.BIN"),
    ("sdv-mybios", "stuff/mybios.bin"),
    ("rx4610-109b", "stuff/rx4610_109B.BIN"),
    ("rx4610-117b", "stuff/rx4610_117B.BIN"),
)

DEFAULT_SUCCESS_RE = (
    r"(?im)(?:^|\r?\n)\s*(?:Shell|fs[0-9]+:)[^>\r\n]*>\s*$"
    r"|EFI\s+Shell\s+(?:version|startup)"
    r"|Boot\s+Manager\s+Menu"
)

PHASES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("reset", (r"reset vector", r"SEC(?:Main| phase)?", r"SecCore")),
    ("pei", (r"\bPEI\b", r"PeiCore", r"InstallPeiMemory", r"DxeIpl")),
    ("dxe", (r"\bDXE\b", r"DxeCore", r"DxeLoad", r"Dispatcher")),
    ("bds", (r"\bBDS\b", r"BdsDxe", r"Boot Manager", r"EFI Shell")),
    ("shell", (r"(?im)(?:^|\r?\n)\s*(?:Shell|fs[0-9]+:)[^>\r\n]*>\s*$",)),
)

BLOCKERS: tuple[tuple[str, str], ...] = (
    ("host-fatal", r"qemu:\s*fatal|Assertion .* failed|Aborted \(core dumped\)"),
    ("unimplemented", r"IA64\s+UNIMPL|gen_unimpl|unimplemented instruction"),
    ("null-branch", r"null (?:pc|branch target)|call_null"),
    ("firmware-assert", r"\bASSERT\b"),
    ("dxe-load", r"DxeLoad\.c(?:\s+Line)?\s*\d+"),
    ("gcd", r"Gcd\.c(?:\s+Line)?\s*\d+"),
    ("architectural-fault", r"\bfault\b.*\b(?:vec|vector)\b"),
    ("watchdog-loop", r"hang_abort|loop guard|progress_dedup_repeat"),
)


@dataclasses.dataclass(frozen=True)
class Firmware:
    name: str
    path: Path


@dataclasses.dataclass
class Result:
    name: str
    firmware: str
    status: str
    returncode: int | None
    timed_out: bool
    elapsed_seconds: float
    phase: str
    blocker: str | None
    blocker_text: str | None
    success_text: str | None
    serial_log: str | None
    host_log: str
    command: list[str]
    error: str | None = None


def parse_assignment(value: str, what: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(f"{what} must be NAME=VALUE: {value!r}")
    name, assigned = value.split("=", 1)
    if not name or not assigned:
        raise argparse.ArgumentTypeError(f"{what} must be NAME=VALUE: {value!r}")
    return name, assigned


def slugify(value: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-.")
    return slug or "firmware"


def read_text(paths: Iterable[Path]) -> str:
    chunks: list[str] = []
    for path in paths:
        try:
            chunks.append(path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
    return "\n".join(chunks)


def compile_regex(pattern: str, label: str) -> Pattern[str]:
    try:
        return re.compile(pattern)
    except re.error as exc:
        raise SystemExit(
            f"invalid {label} regular expression {pattern!r}: {exc}"
        ) from exc


def furthest_phase(log: str) -> str:
    furthest = "none"
    for phase, patterns in PHASES:
        if any(
            re.search(pattern, log, re.IGNORECASE | re.MULTILINE)
            for pattern in patterns
        ):
            furthest = phase
    return furthest


def last_blocker(log: str) -> tuple[str | None, str | None]:
    latest: tuple[int, str, str] | None = None
    for label, pattern in BLOCKERS:
        for match in re.finditer(pattern, log, re.IGNORECASE | re.MULTILINE):
            candidate = (match.start(), label, match.group(0))
            if latest is None or candidate[0] > latest[0]:
                latest = candidate
    if latest is None:
        return None, None
    return latest[1], latest[2]


def stop_process(proc: subprocess.Popen[object]) -> None:
    if proc.poll() is not None:
        return
    try:
        if os.name == "posix":
            os.killpg(proc.pid, signal.SIGTERM)
        else:
            proc.terminate()
        proc.wait(timeout=5)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            if os.name == "posix":
                os.killpg(proc.pid, signal.SIGKILL)
            else:
                proc.kill()
        except ProcessLookupError:
            pass
        proc.wait()


def newest_serial_log(case_dir: Path) -> Path | None:
    logs = sorted(
        case_dir.glob("serial.fw.*.log"),
        key=lambda path: path.stat().st_mtime_ns,
    )
    return logs[-1] if logs else None


def run_case(
    firmware: Firmware,
    args: argparse.Namespace,
    output_dir: Path,
    success_re: Pattern[str],
    success_by_name: dict[str, Pattern[str]],
    extra_env: dict[str, str],
) -> Result:
    case_dir = output_dir / slugify(firmware.name)
    case_dir.mkdir(parents=True, exist_ok=True)
    host_log = case_dir / "host.log"

    command = [str(args.runner)]
    command.extend(args.qemu_arg)

    env = os.environ.copy()
    env.update(extra_env)
    env.update(
        {
            "QEMU_BIN": str(args.qemu),
            "IA64_BIOS": str(firmware.path),
            "IA64_MEM": args.memory,
            "IA64_SMP": str(args.smp),
            "IA64_DISPLAY": "none",
            "IA64_GUEST_ERRORS": "1",
            "IA64_LOGDIR": str(case_dir),
        }
    )

    start = time.monotonic()
    timed_out = False
    returncode: int | None = None
    error: str | None = None

    try:
        with host_log.open("wb") as output:
            proc = subprocess.Popen(
                command,
                stdout=output,
                stderr=subprocess.STDOUT,
                env=env,
                start_new_session=(os.name == "posix"),
            )
            try:
                returncode = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                stop_process(proc)
                returncode = proc.returncode
    except OSError as exc:
        error = str(exc)

    elapsed = time.monotonic() - start
    serial_log = newest_serial_log(case_dir)
    combined = read_text(
        path for path in (serial_log, host_log) if path is not None
    )
    oracle = success_by_name.get(firmware.name, success_re)
    success_match = oracle.search(combined)
    blocker, blocker_text = last_blocker(combined)
    phase = furthest_phase(combined)

    if error is not None:
        status = "ERROR"
    elif success_match is not None:
        status = "PASS"
    elif timed_out:
        status = "FRONTIER"
    else:
        status = "FAIL"

    return Result(
        name=firmware.name,
        firmware=str(firmware.path),
        status=status,
        returncode=returncode,
        timed_out=timed_out,
        elapsed_seconds=round(elapsed, 3),
        phase=phase,
        blocker=blocker,
        blocker_text=blocker_text,
        success_text=success_match.group(0) if success_match else None,
        serial_log=str(serial_log) if serial_log else None,
        host_log=str(host_log),
        command=command,
        error=error,
    )


def write_markdown(results: list[Result], output_dir: Path) -> None:
    lines = [
        "# IA-64 firmware boot matrix",
        "",
        "| Firmware | Result | Phase | Blocker | Seconds |",
        "|---|---:|---:|---|---:|",
    ]
    for result in results:
        blocker = result.blocker or ""
        lines.append(
            f"| `{result.name}` | **{result.status}** | "
            f"`{result.phase}` | `{blocker}` | {result.elapsed_seconds:.3f} |"
        )
    lines.extend(
        [
            "",
            "A `PASS` requires the configured success regular expression. "
            "A timeout without that marker is a `FRONTIER`, never a pass.",
            "",
        ]
    )
    (output_dir / "summary.md").write_text(
        "\n".join(lines), encoding="utf-8"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--qemu",
        type=Path,
        default=Path(os.environ.get("QEMU_BIN", "./build/qemu-system-ia64")),
        help="qemu-system-ia64 executable",
    )
    parser.add_argument(
        "--runner",
        type=Path,
        default=Path(__file__).with_name("run-ia64-firmware.sh"),
        help="single-firmware wrapper",
    )
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--memory", default=os.environ.get("IA64_MEM", "512M"))
    parser.add_argument(
        "--smp", type=int, default=int(os.environ.get("IA64_SMP", "1"))
    )
    parser.add_argument(
        "--output",
        type=Path,
        help=(
            "output directory; defaults to "
            "scratch/ia64-firmware-matrix/<UTC stamp>"
        ),
    )
    parser.add_argument(
        "--firmware",
        action="append",
        default=[],
        metavar="NAME=PATH",
        help="add or replace a firmware image",
    )
    parser.add_argument(
        "--no-defaults",
        action="store_true",
        help="do not include the repository firmware inventory",
    )
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="skip missing or zero-length images instead of failing the matrix",
    )
    parser.add_argument(
        "--success-regex",
        default=DEFAULT_SUCCESS_RE,
        help="default full-boot oracle",
    )
    parser.add_argument(
        "--success",
        action="append",
        default=[],
        metavar="NAME=REGEX",
        help="per-firmware full-boot oracle",
    )
    parser.add_argument(
        "--set-env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="set an environment variable for every firmware run",
    )
    parser.add_argument(
        "--qemu-arg",
        action="append",
        default=[],
        help="extra QEMU argument passed through the single-firmware wrapper",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.timeout <= 0:
        raise SystemExit("--timeout must be greater than zero")
    if args.smp <= 0:
        raise SystemExit("--smp must be greater than zero")

    inventory: dict[str, Path] = {}
    if not args.no_defaults:
        inventory.update((name, Path(path)) for name, path in DEFAULT_FIRMWARES)

    for raw in args.firmware:
        name, path = parse_assignment(raw, "--firmware")
        inventory[name] = Path(path)

    xen_override = os.environ.get("IA64_XEN_BIOS")
    if xen_override and "xen-gfw" in inventory:
        inventory["xen-gfw"] = Path(xen_override)

    if not inventory:
        raise SystemExit("no firmware images selected")

    selected: list[Firmware] = []
    missing: list[Firmware] = []
    for name, path in inventory.items():
        firmware = Firmware(name, path)
        try:
            usable = path.is_file() and path.stat().st_size > 0
        except OSError:
            usable = False
        (selected if usable else missing).append(firmware)

    if missing and not args.allow_missing:
        details = "\n".join(f"  {item.name}: {item.path}" for item in missing)
        raise SystemExit(
            "missing or empty firmware images:\n"
            f"{details}\n"
            "Set IA64_XEN_BIOS, pass --firmware NAME=PATH, or use "
            "--allow-missing."
        )
    if not selected:
        raise SystemExit("no usable firmware images selected")

    success_re = compile_regex(args.success_regex, "--success-regex")
    success_by_name: dict[str, Pattern[str]] = {}
    for raw in args.success:
        name, pattern = parse_assignment(raw, "--success")
        success_by_name[name] = compile_regex(pattern, f"--success {name}")

    extra_env: dict[str, str] = {}
    for raw in args.set_env:
        key, value = parse_assignment(raw, "--set-env")
        extra_env[key] = value

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output_dir = args.output or Path("scratch/ia64-firmware-matrix") / stamp
    output_dir.mkdir(parents=True, exist_ok=True)

    manifest = {
        "created_utc": stamp,
        "qemu": str(args.qemu),
        "runner": str(args.runner),
        "timeout_seconds": args.timeout,
        "memory": args.memory,
        "smp": args.smp,
        "success_regex": args.success_regex,
        "firmware": [
            {"name": item.name, "path": str(item.path)} for item in selected
        ],
        "skipped_missing": [
            {"name": item.name, "path": str(item.path)} for item in missing
        ],
        "extra_environment": extra_env,
        "qemu_arguments": args.qemu_arg,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    results: list[Result] = []
    for firmware in selected:
        print(f"[RUN] {firmware.name}: {firmware.path}", flush=True)
        result = run_case(
            firmware,
            args,
            output_dir,
            success_re,
            success_by_name,
            extra_env,
        )
        results.append(result)
        detail = result.blocker or "no terminal blocker classified"
        print(
            f"[{result.status}] phase={result.phase} blocker={detail} "
            f"elapsed={result.elapsed_seconds:.3f}s",
            flush=True,
        )

    summary = [dataclasses.asdict(result) for result in results]
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    write_markdown(results, output_dir)

    print(f"results: {output_dir}", flush=True)
    return 0 if all(result.status == "PASS" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
