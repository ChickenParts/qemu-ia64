/*
 * IA-64 Processor Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "pal.h"

static IA64PALResult ia64_pal_result(int64_t status,
                                     uint64_t v0,
                                     uint64_t v1,
                                     uint64_t v2)
{
    return (IA64PALResult) {
        .status = status,
        .v0 = v0,
        .v1 = v1,
        .v2 = v2,
        .action = IA64_PAL_ACTION_RETURN,
    };
}

static uint64_t ia64_pal_freq_ratio(uint32_t numerator,
                                    uint32_t denominator)
{
    return denominator | (uint64_t)numerator << 32;
}

IA64PALResult ia64_pal_result_unimplemented(void)
{
    return ia64_pal_result(IA64_PAL_STATUS_UNIMPLEMENTED,
                           0, 0, 0);
}

IA64PALResult ia64_pal_result_halt(void)
{
    IA64PALResult result = ia64_pal_result(
        IA64_PAL_STATUS_SUCCESS, 0, 0, 0);

    result.action = IA64_PAL_ACTION_HALT;
    return result;
}

IA64PALResult ia64_pal_result_cache_summary(void)
{
    return ia64_pal_result(IA64_PAL_STATUS_SUCCESS,
                           IA64_PAL_CACHE_LEVELS,
                           IA64_PAL_UNIQUE_CACHES, 0);
}

IA64PALResult ia64_pal_result_cache_info(uint64_t level,
                                         uint64_t type)
{
    const uint8_t attr = 1;      /* PAL_CACHE_ATTR_WB */
    const uint8_t assoc = 8;
    const uint8_t line_size = 6; /* log2(64) */
    const uint8_t stride = 6;    /* log2(64) */
    bool unified;
    uint32_t cache_size;
    uint64_t info1 = 0;

    if (level >= IA64_PAL_CACHE_LEVELS ||
        (type != IA64_PAL_CACHE_TYPE_INSTRUCTION &&
         type != IA64_PAL_CACHE_TYPE_DATA)) {
        return ia64_pal_result(IA64_PAL_STATUS_EINVAL,
                               0, 0, 0);
    }

    unified = level == IA64_PAL_CACHE_LEVELS - 1;
    switch (level) {
    case 0:
        cache_size = type == IA64_PAL_CACHE_TYPE_INSTRUCTION ?
            32 * KiB : 64 * KiB;
        break;
    case 1:
        cache_size = 256 * KiB;
        break;
    case 2:
        cache_size = 1 * MiB;
        break;
    default:
        g_assert_not_reached();
    }

    /* pal_cache_config_info_1_t::pcci1_data */
    info1 |= (uint64_t)unified;
    info1 |= (uint64_t)attr << 1;
    info1 |= (uint64_t)assoc << 8;
    info1 |= (uint64_t)line_size << 16;
    info1 |= (uint64_t)stride << 24;

    return ia64_pal_result(IA64_PAL_STATUS_SUCCESS,
                           info1, cache_size, 0);
}

IA64PALResult ia64_pal_result_vm_summary(unsigned int pkr_count,
                                         unsigned int dtr_count,
                                         unsigned int itr_count)
{
    const uint8_t vw = 1;
    const uint8_t phys_add_size = 44;
    const uint8_t key_size = 18;
    const uint8_t hash_tag_id = 0;
    const uint8_t impl_va_msb = 60;
    const uint8_t rid_size = 18;
    const uint16_t max_purges = UINT16_MAX;
    uint64_t info1 = 0;
    uint64_t info2 = 0;

    if (pkr_count == 0 || pkr_count > 256 ||
        dtr_count == 0 || dtr_count > 256 ||
        itr_count == 0 || itr_count > 256) {
        return ia64_pal_result(IA64_PAL_STATUS_EINVAL,
                               0, 0, 0);
    }

    info1 |= (uint64_t)vw;
    info1 |= (uint64_t)phys_add_size << 1;
    info1 |= (uint64_t)key_size << 8;
    info1 |= (uint64_t)(pkr_count - 1) << 16;
    info1 |= (uint64_t)hash_tag_id << 24;
    info1 |= (uint64_t)(dtr_count - 1) << 32;
    info1 |= (uint64_t)(itr_count - 1) << 40;
    info1 |= (uint64_t)IA64_PAL_UNIQUE_TCS << 48;
    info1 |= (uint64_t)IA64_PAL_TC_LEVELS << 56;

    info2 |= (uint64_t)impl_va_msb;
    info2 |= (uint64_t)rid_size << 8;
    info2 |= (uint64_t)max_purges << 16;

    return ia64_pal_result(IA64_PAL_STATUS_SUCCESS,
                           info1, info2, 0);
}

IA64PALResult ia64_pal_result_vm_info(uint64_t level,
                                      uint64_t type,
                                      unsigned int entry_count)
{
    uint64_t info = 0;

    if (level >= IA64_PAL_TC_LEVELS ||
        (type != IA64_PAL_CACHE_TYPE_INSTRUCTION &&
         type != IA64_PAL_CACHE_TYPE_DATA) ||
        entry_count == 0 || entry_count > UINT8_MAX) {
        return ia64_pal_result(IA64_PAL_STATUS_EINVAL,
                               0, 0, 0);
    }

    /*
     * The target has separate fully associative instruction and
     * data translation caches.  One set times associativity is the
     * number of entries reported for either non-unified cache.
     */
    info |= UINT64_C(1);                 /* num_sets */
    info |= (uint64_t)entry_count << 8;  /* associativity */
    info |= (uint64_t)entry_count << 16; /* num_entries */

    return ia64_pal_result(IA64_PAL_STATUS_SUCCESS,
                           info, IA64_PAL_VM_PAGE_SIZES, 0);
}

IA64PALResult ia64_pal_result_proc_get_features(
    uint64_t feature_set)
{
    /* No configurable processor feature sets are modeled. */
    (void)feature_set;
    return ia64_pal_result(IA64_PAL_STATUS_EINVAL, 0, 0, 0);
}

IA64PALResult ia64_pal_result_freq_base(void)
{
    return ia64_pal_result(IA64_PAL_STATUS_SUCCESS,
                           IA64_PAL_PLATFORM_BASE_HZ, 0, 0);
}

IA64PALResult ia64_pal_result_freq_ratios(void)
{
    /*
     * The ITC is backed by QEMU virtual nanoseconds and therefore
     * advances at 1 GHz.  Keep the 100 MHz platform base, the
     * 300 MHz processor ratio, and report an exact 10:1 ITC ratio.
     */
    return ia64_pal_result(
        IA64_PAL_STATUS_SUCCESS,
        ia64_pal_freq_ratio(3, 1),
        ia64_pal_freq_ratio(1, 1),
        ia64_pal_freq_ratio(10, 1));
}
