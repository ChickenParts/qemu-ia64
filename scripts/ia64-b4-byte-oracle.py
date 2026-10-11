#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent GNU IA-64 byte oracle for assigned B4 indirect branches.

Positive words always come from GNU as.  Reserved runtime fixtures are derived
by changing only B4 btype/qp bits in those complete GNU-produced bundles.
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
CASES = (
    ("cond-b0", "br.cond.sptk.few b2", 0, 0x20, 0, 2),
    ("ia-b1", "br.ia.dptk.many b3", 1, 0x20, 1, 3),
    ("ret-b2", "br.ret.spnt.few b4", 2, 0x21, 4, 4),
)


def bundle_source(spelling: str, slot: int) -> str:
    if slot == 0:
        fields = (".bbb", spelling, "nop.b 0", "nop.b 0")
    elif slot == 1:
        fields = (".mbb", "nop.m 0", spelling, "nop.b 0")
    elif slot == 2:
        fields = (".mib", "nop.m 0", "nop.i 0", spelling)
    else:
        raise ValueError(f"invalid B slot {slot}")
    return "\n".join((".text", ".explicit", ".align 16", ".global _start",
                      "_start:", "{ " + fields[0], *fields[1:], ";;", "}", ""))


def extract_word(bundle: bytes, slot: int) -> int:
    if len(bundle) != 16:
        raise ValueError(f"expected one IA-64 bundle, got {len(bundle)} bytes")
    return (int.from_bytes(bundle, "little") >> (5 + slot * 41)) & MASK41


def replace_word(bundle: bytes, slot: int, word: int) -> bytes:
    encoded = int.from_bytes(bundle, "little")
    shift = 5 + slot * 41
    encoded &= ~(MASK41 << shift)
    encoded |= (word & MASK41) << shift
    return encoded.to_bytes(16, "little")


def mutate_b4(bundle: bytes, slot: int, *, btype: int | None = None,
              qp: int | None = None) -> bytes:
    word = extract_word(bundle, slot)
    if btype is not None:
        word = (word & ~(7 << 6)) | ((btype & 7) << 6)
    if qp is not None:
        word = (word & ~0x3f) | (qp & 0x3f)
    return replace_word(bundle, slot, word)


def assemble_bundle(spelling: str, slot: int, out: Path, stem: str) -> bytes:
    assembler = shlex.split(os.environ.get("IA64_AS", "ia64-linux-gnu-as"))
    objcopy = shlex.split(os.environ.get(
        "IA64_OBJCOPY", "ia64-linux-gnu-objcopy"))
    out.mkdir(parents=True, exist_ok=True)
    source = out / f"{stem}.S"
    obj = out / f"{stem}.o"
    raw = out / f"{stem}.bin"
    source.write_text(bundle_source(spelling, slot))
    subprocess.run(assembler + ["-o", str(obj), str(source)],
                   check=True, timeout=30)
    subprocess.run(objcopy + ["-O", "binary", "-j", ".text",
                              str(obj), str(raw)], check=True, timeout=30)
    data = raw.read_bytes()
    if len(data) != 16:
        raise ValueError(f"{stem}: expected 16 bytes, observed {len(data)}")
    return data


def fields(word: int) -> dict[str, int]:
    return {
        "major": (word >> 37) & 0xf,
        "x6": (word >> 27) & 0x3f,
        "btype": (word >> 6) & 7,
        "b2": (word >> 13) & 7,
        "qp": word & 0x3f,
        "pa": (word >> 12) & 1,
        "wha": (word >> 33) & 3,
        "d": (word >> 35) & 1,
    }


def run(out: Path) -> dict:
    rows = []
    for name, spelling, slot, x6, btype, b2 in CASES:
        data = assemble_bundle(spelling, slot, out, name)
        word = extract_word(data, slot)
        got = fields(word)
        expected = {"major": 0, "x6": x6, "btype": btype,
                    "b2": b2, "qp": 0}
        for key, value in expected.items():
            if got[key] != value:
                raise ValueError(
                    f"{name}: GNU word {word:011x} {key}={got[key]} != {value}")
        rows.append({"name": name, "assembly": spelling, "slot": slot,
                     "bits41": f"{word:011x}", "fields": got,
                     "bundle_sha256": hashlib.sha256(data).hexdigest()})

    # GNU's B0 opcode class must reject qualification of unpredicated br.ia.
    assembler = shlex.split(os.environ.get("IA64_AS", "ia64-linux-gnu-as"))
    pred = out / "predicated-br-ia.S"
    pred.write_text(bundle_source("(p6) br.ia.sptk.few b3", 2))
    command = subprocess.run(assembler + ["-o", str(out / "predicated-br-ia.o"),
                                          str(pred)],
                             capture_output=True, text=True, timeout=30)
    if command.returncode == 0:
        raise ValueError("GNU assembler unexpectedly accepted predicated br.ia")

    # Prove the reserved fixture builder changes no template/operand field
    # except the specifically requested btype or qp bits.
    base = assemble_bundle("br.cond.sptk.few b2", 0, out, "mutation-base")
    base_word = extract_word(base, 0)
    reserved = extract_word(mutate_b4(base, 0, btype=7), 0)
    qualified_ia_base = assemble_bundle("br.ia.sptk.few b3", 1, out,
                                        "qualified-ia-base")
    ia_word = extract_word(qualified_ia_base, 1)
    qualified = extract_word(mutate_b4(qualified_ia_base, 1, qp=6), 1)
    if (base_word ^ reserved) != (7 << 6):
        raise ValueError("reserved mutation changed fields outside btype")
    if (ia_word ^ qualified) != 6:
        raise ValueError("br.ia mutation changed fields outside qp")

    report = {
        "reference": "Intel Itanium SDM Vol 3 rev 2.3, B4 tables 4-49/4-50",
        "cases": len(rows), "predicated_br_ia_rejected": True,
        "mutation_proofs": {
            "reserved_btype_xor": f"{base_word ^ reserved:011x}",
            "qualified_ia_qp_xor": f"{ia_word ^ qualified:011x}",
        },
        "results": rows,
    }
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print("IA-64 B4 GNU BYTE ORACLE PASS: 3/3; predicated br.ia rejected")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path,
                        default=Path("scratch/ia64-b4-byte-oracle"))
    args = parser.parse_args()
    run(args.out.resolve())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
