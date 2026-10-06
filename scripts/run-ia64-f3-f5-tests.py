#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute F3 fselect and F5 fclass semantics without firmware."""
import argparse, hashlib, json, os, shlex, subprocess, sys, time
from pathlib import Path
PASS='r8=0000000066357061'; FAIL='r8=0000000066356661'
def emit(lines,kind,insn):
    c={'m':['.mii',insn,'nop.i 0','nop.i 0'],
       'i':['.mii','nop.m 0',insn,'nop.i 0'],
       'f':['.mmf','nop.m 0','nop.m 0',insn],
       'b':['.mib','nop.m 0','nop.i 0',insn],
       'l':['.mlx','nop.m 0',insn]}
    p=c[kind]; lines += ['{ '+p[0],*p[1:],' ;;','}']
def literal(lines,r,v): emit(lines,'l',f'movl r{r}={v}')
def compare(lines,r,v):
    literal(lines,18,v); emit(lines,'i',f'cmp.eq p6,p7=r{r},r18')
    emit(lines,'b','(p7) br.cond.spnt fail')
def generate():
    L=['.text','.explicit','.align 16','.global _start','_start:']; D=[]; count=0
    def fill(f,sig,se):
        label=f'in_{len(D)}'; D.append((label,sig,se))
        literal(L,14,label); emit(L,'m',f'ldf.fill f{f}=[r14]')
    def spill(f,sig,se):
        literal(L,14,'output'); emit(L,'m',f'stf.spill [r14]=f{f}')
        emit(L,'m','ld8 r16=[r14],8'); emit(L,'m','ld8 r17=[r14]')
        compare(L,16,hex(sig)); compare(L,17,hex(se))
    mask=0xff00ff00ff00ff00; a=0xaaaaaaaa55555555; b=0x0123456789abcdef
    result=(a&mask)|(b&(~mask&((1<<64)-1)))
    fill(7,mask,0x12345); fill(8,a,0x22222); fill(9,b,0x33333)
    emit(L,'m','rsm 0x30'); emit(L,'f','fselect f6=f8,f9,f7')
    spill(6,result,0x1003e); count+=1
    fill(7,0,0x1fffe); fill(8,1<<63,0xffff); fill(9,1<<63,0xffff)
    emit(L,'f','fselect f6=f8,f9,f7'); spill(6,0,0x1fffe); count+=1
    classes=[(0,0x1fffe,0x100),(0xc000000000000001,0x1ffff,0x080),
      (0x8000000000000001,0x1ffff,0x040),(1<<63,0x1ffff,0x020),
      (0,0,0x004),(1,0,0x008),(0,1,0x008),(1<<63,1,0x010)]
    for sig,se,cls in classes:
        fill(8,sig,se); emit(L,'f',f'fclass.m p6,p7=f8,{cls}')
        emit(L,'b','(p7) br.cond.spnt fail'); count+=1
    fill(8,1,0x1ffff); emit(L,'f','fclass.m p6,p7=f8,511')
    emit(L,'b','(p6) br.cond.spnt fail')
    emit(L,'b','(p7) br.cond.sptk pseudo_ok'); emit(L,'b','br.cond.spnt fail')
    L.append('pseudo_ok:'); count+=1
    fill(8,0,0x1fffe); emit(L,'i','cmp.eq p6,p7=r0,r0')
    emit(L,'f','fclass.m p6,p7=f8,16')
    emit(L,'b','(p6) br.cond.spnt fail'); emit(L,'b','(p7) br.cond.spnt fail'); count+=1
    emit(L,'i','cmp.ne p5,p4=r0,r0'); emit(L,'i','cmp.eq p6,p7=r0,r0')
    emit(L,'f','(p5) fclass.m p6,p7=f8,256'); emit(L,'b','(p7) br.cond.spnt fail'); count+=1
    emit(L,'f','(p5) fclass.m.unc p6,p7=f8,256')
    emit(L,'b','(p6) br.cond.spnt fail'); emit(L,'b','(p7) br.cond.spnt fail'); count+=1
    literal(L,8,'0x66357061'); emit(L,'m','break.m 0')
    L.append('pass_spin:'); emit(L,'b','br.cond.sptk pass_spin')
    L.append('fail:'); literal(L,8,'0x66356661'); emit(L,'m','break.m 0')
    L.append('fail_spin:'); emit(L,'b','br.cond.sptk fail_spin')
    L += ['.data','.align 16','output:','.quad 0,0']
    for label,sig,se in D:
        L += ['.align 16',label+':',f'.quad {sig:#x}, {se:#x}']
    return '\n'.join(L)+'\n',count
def main():
    p=argparse.ArgumentParser()
    p.add_argument('--qemu',default=os.environ.get('QEMU_BIN','./build/qemu-system-ia64'))
    p.add_argument('--out',type=Path,default=Path('scratch/ia64-f3-f5'))
    p.add_argument('--assemble-only',action='store_true'); p.add_argument('--timeout',type=float,default=8)
    a=p.parse_args(); out=a.out.resolve(); out.mkdir(parents=True,exist_ok=True)
    source,count=generate(); src=out/'f3-f5.S'; obj=out/'f3-f5.o'; elf=out/'f3-f5.elf'
    src.write_text(source)
    for var,tool,args in [('AS','as',['-o',str(obj),str(src)]),
                          ('LD','ld',['-static','-nostdlib','-e','_start',
                                     '-Ttext=0x5000000','-Tdata=0x8000000',
                                     '-o',str(elf),str(obj)])]:
        subprocess.run(shlex.split(os.environ.get('IA64_'+var,'ia64-linux-gnu-'+tool))+args,
                       check=True,timeout=60)
    ev={'cases':count,'assembly_sha256':hashlib.sha256(src.read_bytes()).hexdigest(),
        'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'execution':'not-run'}
    (out/'result.json5').write_text(json.dumps(ev,indent=2)+'\n')
    if a.assemble_only:
        print(f'F3/F5 assembly PASS: {count} directed cases'); return
    log=out/'qemu.log'; log.unlink(missing_ok=True)
    env=dict(os.environ,QEMU_IA64_BREAK_LOG='1',QEMU_IA64_LOG_BREAK_STR='0')
    cmd=[str(Path(a.qemu).resolve()),'-accel','tcg','-M','ipf','-m','512M','-smp','1',
         '-display','none','-monitor','none','-vga','none','-nic','none',
         '-serial','file:'+str(out/'serial.log'),'-d','guest_errors','-D',str(log),'-kernel',str(elf)]
    with (out/'stderr.txt').open('w') as err:
        proc=subprocess.Popen(cmd,env=env,stdout=err,stderr=err)
        deadline=time.monotonic()+a.timeout; text=''
        try:
            while proc.poll() is None and time.monotonic()<deadline:
                if log.exists():
                    text=log.read_text(errors='replace')
                    if PASS in text or FAIL in text or 'IA64 UNIMPL' in text: break
                time.sleep(.02)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=2)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=2)
    if log.exists(): text=log.read_text(errors='replace')
    ok=PASS in text and FAIL not in text and 'IA64 UNIMPL' not in text
    ev.update(execution='pass' if ok else 'fail',returncode=proc.returncode,
              qemu_sha256=hashlib.sha256(Path(a.qemu).read_bytes()).hexdigest())
    (out/'result.json5').write_text(json.dumps(ev,indent=2)+'\n')
    if not ok: raise RuntimeError('F3/F5 guest FAILED; see '+str(out))
    print(f'F3/F5 execution PASS: {count} directed cases')
if __name__=='__main__':
    try: main()
    except (OSError,RuntimeError,subprocess.SubprocessError) as exc: sys.exit(str(exc))
