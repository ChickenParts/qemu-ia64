#!/usr/bin/env python3
"""Validate the IA-64 RSE invariants exercised by firmware smoke runs."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


TRACE_RE = re.compile(
    r"IA64: rse_strict tag=(?P<tag>\S+) "
    r"ip=(?P<ip>[0-9a-fA-F]+) "
    r"rsc=(?P<rsc>[0-9a-fA-F]+) "
    r"loadrs=(?P<loadrs>\d+) "
    r"bsp=(?P<bsp>[0-9a-fA-F]+) "
    r"bspstore=(?P<bspstore>[0-9a-fA-F]+).*?"
    r"arg0=(?P<arg0>[0-9a-fA-F]+) "
    r"arg1=(?P<arg1>[0-9a-fA-F]+)"
)

ORDINARY_FRAME_TAGS = {
    "alloc_bsp",
    "call_bsp",
    "ret_bsp",
    "ret_restore_pre",
    "ret_restore_post",
    "cover_bsp",
}


@dataclass(frozen=True)
class Trace:
    line: int
    tag: str
    ip: int
    rsc: int
    loadrs: int
    bsp: int
    bspstore: int
    arg0: int
    arg1: int


def parse_trace(path: Path) -> list[Trace]:
    traces: list[Trace] = []
    with path.open("r", encoding="utf-8", errors="replace") as source:
        for lineno, line in enumerate(source, 1):
            match = TRACE_RE.search(line)
            if match is None:
                continue
            fields = match.groupdict()
            traces.append(
                Trace(
                    line=lineno,
                    tag=fields["tag"],
                    ip=int(fields["ip"], 16),
                    rsc=int(fields["rsc"], 16),
                    loadrs=int(fields["loadrs"], 10),
                    bsp=int(fields["bsp"], 16),
                    bspstore=int(fields["bspstore"], 16),
                    arg0=int(fields["arg0"], 16),
                    arg1=int(fields["arg1"], 16),
                )
            )
    return traces


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path, help="QEMU firmware log")
    parser.add_argument(
        "--require-tag",
        action="append",
        default=[],
        help="trace tag that must occur at least once (repeatable)",
    )
    args = parser.parse_args()

    traces = parse_trace(args.log)
    if not traces:
        print(f"{args.log}: no IA64 RSE strict traces found", file=sys.stderr)
        return 1

    seen = {trace.tag for trace in traces}
    errors: list[str] = []

    for tag in args.require_tag:
        if tag not in seen:
            errors.append(f"required trace tag {tag!r} was not observed")

    for trace in traces:
        if trace.tag in ORDINARY_FRAME_TAGS and trace.loadrs != 0:
            errors.append(
                f"line {trace.line}: {trace.tag} changed/preserved a nonzero "
                f"LOADRS={trace.loadrs} at ip=0x{trace.ip:x}"
            )
        if trace.tag in {"call_bsp", "cover_bsp"} and trace.arg0 != trace.arg1:
            errors.append(
                f"line {trace.line}: {trace.tag} moved BSP "
                f"0x{trace.arg0:x}->0x{trace.arg1:x}"
            )

    max_loadrs = max(trace.loadrs for trace in traces)
    max_dirty = max(max(0, trace.bsp - trace.bspstore) for trace in traces)
    counts = ", ".join(
        f"{tag}={sum(trace.tag == tag for trace in traces)}"
        for tag in sorted(seen)
    )
    print(
        f"{args.log}: traces={len(traces)} max_loadrs={max_loadrs} "
        f"max_dirty_bytes={max_dirty}; {counts}"
    )

    if errors:
        for error in errors[:32]:
            print(f"ERROR: {error}", file=sys.stderr)
        if len(errors) > 32:
            print(f"ERROR: {len(errors) - 32} more violation(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
