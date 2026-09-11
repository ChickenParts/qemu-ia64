/*
 * IA-64 EFI compile bindings for PicoEFI.
 *
 * Derived from the long-standing GNU-EFI IA-64 binding and reduced to the
 * architecture contract used by PicoEFI.
 * Copyright (c) 1998 Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
 */

#ifndef EFI_IA64_EFIBIND_H_
#define EFI_IA64_EFIBIND_H_

#include <stddef.h>
#include <stdint.h>

typedef uint16_t CHAR16;
#define WCHAR CHAR16

typedef uint64_t UINT64;
typedef int64_t INT64;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef uint8_t UINT8;
typedef unsigned char CHAR8;
typedef int8_t INT8;

#undef VOID
typedef void VOID;

typedef int64_t INTN;
typedef uint64_t UINTN;

#define EFIERR(a)           (0x8000000000000000ULL | (a))
#define EFI_ERROR_MASK      0x8000000000000000ULL
#define EFIERR_OEM(a)       (0xc000000000000000ULL | (a))
#define BAD_POINTER         0xFBFBFBFBFBFBFBFBULL
#define MAX_ADDRESS         0xFFFFFFFFFFFFFFFFULL

#define BREAKPOINT()        while (TRUE)
#define MIN_ALIGNMENT_SIZE  8

#define ALIGN_VARIABLE(Value, Adjustment) \
    (UINTN)(Adjustment) = 0; \
    if ((UINTN)(Value) % MIN_ALIGNMENT_SIZE) \
        (UINTN)(Adjustment) = MIN_ALIGNMENT_SIZE - \
            ((UINTN)(Value) % MIN_ALIGNMENT_SIZE); \
    (Value) = (UINTN)(Value) + (UINTN)(Adjustment)

#define EFI_SIGNATURE_16(A,B)             ((A) | ((B) << 8))
#define EFI_SIGNATURE_32(A,B,C,D)         \
    (EFI_SIGNATURE_16(A,B) | (EFI_SIGNATURE_16(C,D) << 16))
#define EFI_SIGNATURE_64(A,B,C,D,E,F,G,H) \
    (EFI_SIGNATURE_32(A,B,C,D) | \
     ((UINT64)EFI_SIGNATURE_32(E,F,G,H) << 32))

#ifndef EFIAPI
#define EFIAPI
#endif

#define VOLATILE volatile
#define MEMORY_FENCE() __asm__ __volatile__("mf.a" ::: "memory")
#define INTERFACE_DECL(x) struct x
#define EFI_FUNCTION

#endif
