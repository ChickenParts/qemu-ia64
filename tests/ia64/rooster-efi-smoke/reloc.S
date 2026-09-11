/*
 * Position-independent IA-64 ELF shared-object relocator.
 * Copyright (C) 1999 Hewlett-Packard Co.
 *   Contributed by David Mosberger <davidm@hpl.hp.com>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS".
 */

.text
.psr abi64
.psr lsb
.lsb

#define MAX_FUNCTION_DESCRIPTORS 750
#define ST_VALUE_OFF 8
#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR 1
#define EFI_BUFFER_TOO_SMALL 5
#define DT_NULL 0
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_SYMENT 11
#define R_IA64_NONE 0
#define R_IA64_DIR64LSB 0x27
#define R_IA64_FPTR64LSB 0x47
#define R_IA64_REL64LSB 0x6f

#define ldbase in0
#define dyn in1
#define d_tag r16
#define d_val r17
#define rela r18
#define relasz r19
#define relaent r20
#define addr r21
#define r_info r22
#define r_offset r23
#define r_addend r24
#define r_type r25
#define r_sym r25
#define fptr r26
#define fptr_limit r27
#define symtab f8
#define syment f9
#define ftmp f10
#define target r16
#define val r17

.global __relocate
.proc __relocate
__relocate:
  alloc r2=ar.pfs,2,0,0,0
  movl fptr=@gprel(fptr_mem_base)
  ;;
  add fptr=fptr,gp
  movl fptr_limit=@gprel(fptr_mem_limit)
  ;;
  add fptr_limit=fptr_limit,gp

.search_dynamic:
  ld8 d_tag=[dyn],8
  ;;
  ld8 d_val=[dyn],8
  cmp.eq p6,p0=DT_NULL,d_tag
(p6) br.cond.sptk.few .apply_relocs
  cmp.eq p7,p0=DT_RELA,d_tag
  cmp.eq p8,p0=DT_RELASZ,d_tag
  cmp.eq p9,p0=DT_RELAENT,d_tag
  cmp.eq p10,p0=DT_SYMTAB,d_tag
  cmp.eq p11,p0=DT_SYMENT,d_tag
  ;;
(p7) add rela=d_val,ldbase
(p8) mov relasz=d_val
(p9) mov relaent=d_val
(p10) add val=d_val,ldbase
  ;;
(p11) setf.sig syment=d_val
  ;;
(p10) setf.sig symtab=val
  br.sptk.few .search_dynamic

.apply_loop:
  ld8 r_offset=[rela]
  add addr=8,rela
  sub relasz=relasz,relaent
  ;;
  ld8 r_info=[addr],8
  ;;
  ld8 r_addend=[addr]
  add target=ldbase,r_offset
  add rela=rela,relaent
  extr.u r_type=r_info,0,32
  ;;
  cmp.eq p6,p0=R_IA64_NONE,r_type
  cmp.eq p7,p0=R_IA64_REL64LSB,r_type
  cmp.eq p8,p0=R_IA64_FPTR64LSB,r_type
(p7) br.cond.sptk.few .apply_REL64
  cmp.eq p7,p0=R_IA64_DIR64LSB,r_type
(p6) br.cond.sptk.few .apply_relocs
(p7) br.cond.sptk.few .apply_REL64
(p8) br.cond.sptk.few .apply_FPTR64

  mov r8=EFI_LOAD_ERROR
  br.ret.sptk.few rp

.apply_relocs:
  cmp.ltu p6,p0=0,relasz
(p6) br.cond.sptk.few .apply_loop
  mov r8=EFI_SUCCESS
  br.ret.sptk.few rp

.apply_REL64:
  ld8 val=[target]
  ;;
  add val=val,ldbase
  ;;
  st8 [target]=val
  br.cond.sptk.few .apply_relocs

.apply_FPTR64:
  st8 [target]=fptr
  extr.u r_sym=r_info,32,32
  add target=8,fptr
  ;;
  setf.sig ftmp=r_sym
  mov r8=EFI_BUFFER_TOO_SMALL
  ;;
  cmp.geu p6,p0=fptr,fptr_limit
  xma.lu ftmp=ftmp,syment,symtab
(p6) br.ret.sptk.few rp
  ;;
  getf.sig addr=ftmp
  st8 [target]=gp
  ;;
  add addr=ST_VALUE_OFF,addr
  ;;
  ld8 val=[addr]
  ;;
  add val=val,ldbase
  ;;
  st8 [fptr]=val,16
  br.cond.sptk.few .apply_relocs
.endp __relocate

.data
.align 16
fptr_mem_base:
  .space MAX_FUNCTION_DESCRIPTORS*16
fptr_mem_limit:

#if defined(__ELF__) && defined(__linux__)
.section .note.GNU-stack,"",%progbits
#endif
