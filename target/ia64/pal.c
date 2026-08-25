/*
 * QEMU IA-64 Processor Abstraction Layer model
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "pal.h"
#include "exec/cpu-common.h"
#include "exec/translation-block.h"
#include "hw/boards.h"
#include "qemu/log.h"
#include "system/address-spaces.h"

static const uint8_t ia64_pal_gateway[] = {
#include "pal-gateway.inc"
};

QEMU_BUILD_BUG_ON(sizeof(ia64_pal_gateway) == 0);
QEMU_BUILD_BUG_ON(sizeof(ia64_pal_gateway) % 16 != 0);

static bool ia64_pal_target_to_phys(uint64_t target, hwaddr *phys)
{
    uint64_t hi32 = target & 0xffffffff00000000ULL;
    uint64_t value;

    /*
     * PAL callers may pass a plain physical address, a sign-extended 32-bit
     * address, or an IA-64 region-encoded address.  Normalize those forms at
     * the PAL boundary instead of teaching the copied gateway firmware quirks.
     */
    if (hi32 == 0 || hi32 == 0xffffffff00000000ULL ||
        hi32 == 0x7fffffff00000000ULL) {
        value = (uint32_t)target;
    } else {
        value = target & ((1ULL << 61) - 1);
    }

    if ((uint64_t)(hwaddr)value != value) {
        return false;
    }
    *phys = (hwaddr)value;
    return true;
}

static void ia64_pal_result_init(int64_t *status, uint64_t *v0,
                                 uint64_t *v1, uint64_t *v2)
{
    *status = IA64_PAL_STATUS_SUCCESS;
    *v0 = 0;
    *v1 = 0;
    *v2 = 0;
}

void ia64_pal_copy_info(uint64_t copy_type, uint64_t num_procs,
                        uint64_t num_iopics, int64_t *status,
                        uint64_t *buffer_size, uint64_t *buffer_align,
                        uint64_t *reserved)
{
    ia64_pal_result_init(status, buffer_size, buffer_align, reserved);

    /*
     * The virtual platform has one position-independent gateway shared by all
     * processors and interrupt controllers.  The topology arguments therefore
     * do not change the image size; they are retained for the architected ABI.
     */
    (void)num_procs;
    (void)num_iopics;

    if (copy_type != IA64_PAL_COPY_TYPE_DEFAULT) {
        *status = IA64_PAL_STATUS_EINVAL;
        return;
    }

    *buffer_size = sizeof(ia64_pal_gateway);
    *buffer_align = IA64_PAL_GATEWAY_ALIGN;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: PAL_COPY_INFO type=%" PRIu64
                  " size=%zu align=%u\n",
                  copy_type, sizeof(ia64_pal_gateway),
                  IA64_PAL_GATEWAY_ALIGN);
}

void ia64_pal_copy_pal(CPUIA64State *env, uint64_t target_addr,
                       uint64_t alloc_size, uint64_t processor,
                       int64_t *status, uint64_t *pal_proc_offset,
                       uint64_t *reserved1, uint64_t *reserved2)
{
    hwaddr target;
    MemTxResult result;
    unsigned cpus = current_machine ? current_machine->smp.cpus : 1;

    ia64_pal_result_init(status, pal_proc_offset, reserved1, reserved2);

    if (processor >= cpus || alloc_size < sizeof(ia64_pal_gateway) ||
        !ia64_pal_target_to_phys(target_addr, &target) ||
        (target & (IA64_PAL_GATEWAY_ALIGN - 1)) != 0 ||
        target > (hwaddr)-1 - sizeof(ia64_pal_gateway)) {
        *status = IA64_PAL_STATUS_EINVAL;
        return;
    }

    result = address_space_write(&address_space_memory, target,
                                 MEMTXATTRS_UNSPECIFIED,
                                 ia64_pal_gateway,
                                 sizeof(ia64_pal_gateway));
    if (result != MEMTX_OK) {
        *status = IA64_PAL_STATUS_ERROR;
        return;
    }

    /* Make an overwritten translation block observe the newly copied code. */
    cpu_flush_icache_range(target, sizeof(ia64_pal_gateway));

    /* The PAL entry is the first bundle in the copied image. */
    *pal_proc_offset = 0;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64: PAL_COPY_PAL cpu=%" PRIu64
                  " target=%016" HWADDR_PRIx " size=%zu\n",
                  processor, target, sizeof(ia64_pal_gateway));

    (void)env;
}
