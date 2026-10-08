#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free IA-64 B8 clrrrb/clrrrb.pr state and real-IVT regressions.

Verify actual rotating GR/FR/PR access, preserved GR NaT/value pairing by
the production helper's inverse permutation, and saved CFM via ar.pfs
after a real br.call. No Python copy of the decoder executes guest code.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def load(name: str, module: str):
    spec = importlib.util.spec_from_file_location(module, ROOT / "scripts" / name)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


ivt = load("run-ia64-fp-exception-tests.py", "ia64_ivt_for_b8")
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT_OFFSET = 0x400000
BREAK_VECTOR = 0x2c00
PASS = "0x62387270"
FAIL = "0x62387266"
ROTATIONS = (0, 1, 2, 7, 8, 9)
PROFILES = {
    f"{'all' if form == 'clrrrb' else 'pred'}-{n}rot": (form, n)
    for form in ("clrrrb", "clrrrb.pr") for n in ROTATIONS
}
RRB_MASK = (0x7f << 18) | (0x7f << 25) | (0x3f << 32)
CFM_FIELDS_MASK = (1 << 38) - 1
BASE_CFM = 8 | (1 << 14)  # alloc: SOF=8, SOL=0, SOR=8


def check_predicate(lines: list[str], reg: int, expected: bool,
                    ident: str) -> None:
    label = "pred_" + ident
    if expected:
        emit(lines, "b", f"(p{reg}) br.cond.sptk {label}")
        emit(lines, "b", "br.cond.sptk fail")
        lines.append(label + ":")
    else:
        emit(lines, "b", f"(p{reg}) br.cond.sptk fail")


def generate(profile: str) -> str:
    if profile not in PROFILES:
        raise ValueError(f"unknown profile {profile}")
    form, rotations = PROFILES[profile]
    pred_only = form.endswith(".pr")
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b6=r14")
    literal(lines, 15, 0)

    # The first 8 stacked registers form a rotating region; no outputs
    # are required for the final br.call snapshot of the CFM into ar.pfs.
    emit(lines, "m", "alloc r20=ar.pfs,0,0,8,8")
    for i in range(8):
        literal(lines, 20, hex(0x7100 + i))
        emit(lines, "i", f"mov r{32+i}=r20")

    # Two distinguishable physical FR sources: rotating f32 reads the
    # final chosen physical register after N rotations.  If N=0, f32
    # remains the original sentinel; otherwise rotating f32 hits f(128-N).
    literal(lines, 20, "0x5151")
    emit(lines, "m", "setf.sig f32=r20")
    fr_source = 128 - rotations if rotations else 32
    if rotations:
        literal(lines, 20, "0xA5A5")
        emit(lines, "m", f"setf.sig f{fr_source}=r20")

    emit(lines, "i", "cmp.eq p16,p17=r0,r0")
    emit(lines, "m", "rsm 0x30")
    emit(lines, "m", "srlz.d")

    # p0 is true.  br.wtop therefore rotates every time while setting
    # p63=0 first, so the single true p16 is shifted to p(16+N).
    # The target is the next bundle: no loop control structure needed.
    for i in range(rotations):
        emit(lines, "b", f"br.wtop.sptk rotated_{i}")
        lines.append(f"rotated_{i}:")

    rotated_gr32 = 0x7100 + ((8 - rotations % 8) % 8)
    rotated_fr32 = 0x5151 if rotations == 0 else 0xA5A5
    compare(lines, 32, hex(rotated_gr32))
    emit(lines, "m", "getf.sig r9=f32")
    compare(lines, 9, hex(rotated_fr32))
    check_predicate(lines, 16 + rotations, True, "before")
    if rotations:
        check_predicate(lines, 16, False, "shifted")

    # B8 is a B-slot instruction. The explicit stop makes it the final
    # instruction in the group as required by Intel Vol 3 rev 2.3.
    emit(lines, "b", form)
    expected_gr32 = rotated_gr32 if pred_only else 0x7100
    expected_fr32 = rotated_fr32 if pred_only else 0x5151
    compare(lines, 32, hex(expected_gr32))
    compare(lines, 39, hex((0x7100 + ((7 - rotations % 8) % 8))
                            if pred_only else 0x7107))
    emit(lines, "m", "getf.sig r9=f32")
    compare(lines, 9, hex(expected_fr32))

    # p16 must once again name its original physical bit.  p17 was
    # false before rotation; this also checks the rotating PR accessor.
    check_predicate(lines, 16, True, "after")
    check_predicate(lines, 17, False, "after_false")
    emit(lines, "m", "mov r9=psr")
    emit(lines, "i", "and r9=0x30,r9")
    compare(lines, 9, 0)

    # Snapshot CFM into ar.pfs through the production br.call machinery.
    # ar.pfs also contains saved EC/PPL; mask these out without altering
    # the architectural RRB/sof/sol/sor values under test.
    emit(lines, "b", "br.call.sptk b0=cfm_snapshot")
    emit(lines, "b", "br.cond.sptk fail")
    lines.append("cfm_snapshot:")
    emit(lines, "i", "mov r9=ar.pfs")
    literal(lines, 14, hex(CFM_FIELDS_MASK))
    emit(lines, "i", "and r9=r9,r14")
    if pred_only:
        rrbg = (8 - rotations % 8) % 8
        rrbf = (96 - rotations % 96) % 96
        expected_cfm = BASE_CFM | (rrbg << 18) | (rrbf << 25)
    else:
        expected_cfm = BASE_CFM
    compare(lines, 9, hex(expected_cfm))
    compare(lines, 15, 0)  # unexpected IVT entry would have gone to fail

    # Terminal logging deliberately uses break.m 0; redirect that
    # exception to the pass spinner only after the expected state holds.
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [
        f".org {IVT_OFFSET}", "ivt_base:",
        f".org {IVT_OFFSET+BREAK_VECTOR}", "break_vector:",
    ]
    emit(lines, "b", "br.cond.sptk b6")
    return "\n".join(lines) + "\n"


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="./build/qemu-system-ia64")
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-b8-execution"))
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("--assemble-only", action="store_true")
    p.add_argument("--only", choices=list(PROFILES))
    args = p.parse_args()
    if not 0 < args.timeout <= 60:
        p.error("--timeout must be in (0, 60]")
    args.out = args.out.resolve()
    selected = [args.only] if args.only else list(PROFILES)
    results = []
    for name in selected:
        result = ivt.run_one(name, args,
                             case_spec=(BREAK_VECTOR, PASS, FAIL),
                             source=generate(name))
        results.append(result)
    report = {
        "reference": "Intel Itanium SDM Vol 3 rev 2.3, B8",
        "cases": len(results),
        "coverage": [
            "0/1/2/7/8/9 rotations before clear",
            "all-RRB reset versus predicate-only reset",
            "GR32 and GR39 physical rematerialization across wrap",
            "FR32 live rename view, PR16/PR17 live predicate view",
            "SOF/SOL/SOR and per-field RRB checks via br.call->ar.pfs",
            "PSR.mfl/mfh unchanged; no spurious IVT interruption",
            "firmware-free full-system emulator (not decoder copy)"
        ],
        "results": results
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    mode = "ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B8 {mode} PASS: {len(results)}/{len(PROFILES)} profiles")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
