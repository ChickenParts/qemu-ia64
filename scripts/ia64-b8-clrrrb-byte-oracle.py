#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent GNU IA-64 assembler byte oracle for B8 clrrrb/clrrrb.pr.

Extracts the 41-bit slot-2 instruction from complete 128-bit IA-64
bundles. Every sample ends its instruction group with an explicit stop.
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
FORMS = (("clrrrb-all", "clrrrb", 0x04),
         ("clrrrb-predicate", "clrrrb.pr", 0x05))
TEMPLATES = (".mib", ".mbb", ".bbb")


def source(spelling: str, template: str) -> str:
    if template == ".mib":
        slots = ("nop.m 0", "nop.i 0", spelling)
    elif template == ".mbb":
        slots = ("nop.m 0", "nop.b 0", spelling)
    elif template == ".bbb":
        slots = ("nop.b 0", "nop.b 0", spelling)
    else:
        raise ValueError(f"invalid template {template}")
    return "\n".join((".text", ".explicit", ".align 16", "_start:",
                      "{ " + template, *slots, ";;", "}", ""))


def run(out: Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    assembler = shlex.split(os.environ.get("IA64_AS", "ia64-linux-gnu-as"))
    objcopy = shlex.split(os.environ.get("IA64_OBJCOPY", "ia64-linux-gnu-objcopy"))
    rows = []
    for name, spelling, extension in FORMS:
        for template in TEMPLATES:
            case = name + "-" + template[1:]
            asm = out / (case + ".S")
            obj = out / (case + ".o")
            binfile = out / (case + ".bin")
            asm.write_text(source(spelling, template))
            subprocess.run(assembler + ["-o", str(obj), str(asm)],
                           check=True, timeout=30)
            subprocess.run(objcopy + ["-O", "binary", "-j", ".text",
                                      str(obj), str(binfile)], check=True, timeout=30)
            data = binfile.read_bytes()
            if len(data) != 16:
                raise ValueError(f"{case}: expected 16 bytes, observed {len(data)}")
            word = (int.from_bytes(data, "little") >> 87) & MASK41
            fields = {"major": (word >> 37) & 15,
                      "x6": (word >> 27) & 63,
                      "fixed_36_33": (word >> 33) & 15,
                      "fixed_26_6": (word >> 6) & 0x1fffff,
                      "qp": word & 63}
            want = {"major": 0, "x6": extension,
                    "fixed_36_33": 0, "fixed_26_6": 0, "qp": 0}
            if fields != want or word != extension << 27:
                raise ValueError(f"{case}: unexpected assembler word {word:011x}; "
                                 f"fields={fields!r}, expected={want!r}")
            rows.append({"case": case, "spelling": spelling,
                         "template": template, "slot": 2,
                         "bits41": f"{word:011x}",
                         "bundle_sha256": hashlib.sha256(data).hexdigest(),
                         "fields": fields})
    evidence = {
        "reference": "Intel Itanium Architecture SDM Vol 3 rev 2.3, B8, table 4-48",
        "assembler": assembler, "cases": len(rows), "results": rows,
        "notes": ["Both B8 instructions are unpredicated and must end a group.",
                  "Without a terminating stop, behavior is undefined in rev 2.3."]
    }
    (out / "summary.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"IA-64 B8 GNU BYTE ORACLE PASS: {len(rows)}/{len(FORMS)*len(TEMPLATES)}")
    return evidence


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-b8-byte-oracle"))
    args = p.parse_args()
    run(args.out.resolve())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
