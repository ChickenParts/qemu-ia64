#!/usr/bin/env python3
"""Classify IA-64 HOB migration using the permanent PHIT write as activation."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys

LINE_RE = re.compile(
    r"IA64_HOB_DUAL hit=(?P<hit>\d+) region=(?P<region>temporary|permanent) "
    r"vcpu=(?P<vcpu>\d+) pc=(?P<pc>[0-9a-fA-F]+) "
    r"vaddr=(?P<vaddr>[0-9a-fA-F]+) size=(?P<size>\d+)"
)


def integer(text: str) -> int:
    return int(text, 0)


def parse(path: pathlib.Path) -> list[dict[str, int | str]]:
    events = []
    for line in path.read_text(errors="replace").splitlines():
        match = LINE_RE.search(line)
        if match is None:
            continue
        events.append(
            {
                "hit": int(match["hit"]),
                "region": match["region"],
                "vcpu": int(match["vcpu"]),
                "pc": int(match["pc"], 16),
                "address": int(match["vaddr"], 16),
                "size": int(match["size"]),
            }
        )
    return sorted(events, key=lambda event: int(event["hit"]))


def overlaps(event: dict[str, int | str], address: int, size: int) -> bool:
    event_address = int(event["address"])
    event_end = event_address + int(event["size"])
    return event_address < address + size and address < event_end


def serialise(event: dict[str, int | str]) -> dict[str, int | str]:
    return {
        **event,
        "pc": hex(int(event["pc"])),
        "address": hex(int(event["address"])),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=pathlib.Path)
    parser.add_argument("--temporary-hob-list", type=integer,
                        default=0xffff7000)
    parser.add_argument("--permanent-hob-list", type=integer,
                        default=0x040ef000)
    parser.add_argument("--phit-size", type=integer, default=56)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    arguments = parser.parse_args(argv)

    try:
        events = parse(arguments.trace)
    except OSError as exc:
        print(f"analyze-ia64-hob-transition.py: {exc}", file=sys.stderr)
        return 1
    if not events:
        print("analyze-ia64-hob-transition.py: no dual-region events found",
              file=sys.stderr)
        return 1

    permanent = [event for event in events if event["region"] == "permanent"]
    temporary = [event for event in events if event["region"] == "temporary"]
    phit_writes = [
        event for event in permanent
        if overlaps(event, arguments.permanent_hob_list, arguments.phit_size)
    ]
    activation = phit_writes[0] if phit_writes else None
    activation_hit = int(activation["hit"]) if activation is not None else None
    temporary_after = [
        event for event in temporary
        if activation_hit is not None and int(event["hit"]) > activation_hit
    ]
    temporary_hob_after = [
        event for event in temporary_after
        if int(event["address"]) >= arguments.temporary_hob_list
    ]

    if activation is None:
        classification = "permanent-phit-not-observed"
    elif temporary_hob_after:
        classification = "temporary-hob-writes-continue-after-permanent-phit"
    else:
        classification = "no-temporary-hob-writes-after-permanent-phit"

    temporary_pcs = collections.Counter(
        int(event["pc"]) for event in temporary_hob_after
    )
    permanent_phit_pcs = collections.Counter(
        int(event["pc"]) for event in phit_writes
    )
    report = {
        "schema": 1,
        "classification": classification,
        "event_count": len(events),
        "temporary_hob_list": hex(arguments.temporary_hob_list),
        "permanent_hob_list": hex(arguments.permanent_hob_list),
        "phit_size": arguments.phit_size,
        "activation_basis": "first permanent write overlapping the PHIT",
        "activation": serialise(activation) if activation is not None else None,
        "permanent_phit_write_count": len(phit_writes),
        "temporary_write_count": len(temporary),
        "temporary_writes_after_activation": len(temporary_after),
        "temporary_hob_writes_after_activation": len(temporary_hob_after),
        "top_post_activation_temporary_writers": [
            {"pc": hex(pc), "count": count}
            for pc, count in temporary_pcs.most_common(32)
        ],
        "top_permanent_phit_writers": [
            {"pc": hex(pc), "count": count}
            for pc, count in permanent_phit_pcs.most_common(32)
        ],
        "first_post_activation_temporary_hob_events": [
            serialise(event) for event in temporary_hob_after[:64]
        ],
        "first_permanent_phit_events": [
            serialise(event) for event in phit_writes[:64]
        ],
    }

    rendered = json.dumps(report, indent=2, sort_keys=True)
    print(rendered)
    if arguments.json is not None:
        arguments.json.write_text(rendered + "\n")
    if arguments.markdown is not None:
        lines = [
            "# IA-64 PHIT-based HOB migration transition",
            "",
            f"- Classification: **{classification}**",
            f"- Activation hit: `{activation_hit}`",
            f"- Permanent PHIT writes: `{len(phit_writes)}`",
            f"- Temporary-HOB writes after activation: "
            f"`{len(temporary_hob_after)}`",
            f"- Post-activation temporary writer PCs: "
            f"`{[hex(pc) for pc, _ in temporary_pcs.most_common(16)]}`",
            "",
        ]
        if classification == (
            "temporary-hob-writes-continue-after-permanent-phit"
        ):
            lines.append(
                "The permanent HOB handoff record exists, but guest code later "
                "continues writing the old temporary HOB arena.  Investigate "
                "HOB-list/PEI-services pointer conversion and IA-64 stack/RSE "
                "migration before copy-length hypotheses."
            )
        elif classification == (
            "no-temporary-hob-writes-after-permanent-phit"
        ):
            lines.append(
                "No writes to the old HOB list follow permanent PHIT creation.  "
                "Investigate the copied list extent/content and PHIT end/free "
                "pointers."
            )
        else:
            lines.append(
                "The trace did not observe creation of the expected permanent "
                "PHIT.  The permanent-list address or the earlier PEI path must "
                "be established first."
            )
        arguments.markdown.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
