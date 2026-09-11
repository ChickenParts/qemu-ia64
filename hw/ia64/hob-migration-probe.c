/*
 * IA-64 PEI HOB migration causality probe
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is deliberately disabled by default.  It does not synthesize a DXE
 * address or key off a firmware instruction address.  When enabled, it finds
 * two structurally valid HOB lists: a temporary list containing firmware-
 * volume HOBs and a permanent list missing those same records.  It copies the
 * missing records into the permanent list using the HOB heap contract.
 *
 * Its purpose is to prove or falsify the observed migration-loss hypothesis.
 * A successful firmware advance is evidence for fixing the underlying guest
 * memory/RSE migration; this probe is not the intended production fix.
 */

#include "qemu/osdep.h"
#include "exec/memattrs.h"
#include "hw/boards.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/notify.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/address-spaces.h"

#define IA64_HOB_HANDOFF 0x0001
#define IA64_HOB_FV      0x0005
#define IA64_HOB_END     0xffff
#define IA64_HOB_HEADER_SIZE 8
#define IA64_HOB_HANDOFF_SIZE 56
#define IA64_HOB_MAX_ENTRIES 4096

#define IA64_HOB_SOURCE_BASE UINT64_C(0xffff0000)
#define IA64_HOB_SOURCE_SIZE UINT64_C(0x00010000)
#define IA64_HOB_TARGET_BASE UINT64_C(0x04000000)
#define IA64_HOB_TARGET_SIZE UINT64_C(0x00200000)
#define IA64_HOB_POLL_NS     UINT64_C(100000)
#define IA64_HOB_POLL_LIMIT  200000

#define IA64_HOB_MAX_FVS 64

struct Ia64HobFvRecord {
    uint64_t base;
    uint64_t length;
    uint8_t raw[24];
};

struct Ia64HobListInfo {
    uint64_t address;
    uint64_t end_address;
    uint64_t free_top;
    uint64_t free_bottom;
    unsigned int entries;
    unsigned int fv_count;
    struct Ia64HobFvRecord fvs[IA64_HOB_MAX_FVS];
};

struct Ia64HobProbeState {
    QEMUTimer *timer;
    unsigned int polls;
    unsigned int repairs;
    bool enabled;
};

static struct Ia64HobProbeState ia64_hob_probe;

static bool ia64_hob_probe_enabled(void)
{
    const char *value = getenv("QEMU_IA64_PEI_FV_HOB_RESTORE");

    return value != NULL && value[0] != '\0' &&
           strcmp(value, "0") != 0 && strcmp(value, "off") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "no") != 0;
}

static bool ia64_hob_read(uint64_t address, void *buffer, size_t length)
{
    return address_space_read(&address_space_memory, address,
                              MEMTXATTRS_UNSPECIFIED, buffer, length) ==
           MEMTX_OK;
}

static bool ia64_hob_write(uint64_t address, const void *buffer, size_t length)
{
    return address_space_write(&address_space_memory, address,
                               MEMTXATTRS_UNSPECIFIED, buffer, length) ==
           MEMTX_OK;
}

static bool ia64_hob_range_inside(uint64_t address, uint64_t length,
                                  uint64_t base, uint64_t size)
{
    uint64_t end;
    uint64_t outer_end;

    return !uadd64_overflow(address, length, &end) &&
           !uadd64_overflow(base, size, &outer_end) &&
           address >= base && end <= outer_end;
}

static bool ia64_hob_parse(const uint8_t *window, uint64_t window_base,
                           size_t window_size, size_t offset,
                           struct Ia64HobListInfo *info)
{
    uint64_t list_address;
    uint64_t end_address;
    uint64_t end_offset;
    size_t cursor;
    unsigned int entries = 0;

    if (offset > window_size || window_size - offset < IA64_HOB_HANDOFF_SIZE) {
        return false;
    }
    if (lduw_le_p(window + offset) != IA64_HOB_HANDOFF ||
        lduw_le_p(window + offset + 2) < IA64_HOB_HANDOFF_SIZE ||
        (lduw_le_p(window + offset + 2) & 7) != 0) {
        return false;
    }

    list_address = window_base + offset;
    info->free_top = ldq_le_p(window + offset + 32);
    info->free_bottom = ldq_le_p(window + offset + 40);
    end_address = ldq_le_p(window + offset + 48);
    if (end_address < list_address + IA64_HOB_HANDOFF_SIZE ||
        !ia64_hob_range_inside(end_address, IA64_HOB_HEADER_SIZE,
                               window_base, window_size)) {
        return false;
    }
    end_offset = end_address - window_base;

    memset(info, 0, sizeof(*info));
    info->address = list_address;
    info->end_address = end_address;
    info->free_top = ldq_le_p(window + offset + 32);
    info->free_bottom = ldq_le_p(window + offset + 40);
    cursor = offset;

    while (entries++ < IA64_HOB_MAX_ENTRIES) {
        uint16_t type;
        uint16_t length;

        if (cursor > window_size || window_size - cursor < IA64_HOB_HEADER_SIZE) {
            return false;
        }
        type = lduw_le_p(window + cursor);
        length = lduw_le_p(window + cursor + 2);
        if (length < IA64_HOB_HEADER_SIZE || (length & 7) != 0 ||
            cursor > window_size || window_size - cursor < length) {
            return false;
        }

        info->entries++;
        if (type == IA64_HOB_FV && length >= 24 &&
            info->fv_count < IA64_HOB_MAX_FVS) {
            struct Ia64HobFvRecord *record = &info->fvs[info->fv_count++];

            record->base = ldq_le_p(window + cursor + 8);
            record->length = ldq_le_p(window + cursor + 16);
            memcpy(record->raw, window + cursor, sizeof(record->raw));
        }

        if (type == IA64_HOB_END) {
            return cursor == end_offset && length == IA64_HOB_HEADER_SIZE;
        }
        cursor += length;
    }

    return false;
}

static bool ia64_hob_find_best(const uint8_t *window, uint64_t window_base,
                               size_t window_size, bool require_fv,
                               struct Ia64HobListInfo *best)
{
    bool found = false;
    size_t offset;

    memset(best, 0, sizeof(*best));
    for (offset = 0; offset + IA64_HOB_HANDOFF_SIZE <= window_size; offset += 8) {
        struct Ia64HobListInfo candidate;

        if (lduw_le_p(window + offset) != IA64_HOB_HANDOFF ||
            !ia64_hob_parse(window, window_base, window_size, offset,
                            &candidate) ||
            (require_fv && candidate.fv_count == 0)) {
            continue;
        }
        if (!found || candidate.fv_count > best->fv_count ||
            (candidate.fv_count == best->fv_count &&
             candidate.entries > best->entries)) {
            *best = candidate;
            found = true;
        }
    }
    return found;
}

static bool ia64_hob_same_fv(const struct Ia64HobFvRecord *left,
                             const struct Ia64HobFvRecord *right)
{
    return left->base == right->base && left->length == right->length;
}

static bool ia64_hob_target_has_fv(const struct Ia64HobListInfo *target,
                                   const struct Ia64HobFvRecord *source)
{
    unsigned int index;

    for (index = 0; index < target->fv_count; index++) {
        if (ia64_hob_same_fv(source, &target->fvs[index])) {
            return true;
        }
    }
    return false;
}

static unsigned int ia64_hob_missing_count(
    const struct Ia64HobListInfo *source,
    const struct Ia64HobListInfo *target)
{
    unsigned int missing = 0;
    unsigned int index;

    for (index = 0; index < source->fv_count; index++) {
        if (source->fvs[index].length != 0 &&
            !ia64_hob_target_has_fv(target, &source->fvs[index])) {
            missing++;
        }
    }
    return missing;
}

static bool ia64_hob_restore_missing(const struct Ia64HobListInfo *source,
                                     struct Ia64HobListInfo *target)
{
    uint8_t end_hob[IA64_HOB_HEADER_SIZE];
    uint64_t insertion = target->end_address;
    uint64_t new_end;
    uint64_t new_free_bottom = target->free_bottom;
    unsigned int missing;
    unsigned int index;

    missing = ia64_hob_missing_count(source, target);
    if (missing == 0) {
        return false;
    }
    if (uadd64_overflow(insertion, (uint64_t)missing * 24, &new_end) ||
        new_end + IA64_HOB_HEADER_SIZE > target->free_top ||
        !ia64_hob_read(target->end_address, end_hob, sizeof(end_hob)) ||
        lduw_le_p(end_hob) != IA64_HOB_END ||
        lduw_le_p(end_hob + 2) != IA64_HOB_HEADER_SIZE) {
        return false;
    }

    for (index = 0; index < source->fv_count; index++) {
        const struct Ia64HobFvRecord *record = &source->fvs[index];

        if (record->length == 0 || ia64_hob_target_has_fv(target, record)) {
            continue;
        }
        if (!ia64_hob_write(insertion, record->raw, sizeof(record->raw))) {
            return false;
        }
        insertion += sizeof(record->raw);
    }
    if (!ia64_hob_write(new_end, end_hob, sizeof(end_hob))) {
        return false;
    }

    if (target->free_bottom >= target->end_address) {
        new_free_bottom += (uint64_t)missing * 24;
        if (!ia64_hob_write(target->address + 40, &new_free_bottom,
                            sizeof(new_free_bottom))) {
            return false;
        }
    }
    if (!ia64_hob_write(target->address + 48, &new_end, sizeof(new_end))) {
        return false;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64 FV_HOB_RESTORE source=%016" PRIx64
                  " target=%016" PRIx64 " missing=%u new_end=%016" PRIx64
                  "\n",
                  source->address, target->address, missing, new_end);
    return true;
}

static void ia64_hob_probe_poll(void *opaque)
{
    struct Ia64HobProbeState *state = opaque;
    g_autofree uint8_t *source_window = NULL;
    g_autofree uint8_t *target_window = NULL;
    struct Ia64HobListInfo source;
    struct Ia64HobListInfo target;

    if (!state->enabled || state->polls++ >= IA64_HOB_POLL_LIMIT) {
        return;
    }

    source_window = g_malloc(IA64_HOB_SOURCE_SIZE);
    target_window = g_malloc(IA64_HOB_TARGET_SIZE);
    if (ia64_hob_read(IA64_HOB_SOURCE_BASE, source_window,
                      IA64_HOB_SOURCE_SIZE) &&
        ia64_hob_read(IA64_HOB_TARGET_BASE, target_window,
                      IA64_HOB_TARGET_SIZE) &&
        ia64_hob_find_best(source_window, IA64_HOB_SOURCE_BASE,
                           IA64_HOB_SOURCE_SIZE, true, &source) &&
        ia64_hob_find_best(target_window, IA64_HOB_TARGET_BASE,
                           IA64_HOB_TARGET_SIZE, false, &target) &&
        target.address != source.address &&
        ia64_hob_missing_count(&source, &target) != 0 &&
        ia64_hob_restore_missing(&source, &target)) {
        state->repairs++;
    }

    timer_mod(state->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + IA64_HOB_POLL_NS);
}

static void ia64_hob_probe_machine_done(Notifier *notifier, void *opaque)
{
    MachineState *machine = MACHINE(qdev_get_machine());

    if (!ia64_hob_probe_enabled() ||
        strcmp(object_get_typename(OBJECT(machine)), "ipf-machine") != 0) {
        return;
    }

    ia64_hob_probe.enabled = true;
    ia64_hob_probe.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        ia64_hob_probe_poll,
                                        &ia64_hob_probe);
    timer_mod(ia64_hob_probe.timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + IA64_HOB_POLL_NS);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IA64 FV_HOB_RESTORE diagnostic probe enabled\n");
}

static Notifier ia64_hob_probe_notifier = {
    .notify = ia64_hob_probe_machine_done,
};

static void ia64_hob_probe_register(void)
{
    qemu_add_machine_init_done_notifier(&ia64_hob_probe_notifier);
}

type_init(ia64_hob_probe_register)
