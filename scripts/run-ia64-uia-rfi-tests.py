#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real firmware-free IA-64 IVT qualification: UIA TRAP on native rfi.

rfi itself never generates LP/TB/SS, even if the restored IPSR enables
them. A bad restored instruction address generates 0x5e00, ISR.code=0;
CR.IIPA and ISR.ei identify the rfi, CR.IIP and IPSR.ri the bad target.
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
    "ia64_uia_rfi_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT = 0x400000
UIA, TBV, SSV = 0x5e00, 0x5f00, 0x6000
BREAK, GENERAL = 0x2c00, 0x5400
IC, LP, TB, SS, RI, IT = 1 << 13, 1 << 25, 1 << 26, 1 << 40, 3 << 41, 1 << 36
HIGH_CANON = (1 << 61) - (1 << 50)
BAD_VA_LOW = 1 << 50  # first address above Merced's low canonical half
BAD_VA_HIGH = HIGH_CANON - 16  # immediately below high canonical half
NI, EI_RFI = 1 << 39, 2 << 41
PA44, PA45 = 1 << 44, 1 << 45
PASS, FAIL = "0x72666970", "0x72666966"
PTE_FLAGS = (1 << 9) | (1 << 6) | (1 << 5) | 1


@dataclass(frozen=True)
class Profile:
    bad: bool = True
    pa: int = PA44
    flags: int = 0
    target_ri: int = 0
    virtual: bool = False

    @property
    def restored_flags(self) -> int:
        return self.flags | (IT if self.virtual else 0)

    @property
    def destination(self) -> str:
        return hex(self.pa) if self.bad else "good_target"

    @property
    def expected_isr(self) -> int:
        # UIA on rfi has no concurrent LP/TB/SS, including with them enabled.
        return EI_RFI | (0 if self.flags & IC else NI)


PROFILES = {
    "rfi-uia-plain": Profile(),
    "rfi-uia-ss": Profile(flags=SS),
    "rfi-uia-tb": Profile(flags=TB),
    "rfi-uia-lp-tb-ss": Profile(flags=LP | TB | SS),
    "rfi-uia-second-pa": Profile(pa=PA45),
    "rfi-uia-restored-ic": Profile(flags=IC),
    "rfi-uia-restored-ic-lp-tb-ss": Profile(flags=IC | LP | TB | SS),
    "rfi-uia-restored-ri1": Profile(target_ri=1),
    "rfi-uia-restored-ri2-ss": Profile(target_ri=2, flags=SS),
    "rfi-valid-return": Profile(bad=False),
    "rfi-valid-collected": Profile(bad=False, flags=IC),
    # Architecturally noncanonical VA gaps: UIA must be selected by
    # restored PSR.it=1 BEFORE any speculative instruction-TLB fetch.
    "rfi-virtual-gap-low-vrn0": Profile(pa=BAD_VA_LOW, virtual=True),
    "rfi-virtual-gap-high-vrn1-ic": Profile(pa=(1 << 61) | BAD_VA_HIGH, virtual=True, flags=IC),
    "rfi-virtual-gap-low-vrn6-tb-ss": Profile(pa=(6 << 61) | BAD_VA_LOW, virtual=True, flags=TB | SS),
    "rfi-virtual-gap-high-vrn7-ic-ss": Profile(pa=(7 << 61) | BAD_VA_HIGH, virtual=True, flags=IC | SS),
}


def install_virtual_ivt(lines: list[str]) -> None:
    # Unlike a physical-mode UIA, a restored PSR.it=1 UIA may preserve
    # translation when dispatching the vector. Preinstall a real 16MiB
    # identity ITC.I mapping that covers BOTH the harness and its IVT.
    # The target VA itself remains deliberately noncanonical/unmapped.
    literal(lines, 14, hex(ivt.TEXT))
    emit(lines, "m", "mov cr.ifa=r14")
    literal(lines, 14, hex(24 << 2))
    emit(lines, "m", "mov cr.itir=r14")
    literal(lines, 20, hex(ivt.TEXT | PTE_FLAGS))
    lines.extend(["{ .mmi", "itc.i r20", ";;",
                  "nop.m 0", "nop.i 0", ";;", "}"])
    emit(lines, "m", "srlz.i")


def generate(case: str) -> str:
    p = PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")
    if p.virtual:
        install_virtual_ivt(lines)
    # rfi is legal only with live PSR.ic=0. The *restored* IC is varied.
    emit(lines, "m", "rsm 0x2000")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, p.destination)
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex(IC | LP | TB | SS | RI | IT))
    emit(lines, "i", "andcm r9=r9,r14")
    literal(lines, 14, hex(p.restored_flags | (p.target_ri << 41)))
    emit(lines, "i", "or r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    lines.append("source_rfi:")
    emit(lines, "b", "rfi")
    lines.append("after_rfi:")
    if p.bad:
        compare(lines, 15, 1)
        emit(lines, "b", "br.cond.sptk pass")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    lines.append("good_target:")
    if p.bad:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        compare(lines, 15, 0)
        emit(lines, "b", "br.cond.sptk pass")
    lines.append("pass:")
    ivt.terminal(lines, PASS, FAIL)

    lines += [f".org {IVT}", "ivt_base:",
              f".org {IVT+BREAK}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk pass_spin")
    lines += [f".org {IVT+GENERAL}", "general_vector:"]
    emit(lines, "b", "br.cond.sptk fail")
    for vector, name in ((UIA, "uia_vector"), (TBV, "tb_vector"),
                         (SSV, "ss_vector")):
        lines += [f".org {IVT+vector}", name + ":"]
        emit(lines, "b", "br.cond.sptk handler_checks" if
             vector == UIA and p.bad else "br.cond.sptk fail")
    lines += [f".org {IVT+0x9000}", "handler_checks:"]
    if p.bad:
        # Detect a second UIA from the handler's subsequent rfi.
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        emit(lines, "m", "mov r9=cr.isr")
        compare(lines, 9, hex(p.expected_isr))
        emit(lines, "m", "mov r9=cr.iipa")
        literal(lines, 14, "source_rfi")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.iip")
        compare(lines, 9, p.destination)
        emit(lines, "m", "mov r9=cr.ipsr")
        literal(lines, 14, hex(IC | LP | TB | SS | RI | IT))
        emit(lines, "i", "and r10=r9,r14")
        compare(lines, 10, hex(p.restored_flags | (p.target_ri << 41)))
        # The target is deliberately unexecutable. Repair saved IIP and
        # clear restored debug flags and RI for a genuine single rfi.
        literal(lines, 14, "after_rfi")
        emit(lines, "m", "mov cr.iip=r14")
        emit(lines, "m", "mov r9=cr.ipsr")
        # Repair to the physically addressed harness; a virtual UIA
        # trap must clear restored PSR.it before the repaired RFI.
        literal(lines, 14, hex(LP | TB | SS | RI | (IT if p.virtual else 0)))
        emit(lines, "i", "andcm r9=r9,r14")
        emit(lines, "m", "mov cr.ipsr=r9")
        emit(lines, "b", "rfi")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="./build/qemu-system-ia64")
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-uia-rfi"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--assemble-only", action="store_true")
    ap.add_argument("--only", choices=list(PROFILES))
    args = ap.parse_args()
    if not 0 < args.timeout <= 60:
        ap.error("timeout must be in (0, 60]")
    args.out = args.out.resolve()
    cases = [args.only] if args.only else list(PROFILES)
    results, failures = [], []
    for case in cases:
        try:
            results.append(ivt.run_one(
                case, args, case_spec=(UIA, PASS, FAIL), source=generate(case)))
        except RuntimeError as exc:
            failures.append((case, str(exc)))
            output = args.out / case / "result.json"
            if output.exists():
                results.append(json.loads(output.read_text()))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps({
        "reference": "Intel Itanium SDM rev2.3 Vol2 §8 UIA 0x5e00, Vol3 rfi",
        "scope": "native rfi UIA on restored PA or noncanonical VA; no LP/TB/SS",
        "cases": len(cases), "results": results, "failures": failures,
    }, indent=2) + "\n")
    count = sum(x.get("execution") ==
                ("not-run" if args.assemble_only else "pass") for x in results)
    print(f"IA64 RFI UIA {'ASSEMBLY' if args.assemble_only else 'EXECUTION'}: "
          f"{count}/{len(cases)} profiles passed")
    if failures:
        raise RuntimeError("; ".join(c + ": " + e for c, e in failures))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as e:
        sys.exit(str(e))
