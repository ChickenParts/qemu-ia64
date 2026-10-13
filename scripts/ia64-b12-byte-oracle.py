#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent GNU IA-64 byte oracle and isolated mutation for B1/B2."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("b4_bytes_for_b12",
                                              ROOT / "scripts/ia64-b4-byte-oracle.py")
b4 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b4)
assemble_bundle = b4.assemble_bundle
extract_word = b4.extract_word
replace_word = b4.replace_word
fields = b4.fields

# Six assigned Intel B1/B2 btypes; three genuine B slot placements.
CASES = (
    ("cond-b0", "br.cond.sptk.few _start", 0, 0, 0),
    ("wexit-b1", "br.wexit.spnt.many _start", 1, 2, 0),
    ("wtop-b2", "(p6) br.wtop.dptk.few _start", 2, 3, 6),
    ("cloop-b0", "br.cloop.sptk.few _start", 0, 5, 0),
    ("cexit-b1", "br.cexit.spnt.many _start", 1, 6, 0),
    ("ctop-b2", "br.ctop.dpnt.few _start", 2, 7, 0),
)

def change_fields(data: bytes, slot: int, *, btype=None, qp=None) -> bytes:
    before = extract_word(data, slot)
    word = before
    if btype is not None:
        word = (word & ~(7 << 6)) | ((btype & 7) << 6)
    if qp is not None:
        word = (word & ~0x3f) | (qp & 0x3f)
    after = replace_word(data, slot, word)
    delta = int.from_bytes(after, "little") ^ int.from_bytes(data, "little")
    if delta != ((before ^ word) << (5 + 41 * slot)):
        raise ValueError("field mutation affected other bundle bits")
    return after

def run(out: Path):
    out.mkdir(parents=True, exist_ok=True)
    positive = []
    for name, asm, slot, btype, qp in CASES:
        raw = assemble_bundle(asm, slot, out, name)
        word = extract_word(raw, slot)
        got = fields(word)
        for key, val in {"major": 4, "btype": btype, "qp": qp}.items():
            if got[key] != val:
                raise ValueError(f"{name} {key}: expected {val}, got {got[key]}")
        positive.append({"name": name, "asm": asm, "slot": slot,
                         "bits41": f"{word:011x}", "fields": got,
                         "sha256": hashlib.sha256(raw).hexdigest()})
    cond = assemble_bundle("br.cond.sptk.few _start", 0, out, "negative-cond-base")
    b2 = assemble_bundle("br.cloop.sptk.few _start", 2, out, "negative-b2-base")
    negative = []
    for btype in (1, 4):
        changed = change_fields(cond, 0, btype=btype)
        negative.append({"reserved_btype": btype,
                         "delta": f"{extract_word(changed, 0)^extract_word(cond, 0):011x}"})
    for qp in (1, 6):
        changed = change_fields(b2, 2, qp=qp)
        negative.append({"counted_b2_qp": qp,
                         "delta": f"{extract_word(changed, 2)^extract_word(b2, 2):011x}"})
    report = {"source": "Intel SDM Vol 3 rev 2.3 Table 4-47/B1/B2",
              "positives": positive, "negatives": negative}
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print("IA-64 B1/B2 GNU BYTE ORACLE PASS: 6/6 assigned + 4 field mutations")

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-b12-oracle"))
    run(ap.parse_args().out.resolve())

if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
