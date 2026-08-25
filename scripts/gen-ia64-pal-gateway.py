#!/usr/bin/env python3
"""Convert the assembled IA-64 PAL gateway into a checked-in C initializer."""

from __future__ import annotations

import argparse
from pathlib import Path

BUNDLE_SIZE = 16


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    data = args.input.read_bytes()
    if not data or len(data) % BUNDLE_SIZE:
        raise SystemExit(
            f"gateway must be a nonempty sequence of {BUNDLE_SIZE}-byte bundles; "
            f"got {len(data)} bytes"
        )
    if len(data) > 256:
        raise SystemExit(f"gateway unexpectedly large: {len(data)} bytes")

    lines = [
        "/* Generated from target/ia64/pal-gateway.S; do not edit. */",
        f"/* IA-64 bundles: {len(data) // BUNDLE_SIZE}; bytes: {len(data)}. */",
    ]
    for offset in range(0, len(data), BUNDLE_SIZE):
        chunk = data[offset : offset + BUNDLE_SIZE]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
