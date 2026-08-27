/*
 * Guest Firmware (GFW) helpers for IA-64.
 *
 * Ported from Xen 3.0 IA-64 guest firmware tooling.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "exec/cpu-common.h" /* cpu_physical_memory_write */

#include "hw/ia64/gfw.h"
#include "target/ia64/pal.h"

typedef struct {
    uint64_t signature;
    uint32_t type;
    uint32_t length;
} HOB_GENERIC_HEADER;

/*
 * INFO HOB is the first data in one HOB list; it contains the control
 * information of the HOB list.
 */
typedef struct {
    HOB_GENERIC_HEADER header;
    uint64_t length;   /* current length of hob */
    uint64_t cur_pos;  /* current position of hob (unused) */
    uint64_t buf_size; /* size of hob buffer */
} HOB_INFO;

typedef struct {
    uint64_t start;
    uint64_t size;
} hob_mem_t;

/* Xen IA-64 guest physical layout leaves the legacy VGA window unmapped. */
#define VGA_IO_START 0x000a0000ULL
#define VGA_IO_SIZE  0x00020000ULL

typedef enum {
    HOB_TYPE_INFO = 0,
    HOB_TYPE_TERMINAL,
    HOB_TYPE_MEM,
    HOB_TYPE_PAL_BUS_GET_FEATURES_DATA,
    HOB_TYPE_PAL_CACHE_SUMMARY,
    HOB_TYPE_PAL_MEM_ATTRIB,
    HOB_TYPE_PAL_CACHE_INFO,
    HOB_TYPE_PAL_CACHE_PROT_INFO,
    HOB_TYPE_PAL_DEBUG_INFO,
    HOB_TYPE_PAL_FIXED_ADDR,
    HOB_TYPE_PAL_FREQ_BASE,
    HOB_TYPE_PAL_FREQ_RATIOS,
    HOB_TYPE_PAL_HALT_INFO,
    HOB_TYPE_PAL_PERF_MON_INFO,
    HOB_TYPE_PAL_PROC_GET_FEATURES,
    HOB_TYPE_PAL_PTCE_INFO,
    HOB_TYPE_PAL_REGISTER_INFO,
    HOB_TYPE_PAL_RSE_INFO,
    HOB_TYPE_PAL_TEST_INFO,
    HOB_TYPE_PAL_VM_SUMMARY,
    HOB_TYPE_PAL_VM_INFO,
    HOB_TYPE_PAL_VM_PAGE_SIZE,
    HOB_TYPE_NR_VCPU,
    HOB_TYPE_NR_NVRAM,
    HOB_TYPE_MAX
} hob_type_t;

static int hob_init(void *buffer, uint64_t buf_size)
{
    HOB_INFO *phit;
    HOB_GENERIC_HEADER *terminal;

    if (sizeof(HOB_INFO) + sizeof(HOB_GENERIC_HEADER) > buf_size) {
        return -1;
    }

    phit = (HOB_INFO *)buffer;
    phit->header.signature = HOB_SIGNATURE;
    phit->header.type = HOB_TYPE_INFO;
    phit->header.length = sizeof(HOB_INFO);
    phit->length = sizeof(HOB_INFO) + sizeof(HOB_GENERIC_HEADER);
    phit->cur_pos = 0;
    phit->buf_size = buf_size;

    terminal = (HOB_GENERIC_HEADER *)((uint8_t *)buffer + sizeof(HOB_INFO));
    terminal->signature = HOB_SIGNATURE;
    terminal->type = HOB_TYPE_TERMINAL;
    terminal->length = sizeof(HOB_GENERIC_HEADER);

    return 0;
}

static int hob_add(void *hob_start, int type, const void *data, uint32_t data_size)
{
    HOB_INFO *phit = (HOB_INFO *)hob_start;
    HOB_GENERIC_HEADER *newhob, *tail;

    if (phit->length + data_size > phit->buf_size) {
        return -1;
    }

    /* Append new HOB just before the terminal. */
    newhob = (HOB_GENERIC_HEADER *)((uint8_t *)hob_start + phit->length -
                                    sizeof(HOB_GENERIC_HEADER));
    newhob->signature = HOB_SIGNATURE;
    newhob->type = type;
    newhob->length = data_size + sizeof(HOB_GENERIC_HEADER);
    memcpy((uint8_t *)newhob + sizeof(HOB_GENERIC_HEADER), data, data_size);

    /* Append terminal HOB. */
    tail = (HOB_GENERIC_HEADER *)((uint8_t *)hob_start + phit->length + data_size);
    tail->signature = HOB_SIGNATURE;
    tail->type = HOB_TYPE_TERMINAL;
    tail->length = sizeof(HOB_GENERIC_HEADER);

    phit->length += sizeof(HOB_GENERIC_HEADER) + data_size;
    return 0;
}

static int get_hob_size(void *hob_buf)
{
    HOB_INFO *phit = (HOB_INFO *)hob_buf;

    if (phit->header.signature != HOB_SIGNATURE) {
        error_report("GFW HOB: incorrect signature");
        return -1;
    }
    return phit->length;
}

static int add_max_hob_entry(void *hob_buf)
{
    int64_t max_hob = 0;
    return hob_add(hob_buf, HOB_TYPE_MAX, &max_hob, sizeof(max_hob));
}

static int add_mem_hob(void *hob_buf, uint64_t dom_mem_size)
{
    hob_mem_t memhob;

    /*
     * Less than 3G accounting legacy VGA hole.
     *
     * Xen's IA-64 HVM builder presents guest RAM as discontiguous around
     * 0xa0000..0xc0000, so the highest RAM address below 3G is
     * dom_mem_size + VGA_IO_SIZE.
     */
    memhob.start = 0;
    if (dom_mem_size < VGA_IO_START) {
        memhob.size = dom_mem_size;
    } else {
        memhob.size = MIN(dom_mem_size + VGA_IO_SIZE, 0xC0000000ULL);
    }
    if (hob_add(hob_buf, HOB_TYPE_MEM, &memhob, sizeof(memhob)) < 0) {
        return -1;
    }

    if (dom_mem_size > 0xC0000000ULL) {
        /* 4G .. 4G + remainder. */
        memhob.start = 0x100000000ULL;
        memhob.size = dom_mem_size + VGA_IO_SIZE - 0xC0000000ULL;
        if (hob_add(hob_buf, HOB_TYPE_MEM, &memhob, sizeof(memhob)) < 0) {
            return -1;
        }
    }

    return 0;
}

static int add_vcpus_hob(void *hob_buf, uint64_t vcpus)
{
    return hob_add(hob_buf, HOB_TYPE_NR_VCPU, &vcpus, sizeof(vcpus));
}

static int add_nvram_hob(void *hob_buf, uint64_t nvram_addr)
{
    return hob_add(hob_buf, HOB_TYPE_NR_NVRAM, &nvram_addr, sizeof(nvram_addr));
}

/* PAL HOB payloads (ported verbatim). */
static const unsigned char config_pal_bus_get_features_data[24] = {
    0, 0, 0, 32, 0, 0, 240, 189, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_cache_summary[16] = {
    3, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_mem_attrib[8] = {
    241, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_cache_info[152] = {
    3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    6, 4, 6, 7, 255, 1, 0, 1, 0, 64, 0, 0, 12, 12,
    49, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 6, 7, 0, 1,
    0, 1, 0, 64, 0, 0, 12, 12, 49, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 6, 8, 7, 7, 255, 7, 0, 11, 0, 0, 16, 0,
    12, 17, 49, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 8, 7,
    7, 7, 5, 9, 11, 0, 0, 4, 0, 12, 15, 49, 0, 254, 255,
    255, 255, 255, 255, 255, 255, 2, 8, 7, 7, 7, 5, 9,
    11, 0, 0, 4, 0, 12, 15, 49, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 3, 12, 7, 7, 7, 14, 1, 3, 0, 0, 192, 0, 12, 20, 49, 0
};

static const unsigned char config_pal_cache_prot_info[200] = {
    3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    45, 0, 16, 8, 0, 76, 12, 64, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    8, 0, 16, 4, 0, 76, 44, 68, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32,
    0, 16, 8, 0, 81, 44, 72, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0,
    112, 12, 0, 79, 124, 76, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 254, 255, 255, 255, 255, 255, 255, 255,
    32, 0, 112, 12, 0, 79, 124, 76, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 160,
    12, 0, 84, 124, 76, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0
};

static const unsigned char config_pal_debug_info[16] = {
    2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_fixed_addr[8] = {
    0, 0, 0, 0, 0, 0, 0, 0
};

/*
 * Development firmware consumes cached PAL results through HOBs.
 * Populate frequency records from the same model as direct PAL calls.
 */
static uint64_t config_pal_freq_base[1];
static uint64_t config_pal_freq_ratios[3];

static const unsigned char config_pal_halt_info[64] = {
    0, 0, 0, 0, 0, 0, 0, 48, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_perf_mon_info[136] = {
    12, 47, 18, 8, 0, 0, 0, 0, 241, 255, 0, 0, 255, 7, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 241, 255, 0, 0, 223, 0, 255, 255,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 240, 255, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 240, 255, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_proc_get_features[104] = {
    3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 64, 6, 64, 49, 0, 0, 0, 0, 64, 6, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0,
    231, 0, 0, 0, 0, 0, 0, 0, 228, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 17, 0, 0, 0, 0, 0, 0, 0,
    63, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_ptce_info[24] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_register_info[64] = {
    255, 0, 47, 127, 17, 17, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0,
    255, 208, 128, 238, 238, 0, 0, 248, 255, 255, 255, 255, 255, 0, 0, 7, 3,
    251, 3, 0, 0, 0, 0, 255, 7, 3, 0, 0, 0, 0, 0, 248, 252, 4,
    252, 255, 255, 255, 255, 2, 248, 252, 255, 255, 255, 255, 255
};

static const unsigned char config_pal_rse_info[16] = {
    96, 0, 0, 0, 0, 0, 0, 0, 96, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_test_info[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_vm_summary[16] = {
    1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_vm_info[48] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char config_pal_vm_page_size[16] = {
    0, 112, 85, 21, 0, 0, 0, 0, 0, 112, 85, 21, 0, 0, 0, 0
};

static void ia64_gfw_refresh_pal_frequency_hobs(void)
{
    IA64PALResult base = ia64_pal_result_freq_base();
    IA64PALResult ratios = ia64_pal_result_freq_ratios();

    g_assert(base.status == IA64_PAL_STATUS_SUCCESS);
    g_assert(ratios.status == IA64_PAL_STATUS_SUCCESS);

    config_pal_freq_base[0] = cpu_to_le64(base.v0);
    config_pal_freq_ratios[0] = cpu_to_le64(ratios.v0);
    config_pal_freq_ratios[1] = cpu_to_le64(ratios.v1);
    config_pal_freq_ratios[2] = cpu_to_le64(ratios.v2);
}

typedef struct {
    hob_type_t type;
    const void *data;
    uint32_t size;
} hob_batch_t;

static const hob_batch_t hob_batch[] = {
    { HOB_TYPE_PAL_BUS_GET_FEATURES_DATA, config_pal_bus_get_features_data, sizeof(config_pal_bus_get_features_data) },
    { HOB_TYPE_PAL_CACHE_SUMMARY, config_pal_cache_summary, sizeof(config_pal_cache_summary) },
    { HOB_TYPE_PAL_MEM_ATTRIB, config_pal_mem_attrib, sizeof(config_pal_mem_attrib) },
    { HOB_TYPE_PAL_CACHE_INFO, config_pal_cache_info, sizeof(config_pal_cache_info) },
    { HOB_TYPE_PAL_CACHE_PROT_INFO, config_pal_cache_prot_info, sizeof(config_pal_cache_prot_info) },
    { HOB_TYPE_PAL_DEBUG_INFO, config_pal_debug_info, sizeof(config_pal_debug_info) },
    { HOB_TYPE_PAL_FIXED_ADDR, config_pal_fixed_addr, sizeof(config_pal_fixed_addr) },
    { HOB_TYPE_PAL_FREQ_BASE, config_pal_freq_base, sizeof(config_pal_freq_base) },
    { HOB_TYPE_PAL_FREQ_RATIOS, config_pal_freq_ratios, sizeof(config_pal_freq_ratios) },
    { HOB_TYPE_PAL_HALT_INFO, config_pal_halt_info, sizeof(config_pal_halt_info) },
    { HOB_TYPE_PAL_PERF_MON_INFO, config_pal_perf_mon_info, sizeof(config_pal_perf_mon_info) },
    { HOB_TYPE_PAL_PROC_GET_FEATURES, config_pal_proc_get_features, sizeof(config_pal_proc_get_features) },
    { HOB_TYPE_PAL_PTCE_INFO, config_pal_ptce_info, sizeof(config_pal_ptce_info) },
    { HOB_TYPE_PAL_REGISTER_INFO, config_pal_register_info, sizeof(config_pal_register_info) },
    { HOB_TYPE_PAL_RSE_INFO, config_pal_rse_info, sizeof(config_pal_rse_info) },
    { HOB_TYPE_PAL_TEST_INFO, config_pal_test_info, sizeof(config_pal_test_info) },
    { HOB_TYPE_PAL_VM_SUMMARY, config_pal_vm_summary, sizeof(config_pal_vm_summary) },
    { HOB_TYPE_PAL_VM_INFO, config_pal_vm_info, sizeof(config_pal_vm_info) },
    { HOB_TYPE_PAL_VM_PAGE_SIZE, config_pal_vm_page_size, sizeof(config_pal_vm_page_size) },
};

static int add_pal_hob(void *hob_buf)
{
    ia64_gfw_refresh_pal_frequency_hobs();

    for (size_t i = 0; i < ARRAY_SIZE(hob_batch); i++) {
        if (hob_add(hob_buf, hob_batch[i].type, hob_batch[i].data, hob_batch[i].size) < 0) {
            return -1;
        }
    }
    return 0;
}

static int build_hob(void *hob_buf, uint64_t hob_buf_size,
                     uint64_t dom_mem_size, uint64_t vcpus,
                     uint64_t nvram_addr)
{
    if (hob_init(hob_buf, hob_buf_size) < 0) {
        error_report("GFW HOB: buffer too small");
        return -1;
    }
    if (add_mem_hob(hob_buf, dom_mem_size) < 0) {
        error_report("GFW HOB: add memory HOB failed");
        return -1;
    }
    if (add_vcpus_hob(hob_buf, vcpus) < 0) {
        error_report("GFW HOB: add vcpus HOB failed");
        return -1;
    }
    if (add_pal_hob(hob_buf) < 0) {
        error_report("GFW HOB: add PAL HOBs failed");
        return -1;
    }
    if (add_nvram_hob(hob_buf, nvram_addr) < 0) {
        error_report("GFW HOB: add NVRAM HOB failed");
        return -1;
    }
    if (add_max_hob_entry(hob_buf) < 0) {
        error_report("GFW HOB: add max HOB failed");
        return -1;
    }
    return 0;
}

static int load_hob(void *hob_buf)
{
    int hob_size = get_hob_size(hob_buf);
    if (hob_size < 0) {
        return -1;
    }
    if ((uint64_t)hob_size > GFW_HOB_SIZE) {
        error_report("GFW HOB: too large (%d)", hob_size);
        return -1;
    }
    cpu_physical_memory_write(GFW_HOB_START, hob_buf, hob_size);
    return 0;
}

int ipf_gfw_build_hob(uint64_t memsize, uint64_t vcpus, uint64_t nvram_addr)
{
    g_autofree uint8_t *hob_buf = g_malloc0(GFW_HOB_SIZE);

    if (build_hob(hob_buf, GFW_HOB_SIZE, memsize, vcpus, nvram_addr) < 0) {
        return -1;
    }
    if (load_hob(hob_buf) < 0) {
        return -1;
    }
    return 0;
}
