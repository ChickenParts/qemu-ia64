#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-IVT F4 legality, disabled-register, V/D and qualification tests."""
import argparse, importlib.util, json, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("ivt",ROOT/"scripts/run-ia64-fp-exception-tests.py")
ivt=importlib.util.module_from_spec(spec);spec.loader.exec_module(ivt)
emit,literal,compare=ivt.emit,ivt.literal,ivt.compare
DFL=0x40000; DIRTY=0x30; IVT=0x400000; V=1; D=2
PASS="0x66346370"; FAIL="0x66346366"
ONE=(1<<63,0xffff); QNAN=(0xc000000000000001,0x1ffff); UNORM=(0x4000000000000000,0xffff)

def fill(lines,data,r,bits,label):
    data+=[".align 16",label+":",f".quad {bits[0]:#x}, {bits[1]:#x}"];literal(lines,14,label);emit(lines,"m",f"ldf.fill f{r}=[r14]")
def setfpsr(lines,v): literal(lines,20,hex(v));emit(lines,"m","mov ar.fpsr=r20")
def chkfpsr(lines,v): emit(lines,"m","mov r9=ar.fpsr");compare(lines,9,hex(v))
def saved(lines,vec,code,label,mask=0):
    compare(lines,12,hex(vec));emit(lines,"m","mov r9=cr.isr");compare(lines,9,hex((2<<41)|code))
    emit(lines,"m","mov r9=cr.iip");literal(lines,14,label);emit(lines,"i","cmp.eq p6,p7=r9,r14");emit(lines,"b","(p7) br.cond.spnt fail")
    emit(lines,"m","mov r9=cr.ipsr");literal(lines,14,hex((3<<41)|DFL|DIRTY));emit(lines,"i","and r9=r9,r14");compare(lines,9,hex((2<<41)|mask))
def skip(lines,after):
    literal(lines,14,after);emit(lines,"m","mov cr.iip=r14");emit(lines,"m","mov r9=cr.ipsr");literal(lines,14,hex(3<<41))
    emit(lines,"i","andcm r9=r9,r14");emit(lines,"m","mov cr.ipsr=r9");emit(lines,"b","rfi")
def clear_saved(lines,mask):
    emit(lines,"m","mov r9=cr.ipsr");literal(lines,14,hex(mask));emit(lines,"i","andcm r9=r9,r14");emit(lines,"m","mov cr.ipsr=r9");emit(lines,"b","rfi")

def generate():
    l=[".text",".explicit",".align 16",".global _start","_start:"];d=[]
    literal(l,14,"ivt_base");emit(l,"m","mov cr.iva=r14");emit(l,"m","srlz.i");literal(l,15,0)
    fill(l,d,8,ONE,"one");fill(l,d,9,QNAN,"qnan");fill(l,d,10,UNORM,"unorm")
    # equal targets, qualified
    literal(l,14,"ill_h");emit(l,"i","mov b6=r14");l.append("ill_f:");emit(l,"f","fcmp.eq.s0 p6,p6=f8,f8");l.append("ill_a:")
    compare(l,15,1);emit(l,"b","br.cond.sptk unc_ill");l.append("ill_h:");literal(l,15,1);saved(l,0x5400,0,"ill_f");skip(l,"ill_a")
    # equal targets, false .unc still illegal
    l.append("unc_ill:");emit(l,"i","cmp.eq p4,p5=r0,r0");literal(l,14,"uill_h");emit(l,"i","mov b6=r14")
    l.append("uill_f:");emit(l,"f","(p5) fcmp.eq.unc.s0 p6,p6=f8,f8");l.append("uill_a:");compare(l,15,2);emit(l,"b","br.cond.sptk dis")
    l.append("uill_h:");literal(l,15,2);saved(l,0x5400,0,"uill_f");skip(l,"uill_a")
    # disabled source then retry
    l.append("dis:");emit(l,"m",f"ssm {DFL:#x}");emit(l,"m","srlz.d");literal(l,14,"dis_h");emit(l,"i","mov b6=r14")
    l.append("dis_f:");emit(l,"f","fcmp.lt.s0 p6,p7=f8,f9");l.append("dis_a:");compare(l,15,3);emit(l,"m",f"rsm {DFL:#x}");emit(l,"m","srlz.d");emit(l,"b","br.cond.sptk inv")
    l.append("dis_h:");literal(l,15,3);saved(l,0x5500,1,"dis_f",DFL);clear_saved(l,DFL)
    # qNaN ordered V fault; mask V in handler and retry
    l.append("inv:");setfpsr(l,0x3e);emit(l,"i","cmp.eq p6,p7=r0,r0");literal(l,14,"inv_h");emit(l,"i","mov b6=r14")
    l.append("inv_f:");emit(l,"f","fcmp.lt.s0 p6,p7=f9,f8");l.append("inv_a:");compare(l,15,4);chkfpsr(l,0x3f|(V<<13));emit(l,"b","br.cond.sptk den")
    l.append("inv_h:");literal(l,15,4);saved(l,0x5c00,V,"inv_f");chkfpsr(l,0x3e);emit(l,"m","mov r9=ar.fpsr");emit(l,"i","or r9=1,r9");emit(l,"m","mov ar.fpsr=r9");emit(l,"b","rfi")
    # unnormal D fault; mask D and retry
    l.append("den:");setfpsr(l,0x3d);literal(l,14,"den_h");emit(l,"i","mov b6=r14")
    l.append("den_f:");emit(l,"f","fcmp.eq.s0 p6,p7=f10,f8");l.append("den_a:");compare(l,15,5);chkfpsr(l,0x3f|(D<<13));emit(l,"b","br.cond.sptk suppress")
    l.append("den_h:");literal(l,15,5);saved(l,0x5c00,D,"den_f");chkfpsr(l,0x3d);emit(l,"m","mov r9=ar.fpsr");emit(l,"i","or r9=2,r9");emit(l,"m","mov ar.fpsr=r9");emit(l,"b","rfi")
    # false normal and false unc suppress DFL source fault (unc clears predicates)
    l.append("suppress:");emit(l,"i","cmp.eq p6,p7=r0,r0");emit(l,"i","cmp.eq p4,p5=r0,r0");emit(l,"m",f"ssm {DFL:#x}");emit(l,"m","srlz.d")
    literal(l,14,"fail");emit(l,"i","mov b6=r14");emit(l,"f","(p5) fcmp.lt.s0 p6,p7=f8,f9");compare(l,15,5)
    emit(l,"f","(p5) fcmp.lt.unc.s0 p6,p7=f8,f9");compare(l,15,5);emit(l,"m",f"rsm {DFL:#x}");emit(l,"m","srlz.d")
    l.append("pass:");ivt.terminal(l,PASS,FAIL);l += [f".org {IVT}","ivt_base:"]
    for v in (0x5400,0x5500,0x5c00):
        l.append(f".org {IVT+v}");literal(l,12,hex(v));emit(l,"b","br.cond.sptk b6")
    l += [".data",*d];return "\n".join(l)+"\n"

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--qemu",default="./build/qemu-system-ia64");p.add_argument("--out",type=Path,default=Path("scratch/ia64-fcmp-exceptions"));p.add_argument("--timeout",type=float,default=10);p.add_argument("--assemble-only",action="store_true");a=p.parse_args()
    if not 0<a.timeout<=60:p.error("--timeout must be in (0,60]")
    a.out=a.out.resolve();e=ivt.run_one("fcmp-faults",a,case_spec=(0x5c00,PASS,FAIL),source=generate());e.update(cases=7,expected_ivt_entries=5,vectors=["0x5400","0x5500","0x5c00"])
    (a.out/"fcmp-faults"/"result.json").write_text(json.dumps(e,indent=2)+"\n");print("F4 IVT "+("assembly" if a.assemble_only else "execution")+" PASS: 5 faults, 2 qualification controls")
if __name__=="__main__":
    try:main()
    except (OSError,RuntimeError,subprocess.SubprocessError) as e:sys.exit(str(e))
