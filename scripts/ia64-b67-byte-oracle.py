#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent GNU assembler byte oracle for IA-64 B6/B7 branch prediction.

This is an encoding/form oracle, not an instruction-semantics oracle. All
inputs are assembled by GNU IA-64 as, rather than synthesizing QEMU's bits.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

MASK41 = (1 << 41) - 1

# name, assembly, slot, major, x6 (or None), wh, ih, b2 (or None)
CASES = [
    ("b6-sptk", "brp.sptk target, tag", 2, 7, None, 0, 0, None),
    ("b6-dptk", "brp.dptk target, tag", 1, 7, None, 2, 0, None),
    ("b6-sptk-imp", "brp.sptk.imp target, tag", 0, 7, None, 0, 1, None),
    ("b6-dptk-imp", "brp.dptk.imp target, tag", 2, 7, None, 2, 1, None),
    ("b6-loop-imp", "brp.loop.imp target, tag", 2, 7, None, 1, 1, None),
    ("b6-exit-imp", "brp.exit.imp target, tag", 2, 7, None, 3, 1, None),
    ("b7-sptk", "brp.sptk b2, tag", 2, 2, 0x10, 0, 0, 2),
    ("b7-dptk", "brp.dptk b3, tag", 1, 2, 0x10, 2, 0, 3),
    ("b7-sptk-imp", "brp.sptk.imp b4, tag", 2, 2, 0x10, 0, 1, 4),
    ("b7-dptk-imp", "brp.dptk.imp b5, tag", 2, 2, 0x10, 2, 1, 5),
    ("b7-ret-sptk", "brp.ret.sptk b6, tag", 0, 2, 0x11, 0, 0, 6),
    ("b7-ret-dptk", "brp.ret.dptk b7, tag", 2, 2, 0x11, 2, 0, 7),
    ("b7-ret-sptk-imp", "brp.ret.sptk.imp b0, tag", 2, 2, 0x11, 0, 1, 0),
    ("b7-ret-dptk-imp", "brp.ret.dptk.imp b1, tag", 2, 2, 0x11, 2, 1, 1),
]


def emit_bundle(slot: int, insn: str) -> list[str]:
    if slot == 2:
        fields = [".mib", "nop.m 0", "nop.i 0", insn]
    elif slot == 1:
        fields = [".mbb", "nop.m 0", insn, "nop.b 0"]
    elif slot == 0:
        fields = [".bbb", insn, "nop.b 0", "nop.b 0"]
    else:
        raise ValueError("invalid B-unit slot")
    return ["{ " + fields[0], *fields[1:], ";;", "}"]


def source(insn: str, slot: int) -> str:
    # tag is one bundle after the hint, and target is two bundles after it.
    # Thus both assembler relocations have real, independently checkable
    # nonzero displacements rather than degenerate all-zero immediates.
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    lines += emit_bundle(slot, insn)
    lines += ["tag:"]
    lines += emit_bundle(2, "nop.b 0")
    lines += ["target:"]
    lines += emit_bundle(2, "nop.b 0")
    return "\n".join(lines) + "\n"


def run(out: Path) -> dict:
    as_bin = shlex.split(os.environ.get("IA64_AS", "ia64-linux-gnu-as"))
    objcopy = shlex.split(os.environ.get("IA64_OBJCOPY", "ia64-linux-gnu-objcopy"))
    out.mkdir(parents=True, exist_ok=True)
    observations = []
    for name, spelling, slot, major, x6, wh, ih, b2 in CASES:
        src = out / (name + ".S")
        obj = out / (name + ".o")
        raw = out / (name + ".bin")
        src.write_text(source(spelling, slot))
        subprocess.run(as_bin + ["-o", str(obj), str(src)], check=True, timeout=30)
        subprocess.run(objcopy + ["-O", "binary", "-j", ".text",
                                  str(obj), str(raw)], check=True, timeout=30)
        data = raw.read_bytes()
        if len(data) != 48:
            raise ValueError(f"{name}: expected three IA-64 bundles, got {len(data)} bytes")
        encoded = int.from_bytes(data[:16], "little")
        word = (encoded >> (5 + slot * 41)) & MASK41
        fields = {
            "major": (word >> 37) & 15,
            "wh": (word >> 3) & 3,
            "ih": (word >> 35) & 1,
            "timm9": (((word >> 33) & 3) << 7) | ((word >> 6) & 127),
            "fixed_low": word & 0x27,
        }
        expected = {"major": major, "wh": wh, "ih": ih,
                    "timm9": 1, "fixed_low": 0}
        if major == 7:
            fields["target_imm21"] = ((word >> 36) & 1) << 20 | (
                (word >> 13) & 0xfffff)
            expected["target_imm21"] = 2
        else:
            fields["x6"] = (word >> 27) & 63
            fields["b2"] = (word >> 13) & 7
            fields["fixed_mid"] = (word >> 16) & 0x7ff
            fields["fixed_36"] = (word >> 36) & 1
            expected.update({"x6": x6, "b2": b2,
                             "fixed_mid": 0, "fixed_36": 0})
        if fields != expected:
            raise ValueError(f"{name}: GNU bytes {word:011x}: "
                             f"fields={fields!r}; expected={expected!r}")
        observations.append({
            "name": name, "assembly": spelling, "slot": slot,
            "bits41": f"{word:011x}",
            "bundle_sha256": hashlib.sha256(data).hexdigest(),
            "fields": fields,
        })
    summary = {
        "reference": "Intel Itanium SDM Vol 3 rev 2.3, B6/B7, tables 4-55..58",
        "assembler": as_bin,
        "cases": len(observations),
        "notes": [
            "B6 .loop/.exit without .imp have undefined hint effect "
            "and are not positive-oracle claims",
            "QEMU does not emulate microarchitectural branch prediction",
            "Each oracle word is extracted from GNU IA-64 binary bundles",
        ],
        "results": observations,
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"IA-64 B6/B7 GNU BYTE ORACLE PASS: {len(observations)}/{len(CASES)}")
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("scratch/ia64-b67-oracle"))
    args = parser.parse_args()
    run(args.out.resolve())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
