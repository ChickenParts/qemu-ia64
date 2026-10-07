#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free directed execution tests for F4 fcmp semantics."""
import importlib.util
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("runner",ROOT/"scripts/run-ia64-f9-tests.py")
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
PASS="r8=0000000066347061"; FAIL="r8=0000000066346661"
ONE=(1<<63,0xffff); TWO=(1<<63,0x10000); NEG_ONE=(1<<63,0x2ffff)
QNAN=(0xc000000000000001,0x1ffff); NAT=(0,0x1fffe); UNORM=(0x4000000000000000,0xffff)

def generate():
    lines=[".text",".explicit",".align 16",".global _start","_start:"];data=[];count=0
    def emit(k,i):
        t={"m":[".mii",i,"nop.i 0","nop.i 0"],"i":[".mii","nop.m 0",i,"nop.i 0"],
           "f":[".mmf","nop.m 0","nop.m 0",i],"b":[".mib","nop.m 0","nop.i 0",i],
           "l":[".mlx","nop.m 0",i]}[k];lines.extend(["{ "+t[0],*t[1:]," ;;","}"])
    def lit(r,v): emit("l",f"movl r{r}={v}")
    def fill(r,v):
        lab=f"in_{len(data)}";data.append((lab,v));lit(14,lab);emit("m",f"ldf.fill f{r}=[r14]")
    def pred(p,v):
        if v:
            lab=f"pok_{len(lines)}";emit("b",f"(p{p}) br.cond.sptk {lab}");emit("b","br.cond.sptk fail");lines.append(lab+":")
        else: emit("b",f"(p{p}) br.cond.spnt fail")
    def fpsr(v):
        emit("m","mov r24=ar.fpsr");lit(18,hex(v));emit("i","cmp.eq p10,p11=r24,r18");emit("b","(p11) br.cond.spnt fail")
    def case(m,a,b,x,y,flags=0,sf=0):
        nonlocal count;count+=1;lines.append(f"case_{count}:");fill(8,a);fill(9,b);lit(20,0x3f);emit("m","mov ar.fpsr=r20")
        emit("f",f"{m} p6,p7=f8,f9");pred(6,x);pred(7,y);fpsr(0x3f|(flags<<(13+13*sf)))
    case("fcmp.eq.s0",ONE,ONE,1,0);case("fcmp.lt.s1",ONE,TWO,1,0,sf=1)
    case("fcmp.le.s2",TWO,ONE,0,1,sf=2);case("fcmp.lt.s3",NEG_ONE,ONE,1,0,sf=3)
    case("fcmp.eq.s0",QNAN,ONE,0,1);case("fcmp.unord.s0",QNAN,ONE,1,0)
    case("fcmp.lt.s0",QNAN,ONE,0,1,1);case("fcmp.eq.s0",UNORM,ONE,0,1,2)
    case("fcmp.lt.s0",NAT,ONE,0,0)
    count+=1;fill(8,QNAN);fill(9,UNORM);emit("i","cmp.eq p6,p7=r0,r0");emit("i","cmp.eq p4,p5=r0,r0")
    lit(20,0);emit("m","mov ar.fpsr=r20");emit("f","(p5) fcmp.lt.s0 p6,p7=f8,f9");pred(6,1);pred(7,0);fpsr(0)
    count+=1;emit("i","cmp.eq p6,p7=r0,r0");emit("i","cmp.eq p4,p5=r0,r0")
    emit("f","(p5) fcmp.lt.unc.s0 p6,p7=f8,f9");pred(6,0);pred(7,0);fpsr(0)
    lit(8,"0x66347061");emit("m","break.m 0");lines.append("pass_spin:");emit("b","br.cond.sptk pass_spin")
    lines.append("fail:");lit(8,"0x66346661");emit("m","break.m 0");lines.append("fail_spin:");emit("b","br.cond.sptk fail_spin")
    lines+=[".data"]
    for lab,v in data: lines+=[".align 16",lab+":",f".quad {v[0]:#x}, {v[1]:#x}"]
    return "\n".join(lines)+"\n",count

def main(): runner.run_generated_guest("FCMP",generate,4,PASS,FAIL)
if __name__=="__main__":
    try: main()
    except (ValueError,RuntimeError,OSError,__import__("subprocess").SubprocessError) as e: sys.exit(str(e))
