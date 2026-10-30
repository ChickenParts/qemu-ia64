#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free real Itanium IVT guests: UIA trap vs LP/TB/SS priority.

The original Merced model implements 44 physical-address bits. Bad target
0x100000000000 is an architecturally unimplemented physical branch
address, even when PSR.tb=PSR.ss=0. We choose the architecture's UIA
TRAP path (vector 0x5e00), not the alternatively permitted fetch FAULT.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location(
    "ia64_uia_ivt",ROOT/"scripts/run-ia64-fp-exception-tests.py")
ivt=importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit,literal,compare=ivt.emit,ivt.literal,ivt.compare

IVT=0x400000
UIA=0x5e00
TBV=0x5f00
SSV=0x6000
BREAK=0x2c00
GENERAL=0x5400
TB,SS,IC,RI=(1<<26),(1<<40),(1<<13),(3<<41)
BAD_PA=0x100000000000
BAD_PA2=0x200000000000
PFS_SENTINEL=0x3000000000000000
PASS,FAIL="0x75696170","0x75696166"

@dataclass(frozen=True)
class Profile:
    kind: str = "cond"
    slot: int = 2
    tb: bool = False
    ss: bool = False
    bad: bool = True
    false_qp: bool = False
    second_bad: bool = False

    @property
    def taken(self)->bool:
        return not self.false_qp

    @property
    def target(self)->str:
        return hex(BAD_PA2 if self.second_bad else BAD_PA) if self.bad else "target_bundle"

    @property
    def vec(self)->int|None:
        if not self.taken:
            return SSV if self.ss else None
        if self.bad:
            return UIA
        if self.tb:
            return TBV
        return SSV if self.ss else None

    @property
    def isr_code(self)->int:
        if not self.taken:
            return 8 if self.ss else 0
        return (4 if self.tb else 0) | (8 if self.ss else 0)

    @property
    def saved_ri(self)->int:
        return 0 if self.taken or self.slot==2 else self.slot+1

    @property
    def saved_iip(self)->str:
        if self.taken:
            return self.target
        return "post_source" if self.slot==2 else "source_bundle"

    @property
    def return_br(self)->int|None:
        return 1 if self.kind=="call" else None

PROFILES={
    "cond-uia-no-flags-b2": Profile(),
    "cond-uia-ss-b2": Profile(ss=True),
    "cond-uia-tb-b2": Profile(tb=True),
    "cond-uia-tb-ss-b2": Profile(tb=True,ss=True),
    "cond-uia-ss-b0": Profile(slot=0,ss=True),
    "cond-uia-tb-ss-b1": Profile(slot=1,tb=True,ss=True),
    "cond-uia-second-pa-ss": Profile(second_bad=True,ss=True),
    "call-uia-no-flags": Profile(kind="call"),
    "call-uia-ss": Profile(kind="call",ss=True),
    "call-uia-tb-ss": Profile(kind="call",tb=True,ss=True),
    "call-uia-slot0-tb-ss": Profile(kind="call",slot=0,tb=True,ss=True),
    "ret-uia-no-flags": Profile(kind="ret"),
    "ret-uia-tb-ss": Profile(kind="ret",tb=True,ss=True),
    "false-cond-no-flags": Profile(false_qp=True),
    "false-cond-ss": Profile(false_qp=True,ss=True),
    "false-cond-tb-ss": Profile(false_qp=True,ss=True,tb=True),
    "false-call-ss": Profile(kind="call",false_qp=True,ss=True),
    "valid-cond-no-flags": Profile(bad=False),
    "valid-cond-ss": Profile(bad=False,ss=True),
    "valid-cond-tb-ss": Profile(bad=False,tb=True,ss=True),
}

def instr(p:Profile)->str:
    if p.kind=="cond":
        base="br.cond.sptk.few b2"
    elif p.kind=="ret":
        base="br.ret.sptk.few b2"
    elif p.kind=="call":
        base="br.call.sptk.many b1=b6"
    else:
        raise ValueError(p.kind)
    return "(p6) "+base if p.false_qp else base

def source(lines:list[str],p:Profile)->None:
    op=instr(p)
    lines.append("source_bundle:")
    if p.slot==2:
        lines.extend(["{ .mib","nop.m 0","nop.i 0",op,";;","}"])
    elif p.slot==1:
        lines.extend(["{ .mbb","nop.m 0",op,"nop.b 0",";;","}"])
    elif p.slot==0:
        lines.extend(["{ .bbb",op,"nop.b 0","nop.b 0",";;","}"])
    else:
        raise ValueError(p.slot)

def check_state(lines:list[str],p:Profile)->None:
    if p.kind=="call":
        emit(lines,"i","mov r9=b1")
        literal(lines,14,"post_source" if p.taken else "0x1110")
        emit(lines,"i","cmp.eq p6,p7=r9,r14")
        emit(lines,"b","(p7) br.cond.spnt fail")
        emit(lines,"i","mov r9=ar.pfs")
        compare(lines,9,hex((0x4008|(2<<52)) if p.taken else PFS_SENTINEL))
    elif p.kind=="cond":
        emit(lines,"i","mov r9=b2")
        literal(lines,14,p.target)
        emit(lines,"i","cmp.eq p6,p7=r9,r14")
        emit(lines,"b","(p7) br.cond.spnt fail")
        emit(lines,"i","mov r9=ar.pfs")
        compare(lines,9,hex(PFS_SENTINEL))

def generate(case:str)->str:
    p=PROFILES[case]
    lines=[".text",".explicit",".align 16",".global _start","_start:"]
    literal(lines,14,"ivt_base")
    emit(lines,"m","mov cr.iva=r14")
    emit(lines,"m","srlz.i")
    literal(lines,15,0)
    literal(lines,14,"fail")
    emit(lines,"i","mov b7=r14")
    emit(lines,"m","alloc r20=ar.pfs,0,0,8,8")
    literal(lines,20,hex(PFS_SENTINEL))
    emit(lines,"i","mov ar.pfs=r20")
    if p.kind=="call":
        literal(lines,20,2)
        emit(lines,"i","mov ar.ec=r20")
        literal(lines,20,"0x1110")
        emit(lines,"i","mov b1=r20")
        literal(lines,20,p.target)
        emit(lines,"i","mov b6=r20")
    else:
        literal(lines,20,p.target)
        emit(lines,"i","mov b2=r20")
    if p.false_qp:
        emit(lines,"i","cmp.ne p6,p7=r0,r0")
    # Enter at the source RI. No extraneous filler slot can trap first.
    literal(lines,14,"source_bundle")
    emit(lines,"m","mov cr.iip=r14")
    emit(lines,"m","mov r9=psr")
    literal(lines,14,hex(IC|(TB if p.tb else 0)|(SS if p.ss else 0)))
    emit(lines,"i","or r9=r9,r14")
    literal(lines,14,hex(RI))
    emit(lines,"i","andcm r9=r9,r14")
    if p.slot:
        literal(lines,14,hex(p.slot<<41))
        emit(lines,"i","or r9=r9,r14")
    emit(lines,"m","mov cr.ipsr=r9")
    emit(lines,"b","rfi")

    source(lines,p)
    lines.append("post_source:")
    if p.taken:
        if not p.bad:
            emit(lines,"b","br.cond.sptk fail")
        else:
            compare(lines,15,1)
            check_state(lines,p)
            emit(lines,"b","br.cond.sptk pass")
    else:
        compare(lines,15,1 if p.vec else 0)
        check_state(lines,p)
        emit(lines,"b","br.cond.sptk pass")
    lines.append("target_bundle:")
    if not p.bad and p.taken:
        compare(lines,15,1 if p.vec else 0)
        check_state(lines,p)
        emit(lines,"b","br.cond.sptk pass")
    else:
        emit(lines,"b","br.cond.sptk fail")

    lines.append("pass:")
    literal(lines,14,"pass_spin")
    emit(lines,"i","mov b6=r14")
    ivt.terminal(lines,PASS,FAIL)
    lines += [f".org {IVT}","ivt_base:",
              f".org {IVT+BREAK}","break_vector:"]
    emit(lines,"b","br.cond.sptk b6")
    lines += [f".org {IVT+GENERAL}","general_vector:"]
    emit(lines,"b","br.cond.sptk fail")
    for vec,name in ((UIA,"uia_vector"),(TBV,"tb_vector"),(SSV,"ss_vector")):
        lines += [f".org {IVT+vec}",name+":"]
        emit(lines,"b","br.cond.sptk handler_checks" if vec==p.vec
             else "br.cond.sptk fail")
    lines += [f".org {IVT+0x9000}","handler_checks:"]
    if p.vec:
        compare(lines,15,0)
        literal(lines,15,1)
        emit(lines,"m","mov r9=cr.isr")
        compare(lines,9,hex((p.slot<<41)|p.isr_code))
        emit(lines,"m","mov r9=cr.iipa")
        literal(lines,14,"source_bundle")
        emit(lines,"i","cmp.eq p6,p7=r9,r14")
        emit(lines,"b","(p7) br.cond.spnt fail")
        emit(lines,"m","mov r9=cr.iip")
        literal(lines,14,p.saved_iip)
        emit(lines,"i","cmp.eq p6,p7=r9,r14")
        emit(lines,"b","(p7) br.cond.spnt fail")
        emit(lines,"m","mov r9=cr.ipsr")
        literal(lines,14,hex(RI|SS|TB|IC))
        emit(lines,"i","and r10=r9,r14")
        compare(lines,10,hex((p.saved_ri<<41) |
                             (SS if p.ss else 0) |
                             (TB if p.tb else 0) | IC))
        check_state(lines,p)
        # An unimplemented target cannot be fetched: patch the saved IIP
        # to one known-good instruction and clear the enabled debug bits.
        if p.bad and p.taken:
            literal(lines,14,"post_source")
            emit(lines,"m","mov cr.iip=r14")
        emit(lines,"m","mov r9=cr.ipsr")
        literal(lines,14,hex(SS|TB|(RI if p.bad and p.taken else 0)))
        emit(lines,"i","andcm r9=r9,r14")
        emit(lines,"m","mov cr.ipsr=r9")
        emit(lines,"b","rfi")
    else:
        emit(lines,"b","br.cond.sptk fail")
    return "\n".join(lines)+"\n"

def main()->None:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu",default="./build/qemu-system-ia64")
    ap.add_argument("--out",type=Path,default=Path("scratch/ia64-uia"))
    ap.add_argument("--timeout",type=float,default=10)
    ap.add_argument("--assemble-only",action="store_true")
    ap.add_argument("--only",choices=list(PROFILES))
    args=ap.parse_args()
    if not 0<args.timeout<=60:
        ap.error("--timeout must be in (0,60]")
    args.out=args.out.resolve()
    selected=[args.only] if args.only else list(PROFILES)
    results,failures=[],[]
    for c in selected:
        try:
            results.append(ivt.run_one(
                c,args,case_spec=(PROFILES[c].vec or UIA,PASS,FAIL),
                source=generate(c)))
        except RuntimeError as e:
            failures.append((c,str(e)))
            f=args.out/c/"result.json"
            if f.exists():
                results.append(json.loads(f.read_text()))
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/"summary.json").write_text(json.dumps({
        "reference":"Intel Itanium SDM Vol2 rev2.3 §4.3.3, §8, UIA/LP 0x5e00",
        "model":"Itanium / Merced, 54 total VA bits and 44 PA bits, UIA trap",
        "cases":len(selected),"results":results,"failures":failures,
    },indent=2)+"\n")
    count=sum(x.get("execution")==("not-run" if args.assemble_only else "pass")
              for x in results)
    print(f"IA64 UIA {'ASSEMBLY' if args.assemble_only else 'EXECUTION'}: "
          f"{count}/{len(selected)} profiles passed")
    if failures:
        raise RuntimeError("; ".join(c+": "+err for c,err in failures))

if __name__=="__main__":
    try:
        main()
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as e:
        sys.exit(str(e))
