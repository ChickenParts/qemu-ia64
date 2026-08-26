#!/usr/bin/env python3
"""Report firmware-shaped and magic-PC logic in IA-64 implementation code."""
from __future__ import annotations

import argparse
import json
import pathlib
import re

RULES = [
    ("firmware-identity", re.compile(r"(?i)\b(?:xen|sdv|flash\.fd|ski)\b"),
     "Firmware identity must not select architectural behaviour."),
    ("magic-pc", re.compile(
        r"(?i)\b(?:pc|ip|iip|cr_iip)\b[^\n;]{0,96}(?:==|!=)\s*0x[0-9a-f]{4,}"),
     "Program-counter-specific behaviour usually indicates a trace-shaped workaround."),
    ("host-delay", re.compile(r"\b(?:g_usleep|usleep|nanosleep|sleep)\s*\("),
     "Host delays are not a substitute for modeled device or timer semantics."),
    ("hack-language", re.compile(
        r"(?i)\b(?:hack|kludge|work\s*around|workaround|firmware quirk|boot quirk)\b"),
     "Review and replace behavioral workarounds with a documented contract."),
    ("forced-success", re.compile(
        r"(?is)\b(?:unimplemented|unsupported|todo|fixme)\b.{0,180}"
        r"\breturn\s+(?:0|PAL_STATUS_SUCCESS)\s*;"),
     "An unimplemented architectural operation must not silently report success."),
]
SUFFIXES = {".c", ".h", ".inc"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--fail", action="store_true",
                        help="return nonzero when findings exist")
    args = parser.parse_args()

    findings = []
    for base in ("target/ia64", "hw/ia64", "include/hw/ia64"):
        root = args.root / base
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for rule, regex, rationale in RULES:
                for match in regex.finditer(text):
                    line = text.count("\n", 0, match.start()) + 1
                    start = text.rfind("\n", 0, match.start()) + 1
                    end = text.find("\n", match.end())
                    if end < 0:
                        end = len(text)
                    findings.append({
                        "rule": rule,
                        "path": path.relative_to(args.root).as_posix(),
                        "line": line,
                        "excerpt": text[start:end].strip()[:240],
                        "rationale": rationale,
                    })

    result = {"schema": 1, "finding_count": len(findings),
              "findings": findings}
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 1 if args.fail and findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
