/*
 * Trace guest stores into an IA-64 PEI HOB arena.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This plugin is intentionally observational.  It does not modify registers or
 * memory and it does not assume a firmware PC.  The default address window is
 * the permanent-HOB arena observed during Xen/IPF PEI migration.
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

static uint64_t watch_base = UINT64_C(0x040e0000);
static uint64_t watch_size = UINT64_C(0x00020000);
static unsigned int hit_limit = 8192;
static gint hit_count;

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

static bool address_is_watched(uint64_t address)
{
    return address >= watch_base && address - watch_base < watch_size;
}

static void trace_store(unsigned int vcpu_index,
                        qemu_plugin_meminfo_t info,
                        uint64_t vaddr,
                        void *userdata)
{
    uint64_t pc = (uint64_t)(uintptr_t)userdata;
    unsigned int hit;
    unsigned int size;
    char line[192];

    if (!address_is_watched(vaddr)) {
        return;
    }

    hit = (unsigned int)g_atomic_int_add(&hit_count, 1);
    if (hit >= hit_limit) {
        return;
    }
    size = 1U << qemu_plugin_mem_size_shift(info);
    snprintf(line, sizeof(line),
             "IA64_HOB_WRITE hit=%u vcpu=%u pc=%016" PRIx64
             " vaddr=%016" PRIx64 " size=%u\n",
             hit, vcpu_index, pc, vaddr, size);
    qemu_plugin_outs(line);
}

static void translate_block(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t count = qemu_plugin_tb_n_insns(tb);
    size_t index;

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

static void plugin_exit(qemu_plugin_id_t id, void *userdata)
{
    char line[128];

    snprintf(line, sizeof(line),
             "IA64_HOB_WRITE summary hits=%u base=%016" PRIx64
             " size=%016" PRIx64 " limit=%u\n",
             (unsigned int)g_atomic_int_get(&hit_count),
             watch_base, watch_size, hit_limit);
    qemu_plugin_outs(line);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    int index;

    for (index = 0; index < argc; index++) {
        const char *argument = argv[index];
        uint64_t value;

        if (strncmp(argument, "base=", 5) == 0 &&
            parse_u64(argument + 5, &value)) {
            watch_base = value;
        } else if (strncmp(argument, "size=", 5) == 0 &&
                   parse_u64(argument + 5, &value) && value != 0) {
            watch_size = value;
        } else if (strncmp(argument, "limit=", 6) == 0 &&
                   parse_u64(argument + 6, &value) &&
                   value > 0 && value <= UINT32_MAX) {
            hit_limit = (unsigned int)value;
        } else {
            fprintf(stderr,
                    "ia64-hob-write-trace: invalid argument: %s\n",
                    argument);
            return -1;
        }
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, translate_block);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
