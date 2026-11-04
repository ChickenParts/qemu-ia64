#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real IA-64 ITR.I fetch fault and instruction-retirement qualifications.

Install a *pinned* instruction translation with real itr.i, then purge
its opportunistic ITC copy using ptc.e. A later PSR.it=1 native RFI
must use the ITR itself. The separately installed identity ITC for
the guest harness and IVT is inserted AFTER the purge.
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
    "ia64_itr_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

TEXT = ivt.TEXT
IVT_OFF = 0x400000
ITLB, ACCESS_BIT, PAGE_NOT_PRESENT, ACCESS_RIGHTS = 0x400, 0x2400, 0x5000, 0x5200
SS_VECTOR, BREAK_VECTOR, GENERAL_VECTOR = 0x6000, 0x2c00, 0x5400
HIGH_CANON = (1 << 61) - (1 << 50)
HIGH_SLOT = HIGH_CANON + 0x3ff0
IT, IC, TB, SS, RI = 1 << 36, 1 << 13, 1 << 26, 1 << 40, 3 << 41
FETCH_ISR = (1 << 32) | (1 << 34)
PASS, FAIL = "0x69747270", "0x69747266"


@dataclass(frozen=True)
class Profile:
    vrn: int
    mode: str = "ok"    # ok, not-present, rights, access-bit, miss
    kind: str = "adds"  # adds or mlx
    tb: bool = False

    @property
    def source_va(self) -> int:
        return (self.vrn << 61) | HIGH_SLOT

    @property
    def next_va(self) -> int:
        return self.source_va + 16

    @property
    def start_ri(self) -> int:
        return 1 if self.kind == "mlx" else 2

    @property
    def flags(self) -> int:
        # SS always enabled. Fault must preempt it; successful instruction
        # must commit once before the post-result 0x6000 SS trap.
        return IT | IC | SS | (TB if self.tb else 0) | (self.start_ri << 41)

    @property
    def pte_flags(self) -> int:
        if self.mode == "not-present":
            # P=0, AR=0, A=0 and D=0: P must outrank permissions/A/D.
            return 0
        ar = 0 if self.mode == "rights" else 1
        accessed = self.mode != "access-bit"
        return (ar << 9) | (1 << 6) | (int(accessed) << 5) | 1

    @property
    def vec(self) -> int:
        return {
            "ok": SS_VECTOR,
            "not-present": PAGE_NOT_PRESENT,
            "rights": ACCESS_RIGHTS,
            "access-bit": ACCESS_BIT,
            "miss": ITLB,
        }[self.mode]

    @property
    def expected_isr(self) -> int:
        return ((2 if self.mode == "ok" else self.start_ri) << 41) | (
            8 if self.mode == "ok" else FETCH_ISR)

    @property
    def expected_reg(self) -> int:
        if self.mode != "ok":
            return 3
        return 4


PROFILES = {
    # Genuine pinned ITR mapped instruction retirement, including high
    # regions that have historically exercised QEMU's Linux shortcuts.
    "itr-r0-valid-ss": Profile(vrn=0),
    "itr-r1-valid-tb-ss": Profile(vrn=1, tb=True),
    "itr-r6-valid-ss": Profile(vrn=6),
    "itr-r7-mlx-valid-tb-ss": Profile(vrn=7, kind="mlx", tb=True),
    # Not-present matching ITR must beat ITLB miss, permissions, access bit
    # and every Linux region-6/7 shortcut.
    "itr-r2-not-present": Profile(vrn=2, mode="not-present"),
    "itr-r5-mlx-not-present": Profile(vrn=5, mode="not-present", kind="mlx"),
    "itr-r6-not-present-tb": Profile(vrn=6, mode="not-present", tb=True),
    "itr-r7-not-present": Profile(vrn=7, mode="not-present"),
    # Permission and accessed-bit faults are independent of P=0, and SS
    # must never fire before the faulting instruction executes.
    "itr-r3-rights": Profile(vrn=3, mode="rights"),
    "itr-r4-access-bit": Profile(vrn=4, mode="access-bit"),
    "itr-r1-mlx-access-bit": Profile(vrn=1, mode="access-bit", kind="mlx"),
    "itr-r3-unmapped": Profile(vrn=3, mode="miss"),
}


def install_itr(lines: list[str], p: Profile) -> None:
    literal(lines, 14, hex(p.source_va & ~0x3fff))
    emit(lines, "m", "mov cr.ifa=r14")
    literal(lines, 14, hex(14 << 2))
    emit(lines, "m", "mov cr.itir=r14")
    literal(lines, 20, "edge_page")
    literal(lines, 21, hex(p.pte_flags))
    emit(lines, "i", "or r20=r20,r21")
    # The privileged instruction inserts the pinned ITR slot (and QEMU's
    # incidental ITC copy); ptc.e immediately purges TC but leaves TR.
    # Only a correct ITR lookup can then fetch the high-VA source.
    emit(lines, "m", "rsm 0x2000")  # privileged itr requires PSR.ic=0
    emit(lines, "m", "srlz.i")
    literal(lines, 22, 3)
    lines.extend(["{ .mmi", "itr.i itr[r22]=r20", ";;",
                  "nop.m 0", "nop.i 0", ";;", "}"])
    literal(lines, 23, hex(p.source_va & ~0x3fff))
    emit(lines, "m", "ptc.e r23")
    emit(lines, "m", "srlz.i")


def install_ivt_itc(lines: list[str]) -> None:
    # Reinstall *after* ptc.e. A fault handler with restored PSR.it=1
    # needs this real identity translation even for a P=0 source ITR.
    literal(lines, 14, hex(TEXT))
    emit(lines, "m", "mov cr.ifa=r14")
    literal(lines, 14, hex(24 << 2))
    emit(lines, "m", "mov cr.itir=r14")
    literal(lines, 20, hex(TEXT | (1 << 9) | (1 << 6) | (1 << 5) | 1))
    lines.extend(["{ .mmi", "itc.i r20", ";;",
                  "nop.m 0", "nop.i 0", ";;", "}"])
    emit(lines, "m", "srlz.i")


def generate(case: str) -> str:
    p = PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)  # expected IVT entries
    literal(lines, 10, 3)  # non-commit sentinel for faults
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")

    if p.mode != "miss":
        install_itr(lines, p)
    install_ivt_itc(lines)

    # Native RFI enters translated source instruction in RI2 (adds) or
    # RI1 (MLX) with SS=1 and PSR.ic=1; live PSR.ic must be zero.
    emit(lines, "m", "rsm 0x2000")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, hex(p.source_va))
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex(IT | IC | TB | SS | RI))
    emit(lines, "i", "andcm r9=r9,r14")
    literal(lines, 14, hex(p.flags))
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
    if p.kind == "mlx":
        lines.extend(["{ .mlx", "nop.m 0", "movl r10=4", ";;", "}"])
    else:
        lines.extend(["{ .mii", "nop.m 0", "nop.i 0",
                      "adds r10=1,r10", ";;", "}"])

    lines.append("pass:")
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)

    lines += [f".org {IVT_OFF}", "ivt_base:"]
    for vector, label in (
        (ITLB, "itlb_vector"),
        (ACCESS_BIT, "inst_access_bit_vector"),
        (BREAK_VECTOR, "break_vector"),
        (PAGE_NOT_PRESENT, "inst_page_not_present_vector"),
        (ACCESS_RIGHTS, "inst_access_rights_vector"),
        (GENERAL_VECTOR, "general_vector"),
        (SS_VECTOR, "ss_vector"),
    ):
        lines += [f".org {IVT_OFF + vector}", label + ":"]
        if vector == BREAK_VECTOR:
            emit(lines, "b", "br.cond.sptk b6")
        else:
            emit(lines, "b", "br.cond.sptk checks" if p.vec == vector
                 else "br.cond.sptk fail")

    lines += [f".org {IVT_OFF + 0x9000}", "checks:"]
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex(p.expected_isr))
    if p.mode != "ok":
        emit(lines, "m", "mov r9=cr.ifa")
        compare(lines, 9, hex(p.source_va))
    else:
        emit(lines, "m", "mov r9=cr.iipa")
        compare(lines, 9, hex(p.source_va))
    emit(lines, "m", "mov r9=cr.iip")
    compare(lines, 9, hex(p.next_va if p.mode == "ok" else p.source_va))
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(IT | IC | TB | SS | RI))
    emit(lines, "i", "and r9=r9,r14")
    # Pre-result faults preserve source RI1/2. Post-result SS records RI0.
    compare(lines, 9, hex(p.flags & ~RI if p.mode == "ok" else p.flags))
    compare(lines, 10, p.expected_reg)
    # Repair to the identity-mapped harness. Clear SS/TB and RI, keep IT.
    literal(lines, 14, "after_trap")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(SS | TB | RI))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="./build/qemu-system-ia64")
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-itr-not-present"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--assemble-only", action="store_true")
    ap.add_argument("--only", choices=list(PROFILES))
    args = ap.parse_args()
    if not 0 < args.timeout <= 60:
        ap.error("timeout must be in (0,60]")
    args.out = args.out.resolve()
    chosen = [args.only] if args.only else list(PROFILES)
    results, failures = [], []
    for name in chosen:
        try:
            results.append(ivt.run_one(
                name, args, case_spec=(PROFILES[name].vec, PASS, FAIL),
                source=generate(name)))
        except (RuntimeError, subprocess.SubprocessError) as exc:
            failures.append((name, str(exc)))
            f = args.out / name / "result.json"
            if f.exists():
                results.append(json.loads(f.read_text()))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps({
        "reference": "Intel Itanium SDM Vol2 4.1.8 and Vol3 itr.i/ptc.e",
        "scope": "Pinned ITR.I real fetch P/AR/A priority, TC purged",
        "cases": len(chosen), "results": results, "failures": failures,
    }, indent=2) + "\n")
    count = sum(r.get("execution") ==
                ("not-run" if args.assemble_only else "pass") for r in results)
    print("IA64 PINNED ITR " +
          ("ASSEMBLY" if args.assemble_only else "EXECUTION") +
          f": {count}/{len(chosen)} profiles passed")
    if failures:
        raise RuntimeError("; ".join(name + ": " + err for name, err in failures))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
