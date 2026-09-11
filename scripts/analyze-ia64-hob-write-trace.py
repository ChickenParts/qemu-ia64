#!/usr/bin/env python3
"""Summarize output from the IA-64 HOB-write TCG plugin."""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import pathlib
import re
import sys

WRITE_RE = re.compile(
    r"IA64_HOB_WRITE hit=(?P<hit>\d+) vcpu=(?P<vcpu>\d+) "
    r"pc=(?P<pc>[0-9a-fA-F]{1,16}) "
    r"vaddr=(?P<vaddr>[0-9a-fA-F]{1,16}) size=(?P<size>\d+)"
)


@dataclasses.dataclass(frozen=True)
class Write:
    hit: int
    vcpu: int
    pc: int
    address: int
    size: int

    @property
    def end(self) -> int:
        return self.address + self.size


def integer(text: str) -> int:
    return int(text, 0)


def parse(path: pathlib.Path) -> list[Write]:
    writes: list[Write] = []
    for line in path.read_text(errors="replace").splitlines():
        match = WRITE_RE.search(line)
        if match is None:
            continue
        writes.append(
            Write(
                hit=int(match["hit"]),
                vcpu=int(match["vcpu"]),
                pc=int(match["pc"], 16),
                address=int(match["vaddr"], 16),
                size=int(match["size"]),
            )
        )
    writes.sort(key=lambda write: write.hit)
    return writes


def overlaps(write: Write, address: int, size: int) -> bool:
    return write.address < address + size and address < write.end


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=pathlib.Path)
    parser.add_argument("--hob-list", type=integer)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    parser.add_argument("--top", type=int, default=32)
    arguments = parser.parse_args(argv)

    try:
        writes = parse(arguments.trace)
    except OSError as exc:
        print(f"analyze-ia64-hob-write-trace.py: {exc}", file=sys.stderr)
        return 1
    if not writes:
        print("analyze-ia64-hob-write-trace.py: no plugin writes found",
              file=sys.stderr)
        return 1

    by_pc: dict[int, list[Write]] = collections.defaultdict(list)
    by_vcpu: collections.Counter[int] = collections.Counter()
    for write in writes:
        by_pc[write.pc].append(write)
        by_vcpu[write.vcpu] += 1

    ranked = sorted(
        by_pc.items(),
        key=lambda item: (-len(item[1]), item[1][0].hit, item[0]),
    )
    pc_records = []
    for pc, pc_writes in ranked[: arguments.top]:
        pc_records.append(
            {
                "pc": hex(pc),
                "count": len(pc_writes),
                "first_hit": pc_writes[0].hit,
                "last_hit": pc_writes[-1].hit,
                "minimum_address": hex(min(write.address for write in pc_writes)),
                "maximum_end": hex(max(write.end for write in pc_writes)),
                "sizes": sorted({write.size for write in pc_writes}),
            }
        )

    report: dict[str, object] = {
        "schema": 1,
        "trace": str(arguments.trace),
        "write_count": len(writes),
        "first_write": dataclasses.asdict(writes[0]),
        "last_write": dataclasses.asdict(writes[-1]),
        "vcpu_counts": {str(key): value for key, value in sorted(by_vcpu.items())},
        "writer_pcs": pc_records,
    }

    if arguments.hob_list is not None:
        fields = {
            "handoff-header": (arguments.hob_list, 8),
            "free-memory-top": (arguments.hob_list + 32, 8),
            "free-memory-bottom": (arguments.hob_list + 40, 8),
            "end-of-hob-list": (arguments.hob_list + 48, 8),
        }
        report["hob_list"] = hex(arguments.hob_list)
        report["field_writers"] = {
            name: [
                {
                    "hit": write.hit,
                    "vcpu": write.vcpu,
                    "pc": hex(write.pc),
                    "address": hex(write.address),
                    "size": write.size,
                }
                for write in writes
                if overlaps(write, address, size)
            ]
            for name, (address, size) in fields.items()
        }

    rendered = json.dumps(report, indent=2, sort_keys=True)
    print(rendered)
    if arguments.json is not None:
        arguments.json.write_text(rendered + "\n")

    if arguments.markdown is not None:
        lines = [
            "# IA-64 permanent-HOB write trace",
            "",
            f"- Writes: `{len(writes)}`",
            f"- First writer PC: `{hex(writes[0].pc)}`",
            f"- First address: `{hex(writes[0].address)}`",
            f"- Last writer PC: `{hex(writes[-1].pc)}`",
            "",
            "| PC | Count | First hit | Address span | Sizes |",
            "|---|---:|---:|---|---|",
        ]
        for record in pc_records:
            lines.append(
                "| `{pc}` | {count} | {first_hit} | `{minimum_address}`–"
                "`{maximum_end}` | `{sizes}` |".format(**record)
            )
        arguments.markdown.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
