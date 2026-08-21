#!/usr/bin/env python3
"""Sample a running IA-64 QEMU through QMP without relying on console output."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from pathlib import Path
from typing import Any


class QMPError(RuntimeError):
    pass


class QMPClient:
    def __init__(self, path: Path, connect_timeout: float) -> None:
        self.path = path
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.monotonic() + connect_timeout
        last_error: OSError | None = None
        while time.monotonic() < deadline:
            try:
                self.sock.connect(str(path))
                break
            except OSError as exc:
                last_error = exc
                time.sleep(0.05)
        else:
            raise QMPError(f"could not connect to {path}: {last_error}")

        self.reader = self.sock.makefile("r", encoding="utf-8")
        greeting = self._read_response(require_return=False)
        if "QMP" not in greeting:
            raise QMPError(f"invalid QMP greeting: {greeting!r}")
        self.execute("qmp_capabilities")

    def close(self) -> None:
        try:
            self.reader.close()
        finally:
            self.sock.close()

    def _read_message(self) -> dict[str, Any]:
        line = self.reader.readline()
        if not line:
            raise QMPError("QMP connection closed")
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise QMPError(f"invalid QMP JSON: {line!r}") from exc
        if not isinstance(value, dict):
            raise QMPError(f"unexpected QMP value: {value!r}")
        return value

    def _read_response(self, require_return: bool = True) -> dict[str, Any]:
        while True:
            value = self._read_message()
            if "event" in value:
                continue
            if "error" in value:
                raise QMPError(json.dumps(value["error"], sort_keys=True))
            if not require_return or "return" in value:
                return value

    def execute(self, command: str, arguments: dict[str, Any] | None = None) -> Any:
        request: dict[str, Any] = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        self.sock.sendall((json.dumps(request) + "\r\n").encode("utf-8"))
        return self._read_response()["return"]

    def hmp(self, command: str) -> str:
        value = self.execute("human-monitor-command", {"command-line": command})
        return value if isinstance(value, str) else json.dumps(value, sort_keys=True)


def wait_for_status(client: QMPClient, running: bool, timeout: float = 2.0) -> None:
    deadline = time.monotonic() + timeout
    wanted = "running" if running else "paused"
    while time.monotonic() < deadline:
        status = client.execute("query-status")
        if isinstance(status, dict) and status.get("running") is running:
            return
        time.sleep(0.02)
    raise QMPError(f"QEMU did not become {wanted}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("socket", type=Path, help="QMP Unix socket")
    parser.add_argument("--samples", type=int, default=6)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--connect-timeout", type=float, default=10.0)
    args = parser.parse_args()

    if args.samples < 1:
        parser.error("--samples must be at least one")
    if args.interval < 0:
        parser.error("--interval must be non-negative")

    client = QMPClient(args.socket, args.connect_timeout)
    try:
        for index in range(args.samples):
            client.execute("stop")
            wait_for_status(client, False)
            status = client.execute("query-status")
            registers = client.hmp("info registers")
            try:
                instructions = client.hmp("x/12i $pc")
            except QMPError as exc:
                instructions = f"<disassembly unavailable: {exc}>"

            print(f"===== IA64 SAMPLE {index} monotonic={time.monotonic():.6f} =====")
            print("STATUS " + json.dumps(status, sort_keys=True))
            print("REGISTERS")
            print(registers.rstrip())
            print("INSTRUCTIONS")
            print(instructions.rstrip())
            print(flush=True)

            client.execute("cont")
            wait_for_status(client, True)
            if index + 1 < args.samples:
                time.sleep(args.interval)
    finally:
        try:
            status = client.execute("query-status")
            if isinstance(status, dict) and not status.get("running", False):
                client.execute("cont")
        except (OSError, QMPError):
            pass
        client.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, QMPError) as exc:
        print(f"ia64-qmp-sample: {exc}", file=sys.stderr)
        raise SystemExit(1)
