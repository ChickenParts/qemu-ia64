#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-IVT return CPL demotion and Lower-Privilege-Transfer priority tests."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location(
    "ia64_lp_ivt", ROOT/"scripts/run-ia64-fp-exception-tests.py")
ivt=importlib.util.module_from_spec(spec)
spec.loader.exec_module(ivt)
emit,literal,compare=ivt.emit,ivt.literal,ivt.compare
IVT=0x400000
LPV,TBV,SSV,BREAK,GENERAL=0x5e00,0x5f00,0x6000,0x2c00,0x5400
LP,TB,SS,IC=(1<<25),(1<<26),(1<<40),(1<<13)
RI_MASK=3<<41
CPL_MASK=3<<32
PASS,FAIL="0x6c707061","0x6c706661"

# saved PFS.ppl, source slot, PSR.lp, PSR.tb, false qp, PSR.ss.
PROFILES={
    "lp-ppl3-b0":(3,0,True,False,False,False),
    "lp-ppl3-b1":(3,1,True,False,False,False),
    "lp-ppl3-b2":(3,2,True,False,False,False),
    "lp-tb-ppl3-b0":(3,0,True,True,False,False),
    "lp-tb-ppl3-b1":(3,1,True,True,False,False),
    "lp-tb-ppl3-b2":(3,2,True,True,False,False),
    "lp-tb-ppl1-b2":(1,2,True,True,False,False),
    "lp-tb-ppl2-b2":(2,2,True,True,False,False),
    "tb-only-ppl3-b2":(3,2,False,True,False,False),
    "no-flags-ppl3-b2":(3,2,False,False,False,False),
    "lp-no-demotion-ppl0-b2":(0,2,True,False,False,False),
    "tb-no-demotion-ppl0-b2":(0,2,True,True,False,False),
    "false-qp-ppl3-b1":(3,1,True,True,True,False),
    # SS requires CR.IPSR.ss + RFI, not mov psr.l or ssm.
    "lp-ss-ppl3-b2":(3,2,True,False,False,True),
    "lp-tb-ss-ppl3-b2":(3,2,True,True,False,True),
    "tb-ss-ppl0-b2":(0,2,False,True,False,True),
    "ss-only-ppl0-b2":(0,2,False,False,False,True),
}

def expected_vector(ppl,lp,tb,false_qp,ss=False):
    if false_qp:
        return None
    if ppl>0 and lp:
        return LPV
    if tb:
        return TBV
    if ss:
        return SSV
    return None

def setup(lines,ppl,enable_lp,enable_tb,false_qp,enable_ss,source_slot):
    literal(lines,14,"ivt_base")
    emit(lines,"m","mov cr.iva=r14")
    emit(lines,"m","srlz.i")
    literal(lines,15,0)
    literal(lines,14,"fail")
    emit(lines,"i","mov b7=r14")
    literal(lines,14,"target_bundle")
    emit(lines,"i","mov b2=r14")
    # Use an independent architected AR.PFS value (no shadow call frame)
    # with PFS.ppl in bits63:62. Source CPL starts at privileged CPL0.
    literal(lines,20,hex(ppl<<62))
    emit(lines,"i","mov ar.pfs=r20")
    # PSR.lp/tb bits25/26 exceed the 24-bit ssm/rsm immediate.
    emit(lines,"m","mov r9=psr")
    literal(lines,14,hex(IC|(LP if enable_lp else 0)|(TB if enable_tb else 0)))
    emit(lines,"i","or r9=r9,r14")
    if not enable_ss:
        # Original low-PSR path for non-single-step return profiles.
        emit(lines,"m","mov psr.l=r9")
        emit(lines,"m","srlz.d")
    if false_qp:
        emit(lines,"i","cmp.ne p6,p7=r0,r0")
    if enable_ss:
        # Bit 40 cannot be set through mov psr.l (low 32 bits), ssm or rsm.
        # Enter directly at the return's source slot; stepping earlier
        # filler NOPs would exercise a different instruction.
        literal(lines,14,"source_bundle")
        emit(lines,"m","mov cr.iip=r14")
        # r9 already contains PSR plus LP/TB/IC. Only saved IPSR
        # acquires these flags, while live PSR.ic remains clear for RFI.
        literal(lines,14,hex(SS))
        emit(lines,"i","or r9=r9,r14")
        literal(lines,14,hex(RI_MASK))
        emit(lines,"i","andcm r9=r9,r14")
        if source_slot:
            literal(lines,14,hex(source_slot<<41))
            emit(lines,"i","or r9=r9,r14")
        emit(lines,"m","mov cr.ipsr=r9")
        emit(lines,"b","rfi")

def br_ret(lines,slot,false_qp):
    spelling="(p6) br.ret.sptk.few b2" if false_qp else "br.ret.sptk.few b2"
    if slot==0:
        lines.extend(["{ .bbb",spelling,"nop.b 0","nop.b 0",";;","}"])
    elif slot==1:
        lines.extend(["{ .mbb","nop.m 0",spelling,"nop.b 0",";;","}"])
    elif slot==2:
        lines.extend(["{ .mib","nop.m 0","nop.i 0",spelling,";;","}"])
    else:
        raise ValueError(f"invalid bundle slot {slot}")

def clear_debug_flags(lines):
    emit(lines,"m","mov r9=psr")
    literal(lines,14,hex(LP|TB))
    emit(lines,"i","andcm r9=r9,r14")
    emit(lines,"m","mov psr.l=r9")
    emit(lines,"m","srlz.d")

def check_return_cpl(lines, expected):
    emit(lines,"m","mov r9=cr.ipsr")
    literal(lines,14,hex(CPL_MASK))
    emit(lines,"i","and r9=r9,r14")
    compare(lines,9,hex(expected<<32))

def generate(case):
    ppl,slot,lp,tb,false_qp,ss=PROFILES[case]
    expected=expected_vector(ppl,lp,tb,false_qp,ss)
    lines=[".text",".explicit",".align 16",".global _start","_start:"]
    setup(lines,ppl,lp,tb,false_qp,ss,slot)
    lines += ["source_bundle:"]
    br_ret(lines,slot,false_qp)
    lines += ["post_source:"]
    if false_qp:
        compare(lines,15,0)
        # Still at CPL0 (the predicated return never executed).
        clear_debug_flags(lines)
        emit(lines,"b","br.cond.sptk cpl_check")
    else:
        emit(lines,"b","br.cond.sptk fail")
    lines += ["target_bundle:"]
    if not false_qp:
        compare(lines,15,1 if expected else 0)
        # Do not read/write PSR at CPL>0: mov r=psr and mov psr.l=r
        # are privileged. Use a user-safe break.m to inspect saved CPL
        # in CR.IPSR from the privileged vector handler instead.
        if ppl==0 and expected is None:
            clear_debug_flags(lines)
        emit(lines,"b","br.cond.sptk cpl_check")
    else:
        emit(lines,"b","br.cond.sptk fail")
    lines += ["cpl_check:"]
    emit(lines,"m","break.m 0")
    lines += ["after_cpl_check:"]
    compare(lines,15,2)
    emit(lines,"b","br.cond.sptk pass")
    lines += ["pass:"]
    literal(lines,14,"pass_spin")
    emit(lines,"i","mov b6=r14")
    ivt.terminal(lines,PASS,FAIL)
    lines += [f".org {IVT}","ivt_base:",f".org {IVT+BREAK}","break_vector:"]
    # r15=2 identifies the terminal pass/fail logging BREAK. A prior
    # break.m is an intentional CPL probe from the return destination.
    literal(lines,20,2)
    emit(lines,"i","cmp.eq p6,p7=r15,r20")
    emit(lines,"b","(p6) br.cond.sptk b6")
    compare(lines,15,1 if expected else 0)
    literal(lines,15,2)
    check_return_cpl(lines,0 if false_qp else ppl)
    literal(lines,14,"after_cpl_check")
    emit(lines,"m","mov cr.iip=r14")
    emit(lines,"m","mov r9=cr.ipsr")
    literal(lines,14,hex(RI_MASK))
    emit(lines,"i","andcm r9=r9,r14")
    emit(lines,"m","mov cr.ipsr=r9")
    emit(lines,"b","rfi")
    lines += [f".org {IVT+GENERAL}","general_vector:"]
    emit(lines,"b","br.cond.sptk fail")
    # IA-64 vector entries are just 0x100 bytes apart. The complete
    # handler does not fit between LP and TB: put a short trampoline
    # in each vector, then the shared assertions at a distant address.
    for vec,label in ((LPV,"lp_vector"),(TBV,"tb_vector"),
                      (SSV,"ss_vector")):
        lines += [f".org {IVT+vec}",label+":"]
        emit(lines,"b","br.cond.sptk trap_checks" if vec==expected
             else "br.cond.sptk fail")
    lines += [f".org {IVT+0x8000}","trap_checks:"]
    if expected is not None:
        vec=expected
        compare(lines,15,0)
        literal(lines,15,1)
        emit(lines,"m","mov r9=cr.isr")
        code=((slot<<41)|((2 if vec==LPV else 0)|
                           (4 if tb else 0)|(8 if ss else 0)))
        compare(lines,9,hex(code))
        emit(lines,"m","mov r9=cr.iipa")
        literal(lines,14,"source_bundle")
        emit(lines,"i","cmp.eq p6,p7=r9,r14")
        emit(lines,"b","(p7) br.cond.spnt fail")
        emit(lines,"m","mov r9=cr.iip")
        literal(lines,14,"target_bundle")
        emit(lines,"i","cmp.eq p6,p7=r9,r14")
        emit(lines,"b","(p7) br.cond.spnt fail")
        emit(lines,"m","mov r9=cr.ipsr")
        literal(lines,14,hex(RI_MASK|CPL_MASK|LP|TB|SS|IC))
        emit(lines,"i","and r10=r9,r14")
        compare(lines,10,hex((ppl<<32)|(LP if lp else 0)|
                              (TB if tb else 0)|(SS if ss else 0)|IC))
        # Handler entered CPL0. RFI must restore the committed target CPL,
        # but remove LP/TB in IPSR so subsequent branch checks do not trap.
        literal(lines,14,hex(LP|TB|SS))
        emit(lines,"i","andcm r9=r9,r14")
        emit(lines,"m","mov cr.ipsr=r9")
        emit(lines,"b","rfi")
    return "\n".join(lines)+"\n"

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu",default="./build/qemu-system-ia64")
    p.add_argument("--out",type=Path,default=Path("scratch/ia64-psr-lp"))
    p.add_argument("--timeout",type=float,default=10)
    p.add_argument("--assemble-only",action="store_true")
    p.add_argument("--only",choices=list(PROFILES))
    args=p.parse_args()
    if not 0<args.timeout<=60:
        p.error("timeout must be in (0,60]")
    args.out=args.out.resolve()
    chosen=[args.only] if args.only else list(PROFILES)
    results=[ivt.run_one(k,args,case_spec=(LPV,PASS,FAIL),source=generate(k))
             for k in chosen]
    info={"reference":"Intel Itanium SDM Vol2 rev2.3 p2:205 and Vol3 br.ret",
          "cases":len(chosen),
          "scope":"PFS.ppl CPL demotion, PSR.lp priority over TB/SS, and SS-only br.ret",
          "results":results}
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/"summary.json").write_text(json.dumps(info,indent=2)+"\n")
    mode="ASSEMBLY" if args.assemble_only else "EXECUTION"
    print(f"IA-64 PSR.LP {mode} PASS: {len(chosen)}/{len(PROFILES)} profiles")

if __name__=="__main__":
    try: main()
    except (OSError,RuntimeError,ValueError,subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
