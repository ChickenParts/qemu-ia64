#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real IVT + side-effect tests for IA-64 B1/B2 loop execution and slots."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
def load(path,key):
    spec=importlib.util.spec_from_file_location(key,ROOT/"scripts"/path)
    obj=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(obj)
    return obj
ivt=load("run-ia64-fp-exception-tests.py","b12_loop_ivt")
oracle=load("ia64-b12-loop-byte-oracle.py","b12_loop_gnu")
emit,literal,compare=ivt.emit,ivt.literal,ivt.compare
IVT,BREAK,GENERAL=0x400000,0x2c00,0x5400
PASS,FAIL="0x62324c70","0x62324c66"
RI_MASK=3<<41
RRBG_MASK=0x7f<<18
EC_VALUES=(0,1,2,63)
# 10 executed while profiles: both false p6 and true p6.
POSITIVES={
    **{f"{kind}-false-ec{ec}":(kind,False,ec)
       for kind in ("wtop","wexit") for ec in EC_VALUES},
    "wtop-true-ec0":("wtop",True,0),
    "wexit-true-ec0":("wexit",True,0),
}
FAULTS={f"{kind}-slot{slot}":(kind,slot,btype,qp,asm)
        for kind,asm,btype,qp in oracle.FORMS
        for slot in (0,1)}
PROFILES={**FAULTS,**POSITIVES}

def terminal(lines:list[str],slot:int|None)->None:
    literal(lines,14,"pass_spin")
    emit(lines,"i","mov b6=r14")
    ivt.terminal(lines,PASS,FAIL)
    lines += [f".org {IVT}","ivt_base:",
              f".org {IVT+BREAK}","break_vector:"]
    emit(lines,"b","br.cond.sptk b6")
    lines += [f".org {IVT+GENERAL}","general_vector:"]
    if slot is None:
        emit(lines,"b","br.cond.sptk fail")
        return
    compare(lines,15,0)
    literal(lines,15,1)
    emit(lines,"m","mov r9=cr.isr")
    compare(lines,9,hex(slot<<41))
    emit(lines,"m","mov r9=cr.iip")
    literal(lines,14,"fault_bundle")
    emit(lines,"i","cmp.eq p6,p7=r9,r14")
    emit(lines,"b","(p7) br.cond.spnt fail")
    emit(lines,"m","mov r9=cr.ipsr")
    literal(lines,14,hex(RI_MASK))
    emit(lines,"i","and r9=r9,r14")
    compare(lines,9,hex(slot<<41))
    emit(lines,"m","mov r9=cr.iim")
    compare(lines,9,0)
    emit(lines,"i","mov r9=ar.ec")
    compare(lines,9,hex(2<<58))
    emit(lines,"i","mov r9=ar.lc")
    compare(lines,9,5)
    literal(lines,14,"after_loop")
    emit(lines,"m","mov cr.iip=r14")
    emit(lines,"m","mov r9=cr.ipsr")
    literal(lines,14,hex(RI_MASK))
    emit(lines,"i","andcm r9=r9,r14")
    emit(lines,"m","mov cr.ipsr=r9")
    emit(lines,"b","rfi")

def setup(lines:list[str],ec:int,lc:int,*,true_p6:bool)->None:
    literal(lines,14,"ivt_base")
    emit(lines,"m","mov cr.iva=r14")
    emit(lines,"m","srlz.i")
    literal(lines,14,"fail")
    emit(lines,"i","mov b6=r14")
    literal(lines,15,0)
    emit(lines,"m","alloc r20=ar.pfs,0,0,8,8")
    literal(lines,20,ec)
    emit(lines,"i","mov ar.ec=r20")
    literal(lines,20,lc)
    emit(lines,"i","mov ar.lc=r20")
    # Architecturally nonzero PR63 makes absence of the required write visible.
    emit(lines,"i","cmp.eq p63,p62=r0,r0")
    if true_p6:
        emit(lines,"i","cmp.eq p6,p7=r0,r0")
    else:
        emit(lines,"i","cmp.ne p6,p7=r0,r0")

def check_state(lines:list[str],*,ec:int,lc:int,rotated:bool)->None:
    emit(lines,"i","mov r9=ar.ec")
    compare(lines,9,hex(ec<<58))
    emit(lines,"i","mov r9=ar.lc")
    compare(lines,9,lc)
    emit(lines,"b","(p63) br.cond.sptk fail")
    # The actual br.call helper snapshots the CFM before it changes.
    # One register rotation of a size-8 window yields RRB.GR = 7.
    emit(lines,"b","br.call.sptk b0=rrb_snapshot")
    emit(lines,"b","br.cond.sptk fail")
    lines += ["rrb_snapshot:"]
    emit(lines,"i","mov r9=ar.pfs")
    literal(lines,14,hex(RRBG_MASK))
    emit(lines,"i","and r9=r9,r14")
    compare(lines,9,hex((7<<18) if rotated else 0))

def positive_source(case:str)->str:
    kind,true_p6,ec=POSITIVES[case]
    taken=(true_p6 or ec>1)
    if kind=="wexit":
        taken=not taken
    rotation=true_p6 or ec>0
    next_ec=ec if true_p6 or ec==0 else ec-1
    lines=[".text",".explicit",".align 16",".global _start","_start:"]
    setup(lines,ec,0,true_p6=true_p6)
    lines += ["loop_bundle:","{ .mib","nop.m 0","nop.i 0",
              f"(p6) br.{kind}.sptk.few branch_target",";;","}"]
    lines += ["fallthrough:"]
    if taken:
        emit(lines,"b","br.cond.sptk fail")
    else:
        check_state(lines,ec=next_ec,lc=0,rotated=rotation)
        emit(lines,"b","br.cond.sptk pass")
    lines += ["branch_target:"]
    if taken:
        check_state(lines,ec=next_ec,lc=0,rotated=rotation)
        emit(lines,"b","br.cond.sptk pass")
    else:
        emit(lines,"b","br.cond.sptk fail")
    lines += ["pass:"]
    terminal(lines,None)
    return "\n".join(lines)+"\n"

def fault_source(case:str,bundle:bytes)->str:
    kind,slot,_,_,_=FAULTS[case]
    lines=[".text",".explicit",".align 16",".global _start","_start:"]
    setup(lines,2,5,true_p6=False)
    lines += [".align 16","fault_bundle:",
              ".byte "+", ".join(f"0x{x:02x}" for x in bundle),
              ".align 16","after_loop:"]
    compare(lines,15,1)
    emit(lines,"i","mov r9=ar.ec")
    compare(lines,9,hex(2<<58))
    emit(lines,"i","mov r9=ar.lc")
    compare(lines,9,5)
    emit(lines,"b","br.cond.sptk pass")
    lines += ["pass:"]
    terminal(lines,slot)
    return "\n".join(lines)+"\n"

def main()->None:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu",default="./build/qemu-system-ia64")
    ap.add_argument("--out",type=Path,default=Path("scratch/ia64-b12-loops"))
    ap.add_argument("--timeout",type=float,default=10)
    ap.add_argument("--assemble-only",action="store_true")
    ap.add_argument("--only",choices=list(PROFILES))
    args=ap.parse_args()
    if not 0<args.timeout<=60:
        ap.error("timeout must be in (0,60]")
    args.out=args.out.resolve()
    selected=[args.only] if args.only else list(PROFILES)
    bundles={}
    for kind,asm,_,_ in oracle.FORMS:
        bundles[kind]=oracle.assemble(asm,args.out/"gnu-bases",kind)
    results=[]
    for case in selected:
        if case in FAULTS:
            kind,slot,_,_,_=FAULTS[case]
            bundle=oracle.byte_swap(bundles[kind],slot)
            source=fault_source(case,bundle)
        else:
            source=positive_source(case)
        results.append(ivt.run_one(case,args,case_spec=(GENERAL,PASS,FAIL),
                                   source=source))
    report={"reference":"Intel SDM rev2.3 Vol3 br pp3:23–27",
            "cases":len(results),
            "scope":"B1/B2 while false qp and slot-legality; PSR.tb separate",
            "results":results}
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/"summary.json").write_text(json.dumps(report,indent=2)+"\n")
    mode="ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 B1/B2 LOOPS {mode} PASS: {len(results)}/{len(PROFILES)} profiles")
if __name__=="__main__":
    try:
        main()
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as e:
        sys.exit(str(e))
