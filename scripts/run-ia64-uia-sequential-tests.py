#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real IVT guests for Merced nonbranch slot-2 upper-VA UIA TRAP.

A genuine ITC.I entry maps one executable 16KiB page to the last page in
implemented region-0 virtual space, and a 16MiB identity entry keeps
the IVT/guest code executable while PSR.it=1.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_uia_seq_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

TEXT = ivt.TEXT
IVT_OFF = 0x400000
UIA, SSV, TBV = 0x5e00, 0x6000, 0x5f00
BREAK, GENERAL = 0x2c00, 0x5400
EDGE = (1 << 50) - 16
IT, IC, LP, TB, SS, RI = 1 << 36, 1 << 13, 1 << 25, 1 << 26, 1 << 40, 3 << 41
NI = 1 << 39
PASS, FAIL = "0x73657170", "0x73657166"
PTE_FLAGS = (1 << 9) | (1 << 6) | (1 << 5) | 1

@dataclass(frozen=True)
class Profile:
    kind: str = "adds"
    ss: bool = False
    tb: bool = False
    ic: bool = False
    near: bool = False

    @property
    def source_va(self) -> int:
        return EDGE - 0x4000 if self.near else EDGE

    @property
    def next_va(self) -> int:
        return self.source_va + 16

    @property
    def vec(self) -> int:
        return SSV if self.near else UIA

    @property
    def expected_isr(self) -> int:
        return (2 << 41) | (8 if self.ss else 0) | (0 if self.ic else NI)

    @property
    def expected_reg(self) -> int:
        return 4 if self.kind in ("adds", "mlx") else 3

PROFILES = {
    "edge-adds-no-flags": Profile(),
    "edge-adds-ss": Profile(ss=True),
    "edge-adds-tb": Profile(tb=True),
    "edge-adds-tb-ss": Profile(tb=True, ss=True),
    "edge-adds-ic-ss": Profile(ic=True, ss=True),
    "edge-nop-b-tb-ss": Profile(kind="nop-b", tb=True, ss=True),
    "edge-nop-f-ss": Profile(kind="nop-f", ss=True),
    "edge-long-mov-ss": Profile(kind="mlx", ss=True),
    "edge-false-branch-tb-ss": Profile(kind="false-branch", tb=True, ss=True),
    "near-adds-ss": Profile(ss=True, near=True),
    "near-nop-b-ss": Profile(kind="nop-b", ss=True, near=True),
}

def install_itc(lines: list[str], va: str, pte: str, page_shift: int) -> None:
    literal(lines, 14, va)
    emit(lines, "m", "mov cr.ifa=r14")
    literal(lines, 14, hex(page_shift << 2))
    emit(lines, "m", "mov cr.itir=r14")
    literal(lines, 20, pte)
    if pte == "edge_page":
        literal(lines, 21, hex(PTE_FLAGS))
        emit(lines, "i", "or r20=r20,r21")
    lines.extend(["{ .mmi", "itc.i r20", ";;",
                  "nop.m 0", "nop.i 0", ";;", "}"])
    emit(lines, "m", "srlz.i")

def edge_instruction(lines: list[str], kind: str) -> None:
    if kind == "adds":
        lines.extend(["{ .mii", "nop.m 0", "nop.i 0",
                      "adds r10=1,r10", ";;", "}"])
    elif kind == "nop-b":
        lines.extend(["{ .mib", "nop.m 0", "nop.i 0",
                      "nop.b 0", ";;", "}"])
    elif kind == "nop-f":
        lines.extend(["{ .mmf", "nop.m 0", "nop.m 0",
                      "nop.f 0", ";;", "}"])
    elif kind == "mlx":
        lines.extend(["{ .mlx", "nop.m 0",
                      "movl r10=4", ";;", "}"])
    elif kind == "false-branch":
        lines.extend(["{ .mib", "nop.m 0", "nop.i 0",
                      "(p6) br.cond.sptk fail", ";;", "}"])
    else:
        raise ValueError(kind)

def generate(case: str) -> str:
    p = PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)
    literal(lines, 10, 3)
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")
    if p.kind == "false-branch":
        emit(lines, "i", "cmp.ne p6,p7=r0,r0")

    install_itc(lines, hex(TEXT), hex(TEXT | PTE_FLAGS), 24)
    install_itc(lines, hex(p.source_va & ~0x3fff), "edge_page", 14)

    # RFI is a valid transfer to an implemented source, slot RI2.
    # Live PSR.ic=0 is required; restored PSR.it=1 tests true VA.
    emit(lines, "m", "rsm 0x2000")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, hex(p.source_va))
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex(IT | IC | LP | TB | SS | RI))
    emit(lines, "i", "andcm r9=r9,r14")
    flags = IT | (IC if p.ic else 0) | (TB if p.tb else 0) | \
            (SS if p.ss else 0) | (2 << 41)
    literal(lines, 14, hex(flags))
    emit(lines, "i", "or r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")
    emit(lines, "b", "br.cond.sptk fail")
    lines.append("after_trap:")
    compare(lines, 15, 1)
    compare(lines, 10, p.expected_reg)
    emit(lines, "b", "br.cond.sptk pass")

    lines.extend([".balign 16384", "edge_page:",
                  ".space 16368, 0", "edge_bundle:"])
    edge_instruction(lines, p.kind)
    lines.append("pass:")
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [f".org {IVT_OFF}", "ivt_base:",
              f".org {IVT_OFF + BREAK}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT_OFF + GENERAL}", "general_vector:"]
    emit(lines, "b", "br.cond.sptk fail")
    for vector, name in ((UIA, "uia_vector"), (TBV, "tb_vector"),
                         (SSV, "ss_vector")):
        lines += [f".org {IVT_OFF + vector}", name + ":"]
        emit(lines, "b", "br.cond.sptk checks" if vector == p.vec
             else "br.cond.sptk fail")
    lines += [f".org {IVT_OFF + 0x9000}", "checks:"]
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex(p.expected_isr))
    emit(lines, "m", "mov r9=cr.iipa")
    compare(lines, 9, hex(p.source_va))
    emit(lines, "m", "mov r9=cr.iip")
    compare(lines, 9, hex(p.next_va))
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(IT | IC | LP | TB | SS | RI))
    emit(lines, "i", "and r9=r9,r14")
    compare(lines, 9, hex(flags))
    compare(lines, 10, p.expected_reg)
    literal(lines, 14, "after_trap")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(SS | TB | RI))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")
    return "\n".join(lines) + "\n"

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="./build/qemu-system-ia64")
    p.add_argument("--out", type=Path, default=Path("scratch/ia64-uia-sequential"))
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("--assemble-only", action="store_true")
    p.add_argument("--only", choices=list(PROFILES))
    args = p.parse_args()
    if not 0 < args.timeout <= 60:
        p.error("timeout must be in (0, 60]")
    args.out = args.out.resolve()
    selected = [args.only] if args.only else list(PROFILES)
    results, failures = [], []
    for name in selected:
        try:
            results.append(ivt.run_one(
                name, args, case_spec=(PROFILES[name].vec, PASS, FAIL),
                source=generate(name)))
        except (RuntimeError, subprocess.SubprocessError) as exc:
            failures.append((name, str(exc)))
            path = args.out / name / "result.json"
            if path.exists():
                results.append(json.loads(path.read_text()))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps({
        "reference": "Intel Itanium SDM rev2.3 Vol2 §5.3, UIA 0x5e00 pp2:205–206",
        "mode": "real PSR.it=1, 16KiB high-VA ITC.I mapping",
        "cases": len(selected), "results": results, "failures": failures,
    }, indent=2) + "\n")
    count = sum(x.get("execution") ==
                ("not-run" if args.assemble_only else "pass") for x in results)
    print(f"IA64 UIA SEQUENTIAL "
          f"{'ASSEMBLY' if args.assemble_only else 'EXECUTION'}: "
          f"{count}/{len(selected)} profiles passed")
    if failures:
        raise RuntimeError("; ".join(n + ": " + e for n, e in failures))

if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as e:
        sys.exit(str(e))
