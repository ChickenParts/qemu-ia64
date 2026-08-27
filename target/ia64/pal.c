/*
 * IA-64 Processor Abstraction Layer result helpers
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "pal.h"

typedef struct IA64PALCacheDesc {
    uint32_t size;
    uint8_t attr;
    uint8_t associativity;
    uint8_t line_size;
    uint8_t stride;
    uint8_t store_latency;
    uint8_t load_latency;
    bool unified;
} IA64PALCacheDesc;

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
    return (uint64_t)denominator | ((uint64_t)numerator << 32);
}

static bool ia64_pal_cache_desc(uint64_t level, uint64_t type,
                                IA64PALCacheDesc *desc)
{
    if (level >= IA64_PAL_CACHE_LEVELS ||
        (type != IA64_PAL_CACHE_TYPE_INSTRUCTION &&
         type != IA64_PAL_CACHE_TYPE_DATA)) {
        return false;
    }

    switch (level) {
    case 0:
        *desc = (IA64PALCacheDesc) {
            .size = 16 * KiB,
            .attr = IA64_PAL_CACHE_ATTR_WT,
            .associativity = 4,
            .line_size = 5,
            .stride = 5,
            .store_latency =
                type == IA64_PAL_CACHE_TYPE_DATA ? 2 : 0,
            .load_latency =
                type == IA64_PAL_CACHE_TYPE_DATA ? 2 : 1,
        };
        break;
    case 1:
        *desc = (IA64PALCacheDesc) {
            .size = 96 * KiB,
            .attr = IA64_PAL_CACHE_ATTR_WB,
            .associativity = 6,
            .line_size = 6,
            .stride = 6,
            .store_latency = 6,
            .load_latency = 6,
            .unified = true,
        };
        break;
    case 2:
        *desc = (IA64PALCacheDesc) {
            .size = IA64_PAL_MERCED_L3_SIZE,
            .attr = IA64_PAL_CACHE_ATTR_WB,
            .associativity = 4,
            .line_size = 6,
            .stride = 6,
            .store_latency = 21,
            .load_latency = 21,
            .unified = true,
        };
        break;
    default:
        g_assert_not_reached();
    }

    return true;
}

IA64PALResult ia64_pal_result_unimplemented(void)
{
    return ia64_pal_result(IA64_PAL_STATUS_UNIMPLEMENTED, 0, 0, 0);
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
    IA64PALCacheDesc desc;
    uint64_t info1 = 0;
    uint64_t info2 = 0;

    if (!ia64_pal_cache_desc(level, type, &desc)) {
        return ia64_pal_result(IA64_PAL_STATUS_EINVAL, 0, 0, 0);
    }

    /* pal_cache_config_info_1_t::pcci1_data */
    info1 |= (uint64_t)desc.unified;
    info1 |= (uint64_t)desc.attr << 1;
    info1 |= (uint64_t)desc.associativity << 8;
    info1 |= (uint64_t)desc.line_size << 16;
    info1 |= (uint64_t)desc.stride << 24;
    info1 |= (uint64_t)desc.store_latency << 32;
    info1 |= (uint64_t)desc.load_latency << 40;

    /*
     * pal_cache_config_info_2_t::pcci2_data.  The cache geometry
     * fixes the least-significant tag bit; the target implements
     * a 44-bit physical address, so bit 43 is the highest tag bit.
     * Alias-boundary and hint fields remain zero because the TCG
     * cacheless execution model has no such performance constraint.
     */
    info2 |= desc.size;
    info2 |= (uint64_t)desc.line_size << 40;
    info2 |= UINT64_C(43) << 48;

    return ia64_pal_result(IA64_PAL_STATUS_SUCCESS,
                           info1, info2, 0);
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
        return ia64_pal_result(IA64_PAL_STATUS_EINVAL, 0, 0, 0);
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
        return ia64_pal_result(IA64_PAL_STATUS_EINVAL, 0, 0, 0);
    }

    /*
     * TCG has one fully associative instruction TC and one fully
     * associative data TC.  The counts come from CPU state rather
     * than from a firmware compatibility constant.
     */
    info |= UINT64_C(1);                 /* num_sets */
    info |= (uint64_t)entry_count << 8;  /* associativity */
    info |= (uint64_t)entry_count << 16; /* num_entries */

    return ia64_pal_result(IA64_PAL_STATUS_SUCCESS,
                           info, IA64_PAL_VM_PAGE_SIZES, 0);
}

IA64PALResult ia64_pal_result_vm_page_size(void)
{
    return ia64_pal_result(IA64_PAL_STATUS_SUCCESS,
                           IA64_PAL_VM_PAGE_SIZES,
                           IA64_PAL_VM_PAGE_SIZES, 0);
}

IA64PALResult ia64_pal_result_proc_get_features(uint64_t feature_set)
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
    return ia64_pal_result(
        IA64_PAL_STATUS_SUCCESS,
        ia64_pal_freq_ratio(IA64_PAL_MERCED_PROC_RATIO_NUM,
                            IA64_PAL_MERCED_PROC_RATIO_DEN),
        ia64_pal_freq_ratio(IA64_PAL_MERCED_BUS_RATIO_NUM,
                            IA64_PAL_MERCED_BUS_RATIO_DEN),
        ia64_pal_freq_ratio(IA64_PAL_VIRTUAL_ITC_RATIO_NUM,
                            IA64_PAL_VIRTUAL_ITC_RATIO_DEN));
}
