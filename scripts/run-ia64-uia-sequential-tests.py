#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real IVT guests for Merced low-edge UIA, high-canonical SS and ITLB miss.

A 16KiB ITC.I page maps the actual low-edge or high canonical virtual
bundle; a 16MiB identity ITC.I entry keeps the IVT/guest code executable
with PSR.it=1. Unmapped high-canonical controls intentionally omit the
source translation and must FAULT before executing the instruction.
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
UIA, SSV, TBV, ITLB = 0x5e00, 0x6000, 0x5f00, 0x400
BREAK, GENERAL = 0x2c00, 0x5400
EDGE = (1 << 50) - 16
HIGH_CANON = (1 << 61) - (1 << 50)
HIGH_SLOT = HIGH_CANON + 0x3ff0  # last bundle of a mapped 16KiB high-half page
ISR_FETCH = (1 << 32) | (1 << 34)  # modeled instruction-TLB fault X|R
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
    vrn: int = 0
    high: bool = False
    miss: bool = False

    @property
    def source_va(self) -> int:
        # VRN [63:61] is independent of the canonical sign-extended
        # within-region address bits [60:0]. A high-half address is valid
        # only when [60:51] are ALL copies of bit 50 (=1).
        within = HIGH_SLOT if self.high else EDGE - 0x4000 if self.near else EDGE
        return (self.vrn << 61) | within

    @property
    def next_va(self) -> int:
        return self.source_va + 16

    @property
    def vec(self) -> int:
        if self.miss:
            return ITLB
        return SSV if self.near or self.high else UIA

    @property
    def expected_isr(self) -> int:
        if self.miss:
            # Pre-result instruction fetch fault: EI is the attempted
            # starting slot; no SS/TB/UIA trap codes, no committed r10.
            return ((1 if self.kind == "mlx" else 2) << 41) | ISR_FETCH
        return (2 << 41) | (8 if self.ss else 0) | (0 if self.ic else NI)

    @property
    def expected_reg(self) -> int:
        return 3 if self.miss else 4 if self.kind in ("adds", "mlx") else 3

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
    # Real ITC.I/IVT execution, not merely host address-policy testing,
    # across *every* virtual-region number 1..7. Region 0 is above.
    "edge-vrn1-adds": Profile(vrn=1),
    "edge-vrn2-adds-ss": Profile(vrn=2, ss=True),
    "edge-vrn3-adds-tb": Profile(vrn=3, tb=True),
    "edge-vrn4-adds-tb-ss": Profile(vrn=4, tb=True, ss=True),
    "edge-vrn5-adds-ic-ss": Profile(vrn=5, ic=True, ss=True),
    "edge-vrn6-nop-b-tb-ss": Profile(vrn=6, kind="nop-b", tb=True, ss=True),
    "edge-vrn7-long-mov-ss": Profile(vrn=7, kind="mlx", ss=True),
    # In-range controls guard against a false region-boundary UIA.
    "near-vrn6-adds-ss": Profile(vrn=6, ss=True, near=True),
    "near-vrn7-adds-ss": Profile(vrn=7, ss=True, near=True),
    # Real instruction execution at the *upper sign-extended* half in
    # every region: valid high VA, with no mapping for the NEXT bundle.
    # SS must retire first, reporting 0x6000 rather than UIA or a fetch miss.
    "high-vrn0-adds-ss": Profile(high=True, ss=True),
    "high-vrn1-nop-b-ss": Profile(vrn=1, high=True, kind="nop-b", ss=True),
    "high-vrn2-adds-ic-ss": Profile(vrn=2, high=True, ic=True, ss=True),
    "high-vrn3-long-mov-tb-ss": Profile(vrn=3, high=True, kind="mlx", tb=True, ss=True),
    "high-vrn4-false-branch-tb-ss": Profile(vrn=4, high=True, kind="false-branch", tb=True, ss=True),
    "high-vrn5-nop-f-ss": Profile(vrn=5, high=True, kind="nop-f", ss=True),
    "high-vrn6-adds-tb-ss": Profile(vrn=6, high=True, tb=True, ss=True),
    "high-vrn7-long-mov-ic-ss": Profile(vrn=7, high=True, kind="mlx", ic=True, ss=True),
    # An otherwise VALID high-half source without an ITC.I entry faults
    # on instruction fetch (0x400), before an enabled PSR.ss can retire.
    "high-miss-vrn0-adds-ic-ss": Profile(high=True, miss=True, ic=True, ss=True),
    "high-miss-vrn3-adds-tb-ic-ss": Profile(vrn=3, high=True, miss=True, tb=True, ic=True, ss=True),
    "high-miss-vrn6-adds-ic-ss": Profile(vrn=6, high=True, miss=True, ic=True, ss=True),
    "high-miss-vrn7-long-mov-ic-ss": Profile(vrn=7, high=True, miss=True, kind="mlx", ic=True, ss=True),
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
    if not p.miss:
        install_itc(lines, hex(p.source_va & ~0x3fff), "edge_page", 14)

    # RFI enters an ordinary final slot at RI2, but the long MLX L+X
    # instruction must begin at RI1 and retires once at source EI2.
    # Live PSR.ic=0 is required; restored PSR.it=1 tests true VA.
    emit(lines, "m", "rsm 0x2000")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, hex(p.source_va))
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex(IT | IC | LP | TB | SS | RI))
    emit(lines, "i", "andcm r9=r9,r14")
    start_ri = 1 if p.kind == "mlx" else 2
    flags = IT | (IC if p.ic else 0) | (TB if p.tb else 0) | \
            (SS if p.ss else 0) | (start_ri << 41)
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
              f".org {IVT_OFF + ITLB}", "instruction_tlb_vector:"]
    emit(lines, "b", "br.cond.sptk checks" if p.vec == ITLB
         else "br.cond.sptk fail")
    lines += [f".org {IVT_OFF + BREAK}", "break_vector:"]
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
    if p.miss:
        # Faults save the source IIP/RI and CR.IFA, not the post-result
        # CR.IIPA / next-bundle state of SS or sequential UIA traps.
        emit(lines, "m", "mov r9=cr.ifa")
        compare(lines, 9, hex(p.source_va))
    else:
        emit(lines, "m", "mov r9=cr.iipa")
        compare(lines, 9, hex(p.source_va))
    emit(lines, "m", "mov r9=cr.iip")
    compare(lines, 9, hex(p.source_va if p.miss else p.next_va))
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(IT | IC | LP | TB | SS | RI))
    emit(lines, "i", "and r9=r9,r14")
    # Faults restart at source RI1/2; post-result traps save next RI0.
    saved_ipsr = hex(flags) if p.miss else hex(flags & ~RI)
    compare(lines, 9, saved_ipsr)
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
        "mode": "real PSR.it=1; low-edge UIA/high-canonical SS/ITLB pre-result fault",
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
