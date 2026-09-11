/*
 * Sample IA-64 registers when guest code writes the permanent PEI HOB arena.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This plugin is observational.  Register values are emitted as the raw byte
 * sequence returned by QEMU's plugin register API, avoiding assumptions about
 * host byte order or a particular IA-64 ABI role for each scratch register.
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

#define IA64_REG_COUNT 32

static uint64_t watch_base = UINT64_C(0x040e0000);
static uint64_t watch_size = UINT64_C(0x00020000);
static unsigned int hit_limit = 512;
static gint hit_count;
static struct qemu_plugin_register *general_registers[IA64_REG_COUNT];
static struct qemu_plugin_register *pfs_register;
static struct qemu_plugin_register *bsp_register;
static struct qemu_plugin_register *bspstore_register;
static struct qemu_plugin_register *rsc_register;

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

static void append_register(GString *line, const char *name,
                            struct qemu_plugin_register *handle)
{
    g_autoptr(GByteArray) value = g_byte_array_new();
    unsigned int index;

    if (handle == NULL || qemu_plugin_read_register(handle, value) <= 0) {
        return;
    }
    g_string_append_printf(line, " %s=", name);
    for (index = 0; index < value->len; index++) {
        g_string_append_printf(line, "%02x", value->data[index]);
    }
}

static void trace_store(unsigned int vcpu_index,
                        qemu_plugin_meminfo_t info,
                        uint64_t vaddr,
                        void *userdata)
{
    uint64_t pc = (uint64_t)(uintptr_t)userdata;
    unsigned int hit;
    unsigned int size;
    unsigned int index;
    g_autoptr(GString) line = NULL;

    if (!address_is_watched(vaddr)) {
        return;
    }
    hit = (unsigned int)g_atomic_int_add(&hit_count, 1);
    if (hit >= hit_limit) {
        return;
    }

    size = 1U << qemu_plugin_mem_size_shift(info);
    line = g_string_new(NULL);
    g_string_append_printf(line,
                           "IA64_HOB_REGS hit=%u vcpu=%u pc=%016" PRIx64
                           " vaddr=%016" PRIx64 " size=%u",
                           hit, vcpu_index, pc, vaddr, size);
    for (index = 1; index < IA64_REG_COUNT; index++) {
        char name[8];

        snprintf(name, sizeof(name), "r%u", index);
        append_register(line, name, general_registers[index]);
    }
    append_register(line, "ar.pfs", pfs_register);
    append_register(line, "ar.bsp", bsp_register);
    append_register(line, "ar.bspstore", bspstore_register);
    append_register(line, "ar.rsc", rsc_register);
    g_string_append_c(line, '\n');
    qemu_plugin_outs(line->str);
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
            QEMU_PLUGIN_CB_R_REGS,
            QEMU_PLUGIN_MEM_W,
            (void *)(uintptr_t)pc);
    }
}

static void discover_registers(void)
{
    g_autoptr(GArray) descriptors = qemu_plugin_get_registers();
    unsigned int index;

    if (descriptors == NULL) {
        return;
    }
    for (index = 0; index < descriptors->len; index++) {
        qemu_plugin_reg_descriptor descriptor =
            g_array_index(descriptors, qemu_plugin_reg_descriptor, index);
        unsigned int number;

        if (sscanf(descriptor.name, "r%u", &number) == 1 &&
            number < IA64_REG_COUNT) {
            general_registers[number] = descriptor.handle;
        } else if (strcmp(descriptor.name, "ar.pfs") == 0) {
            pfs_register = descriptor.handle;
        } else if (strcmp(descriptor.name, "ar.bsp") == 0) {
            bsp_register = descriptor.handle;
        } else if (strcmp(descriptor.name, "ar.bspstore") == 0) {
            bspstore_register = descriptor.handle;
        } else if (strcmp(descriptor.name, "ar.rsc") == 0) {
            rsc_register = descriptor.handle;
        }
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *userdata)
{
    char line[128];

    (void)id;
    (void)userdata;
    snprintf(line, sizeof(line),
             "IA64_HOB_REGS summary hits=%u base=%016" PRIx64
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

    (void)info;
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
                    "ia64-hob-write-regs: invalid argument: %s\n",
                    argument);
            return -1;
        }
    }

    discover_registers();
    qemu_plugin_register_vcpu_tb_trans_cb(id, translate_block);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
