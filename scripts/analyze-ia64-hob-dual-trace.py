#!/usr/bin/env python3
"""Analyze the global write timeline for temporary/permanent PEI HOB arenas."""

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


def parse(path: pathlib.Path) -> list[dict[str, int | str]]:
    events: list[dict[str, int | str]] = []
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
    events.sort(key=lambda event: int(event["hit"]))
    return events


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    arguments = parser.parse_args(argv)

    try:
        events = parse(arguments.trace)
    except OSError as exc:
        print(f"analyze-ia64-hob-dual-trace.py: {exc}", file=sys.stderr)
        return 1
    if not events:
        print("analyze-ia64-hob-dual-trace.py: no dual-region events found",
              file=sys.stderr)
        return 1

    temporary = [event for event in events if event["region"] == "temporary"]
    permanent = [event for event in events if event["region"] == "permanent"]
    first_permanent_hit = (
        int(permanent[0]["hit"]) if permanent else None
    )
    temporary_after_permanent = [
        event for event in temporary
        if first_permanent_hit is not None and
        int(event["hit"]) > first_permanent_hit
    ]
    permanent_after_activation = [
        event for event in permanent
        if first_permanent_hit is not None and
        int(event["hit"]) >= first_permanent_hit
    ]

    temporary_pcs = collections.Counter(int(event["pc"]) for event in temporary)
    permanent_pcs = collections.Counter(int(event["pc"]) for event in permanent)
    cross_region_pcs = sorted(set(temporary_pcs) & set(permanent_pcs))

    if not permanent:
        classification = "permanent-arena-never-written"
    elif temporary_after_permanent:
        classification = "temporary-writes-continue-after-permanent-activation"
    elif temporary and permanent:
        classification = "one-way-temporary-to-permanent-transition"
    else:
        classification = "permanent-only-observation"

    report = {
        "schema": 1,
        "classification": classification,
        "event_count": len(events),
        "temporary_event_count": len(temporary),
        "permanent_event_count": len(permanent),
        "first_temporary_hit": (
            int(temporary[0]["hit"]) if temporary else None
        ),
        "last_temporary_hit": (
            int(temporary[-1]["hit"]) if temporary else None
        ),
        "first_permanent_hit": first_permanent_hit,
        "last_permanent_hit": (
            int(permanent[-1]["hit"]) if permanent else None
        ),
        "temporary_events_after_permanent_activation": len(
            temporary_after_permanent
        ),
        "permanent_events_after_activation": len(permanent_after_activation),
        "cross_region_writer_pcs": [hex(pc) for pc in cross_region_pcs],
        "top_temporary_writers": [
            {"pc": hex(pc), "count": count}
            for pc, count in temporary_pcs.most_common(32)
        ],
        "top_permanent_writers": [
            {"pc": hex(pc), "count": count}
            for pc, count in permanent_pcs.most_common(32)
        ],
        "first_permanent_events": [
            {
                **event,
                "pc": hex(int(event["pc"])),
                "address": hex(int(event["address"])),
            }
            for event in permanent[:32]
        ],
        "first_temporary_events_after_permanent": [
            {
                **event,
                "pc": hex(int(event["pc"])),
                "address": hex(int(event["address"])),
            }
            for event in temporary_after_permanent[:64]
        ],
    }

    rendered = json.dumps(report, indent=2, sort_keys=True)
    print(rendered)
    if arguments.json is not None:
        arguments.json.write_text(rendered + "\n")
    if arguments.markdown is not None:
        lines = [
            "# IA-64 temporary/permanent HOB write timeline",
            "",
            f"- Classification: **{classification}**",
            f"- Temporary writes: `{len(temporary)}`",
            f"- Permanent writes: `{len(permanent)}`",
            f"- First permanent hit: `{first_permanent_hit}`",
            f"- Temporary writes after that hit: `{len(temporary_after_permanent)}`",
            f"- PCs observed in both arenas: `{[hex(pc) for pc in cross_region_pcs]}`",
            "",
            "## Interpretation",
            "",
        ]
        if classification == "temporary-writes-continue-after-permanent-activation":
            lines.append(
                "The firmware continues mutating the temporary arena after the "
                "permanent arena has become active.  This supports a stale or "
                "unconverted HOB-list pointer, or a migration snapshot taken "
                "before later FV HOB creation; it is not explained by a single "
                "short copy alone."
            )
        elif classification == "one-way-temporary-to-permanent-transition":
            lines.append(
                "Writes transition cleanly to permanent memory.  Investigate "
                "the migration copy extent/content and the target list's end "
                "pointer rather than a lingering temporary-list producer."
            )
        else:
            lines.append(
                "The observed window did not capture both sides of the expected "
                "migration; widen or retime the trace before drawing a causal "
                "conclusion."
            )
        arguments.markdown.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
