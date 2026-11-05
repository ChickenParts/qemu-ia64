#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""GNU IA-64 real-IVT DTLB and unimplemented data-address faults.

Run M-slot loads/stores with PSR.dt=1, using actual ITC.D or pinned DTR
entries for mapped cases. Noncanonical VA cases must fault at general
exception 0x5400 ahead of DTLB misses or region 6/7 direct-map shortcuts.
Handlers check only architecturally defined fault state; CR.IFA is not
specified for unimplemented data address faults.
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
    "ia64_dtlb_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare

IVT_OFF = 0x400000
DTLB, DIRTY, ACCESS_BIT, BREAK = 0x800, 0x2000, 0x2800, 0x2c00
NOT_PRESENT, ACCESS_RIGHTS, GENERAL = 0x5000, 0x5300, 0x5400
DT, IC, IT, TB, SS, RI = 1 << 17, 1 << 13, 1 << 36, 1 << 26, 1 << 40, 3 << 41
READ_ISR, WRITE_ISR = 1 << 34, 1 << 33
INIT_DATA = 0x1122334455667788
STORE_DATA = 0x123456789ABCDEF0
INIT_REG = 0x3333
HIGH_BASE = (1 << 61) - (1 << 50)
UAD_CODE = 0x30  # General Exception ISR.code{7:4}=3, rs=0
PASS, FAIL = "0x64746c70", "0x64746c66"


@dataclass(frozen=True)
class Profile:
    kind: str = "load"
    vrn: int = 0
    mapped: bool = True
    ar: int = 2
    accessed: bool = True
    dirty: bool = True
    present: bool = True
    high: bool = False
    tr: bool = False
    gap: str = ""  # "low" or "high": noncanonical Merced data VA
    vec: int = 0

    @property
    def va(self) -> int:
        if self.gap == "low":
            return (self.vrn << 61) | (1 << 50)
        if self.gap == "high":
            return (self.vrn << 61) | (HIGH_BASE - 0x4000)
        return (self.vrn << 61) | ((HIGH_BASE if self.high else 0) + 0x8000)

    @property
    def pte_flags(self) -> int:
        # P=0 entries have no associated attribute bits in the insert.
        return ((self.ar << 9) | (int(self.dirty) << 6) |
                (int(self.accessed) << 5) | 1) if self.present else 0

    @property
    def expected_isr(self) -> int:
        return (WRITE_ISR if self.kind == "store" else READ_ISR) | (
            UAD_CODE if self.gap else 0)

    @property
    def expected_reg(self) -> int:
        return INIT_DATA if self.kind == "load" and not self.vec else INIT_REG

    @property
    def expected_memory(self) -> int:
        return STORE_DATA if self.kind == "store" and not self.vec else INIT_DATA


PROFILES = {
    # Explicit mappings must beat region 6/7 Linux-compatible direct maps.
    "mapped-r6-load": Profile(vrn=6),
    "mapped-r7-store": Profile(kind="store", vrn=7),
    "mapped-r3-load": Profile(vrn=3),
    "mapped-r5-high-load": Profile(vrn=5, high=True),
    "mapped-r7-readonly-load": Profile(vrn=7, ar=1),
    # Data miss must be a pre-result fault, without an ITC.D insertion.
    "miss-r0-load": Profile(mapped=False, vec=DTLB),
    "miss-r3-store": Profile(vrn=3, mapped=False, kind="store", vec=DTLB),
    # AR denial is higher priority than A=0 and/or D=0.
    "rights-r6-store-ad0": Profile(kind="store", vrn=6, ar=1,
                                  accessed=False, dirty=False, vec=ACCESS_RIGHTS),
    "rights-r7-store-d0": Profile(kind="store", vrn=7, ar=1,
                                  dirty=False, vec=ACCESS_RIGHTS),
    "rights-r0-store": Profile(kind="store", ar=1, vec=ACCESS_RIGHTS),
    # A=0 is checked before D=0 when access rights allow the store.
    "access-r1-load": Profile(vrn=1, accessed=False, vec=ACCESS_BIT),
    "access-r2-store-d0": Profile(kind="store", vrn=2,
                                 accessed=False, dirty=False, vec=ACCESS_BIT),
    "dirty-r4-store": Profile(kind="store", vrn=4, dirty=False, vec=DIRTY),
    # P=0 is *present in the cache* but faults, never becomes a DTLB miss.
    "not-present-r5-load": Profile(vrn=5, present=False, accessed=False,
                                   dirty=False, vec=NOT_PRESENT),
    "not-present-r6-store": Profile(kind="store", vrn=6, ar=1,
                                    present=False, accessed=False,
                                    dirty=False, vec=NOT_PRESENT),
    # Pinned DTR tests purge the translation cache after itr.d, forcing
    # a true Translation Register match (never the implicit TC copy).
    "tr-positive-r0-load": Profile(tr=True),
    "tr-positive-r6-store": Profile(tr=True, kind="store", vrn=6),
    "tr-readonly-r7-load": Profile(tr=True, vrn=7, ar=1),
    "tr-rights-r2-store-ad0": Profile(tr=True, kind="store", vrn=2,
                                     ar=1, accessed=False, dirty=False,
                                     vec=ACCESS_RIGHTS),
    "tr-access-r3-load": Profile(tr=True, vrn=3, accessed=False,
                                 vec=ACCESS_BIT),
    "tr-dirty-r4-store": Profile(tr=True, kind="store", vrn=4,
                                 dirty=False, vec=DIRTY),
    "tr-notpresent-r5-load": Profile(tr=True, vrn=5, present=False,
                                     accessed=False, dirty=False,
                                     vec=NOT_PRESENT),
    "tr-notpresent-r6-store": Profile(tr=True, kind="store", vrn=6,
                                      present=False, accessed=False,
                                      dirty=False, vec=NOT_PRESENT),
    "tr-notpresent-r7-store": Profile(tr=True, kind="store", vrn=7,
                                      present=False, accessed=False,
                                      dirty=False, vec=NOT_PRESENT),
    # Intel SDM Vol2 §4.3.3: invalid VA beats DTLB miss and region 6/7
    # compatibility mappings. Low/high gap cases cover EVERY VRN.
    # Alternate load/store to verify ISR.r/w and noncommit across both.
    "uad-r0-low-load": Profile(vrn=0, mapped=False, gap="low", vec=GENERAL),
    "uad-r0-high-store": Profile(vrn=0, mapped=False, gap="high",
                                 kind="store", vec=GENERAL),
    "uad-r1-low-load": Profile(vrn=1, mapped=False, gap="low", vec=GENERAL),
    "uad-r1-high-store": Profile(vrn=1, mapped=False, gap="high",
                                 kind="store", vec=GENERAL),
    "uad-r2-low-load": Profile(vrn=2, mapped=False, gap="low", vec=GENERAL),
    "uad-r2-high-store": Profile(vrn=2, mapped=False, gap="high",
                                 kind="store", vec=GENERAL),
    "uad-r3-low-load": Profile(vrn=3, mapped=False, gap="low", vec=GENERAL),
    "uad-r3-high-store": Profile(vrn=3, mapped=False, gap="high",
                                 kind="store", vec=GENERAL),
    "uad-r4-low-load": Profile(vrn=4, mapped=False, gap="low", vec=GENERAL),
    "uad-r4-high-store": Profile(vrn=4, mapped=False, gap="high",
                                 kind="store", vec=GENERAL),
    "uad-r5-low-load": Profile(vrn=5, mapped=False, gap="low", vec=GENERAL),
    "uad-r5-high-store": Profile(vrn=5, mapped=False, gap="high",
                                 kind="store", vec=GENERAL),
    "uad-r6-low-load": Profile(vrn=6, mapped=False, gap="low", vec=GENERAL),
    "uad-r6-high-store": Profile(vrn=6, mapped=False, gap="high",
                                 kind="store", vec=GENERAL),
    "uad-r7-low-load": Profile(vrn=7, mapped=False, gap="low", vec=GENERAL),
    "uad-r7-high-store": Profile(vrn=7, mapped=False, gap="high",
                                 kind="store", vec=GENERAL),
    # Valid high-canonical controls must NOT be misclassified as UAD.
    "mapped-r6-high-load": Profile(vrn=6, high=True),
    "mapped-r7-high-store": Profile(vrn=7, high=True, kind="store"),
}


def install_dtlb(lines: list[str], profile: Profile) -> None:
    literal(lines, 14, hex(profile.va & ~0x3fff))
    emit(lines, "m", "mov cr.ifa=r14")
    literal(lines, 14, hex(14 << 2))
    emit(lines, "m", "mov cr.itir=r14")
    literal(lines, 20, "data_page")
    literal(lines, 21, hex(profile.pte_flags))
    emit(lines, "i", "or r20=r20,r21")
    if profile.tr:
        # itr.d requires uncollected interruption state (PSR.ic=0).
        emit(lines, "m", "rsm 0x2000")
        emit(lines, "m", "srlz.i")
        literal(lines, 22, 3)  # pinned DTR slot, independent of TC ring
        lines.extend(["{ .mmi", "itr.d dtr[r22]=r20", ";;",
                      "nop.m 0", "nop.i 0", ";;", "}"])
        # itr.d also seeds an opportunistic TC copy in this QEMU model.
        # ptc.e purges only translation caches; the pinned DTR survives.
        # A P=0 test can now PASS only via the real DTR lookup path.
        literal(lines, 23, hex(profile.va & ~0x3fff))
        emit(lines, "m", "ptc.e r23")
    else:
        lines.extend(["{ .mmi", "itc.d r20", ";;",
                      "nop.m 0", "nop.i 0", ";;", "}"])
    emit(lines, "m", "srlz.d")


def generate(case: str) -> str:
    p = PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)
    literal(lines, 11, hex(INIT_REG))
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")

    if p.mapped:
        install_dtlb(lines, p)

    literal(lines, 20, hex(p.va))
    literal(lines, 21, hex(STORE_DATA))
    # Enter the faulting/successful M-slot through real rfi with PSR.dt=1.
    # The native instructions remain physical (PSR.it=0).
    emit(lines, "m", "rsm 0x2000")
    emit(lines, "m", "srlz.i")
    literal(lines, 14, "source_bundle")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex(DT | IC | IT | TB | SS | RI))
    emit(lines, "i", "andcm r9=r9,r14")
    literal(lines, 14, hex(DT | IC))
    emit(lines, "i", "or r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

    lines.append("source_bundle:")
    emit(lines, "m", "ld8 r11=[r20]" if p.kind == "load" else "st8 [r20]=r21")
    lines.append("after_source:")
    # Success flows through here at PSR.dt=1; repaired fault rfi arrives
    # here with DT already cleared. Physical verification is therefore safe.
    emit(lines, "m", "rsm 0x20000")
    emit(lines, "m", "srlz.d")
    compare(lines, 15, 1 if p.vec else 0)
    compare(lines, 11, hex(p.expected_reg))
    literal(lines, 14, "data_page")
    emit(lines, "m", "ld8 r9=[r14]")
    compare(lines, 9, hex(p.expected_memory))
    emit(lines, "b", "br.cond.sptk pass")

    lines.append("pass:")
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)

    lines += [f".org {IVT_OFF}", "ivt_base:"]
    for vector, label in ((DTLB, "dtlb_vector"),
                          (DIRTY, "dirty_vector"),
                          (ACCESS_BIT, "access_bit_vector"),
                          (BREAK, "break_vector"),
                          (NOT_PRESENT, "not_present_vector"),
                          (ACCESS_RIGHTS, "rights_vector"),
                          (GENERAL, "general_vector")):
        lines += [f".org {IVT_OFF + vector}", label + ":"]
        if vector == BREAK:
            emit(lines, "b", "br.cond.sptk b6")
        else:
            emit(lines, "b", "br.cond.sptk fault_checks" if vector == p.vec
                 else "br.cond.sptk fail")

    lines += [f".org {IVT_OFF + 0x9000}", "fault_checks:"]
    if p.vec:
        # A data fault does not implicitly clear live PSR.dt. The IVT
        # checks the physical data-page sentinel, so explicitly switch
        # the *live* handler to physical data mode before that load.
        # Saved CR.IPSR.dt remains set and is verified below.
        emit(lines, "m", "rsm 0x20000")
        emit(lines, "m", "srlz.d")
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        emit(lines, "m", "mov r9=cr.isr")
        compare(lines, 9, hex(p.expected_isr))
        if not p.gap:
            # Intel Table 8-1 marks CR.IFA unspecified on UAD.
            # Never compare a fabricated faulting VA for this vector.
            emit(lines, "m", "mov r9=cr.ifa")
            compare(lines, 9, hex(p.va))
        emit(lines, "m", "mov r9=cr.iip")
        literal(lines, 14, "source_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.ipsr")
        literal(lines, 14, hex(DT | IC | IT | SS | TB | RI))
        emit(lines, "i", "and r9=r9,r14")
        compare(lines, 9, hex(DT | IC))  # Fault restarts at source RI0
        compare(lines, 11, hex(INIT_REG))
        # The failed store must not have written physical memory.
        literal(lines, 14, "data_page")
        emit(lines, "m", "ld8 r9=[r14]")
        compare(lines, 9, hex(INIT_DATA))
        # Repair target and clear DT: the next code/data is physical.
        literal(lines, 14, "after_source")
        emit(lines, "m", "mov cr.iip=r14")
        emit(lines, "m", "mov r9=cr.ipsr")
        literal(lines, 14, hex(DT | SS | TB | RI))
        emit(lines, "i", "andcm r9=r9,r14")
        emit(lines, "m", "mov cr.ipsr=r9")
        emit(lines, "b", "rfi")
    else:
        emit(lines, "b", "br.cond.sptk fail")

    lines += [".data", ".balign 16384", "data_page:",
              f".quad {hex(INIT_DATA)}"]
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="./build/qemu-system-ia64")
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-dtlb-priority"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--assemble-only", action="store_true")
    ap.add_argument("--only", choices=list(PROFILES))
    args = ap.parse_args()
    if not 0 < args.timeout <= 60:
        ap.error("timeout must be in (0,60]")
    args.out = args.out.resolve()
    selected = [args.only] if args.only else list(PROFILES)
    results, failures = [], []
    for case in selected:
        try:
            results.append(ivt.run_one(
                case, args, case_spec=(PROFILES[case].vec or DTLB, PASS, FAIL),
                source=generate(case)))
        except (RuntimeError, subprocess.SubprocessError) as exc:
            failures.append((case, str(exc)))
            file = args.out / case / "result.json"
            if file.exists():
                results.append(json.loads(file.read_text()))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps({
        "reference": "Intel Itanium SDM Vol2 §§4.1.8,4.3.3,5.3 and §8",
        "scope": "ITC.D, pinned DTR and noncanonical data VA / General Exception",
        "cases": len(selected), "results": results, "failures": failures
    }, indent=2) + "\n")
    count = sum(x.get("execution") ==
                ("not-run" if args.assemble_only else "pass") for x in results)
    print(f"IA64 DTLB PRIORITY "
          f"{'ASSEMBLY' if args.assemble_only else 'EXECUTION'}: "
          f"{count}/{len(selected)} profiles passed")
    if failures:
        raise RuntimeError("; ".join(k + ": " + v for k, v in failures))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
