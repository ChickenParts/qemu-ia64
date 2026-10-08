#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent GNU assembler byte oracle for IA-64 B9 break/nop/hint.

Extract actual 41-bit B-slot encodings from binary IA-64 bundles, not
an interpreter copy of QEMU's production decoder.
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
CASE_DEFS = [
    ("break-zero", "break.b 0", 2, 0, 0x00, 0, 0),
    ("break-full", "break.b 0x1abcde", 2, 0, 0x00, 0x1abcde, 0),
    ("break-pred", "(p5) break.b 0x55", 2, 0, 0x00, 0x55, 5),
    ("break-slot1", "break.b 0x12345", 1, 0, 0x00, 0x12345, 0),
    ("break-slot0", "break.b 0x345", 0, 0, 0x00, 0x345, 0),
    ("nop-zero", "nop.b 0", 2, 2, 0x00, 0, 0),
    ("nop-full", "nop.b 0x12345", 2, 2, 0x00, 0x12345, 0),
    ("hint-zero", "hint.b 0", 2, 2, 0x01, 0, 0),
    ("hint-full", "hint.b 0x1abcde", 2, 2, 0x01, 0x1abcde, 0),
]


def bundle(insn: str, slot: int) -> str:
    if slot == 2:
        body = [".mib", "nop.m 0", "nop.i 0", insn]
    elif slot == 1:
        body = [".mbb", "nop.m 0", insn, "nop.b 0"]
    elif slot == 0:
        body = [".bbb", insn, "nop.b 0", "nop.b 0"]
    else:
        raise ValueError("invalid B9 slot")
    return "\n".join([
        ".text", ".explicit", ".align 16", ".global _start", "_start:",
        "{ " + body[0], *body[1:], ";;", "}", "",
    ])


def run(out: Path) -> dict:
    # GNU binutils rejects hint.b by default; test the architectural word.
    as_bin = shlex.split(os.environ.get("IA64_AS", "ia64-linux-gnu-as -mhint.b=ok"))
    objcopy = shlex.split(os.environ.get("IA64_OBJCOPY", "ia64-linux-gnu-objcopy"))
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, instr, slot, major, x6, imm, qp in CASE_DEFS:
        src = out / (name + ".S")
        obj = out / (name + ".o")
        raw = out / (name + ".bin")
        src.write_text(bundle(instr, slot))
        subprocess.run(as_bin + ["-o", str(obj), str(src)], check=True, timeout=30)
        subprocess.run(objcopy + ["-O", "binary", "-j", ".text",
                                  str(obj), str(raw)], check=True, timeout=30)
        data = raw.read_bytes()
        if len(data) != 16:
            raise ValueError(f"{name}: expected one 16-byte bundle, got {len(data)}")
        encoded = int.from_bytes(data, "little")
        insn = (encoded >> (5 + slot * 41)) & MASK41
        fields = {
            "major": (insn >> 37) & 15,
            "x6": (insn >> 27) & 63,
            "imm": ((insn >> 36) & 1) << 20 | ((insn >> 6) & 0xfffff),
            "qp": insn & 63,
        }
        expected = {"major": major, "x6": x6, "imm": imm, "qp": qp}
        if fields != expected:
            raise ValueError(
                f"{name}: assembler bytes did not match B9 fields: "
                f"observed {fields!r}; expected {expected!r}"
            )
        if (name == "break-zero") != (insn == 0):
            raise ValueError(
                f"{name}: the zero B-slot should be break.b 0 only"
            )
        rows.append({
            "case": name,
            "instruction": instr,
            "slot": slot,
            "bits41": f"{insn:011x}",
            "bundle_sha256": hashlib.sha256(data).hexdigest(),
            "fields": fields,
        })
    evidence = {
        "reference": "Intel Itanium Architecture Software Developer Manual "
                     "vol 3 rev 2.3, B9",
        "assembler": as_bin,
        "cases": len(rows),
        "results": rows,
    }
    (out / "summary.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"IA-64 B9 BYTE ORACLE PASS: {len(rows)}/{len(CASE_DEFS)}")
    return evidence


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("scratch/ia64-b9-oracle"))
    args = parser.parse_args()
    run(args.out.resolve())


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
