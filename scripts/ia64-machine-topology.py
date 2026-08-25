#!/usr/bin/env python3
"""Capture a stable QMP/QOM topology snapshot of an IA-64 machine.

The snapshot is firmware-independent and records what QEMU actually realizes,
not merely which device models or source symbols exist.  It is intended to be
used alongside ia64-ipf-audit.py while porting the historical IPF.c devices.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
from typing import Any


class QMPError(RuntimeError):
    pass


class QMP:
    def __init__(self, path: pathlib.Path, timeout: float) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    raise QMPError(f"QMP socket did not become ready: {path}")
                time.sleep(0.02)
        self.reader = self.sock.makefile("r", encoding="utf-8")
        greeting = self._read()
        if "QMP" not in greeting:
            raise QMPError(f"invalid QMP greeting: {greeting!r}")
        self.execute("qmp_capabilities")

    def close(self) -> None:
        self.reader.close()
        self.sock.close()

    def _read(self) -> dict[str, Any]:
        while True:
            line = self.reader.readline()
            if not line:
                raise QMPError("QMP connection closed")
            message = json.loads(line)
            if "event" not in message:
                return message

    def execute(self, command: str, arguments: dict[str, Any] | None = None) -> Any:
        request: dict[str, Any] = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        self.sock.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode())
        response = self._read()
        if "error" in response:
            raise QMPError(f"{command}: {response['error']}")
        return response.get("return")


def stable(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: stable(value[key]) for key in sorted(value)}
    if isinstance(value, list):
        values = [stable(item) for item in value]
        if all(isinstance(item, dict) for item in values):
            return sorted(values, key=lambda item: json.dumps(item, sort_keys=True))
        return values
    return value


def qom_walk(qmp: QMP, path: str, seen: set[str]) -> dict[str, Any]:
    if path in seen:
        return {"path": path, "cycle": True}
    seen.add(path)
    node: dict[str, Any] = {"path": path, "children": []}
    try:
        entries = qmp.execute("qom-list", {"path": path})
    except QMPError as exc:
        node["error"] = str(exc)
        return node
    node["properties"] = sorted(
        ({"name": entry.get("name"), "type": entry.get("type")}
         for entry in entries),
        key=lambda entry: (entry["name"] or "", entry["type"] or ""),
    )
    for entry in entries:
        name = entry.get("name")
        type_name = entry.get("type", "")
        if not name or not type_name.startswith("child<"):
            continue
        child = path.rstrip("/") + "/" + name if path != "/" else "/" + name
        node["children"].append(qom_walk(qmp, child, seen))
    node["children"].sort(key=lambda child: child["path"])
    return node


def optional(qmp: QMP, command: str, arguments: dict[str, Any] | None = None) -> Any:
    try:
        return qmp.execute(command, arguments)
    except QMPError as exc:
        return {"unsupported": str(exc)}


def capture(args: argparse.Namespace) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="qemu-ia64-qmp-") as temp:
        socket_path = pathlib.Path(temp) / "qmp.sock"
        command = [
            str(args.qemu),
            "-S", "-display", "none", "-monitor", "none",
            "-qmp", f"unix:{socket_path},server=on,wait=off",
        ]
        if args.machine:
            command += ["-machine", args.machine]
        if args.firmware:
            command += ["-bios", str(args.firmware)]
        command += args.extra
        process = subprocess.Popen(
            command, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        qmp: QMP | None = None
        try:
            qmp = QMP(socket_path, args.timeout)
            result = {
                "schema": 1,
                "command": command,
                "version": qmp.execute("query-version"),
                "current_machine": optional(qmp, "query-current-machine"),
                "machines": optional(qmp, "query-machines"),
                "cpus": optional(qmp, "query-cpus-fast"),
                "pci": optional(qmp, "query-pci"),
                "memory_devices": optional(qmp, "query-memory-devices"),
                "qom": qom_walk(qmp, "/machine", set()),
            }
            return stable(result)
        finally:
            if qmp is not None:
                try:
                    qmp.execute("quit")
                except (QMPError, OSError):
                    pass
                qmp.close()
            try:
                _, stderr = process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                _, stderr = process.communicate()
            if process.returncode not in (0, None) and not args.allow_qemu_failure:
                print(stderr, file=sys.stderr)


def compare(left_path: pathlib.Path, right_path: pathlib.Path) -> int:
    left = json.loads(left_path.read_text(encoding="utf-8"))
    right = json.loads(right_path.read_text(encoding="utf-8"))
    if left == right:
        print("topologies are identical")
        return 0
    import difflib
    left_text = json.dumps(left, indent=2, sort_keys=True).splitlines(True)
    right_text = json.dumps(right, indent=2, sort_keys=True).splitlines(True)
    sys.stdout.writelines(difflib.unified_diff(
        left_text, right_text, fromfile=str(left_path), tofile=str(right_path)))
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", type=pathlib.Path,
                        help="qemu-system-ia64 executable")
    parser.add_argument("--machine")
    parser.add_argument("--firmware", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--allow-qemu-failure", action="store_true")
    parser.add_argument("--extra", action="append", default=[],
                        help="one additional QEMU argument; repeat as needed")
    parser.add_argument("--compare", nargs=2, metavar=("LEFT", "RIGHT"),
                        type=pathlib.Path)
    args = parser.parse_args()
    if args.compare:
        return compare(*args.compare)
    if not args.qemu or not args.output:
        parser.error("capture mode requires --qemu and --output")
    result = capture(args)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
