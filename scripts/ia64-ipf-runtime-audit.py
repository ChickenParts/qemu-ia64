#!/usr/bin/env python3
"""Combine static IPF.c migration evidence with a realized QOM snapshot."""
from __future__ import annotations

import argparse
import json
import pathlib
import re
from collections import Counter
from typing import Any, Iterator


def qom_nodes(node: dict[str, Any]) -> Iterator[dict[str, Any]]:
    yield node
    for child in node.get("children", []):
        yield from qom_nodes(child)


def terms(value: str) -> set[str]:
    result = {part.lower() for part in re.split(r"[^A-Za-z0-9]+", value)
              if len(part) >= 3}
    aliases = {
        "serial": {"uart", "16550", "serial"},
        "rtc": {"rtc", "mc146818"},
        "ide": {"ide", "ata", "piix"},
        "ethernet": {"ethernet", "nic", "network"},
        "vga": {"vga", "display"},
        "usb": {"usb", "uhci", "ohci", "ehci", "xhci"},
        "pci": {"pci", "pcie"},
        "sapic": {"sapic", "iosapic", "interrupt"},
    }
    expanded = set(result)
    for term in tuple(result):
        expanded |= aliases.get(term, set())
    return expanded


def node_text(node: dict[str, Any]) -> str:
    chunks = [str(node.get("path", ""))]
    for prop in node.get("properties", []):
        chunks.append(str(prop.get("name", "")))
        chunks.append(str(prop.get("type", "")))
    return " ".join(chunks).lower()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--static", required=True, type=pathlib.Path,
                        help="JSON from ia64-ipf-audit.py")
    parser.add_argument("--topology", required=True, type=pathlib.Path,
                        help="JSON from ia64-machine-topology.py")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    static = json.loads(args.static.read_text(encoding="utf-8"))
    topology = json.loads(args.topology.read_text(encoding="utf-8"))
    nodes = list(qom_nodes(topology["qom"]))
    indexed = [(node, node_text(node)) for node in nodes]

    rows = []
    for obligation in static["obligations"]:
        wanted = terms(obligation["value"])
        runtime_hits = []
        if wanted:
            for node, text in indexed:
                if any(term in text for term in wanted):
                    runtime_hits.append(node.get("path"))
        source_status = obligation["status"]
        if runtime_hits:
            status = "runtime-evidence"
        elif source_status == "machine-wired":
            status = "source-only"
        elif source_status == "model-available":
            status = "model-only"
        else:
            status = "unresolved"
        rows.append({**obligation, "runtime_status": status,
                     "runtime_paths": sorted(set(runtime_hits))})

    counts = Counter(row["runtime_status"] for row in rows)
    total = len(rows)
    result = {
        "schema": 1,
        "warning": (
            "A QOM name match is runtime evidence, not proof of register, IRQ, "
            "DMA, reset, or firmware-enumeration correctness."),
        "legacy": static["legacy"],
        "summary": {
            "obligations": total,
            "runtime_status": dict(sorted(counts.items())),
            "runtime_evidence_percent":
                round(100.0 * counts["runtime-evidence"] / total, 1)
                if total else 100.0,
        },
        "obligations": rows,
    }
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
