#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""GNU IA-64 byte oracle for B5 indirect calls and reserved wh mutations."""
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
CASES = (
    ("sptk-b0", "br.call.sptk.many b1=b2", 0, 1, 1, 2),
    ("spnt-b1", "br.call.spnt.few b3=b4", 1, 3, 3, 4),
    ("dptk-b2", "br.call.dptk.many b5=b6", 2, 5, 5, 6),
    ("dpnt-b2", "br.call.dpnt.few b7=b0", 2, 7, 7, 0),
)


def source(spelling: str, slot: int) -> str:
    if slot == 0:
        fields = (".bbb", spelling, "nop.b 0", "nop.b 0")
    elif slot == 1:
        fields = (".mbb", "nop.m 0", spelling, "nop.b 0")
    elif slot == 2:
        fields = (".mib", "nop.m 0", "nop.i 0", spelling)
    else:
        raise ValueError("invalid B slot")
    return "\n".join((".text", ".explicit", ".align 16", ".global _start",
                      "_start:", "{ " + fields[0], *fields[1:], ";;", "}", ""))


def extract_word(bundle: bytes, slot: int) -> int:
    if len(bundle) != 16:
        raise ValueError(f"expected 16-byte bundle, got {len(bundle)}")
    return (int.from_bytes(bundle, "little") >> (5 + 41 * slot)) & MASK41


def replace_word(bundle: bytes, slot: int, word: int) -> bytes:
    shift = 5 + 41 * slot
    value = int.from_bytes(bundle, "little")
    value = (value & ~(MASK41 << shift)) | ((word & MASK41) << shift)
    return value.to_bytes(16, "little")


def mutate_wh(bundle: bytes, slot: int, wh: int) -> bytes:
    word = extract_word(bundle, slot)
    word = (word & ~(7 << 32)) | ((wh & 7) << 32)
    return replace_word(bundle, slot, word)


def assemble(spelling: str, slot: int, out: Path, stem: str) -> bytes:
    assembler = shlex.split(os.environ.get("IA64_AS", "ia64-linux-gnu-as"))
    objcopy = shlex.split(os.environ.get(
        "IA64_OBJCOPY", "ia64-linux-gnu-objcopy"))
    out.mkdir(parents=True, exist_ok=True)
    asm, obj, raw = out / f"{stem}.S", out / f"{stem}.o", out / f"{stem}.bin"
    asm.write_text(source(spelling, slot))
    subprocess.run(assembler + ["-o", str(obj), str(asm)], check=True, timeout=30)
    subprocess.run(objcopy + ["-O", "binary", "-j", ".text",
                              str(obj), str(raw)], check=True, timeout=30)
    return raw.read_bytes()


def fields(word: int) -> dict[str, int]:
    return {"major": (word >> 37) & 15, "d": (word >> 35) & 1,
            "wh": (word >> 32) & 7, "b2": (word >> 13) & 7,
            "p": (word >> 12) & 1, "b1": (word >> 6) & 7,
            "qp": word & 0x3f}


def run(out: Path) -> dict:
    rows = []
    bases = {}
    for name, spelling, slot, wh, b1, b2 in CASES:
        data = assemble(spelling, slot, out, name)
        word = extract_word(data, slot)
        got = fields(word)
        expected = {"major": 1, "wh": wh, "b1": b1, "b2": b2, "qp": 0}
        for key, value in expected.items():
            if got[key] != value:
                raise ValueError(f"{name}: {key}={got[key]} expected {value}; word={word:011x}")
        bases[wh] = (data, slot)
        rows.append({"name": name, "assembly": spelling, "slot": slot,
                     "bits41": f"{word:011x}", "fields": got,
                     "bundle_sha256": hashlib.sha256(data).hexdigest()})

    mutations = []
    for even, odd in ((0, 1), (2, 3), (4, 5), (6, 7)):
        data, slot = bases[odd]
        before = extract_word(data, slot)
        mutated_data = mutate_wh(data, slot, even)
        after = extract_word(mutated_data, slot)
        if (before ^ after) != ((odd ^ even) << 32):
            raise ValueError(f"wh={even}: mutation changed non-wh fields")
        got = fields(after)
        if got["major"] != 1 or got["wh"] != even:
            raise ValueError(f"wh={even}: malformed reserved fixture")
        mutations.append({"wh": even, "slot": slot,
                          "base_wh": odd, "bits41": f"{after:011x}",
                          "bundle_sha256": hashlib.sha256(mutated_data).hexdigest()})

    report = {"reference": "Intel Itanium SDM Vol 3 rev 2.3 B5/Table 4-53",
              "positive_cases": len(rows), "reserved_mutations": mutations,
              "results": rows}
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print("IA-64 B5 GNU BYTE ORACLE PASS: 4/4 assigned + 4 reserved wh mutations")
    return report


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-b5-byte-oracle"))
    args = p.parse_args()
    run(args.out.resolve())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
