#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real IA-64 guest tests: taken-branch post-commit trap and rfi target resume."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "ia64_tb_ivt", ROOT / "scripts/run-ia64-fp-exception-tests.py")
ivt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit, literal, compare = ivt.emit, ivt.literal, ivt.compare
IVT, TAKEN, BREAK, GENERAL = 0x400000, 0x5f00, 0x2c00, 0x5400
PASS, FAIL = "0x54627061", "0x54626661"
TB, IC, RI_MASK = (1 << 26), (1 << 13), (3 << 41)

# kind, source slot, should take, PSR.tb enabled, EC, LC, fpsr
PROFILES = {
    "cond-b0": ("cond", 0, True, True, 0, 0, 0),
    "cond-b1": ("cond", 1, True, True, 0, 0, 0),
    "cond-b2": ("cond", 2, True, True, 0, 0, 0),
    "false-cond-b0": ("cond", 0, False, True, 0, 0, 0),
    "false-cond-b1": ("cond", 1, False, True, 0, 0, 0),
    "false-cond-b2": ("cond", 2, False, True, 0, 0, 0),
    "tb-disabled-cond-b2": ("cond", 2, True, False, 0, 0, 0),
    "call-imm-b2": ("call", 2, True, True, 0, 0, 0),
    "call-reg-b2": ("callreg", 2, True, True, 0, 0, 0),
    "indirect-b2": ("indirect", 2, True, True, 0, 0, 0),
    "cloop-lc2-b2": ("cloop", 2, True, True, 0, 2, 0),
    "wtop-false-ec2-b2": ("wtop", 2, True, True, 2, 0, 0),
    "wtop-false-ec1-b2": ("wtop", 2, False, True, 1, 0, 0),
    "fchkf-true-f2": ("fchkf", 2, True, True, 0, 0, 1 << 26),
    "fchkf-false-f2": ("fchkf", 2, False, True, 0, 0, 0),
    "brl-cond-x2": ("brl", 2, True, True, 0, 0, 0),
    "brl-call-x2": ("brlcall", 2, True, True, 0, 0, 0),
}

def setup(lines: list[str], kind: str, expect: bool,
          tb_enabled: bool, ec: int, lc: int, fpsr: int) -> None:
    literal(lines, 14, "ivt_base")
    emit(lines, "m", "mov cr.iva=r14")
    emit(lines, "m", "srlz.i")
    literal(lines, 15, 0)
    literal(lines, 14, "fail")
    emit(lines, "i", "mov b7=r14")
    # PSR.ic=1 makes CR.IIPA and ISR.ei defined at the interruption.
    emit(lines, "m", "ssm 0x2000")
    if tb_enabled:
        # ssm/rsm take imm24: PSR.tb (bit26) cannot be expressed there.
        # Change psr.l via the architected register move, preserving all
        # other current PSR low bits including ic/CPL.
        emit(lines, "m", "mov r9=psr")
        literal(lines, 20, hex(TB))
        emit(lines, "i", "or r20=r9,r20")
        emit(lines, "m", "mov psr.l=r20")
    emit(lines, "m", "srlz.d")
    if kind in ("wtop", "cloop"):
        emit(lines, "m", "alloc r20=ar.pfs,0,0,8,8")
    if kind == "wtop":
        literal(lines, 20, ec)
        emit(lines, "i", "mov ar.ec=r20")
        emit(lines, "i", "cmp.ne p6,p7=r0,r0")  # p6 is false
    elif kind == "cloop":
        literal(lines, 20, lc)
        emit(lines, "i", "mov ar.lc=r20")
    elif kind == "fchkf":
        literal(lines, 20, hex(fpsr))
        emit(lines, "m", "mov ar.fpsr=r20")
    elif kind == "cond" and not expect:
        emit(lines, "i", "cmp.ne p6,p7=r0,r0")
    if kind in ("indirect", "callreg"):
        literal(lines, 14, "target_bundle")
        emit(lines, "i", "mov b6=r14")

def branch(lines: list[str], kind: str, slot: int, expect: bool) -> None:
    if kind == "fchkf":
        emit(lines, "f", "fchkf.s1 target_bundle")
        return
    if kind.startswith("brl"):
        insn = ("brl.call.sptk b0=target_bundle"
                if kind == "brlcall" else "brl.cond.sptk target_bundle")
        emit(lines, "l", insn)
        return
    if kind == "cond":
        insn = ("br.cond.sptk target_bundle" if expect else
                "(p6) br.cond.sptk target_bundle")
    elif kind == "call":
        insn = "br.call.sptk b0=target_bundle"
    elif kind == "callreg":
        insn = "br.call.sptk b0=b6"
    elif kind == "indirect":
        insn = "br.cond.sptk b6"
    elif kind == "cloop":
        insn = "br.cloop.sptk.few target_bundle"
    elif kind == "wtop":
        insn = "(p6) br.wtop.sptk.few target_bundle"
    else:
        raise ValueError(f"unsupported kind {kind}")
    if slot == 0:
        lines.extend(["{ .bbb", insn, "nop.b 0", "nop.b 0", ";;", "}"])
    elif slot == 1:
        lines.extend(["{ .mbb", "nop.m 0", insn, "nop.b 0", ";;", "}"])
    elif slot == 2:
        lines.extend(["{ .mib", "nop.m 0", "nop.i 0", insn, ";;", "}"])
    else:
        raise ValueError("only IA-64 slots 0/1/2")

def check_committed(lines: list[str], case: str, kind: str) -> None:
    if kind == "cloop":
        emit(lines, "i", "mov r9=ar.lc")
        compare(lines, 9, 1)
    elif kind == "wtop":
        emit(lines, "i", "mov r9=ar.ec")
        compare(lines, 9, hex(1 << 58))
        emit(lines, "b", "(p63) br.cond.sptk fail")
    if kind in ("call", "callreg", "brlcall"):
        emit(lines, "i", "mov r9=b0")
        literal(lines, 14, "post_source")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")

def generate(case: str) -> str:
    kind, slot, expect, enabled, ec, lc, fpsr = PROFILES[case]
    do_trap = enabled and expect
    lines = [".text", ".explicit", ".align 16", ".global _start", "_start:"]
    setup(lines, kind, expect, enabled, ec, lc, fpsr)
    lines.append("source_bundle:")
    branch(lines, kind, slot, expect)
    lines.append("post_source:")
    if expect:
        # Reaching fallthrough means a taken branch was ignored.
        emit(lines, "b", "br.cond.sptk fail")
    else:
        # PSR.tb is not in the immediate mask of rsm.
        emit(lines, "m", "mov r9=psr")
        literal(lines, 14, hex(TB))
        emit(lines, "i", "andcm r9=r9,r14")
        emit(lines, "m", "mov psr.l=r9")
        emit(lines, "m", "srlz.d")
        compare(lines, 15, 0)
        emit(lines, "b", "br.cond.sptk pass")
    lines.append("target_bundle:")
    if expect:
        compare(lines, 15, 1 if do_trap else 0)
        check_committed(lines, case, kind)
        # PSR.tb is not in the immediate mask of rsm.
        emit(lines, "m", "mov r9=psr")
        literal(lines, 14, hex(TB))
        emit(lines, "i", "andcm r9=r9,r14")
        emit(lines, "m", "mov psr.l=r9")
        emit(lines, "m", "srlz.d")
        emit(lines, "b", "br.cond.sptk pass")
    else:
        emit(lines, "b", "br.cond.sptk fail")
    lines.append("pass:")
    literal(lines, 14, "pass_spin")
    emit(lines, "i", "mov b6=r14")
    ivt.terminal(lines, PASS, FAIL)
    lines += [f".org {IVT}", "ivt_base:",
              f".org {IVT + BREAK}", "break_vector:"]
    # Terminal pass/fail break.m is an intentional logging marker.
    emit(lines, "b", "br.cond.sptk b6")
    lines += [f".org {IVT + GENERAL}", "general_vector:"]
    emit(lines, "b", "br.cond.sptk fail")
    lines += [f".org {IVT + TAKEN}", "taken_branch_handler:"]
    if not do_trap:
        emit(lines, "b", "br.cond.sptk fail")
    else:
        compare(lines, 15, 0)
        literal(lines, 15, 1)
        emit(lines, "m", "mov r9=cr.isr")
        compare(lines, 9, hex((slot << 41) | 4))
        emit(lines, "m", "mov r9=cr.iip")
        literal(lines, 14, "target_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.iipa")
        literal(lines, 14, "source_bundle")
        emit(lines, "i", "cmp.eq p6,p7=r9,r14")
        emit(lines, "b", "(p7) br.cond.spnt fail")
        emit(lines, "m", "mov r9=cr.ipsr")
        literal(lines, 14, hex(RI_MASK | TB | IC))
        emit(lines, "i", "and r10=r9,r14")
        compare(lines, 10, hex(TB | IC))
        # Clear TB only in saved IPSR; rfi must return to target, not
        # retrigger the same branch. The handler itself runs with tb=0.
        literal(lines, 14, hex(TB))
        emit(lines, "i", "andcm r9=r9,r14")
        emit(lines, "m", "mov cr.ipsr=r9")
        check_committed(lines, case, kind)
        emit(lines, "b", "rfi")
    return "\n".join(lines) + "\n"

def main() -> None:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default="./build/qemu-system-ia64")
    ap.add_argument("--out", type=Path, default=Path("scratch/ia64-taken-branch"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--assemble-only", action="store_true")
    ap.add_argument("--only", choices=list(PROFILES))
    args=ap.parse_args()
    if not 0<args.timeout<=60:
        ap.error("--timeout must be in (0,60]")
    args.out=args.out.resolve()
    chosen=[args.only] if args.only else list(PROFILES)
    results=[ivt.run_one(k,args,case_spec=(TAKEN,PASS,FAIL),
                         source=generate(k)) for k in chosen]
    data={"reference":"Intel SDM Vol2 rev2.3 §§7.1,8 and Vol3 br/fchkf/brl",
          "scope":"PSR.tb branch target resume, source slot, side effects",
          "total":len(PROFILES), "results":results}
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/"summary.json").write_text(json.dumps(data,indent=2)+"\n")
    mode="ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 PSR.TB {mode} PASS: {len(results)}/{len(PROFILES)} profiles")
if __name__=="__main__":
    try: main()
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
