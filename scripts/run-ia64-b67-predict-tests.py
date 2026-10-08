#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real qemu-system-ia64 architectural no-effect checks for B6/B7 brp.

Uses independently assembled B6/B7 positive spellings in B slots 0/1/2;
the same IVT harness as the F10/F12/B9 exception regressions supplies
firmware-free execution, not a copied Python translator.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def load(filename: str, module_name: str):
    spec = importlib.util.spec_from_file_location(module_name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ivt = load("run-ia64-fp-exception-tests.py", "ia64_ivt_for_b67")
oracle = load("ia64-b67-byte-oracle.py", "ia64_b67_byte_oracle")
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare
PROFILES = {entry[0]: entry for entry in oracle.CASES}

IVT_OFFSET = 0x400000
BREAK_VECTOR = 0x2c00
PASS = "0x62363770"
FAIL = "0x62363766"


def generate(name: str) -> str:
    if name not in PROFILES:
        raise ValueError(f"unknown B6/B7 profile {name}")
    _, instruction, slot, major, _, _, _, b2 = PROFILES[name]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)  # no IVT handler should execute

    # A real branch register input and sentinel confirm that B7 predicts
    # rather than actually branching or corrupting the named register.
    if b2 is not None:
        literal(lines, 20, "0x1230")
        emit(lines, "i", f"mov b{b2}=r20")
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")

    lines.append("prediction_bundle:")
    lines += oracle.emit_bundle(slot, instruction)
    lines.append("tag:")
    lines += oracle.emit_bundle(2, "nop.b 0")
    lines.append("target:")
    lines += oracle.emit_bundle(2, "nop.b 0")

    compare(lines, 15, 0)
    if b2 is not None:
        emit(lines, "i", f"mov r9=b{b2}")
        compare(lines, 9, "0x1230")
    emit(lines, "m", "mov r9=psr")
    emit(lines, "i", "and r9=0x30,r9")
    compare(lines, 9, 0)

    # The ordinary test terminal logs via break.m. Arrange for the IVT
    # break vector to return to the PASS spin after recording the marker.
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [
        f".org {IVT_OFFSET}", "ivt_base:",
        f".org {IVT_OFFSET + BREAK_VECTOR}", "break_vector:",
    ]
    emit(lines, "b", "br.cond.sptk b6")
    return "\n".join(lines) + "\n"


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="./build/qemu-system-ia64")
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-b67-execution"))
    p.add_argument("--timeout", type=float, default=10.0)
    p.add_argument("--assemble-only", action="store_true")
    p.add_argument("--only", choices=list(PROFILES))
    args = p.parse_args()
    if not 0 < args.timeout <= 60:
        p.error("--timeout must be in (0, 60]")
    args.out = args.out.resolve()
    selected = [args.only] if args.only else list(PROFILES)
    results = []
    for name in selected:
        evidence = ivt.run_one(name, args, case_spec=(BREAK_VECTOR, PASS, FAIL),
                               source=generate(name))
        results.append(evidence)
    summary = {
        "reference": "Intel Itanium SDM Vol 3 rev 2.3, B6/B7",
        "cases": len(results),
        "coverage": [
            "real full-system qemu-system-ia64 guest, no private firmware",
            "B6/B7 GNU assembler spellings (not hand-synthesized opcodes)",
            "B slots 0/1/2; wh and ih control variants",
            "forward nonzero tag displacement and target displacement",
            "no unexpected interruption; sequential fallthrough",
            "B7 branch register unchanged; PSR.mfl/mfh unchanged",
        ],
        "results": results,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B6/B7 {mode} PASS: {len(results)}/{len(PROFILES)} profiles")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
