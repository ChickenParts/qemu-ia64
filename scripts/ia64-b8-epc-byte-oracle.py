#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""GNU IA-64 assembler oracle for unpredicated B8 epc (3 B-slot positions)."""
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


def assemble_text(slot: int, insn: str = "epc") -> str:
    if slot == 2:
        fields = [".mib", "nop.m 0", "nop.i 0", insn]
    elif slot == 1:
        fields = [".mbb", "nop.m 0", insn, "nop.b 0"]
    elif slot == 0:
        fields = [".bbb", insn, "nop.b 0", "nop.b 0"]
    else:
        raise ValueError("bad slot")
    return "\n".join([".text", ".explicit", ".align 16",
                      ".global _start", "_start:",
                      "{ " + fields[0], *fields[1:], ";;", "}", ""])


def run(out: Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    assembler = shlex.split(os.environ.get("IA64_AS", "ia64-linux-gnu-as"))
    objcopy = shlex.split(os.environ.get("IA64_OBJCOPY", "ia64-linux-gnu-objcopy"))
    records = []
    for slot in (0, 1, 2):
        source = out / f"epc-slot{slot}.S"
        obj = out / f"epc-slot{slot}.o"
        raw = out / f"epc-slot{slot}.bin"
        source.write_text(assemble_text(slot))
        subprocess.run(assembler + ["-o", str(obj), str(source)],
                       check=True, timeout=30)
        subprocess.run(objcopy + ["-O", "binary", "-j", ".text",
                                  str(obj), str(raw)], check=True, timeout=30)
        data = raw.read_bytes()
        if len(data) != 16:
            raise ValueError(f"slot {slot}: expected one 128-bit bundle")
        word = (int.from_bytes(data, "little") >> (5 + slot * 41)) & MASK41
        expected = 0x10 << 27
        if word != expected:
            raise ValueError(f"slot {slot}: GNU bytes {word:011x} != {expected:011x}")
        records.append({"slot": slot, "instruction": "epc", "bits41": f"{word:011x}",
                        "bundle_sha256": hashlib.sha256(data).hexdigest()})
    # Architectural restriction: no qp. An assembler rejection is evidence
    # of syntax illegality, not a substitute for the runtime raw-bit test.
    pred_source = out / "epc-predicated.S"
    pred_source.write_text(assemble_text(2, "(p5) epc"))
    pred_obj = out / "epc-predicated.o"
    cmd = subprocess.run(assembler + ["-o", str(pred_obj), str(pred_source)],
                         capture_output=True, text=True, timeout=30)
    if cmd.returncode == 0:
        raise ValueError("GNU assembler unexpectedly accepted qualified epc")
    record = {"reference": "Intel Itanium SDM Vol 3 rev 2.3 B8/epc, page 3:53",
              "assembler": assembler, "cases": len(records),
              "predicated_rejected": True, "results": records}
    (out / "summary.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"IA-64 B8 EPC GNU BYTE ORACLE PASS: {len(records)}/3, predicated rejected")
    return record


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-b8-epc-byte-oracle"))
    args = p.parse_args()
    run(args.out.resolve())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
