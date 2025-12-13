/*
 * Guest Firmware (GFW) helpers for IA-64.
 *
 * This code builds the HOB list and provides basic NVRAM staging used by the
 * Xen/KVM IA-64 guest firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef HW_IA64_GFW_H
#define HW_IA64_GFW_H

#include "qemu/typedefs.h"
#include <stdint.h>

#define GFW_SIZE        (16ULL << 20)
#define GFW_START       ((4ULL << 30) - GFW_SIZE)

#define HOB_SIGNATURE   0x3436474953424f48ULL /* "HOBSIG64" */
#define GFW_HOB_START   ((4ULL << 30) - (14ULL << 20)) /* 4G - 14M */
#define GFW_HOB_SIZE    (1ULL << 20) /* 1M */

#define NVRAM_OFFSET    (10ULL << 20)
#define NVRAM_START     (GFW_START + NVRAM_OFFSET)
#define NVRAM_SIZE      (64ULL << 10)

int ipf_gfw_build_hob(uint64_t memsize, uint64_t vcpus, uint64_t nvram_addr);

#endif /* HW_IA64_GFW_H */

