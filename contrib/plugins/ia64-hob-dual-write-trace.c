/*
 * Correlate writes to the temporary and permanent IA-64 PEI HOB arenas.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

struct WatchedRegion {
    const char *name;
    uint64_t base;
    uint64_t size;
};

static struct WatchedRegion regions[] = {
    { "temporary", UINT64_C(0xffff0000), UINT64_C(0x00010000) },
    { "permanent", UINT64_C(0x040e0000), UINT64_C(0x00020000) },
};
static unsigned int hit_limit = 65536;
static gint hit_count;
static gint temporary_hits;
static gint permanent_hits;

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    parsed = strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

static const struct WatchedRegion *find_region(uint64_t address)
{
    unsigned int index;

    for (index = 0; index < G_N_ELEMENTS(regions); index++) {
        const struct WatchedRegion *region = &regions[index];

        if (address >= region->base && address - region->base < region->size) {
            return region;
        }
    }
    return NULL;
}

static void trace_store(unsigned int vcpu_index,
                        qemu_plugin_meminfo_t info,
                        uint64_t vaddr,
                        void *userdata)
{
    const struct WatchedRegion *region = find_region(vaddr);
    uint64_t pc = (uint64_t)(uintptr_t)userdata;
    unsigned int hit;
    unsigned int size;
    char line[224];

    if (region == NULL) {
        return;
    }
    hit = (unsigned int)g_atomic_int_add(&hit_count, 1);
    if (hit >= hit_limit) {
        return;
    }
    if (region == &regions[0]) {
        g_atomic_int_inc(&temporary_hits);
    } else {
        g_atomic_int_inc(&permanent_hits);
    }

    size = 1U << qemu_plugin_mem_size_shift(info);
    snprintf(line, sizeof(line),
             "IA64_HOB_DUAL hit=%u region=%s vcpu=%u pc=%016" PRIx64
             " vaddr=%016" PRIx64 " size=%u\n",
             hit, region->name, vcpu_index, pc, vaddr, size);
    qemu_plugin_outs(line);
}

static void translate_block(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t count = qemu_plugin_tb_n_insns(tb);
    size_t index;

    (void)id;
    for (index = 0; index < count; index++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, index);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);

        qemu_plugin_register_vcpu_mem_cb(
            insn,
            trace_store,
            QEMU_PLUGIN_CB_NO_REGS,
            QEMU_PLUGIN_MEM_W,
            (void *)(uintptr_t)pc);
    }
}

static bool set_region_argument(const char *argument,
                                const char *key,
                                uint64_t *destination)
{
    size_t length = strlen(key);
    uint64_t value;

    if (strncmp(argument, key, length) != 0 || argument[length] != '=') {
        return false;
    }
    if (!parse_u64(argument + length + 1, &value)) {
        return false;
    }
    *destination = value;
    return true;
}

static void plugin_exit(qemu_plugin_id_t id, void *userdata)
{
    char line[192];

    (void)id;
    (void)userdata;
    snprintf(line, sizeof(line),
             "IA64_HOB_DUAL summary hits=%u temporary=%u permanent=%u "
             "limit=%u\n",
             (unsigned int)g_atomic_int_get(&hit_count),
             (unsigned int)g_atomic_int_get(&temporary_hits),
             (unsigned int)g_atomic_int_get(&permanent_hits),
             hit_limit);
    qemu_plugin_outs(line);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    int index;

    (void)info;
    for (index = 0; index < argc; index++) {
        const char *argument = argv[index];
        uint64_t value;

        if (set_region_argument(argument, "temporary-base", &regions[0].base) ||
            set_region_argument(argument, "temporary-size", &regions[0].size) ||
            set_region_argument(argument, "permanent-base", &regions[1].base) ||
            set_region_argument(argument, "permanent-size", &regions[1].size)) {
            continue;
        }
        if (strncmp(argument, "limit=", 6) == 0 &&
            parse_u64(argument + 6, &value) &&
            value > 0 && value <= UINT32_MAX) {
            hit_limit = (unsigned int)value;
            continue;
        }
        fprintf(stderr, "ia64-hob-dual-write-trace: invalid argument: %s\n",
                argument);
        return -1;
    }
    if (regions[0].size == 0 || regions[1].size == 0) {
        fprintf(stderr, "ia64-hob-dual-write-trace: region size cannot be zero\n");
        return -1;
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, translate_block);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
