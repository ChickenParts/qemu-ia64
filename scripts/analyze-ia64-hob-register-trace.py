#!/usr/bin/env python3
"""Infer likely source, destination, and counter registers in a HOB copy loop."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys

LINE_RE = re.compile(
    r"IA64_HOB_REGS hit=(?P<hit>\d+) vcpu=(?P<vcpu>\d+) "
    r"pc=(?P<pc>[0-9a-fA-F]+) vaddr=(?P<vaddr>[0-9a-fA-F]+) "
    r"size=(?P<size>\d+)(?P<registers>.*)$"
)
REG_RE = re.compile(r"\s+(?P<name>[A-Za-z0-9.]+)=(?P<value>[0-9a-fA-F]+)")


def integer(text: str) -> int:
    return int(text, 0)


def decode_register(text: str) -> int:
    raw = bytes.fromhex(text)
    return int.from_bytes(raw, "little")


def parse(path: pathlib.Path) -> list[dict[str, object]]:
    samples: list[dict[str, object]] = []
    for line in path.read_text(errors="replace").splitlines():
        match = LINE_RE.search(line)
        if match is None:
            continue
        registers = {
            item["name"]: decode_register(item["value"])
            for item in REG_RE.finditer(match["registers"])
        }
        samples.append(
            {
                "hit": int(match["hit"]),
                "vcpu": int(match["vcpu"]),
                "pc": int(match["pc"], 16),
                "address": int(match["vaddr"], 16),
                "size": int(match["size"]),
                "registers": registers,
            }
        )
    samples.sort(key=lambda sample: int(sample["hit"]))
    return samples


def values_for(samples: list[dict[str, object]], name: str) -> list[int]:
    values: list[int] = []
    for sample in samples:
        registers = sample["registers"]
        assert isinstance(registers, dict)
        if name in registers:
            values.append(int(registers[name]))
    return values


def monotonic_deltas(values: list[int]) -> list[int]:
    return [right - left for left, right in zip(values, values[1:])]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=pathlib.Path)
    parser.add_argument("--source-base", type=integer, default=0xffff0000)
    parser.add_argument("--source-size", type=integer, default=0x10000)
    parser.add_argument("--target-base", type=integer, default=0x040e0000)
    parser.add_argument("--target-size", type=integer, default=0x20000)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    arguments = parser.parse_args(argv)

    try:
        samples = parse(arguments.trace)
    except OSError as exc:
        print(f"analyze-ia64-hob-register-trace.py: {exc}", file=sys.stderr)
        return 1
    if not samples:
        print("analyze-ia64-hob-register-trace.py: no samples found",
              file=sys.stderr)
        return 1

    register_names = sorted(
        {
            name
            for sample in samples
            for name in sample["registers"]
        }
    )
    destination_matches: collections.Counter[str] = collections.Counter()
    source_matches: collections.Counter[str] = collections.Counter()
    target_matches: collections.Counter[str] = collections.Counter()
    for sample in samples:
        address = int(sample["address"])
        size = int(sample["size"])
        registers = sample["registers"]
        assert isinstance(registers, dict)
        for name, raw_value in registers.items():
            value = int(raw_value)
            if value in (address, address + size, address - size):
                destination_matches[name] += 1
            if (arguments.source_base <= value <
                    arguments.source_base + arguments.source_size):
                source_matches[name] += 1
            if (arguments.target_base <= value <
                    arguments.target_base + arguments.target_size):
                target_matches[name] += 1

    counters = []
    for name in register_names:
        values = values_for(samples, name)
        if len(values) < 3:
            continue
        deltas = monotonic_deltas(values)
        nonzero = [delta for delta in deltas if delta != 0]
        if not nonzero:
            continue
        all_increasing = all(delta >= 0 for delta in deltas)
        all_decreasing = all(delta <= 0 for delta in deltas)
        if not (all_increasing or all_decreasing):
            continue
        common_delta, common_count = collections.Counter(nonzero).most_common(1)[0]
        counters.append(
            {
                "register": name,
                "samples": len(values),
                "first": hex(values[0]),
                "last": hex(values[-1]),
                "direction": "increasing" if all_increasing else "decreasing",
                "common_delta": common_delta,
                "common_delta_count": common_count,
                "distinct_values": len(set(values)),
            }
        )
    counters.sort(
        key=lambda item: (
            -int(item["common_delta_count"]),
            -int(item["distinct_values"]),
            str(item["register"]),
        )
    )

    by_pc = collections.Counter(int(sample["pc"]) for sample in samples)
    report = {
        "schema": 1,
        "sample_count": len(samples),
        "source_window": {
            "base": hex(arguments.source_base),
            "size": hex(arguments.source_size),
        },
        "target_window": {
            "base": hex(arguments.target_base),
            "size": hex(arguments.target_size),
        },
        "writer_pcs": [
            {"pc": hex(pc), "samples": count}
            for pc, count in by_pc.most_common()
        ],
        "destination_pointer_candidates": destination_matches.most_common(),
        "source_pointer_candidates": source_matches.most_common(),
        "target_pointer_candidates": target_matches.most_common(),
        "monotonic_counter_candidates": counters,
        "first_samples": [
            {
                "hit": sample["hit"],
                "vcpu": sample["vcpu"],
                "pc": hex(int(sample["pc"])),
                "address": hex(int(sample["address"])),
                "size": sample["size"],
                "registers": {
                    name: hex(int(value))
                    for name, value in sample["registers"].items()
                },
            }
            for sample in samples[:32]
        ],
    }

    rendered = json.dumps(report, indent=2, sort_keys=True)
    print(rendered)
    if arguments.json is not None:
        arguments.json.write_text(rendered + "\n")
    if arguments.markdown is not None:
        lines = [
            "# IA-64 HOB writer register roles",
            "",
            f"- Samples: `{len(samples)}`",
            "",
            "## Pointer candidates",
            "",
            f"- Destination: `{destination_matches.most_common(8)}`",
            f"- Temporary-HOB source: `{source_matches.most_common(8)}`",
            f"- Permanent-HOB target: `{target_matches.most_common(8)}`",
            "",
            "## Monotonic candidates",
            "",
            "| Register | Direction | First | Last | Common delta | Hits |",
            "|---|---|---:|---:|---:|---:|",
        ]
        for item in counters[:16]:
            lines.append(
                "| `{register}` | {direction} | `{first}` | `{last}` | "
                "`{common_delta}` | {common_delta_count} |".format(**item)
            )
        arguments.markdown.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
