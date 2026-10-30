#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real IVT B1/B2 loop and B3/B5 call retirement with SS-only / TB+SS.

Every test enters directly at the source RI via CR.IPSR+RFI and verifies
the priority vector, ISR, IIPA, target IIP/RI, committed architectural
side effects, and single RFI resume with SS/TB cleared.
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
    "ia64_branch_ss_ivt", ROOT/"scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare
IVT, BREAK, GENERAL, TBV, SSV = 0x400000, 0x2c00, 0x5400, 0x5f00, 0x6000
SS, TB, IC, RI = 1 << 40, 1 << 26, 1 << 13, 3 << 41
RRBG, CFM_SHAPE = 0x7f << 18, 0x7fff
PFS_SENTINEL = 0x3000000000000000
PASS, FAIL = "0x42537370", "0x42537366"

@dataclass(frozen=True)
class Profile:
    kind: str
    tb: bool = False
    slot: int = 2
    lc: int = 0
    ec: int = 0
    qp: bool = False
    false_call: bool = False

    @property
    def is_call(self) -> bool:
        return self.kind in ("b3-b0", "b3-b3", "b5-b1", "b5-alias")

    @property
    def taken(self) -> bool:
        if self.is_call:
            return not self.false_call
        if self.kind == "cloop":
            return self.lc > 0
        if self.kind in ("wtop", "wexit"):
            cond = self.qp or self.ec > 1
            return cond if self.kind == "wtop" else not cond
        if self.kind in ("ctop", "cexit"):
            cond = self.lc > 0 or self.ec > 1
            return cond if self.kind == "ctop" else not cond
        raise ValueError(self.kind)

    @property
    def next_lc(self) -> int:
        return self.lc - 1 if self.kind in ("cloop", "ctop", "cexit") and self.lc else self.lc

    @property
    def next_ec(self) -> int:
        if self.kind in ("wtop", "wexit"):
            return self.ec - 1 if not self.qp and self.ec else self.ec
        if self.kind in ("ctop", "cexit"):
            return self.ec - 1 if not self.lc and self.ec else self.ec
        return self.ec

    @property
    def rotates(self) -> bool:
        if self.kind in ("wtop", "wexit"):
            return self.qp or self.ec > 0
        return self.kind in ("ctop", "cexit") and (self.lc > 0 or self.ec > 0)

    @property
    def vector(self) -> int:
        return TBV if self.taken and self.tb else SSV

    @property
    def isr_code(self) -> int:
        return 12 if self.taken and self.tb else 8

    @property
    def next_ri(self) -> int:
        return 0 if self.taken or self.slot == 2 else self.slot + 1

    @property
    def target_iip(self) -> str:
        return "target_bundle" if self.taken else ("post_source" if self.slot == 2 else "source_bundle")

    @property
    def saved_pfs(self) -> int:
        return (0x4008 | (self.ec << 52)) if self.is_call and self.taken else PFS_SENTINEL

    @property
    def dest_br(self) -> int | None:
        return {"b3-b0": 0, "b3-b3": 3, "b5-b1": 1, "b5-alias": 3}.get(self.kind)

    @property
    def initial_br(self) -> str | None:
        return "target_bundle" if self.kind == "b5-alias" else ("0x1110" if self.is_call else None)

PROFILES = {
    "cloop-lc2-ss": Profile("cloop", lc=2),
    "cloop-lc2-tb-ss": Profile("cloop", tb=True, lc=2),
    "cloop-lc0-ss": Profile("cloop"),
    "cloop-lc0-tb-ss": Profile("cloop", tb=True),
    "wtop-false-ec2-ss": Profile("wtop", ec=2),
    "wtop-false-ec2-tb-ss": Profile("wtop", tb=True, ec=2),
    "wtop-false-ec1-ss": Profile("wtop", ec=1),
    "wtop-false-ec1-tb-ss": Profile("wtop", tb=True, ec=1),
    "wtop-true-ec0-ss": Profile("wtop", qp=True),
    "wexit-false-ec0-ss": Profile("wexit"),
    "wexit-false-ec0-tb-ss": Profile("wexit", tb=True),
    "wexit-false-ec2-ss": Profile("wexit", ec=2),
    "wexit-false-ec2-tb-ss": Profile("wexit", tb=True, ec=2),
    "ctop-lc2-ss": Profile("ctop", lc=2),
    "ctop-lc2-tb-ss": Profile("ctop", tb=True, lc=2),
    "ctop-ec2-ss": Profile("ctop", ec=2),
    "ctop-ec2-tb-ss": Profile("ctop", tb=True, ec=2),
    "ctop-zero-ss": Profile("ctop"),
    "cexit-zero-ss": Profile("cexit"),
    "cexit-zero-tb-ss": Profile("cexit", tb=True),
    "cexit-lc2-ss": Profile("cexit", lc=2),
    "cexit-lc2-tb-ss": Profile("cexit", tb=True, lc=2),
    "b3-b0-ss": Profile("b3-b0", ec=2, lc=5),
    "b3-b0-tb-ss": Profile("b3-b0", tb=True, ec=2, lc=5),
    "b3-b3-slot0-ss": Profile("b3-b3", slot=0, ec=2, lc=5),
    "b3-b3-slot1-tb-ss": Profile("b3-b3", slot=1, tb=True, ec=2, lc=5),
    "b5-b1-ss": Profile("b5-b1", ec=2, lc=5),
    "b5-b1-tb-ss": Profile("b5-b1", tb=True, ec=2, lc=5),
    "b5-b1-slot0-ss": Profile("b5-b1", slot=0, ec=2, lc=5),
    "b5-b1-slot1-tb-ss": Profile("b5-b1", slot=1, tb=True, ec=2, lc=5),
    "b5-alias-b3-ss": Profile("b5-alias", ec=2, lc=5),
    "b5-alias-b3-tb-ss": Profile("b5-alias", tb=True, ec=2, lc=5),
    "b3-false-b0-ss": Profile("b3-b0", ec=2, lc=5, false_call=True),
    "b3-false-b0-tb-ss": Profile("b3-b0", tb=True, ec=2, lc=5, false_call=True),
    "b5-false-b1-slot1-ss": Profile("b5-b1", slot=1, ec=2, lc=5, false_call=True),
    "b5-false-b1-slot1-tb-ss": Profile("b5-b1", slot=1, tb=True, ec=2, lc=5, false_call=True),
}

def branch_instruction(p: Profile) -> str:
    if p.kind in ("wtop", "wexit"):
        return f"(p6) br.{p.kind}.sptk.few target_bundle"
    if p.kind in ("cloop", "ctop", "cexit"):
        return f"br.{p.kind}.sptk.few target_bundle"
    if p.kind in ("b3-b0", "b3-b3"):
        dest = "b0" if p.kind == "b3-b0" else "b3"
        op = f"br.call.sptk {dest}=target_bundle"
    elif p.kind == "b5-b1":
        op = "br.call.sptk.many b1=b6"
    elif p.kind == "b5-alias":
        op = "br.call.sptk.many b3=b3"
    else:
        raise ValueError(p.kind)
    return "(p6) " + op if p.false_call else op

def source_bundle(lines: list[str], p: Profile) -> None:
    op = branch_instruction(p)
    lines.append("source_bundle:")
    if p.slot == 2:
        lines.extend(["{ .mib", "nop.m 0", "nop.i 0", op, ";;", "}"])
    elif p.slot == 1 and p.is_call:
        lines.extend(["{ .mbb", "nop.m 0", op, "nop.b 0", ";;", "}"])
    elif p.slot == 0 and p.is_call:
        lines.extend(["{ .bbb", op, "nop.b 0", "nop.b 0", ";;", "}"])
    else:
        raise ValueError("B1/B2 loops require slot2; calls may use 0/1/2")

def check_state(lines: list[str], p: Profile) -> None:
    emit(lines, "i", "mov r9=ar.ec")
    compare(lines, 9, hex(p.next_ec << 58))
    emit(lines, "i", "mov r9=ar.lc")
    compare(lines, 9, p.next_lc)
    if p.kind in ("wtop", "wexit"):
        emit(lines, "b", "(p63) br.cond.sptk fail")
    emit(lines, "i", "mov r9=ar.pfs")
    compare(lines, 9, hex(p.saved_pfs))
    if p.dest_br is not None:
        emit(lines, "i", f"mov r9=b{p.dest_br}")
        literal(lines, 14, "post_source" if p.taken else p.initial_br)
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")

def setup(lines: list[str], p: Profile) -> None:
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")
    # Allocate an eight-GR rotating frame, CFM=SOF8|SOR1 = 0x4008.
    emit(lines, "m", "alloc r20=ar.pfs,0,0,8,8")
    literal(lines, 20, hex(PFS_SENTINEL))
    emit(lines, "i", "mov ar.pfs=r20")
    literal(lines, 20, p.ec)
    emit(lines, "i", "mov ar.ec=r20")
    literal(lines, 20, p.lc)
    emit(lines, "i", "mov ar.lc=r20")
    if p.kind in ("wtop", "wexit"):
        emit(lines, "i", "cmp.eq p63,p62=r0,r0")
        emit(lines, "i", "cmp.eq p6,p7=r0,r0" if p.qp else "cmp.ne p6,p7=r0,r0")
    else:
        emit(lines, "i", "cmp.ne p63,p62=r0,r0")
        if p.false_call:
            emit(lines, "i", "cmp.ne p6,p7=r0,r0")
    if p.dest_br is not None:
        literal(lines, 20, p.initial_br)
        emit(lines, "i", f"mov b{p.dest_br}=r20")
    if p.kind == "b5-b1":
        literal(lines, 20, "target_bundle")
        emit(lines, "i", "mov b6=r20")
    # Enable saved bit40 (PSR.ss) with rfi, live PSR.ic remains clear.
    literal(lines, 14, "source_bundle")
    emit(lines, "m", "mov cr.iip=r14")
    emit(lines, "m", "mov r9=psr")
    literal(lines, 14, hex(SS | (TB if p.tb else 0) | IC))
    emit(lines, "i", "or r9=r9,r14")
    literal(lines, 14, hex(RI))
    emit(lines, "i", "andcm r9=r9,r14")
    if p.slot:
        literal(lines, 14, hex(p.slot << 41))
        emit(lines, "i", "or r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")

def frame_probe(lines: list[str], p: Profile) -> None:
    # A NEW call at resumed target exposes the prior CFM in AR.PFS;
    # do not read implementation-only env->cfm or alter trap-time state.
    lines.append("frame_probe_entry:")
    emit(lines, "b", "br.call.sptk b5=frame_probe_target")
    emit(lines, "b", "br.cond.sptk fail")
    lines.append("frame_probe_target:")
    emit(lines, "i", "mov r9=ar.pfs")
    literal(lines, 14, hex(RRBG | CFM_SHAPE))
    emit(lines, "i", "and r9=r9,r14")
    expected = (8 if p.is_call and p.taken else 0x4008)
    if not p.is_call and p.rotates:
        expected |= 7 << 18
    compare(lines, 9, hex(expected))
    emit(lines, "b", "br.cond.sptk pass")

def generate(case: str) -> str:
    p = PROFILES[case]
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, p)
    source_bundle(lines, p)
    lines.append("post_source:")
    if p.taken:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        compare(lines, 15, 1)
        check_state(lines, p)
        emit(lines, "b", "br.cond.sptk frame_probe_entry")
    lines.append("target_bundle:")
    if p.taken:
        compare(lines, 15, 1)
        check_state(lines, p)
        emit(lines, "b", "br.cond.sptk frame_probe_entry")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    frame_probe(lines, p)
    lines.append("pass:")
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [f".org {IVT}", "ivt_base:",
              f".org {IVT + BREAK}", "break_vector:"]
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT + GENERAL}", "general_vector:"]
    emit(lines, "b", "br.cond.sptk fail")
    for vector, label in ((TBV, "tb_vector"), (SSV, "ss_vector")):
        lines += [f".org {IVT + vector}", label + ":"]
        emit(lines, "b", "br.cond.sptk trap_checks" if vector == p.vector else "br.cond.sptk fail")
    lines += [f".org {IVT + 0x9000}", "trap_checks:"]
    compare(lines, 15, 0)
    literal(lines, 15, 1)
    emit(lines, "m", "mov r9=cr.isr")
    compare(lines, 9, hex((p.slot << 41) | p.isr_code))
    emit(lines, "m", "mov r9=cr.iipa")
    literal(lines, 14, "source_bundle")
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.iip")
    literal(lines, 14, p.target_iip)
    emit(lines, "i", "cmp.eq p6,p7=r9,r14")
    emit(lines, "b", "(p7) br.cond.spnt fail")
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(RI | SS | TB | IC))
    emit(lines, "i", "and r10=r9,r14")
    compare(lines, 10, hex((p.next_ri << 41) | SS | (TB if p.tb else 0) | IC))
    check_state(lines, p)
    emit(lines, "m", "mov r9=cr.ipsr")
    literal(lines, 14, hex(SS | TB))
    emit(lines, "i", "andcm r9=r9,r14")
    emit(lines, "m", "mov cr.ipsr=r9")
    emit(lines, "b", "rfi")
    return "\n".join(lines) + "\n"

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="./build/qemu-system-ia64")
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-branch-ss-loop-call"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--assemble-only", action="store_true")
    ap.add_argument("--only", choices=list(PROFILES))
    args = ap.parse_args()
    if not 0 < args.timeout <= 60:
        ap.error("--timeout must be in (0,60]")
    args.out = args.out.resolve()
    chosen = [args.only] if args.only else list(PROFILES)
    results, failures = [], []
    for case in chosen:
        try:
            results.append(ivt.run_one(case, args,
                                       case_spec=(PROFILES[case].vector,PASS,FAIL),
                                       source=generate(case)))
        except RuntimeError as exc:
            failures.append((case, str(exc)))
            file = args.out / case / "result.json"
            if file.exists():
                results.append(json.loads(file.read_text()))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps({
        "reference": "Intel Itanium SDM rev2.3 Vol2 Table 8-3 and Vol3 br",
        "scope": "post-commit B1/B2 and B3/B5 SS-only/TB+SS; LC/EC, CFM/PFS/BR",
        "cases": len(chosen), "results": results, "failures": failures,
    }, indent=2) + "\n")
    ok = sum(x.get("execution") == ("not-run" if args.assemble_only else "pass") for x in results)
    print(f"IA64 BRANCH SS {'ASSEMBLY' if args.assemble_only else 'EXECUTION'}:"
          f" {ok}/{len(chosen)} profiles passed")
    if failures:
        raise RuntimeError("; ".join(f"{case}: {err}" for case, err in failures))

if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
