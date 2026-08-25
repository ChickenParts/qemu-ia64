/*
 * QEMU IA-64 Processor Abstraction Layer model
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef QEMU_IA64_PAL_H
#define QEMU_IA64_PAL_H

#include "cpu.h"

/* Architected PAL status values used by the model. */
#define IA64_PAL_STATUS_SUCCESS       0
#define IA64_PAL_STATUS_UNIMPLEMENTED (-1)
#define IA64_PAL_STATUS_EINVAL        (-2)
#define IA64_PAL_STATUS_ERROR         (-3)

/*
 * Relocatable PAL gateway contract.
 *
 * The copied image is QEMU-owned IA-64 code.  It enters the existing PAL
 * helper through break 0x1000 and returns through b0, so it is ordinary guest
 * RAM and naturally participates in migration and snapshots.
 */
#define IA64_PAL_COPY_TYPE_DEFAULT 0
#define IA64_PAL_GATEWAY_ALIGN     16

void ia64_pal_copy_info(uint64_t copy_type, uint64_t num_procs,
                        uint64_t num_iopics, int64_t *status,
                        uint64_t *buffer_size, uint64_t *buffer_align,
                        uint64_t *reserved);
void ia64_pal_copy_pal(CPUIA64State *env, uint64_t target_addr,
                       uint64_t alloc_size, uint64_t processor,
                       int64_t *status, uint64_t *pal_proc_offset,
                       uint64_t *reserved1, uint64_t *reserved2);

#endif /* QEMU_IA64_PAL_H */
