/*
 * IA-64 EFI startup for PicoEFI.
 *
 * The IA-64 PE32+ entry point is a PLABEL (code address + global pointer),
 * not the raw instruction address.  The ABI structure follows the historical
 * IA-64 EFI crt0 contract; the PE image header/layout follows PicoEFI.
 *
 * Copyright (C) 1999 Hewlett-Packard Co.
 *   Contributed by David Mosberger <davidm@hpl.hp.com>.
 * Copyright (C) 2022-2026 Mintsuki and contributors.
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

.section .pe_header,"ax"
  .align 0x1000
fbase:
  .short 0x5a4d, 0x0090, 0x0003, 0x0000, 0x0004, 0x0000, 0xffff, 0x0000
  .short 0x00b8, 0x0000, 0x0000, 0x0000, 0x0040, 0x0000, 0x0000, 0x0000
  .short 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
  .short 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0080, 0x0000
  .byte 0x0e, 0x1f, 0xba, 0x0e, 0x00, 0xb4, 0x09, 0xcd, 0x21, 0xb8, 0x01
  .byte 0x4c, 0xcd, 0x21, 0x54, 0x68, 0x69, 0x73, 0x20, 0x70, 0x72, 0x6f
  .byte 0x67, 0x72, 0x61, 0x6d, 0x20, 0x63, 0x61, 0x6e, 0x6e, 0x6f, 0x74
  .byte 0x20, 0x62, 0x65, 0x20, 0x72, 0x75, 0x6e, 0x20, 0x69, 0x6e, 0x20
  .byte 0x44, 0x4f, 0x53, 0x20, 0x6d, 0x6f, 0x64, 0x65, 0x2e, 0x0d, 0x0d
  .byte 0x0a, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

pe_header:
  .long 0x00004550
  .short 0x0200
  .short 0x0003
  .long 0
  .long 0
  .long 0
  .short data_dirs_end - opt_header
  .short 0x0206

opt_header:
  .short 0x020b
  .byte 0x02
  .byte 0x26
  .long __text_size - fbase
  .long __data_rawsize - fbase
  .long __bss_size - fbase
  .long _start_plabel - fbase
  .long __text_start - fbase
  .quad __image_base - fbase
  .long 0x1000
  .long 0x1000
  .short 0, 0, 0, 0, 0, 0
  .long 0
  .long __image_size - fbase
  .long __text_start - fbase
  .long 0
  .short 0x0a
  .short 0
  .quad 0, 0, 0, 0
  .long 0
  .long 6

data_dirs:
  .long 0, 0
  .long 0, 0
  .long 0, 0
  .long 0, 0
  .long 0, 0
  .long pe_reloc - fbase, 12
data_dirs_end:

  .ascii ".text\0\0\0"
  .long __text_size - fbase
  .long __text_start - fbase
  .long __text_size - fbase
  .long __text_start - fbase
  .long 0, 0
  .short 0, 0
  .long 0x60000020

  .ascii ".reloc\0\0"
  .long 12
  .long pe_reloc - fbase
  .long 0x1000
  .long pe_reloc - fbase
  .long 0, 0
  .short 0, 0
  .long 0x42000040

  .ascii ".data\0\0\0"
  .long __data_size - fbase
  .long __data_start - fbase
  .long __data_rawsize - fbase
  .long __data_start - fbase
  .long 0, 0
  .short 0, 0
  .long 0xc0000040

.text
.psr abi64
.psr lsb
.lsb
.align 16
.global _start
.proc _start
_start:
  alloc loc0=ar.pfs,2,4,2,0
  mov loc1=rp
  mov loc2=in0
  mov loc3=in1

  movl out0=@gprel(__image_base)
  movl out1=@gprel(_DYNAMIC)
  ;;
  add out0=out0,gp
  add out1=out1,gp
  br.call.sptk.few rp=__relocate
  ;;
  cmp.ne p6,p0=r0,r8
(p6) br.cond.sptk.few .exit

  mov out0=loc2
  mov out1=loc3
  br.call.sptk.few rp=efi_main
  ;;

.exit:
  mov ar.pfs=loc0
  mov rp=loc1
  ;;
  br.ret.sptk.few rp
.endp _start

.section .plabel,"a"
.align 0x1000
.global _start_plabel
_start_plabel:
  data8 _start
  data8 __gp

#define IMAGE_REL_BASED_DIR64 10
.section .dummy_reloc,"a"
.align 4
pe_reloc:
  data4 _start_plabel
  data4 12
  data2 (IMAGE_REL_BASED_DIR64 << 12) + 0
  data2 (IMAGE_REL_BASED_DIR64 << 12) + 8

#if defined(__ELF__) && defined(__linux__)
.section .note.GNU-stack,"",%progbits
#endif