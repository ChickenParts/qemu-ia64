#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""GNU IA-64 oracle for B1/B2 loop instruction words and illegal B slots."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

MASK41=(1<<41)-1
FORMS = (
    ("wtop", "(p6) br.wtop.sptk.few _start", 3, 6),
    ("wexit", "(p6) br.wexit.sptk.few _start", 2, 6),
    ("cloop", "br.cloop.sptk.few _start", 5, 0),
    ("ctop", "br.ctop.sptk.few _start", 7, 0),
    ("cexit", "br.cexit.sptk.few _start", 6, 0),
)

def extract(data: bytes, slot: int) -> int:
    if len(data) != 16:
        raise ValueError(f"expected 16-byte bundle, got {len(data)}")
    return (int.from_bytes(data,"little")>>(5+41*slot))&MASK41

def replace(data:bytes,slot:int,word:int)->bytes:
    shift=5+41*slot
    bits=int.from_bytes(data,"little")
    bits=(bits&~(MASK41<<shift))|((word&MASK41)<<shift)
    return bits.to_bytes(16,"little")

def byte_swap(data:bytes,slot:int)->bytes:
    if slot not in (0,1):
        raise ValueError("slot 0 or 1 required for negative fixture")
    old=extract(data,slot)
    loop=extract(data,2)
    mutated=replace(replace(data,slot,loop),2,old)
    if extract(mutated,slot)!=loop or extract(mutated,2)!=old:
        raise ValueError("incomplete loop/nop slot exchange")
    if int.from_bytes(mutated,"little")&0x1f != int.from_bytes(data,"little")&0x1f:
        raise ValueError("bundle template changed")
    if loop>>37 !=4 or ((loop>>6)&7) not in (2,3,5,6,7):
        raise ValueError("not a major-4 loop instruction")
    return mutated

def assemble(asm:str,out:Path,name:str)->bytes:
    out.mkdir(parents=True,exist_ok=True)
    source="\n".join((".text",".explicit",".align 16",
        ".global _start","_start:","{ .bbb","nop.b 0",
        "nop.b 0",asm,";;","}",""))
    s,o,r=out/f"{name}.S",out/f"{name}.o",out/f"{name}.bin"
    s.write_text(source)
    subprocess.run(shlex.split(os.environ.get("IA64_AS","ia64-linux-gnu-as"))+
                   ["-o",str(o),str(s)],check=True,timeout=30)
    subprocess.run(shlex.split(os.environ.get("IA64_OBJCOPY","ia64-linux-gnu-objcopy"))+
                   ["-O","binary","-j",".text",str(o),str(r)],check=True,timeout=30)
    result=r.read_bytes()
    if len(result)!=16:
        raise ValueError(f"GNU produced {len(result)} bytes, wanted 16")
    return result

def run(out:Path)->dict:
    positives, negatives=[],[]
    for name,asm,btype,qp in FORMS:
        source=assemble(asm,out,name)
        word=extract(source,2)
        if word>>37 != 4 or ((word>>6)&7)!=btype or (word&63)!=qp:
            raise ValueError(f"{name} GNU opcode mismatch: {word:011x}")
        positives.append({"name":name,"assembly":asm,"slot":2,
                          "bits41":f"{word:011x}",
                          "sha256":hashlib.sha256(source).hexdigest()})
        for slot in (0,1):
            moved=byte_swap(source,slot)
            if extract(moved,slot)!=word:
                raise ValueError("changed instruction while moving slot")
            negatives.append({"name":name,"illegal_slot":slot,
                              "bits41":f"{extract(moved,slot):011x}",
                              "source_sha256":hashlib.sha256(moved).hexdigest()})
    out.mkdir(parents=True,exist_ok=True)
    report={"reference":"Intel IA-64 Vol 3 rev2.3 pp3:24,3:27",
            "positives":positives,"illegal_slot_mutations":negatives}
    (out/"summary.json").write_text(json.dumps(report,indent=2)+"\n")
    print(f"IA-64 LOOP GNU ORACLE PASS: {len(positives)}/5 positives and "+
          f"{len(negatives)}/10 slot mutations")
    return report

def main()->None:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out",type=Path,default=Path("scratch/ia64-loop-oracle"))
    run(ap.parse_args().out.resolve())

if __name__=="__main__":
    try:
        main()
    except (OSError,ValueError,subprocess.SubprocessError) as e:
        sys.exit(str(e))
