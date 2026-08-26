/*
 * IA-64 IPF firmware scratchpad device
 *
 * Copyright (c) 2026 Bryce Lanham
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_IA64_IPF_SPAD_H
#define HW_IA64_IPF_SPAD_H

#include "hw/sysbus.h"

#define TYPE_IA64_IPF_SPAD "ia64-ipf-spad"
OBJECT_DECLARE_SIMPLE_TYPE(IA64IPFSPADState, IA64_IPF_SPAD)

#define IA64_IPF_SPAD_BASE                 0x00000000ff37fc00ULL
#define IA64_IPF_SPAD_SIZE                 0x400
#define IA64_IPF_SPAD_LOCK_PTR_OFFSET      0x008
#define IA64_IPF_SPAD_MP_RECORD_OFFSET     0x168
#define IA64_IPF_SPAD_MP_RECORD_SIZE       0x100
#define IA64_IPF_SPAD_MP_SIGNATURE_OFFSET  0x020

#endif /* HW_IA64_IPF_SPAD_H */
