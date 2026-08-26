/*
 * IA-64 IPF 460GX firmware-visible chipset device
 *
 * Copyright (c) 2026 Bryce Lanham
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ia64/ipf-460gx.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "target/ia64/cpu.h"

#define IPF_460GX_CSE_BUS 0xff
#define IPF_460GX_DEVICE_COUNT 32
#define IPF_460GX_MAX_FUNCTIONS 8
#define IPF_460GX_FUNCTION_COUNT \
    (IPF_460GX_DEVICE_COUNT * IPF_460GX_MAX_FUNCTIONS)
#define IPF_460GX_CONFIG_BYTES \
    (IPF_460GX_FUNCTION_COUNT * PCI_CONFIG_SPACE_SIZE)

#define IPF_460GX_DEV_SAC 0
#define IPF_460GX_DEV_GXB 2
#define IPF_460GX_DEV_SDC 4
#define IPF_460GX_DEV_MAC 5
#define IPF_460GX_DEV_MDC 6

#define IPF_460GX_DEVICE_ID_SAC 0x84e0
#define IPF_460GX_DEVICE_ID_SDC 0x84e1
#define IPF_460GX_DEVICE_ID_GXB_FN1 0x84ea
#define IPF_460GX_DEVICE_ID_GXB_FN2 0x84e2
#define IPF_460GX_DEVICE_ID_MAC 0x84e3
#define IPF_460GX_DEVICE_ID_MDC 0x84e4
#define IPF_460GX_DEVICE_ID_WXB 0x84e6
#define IPF_460GX_DEVICE_ID_IHPC 0x123f

#define IPF_460GX_MMIO_SIZE 0x1000
#define IPF_460GX_MMIO_REG_CB0 0x0cb0
#define IPF_460GX_MMIO_REG_CC0 0x0cc0

struct IA64IPF460GXState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion mmio_alias;

    uint32_t config_address;
    uint32_t cb0;
    uint32_t cc0;
    uint16_t sac_strap;

    uint8_t present[IPF_460GX_FUNCTION_COUNT];
    uint8_t config[IPF_460GX_CONFIG_BYTES];
    uint8_t write_mask[IPF_460GX_CONFIG_BYTES];
    uint8_t w1c[IPF_460GX_CONFIG_BYTES];
};

static size_t ipf_460gx_function_index(unsigned int dev,
                                       unsigned int function)
{
    return dev * IPF_460GX_MAX_FUNCTIONS + function;
}

static size_t ipf_460gx_config_index(unsigned int dev,
                                     unsigned int function,
                                     unsigned int offset)
{
    return ipf_460gx_function_index(dev, function) * PCI_CONFIG_SPACE_SIZE +
           offset;
}

static uint8_t *ipf_460gx_config(IA64IPF460GXState *s, unsigned int dev,
                                 unsigned int function)
{
    return &s->config[ipf_460gx_config_index(dev, function, 0)];
}

static uint8_t *ipf_460gx_write_mask(IA64IPF460GXState *s,
                                     unsigned int dev,
                                     unsigned int function)
{
    return &s->write_mask[ipf_460gx_config_index(dev, function, 0)];
}

static uint8_t *ipf_460gx_w1c(IA64IPF460GXState *s, unsigned int dev,
                              unsigned int function)
{
    return &s->w1c[ipf_460gx_config_index(dev, function, 0)];
}

static bool ipf_460gx_present(IA64IPF460GXState *s, unsigned int dev,
                              unsigned int function)
{
    return dev < IPF_460GX_DEVICE_COUNT &&
           function < IPF_460GX_MAX_FUNCTIONS &&
           s->present[ipf_460gx_function_index(dev, function)];
}

static void ipf_460gx_set(IA64IPF460GXState *s, unsigned int dev,
                          unsigned int function, uint16_t offset,
                          unsigned int size, uint64_t value,
                          uint64_t write_mask, uint64_t w1c)
{
    uint8_t *config = ipf_460gx_config(s, dev, function);
    uint8_t *mask = ipf_460gx_write_mask(s, dev, function);
    uint8_t *clear = ipf_460gx_w1c(s, dev, function);

    for (unsigned int i = 0; i < size; i++) {
        uint16_t index = offset + i;

        if (index >= PCI_CONFIG_SPACE_SIZE) {
            break;
        }
        config[index] = value >> (i * 8);
        mask[index] = write_mask >> (i * 8);
        clear[index] = w1c >> (i * 8);
    }
}

static void ipf_460gx_set_ro(IA64IPF460GXState *s, unsigned int dev,
                             unsigned int function, uint16_t offset,
                             unsigned int size, uint64_t value)
{
    ipf_460gx_set(s, dev, function, offset, size, value, 0, 0);
}

static void ipf_460gx_set_rw(IA64IPF460GXState *s, unsigned int dev,
                             unsigned int function, uint16_t offset,
                             unsigned int size, uint64_t value,
                             uint64_t write_mask)
{
    ipf_460gx_set(s, dev, function, offset, size, value, write_mask, 0);
}

static void ipf_460gx_set_w1c(IA64IPF460GXState *s, unsigned int dev,
                              unsigned int function, uint16_t offset,
                              unsigned int size, uint64_t value,
                              uint64_t w1c)
{
    ipf_460gx_set(s, dev, function, offset, size, value, 0, w1c);
}

static void ipf_460gx_init_function(IA64IPF460GXState *s,
                                    unsigned int dev,
                                    unsigned int function,
                                    uint16_t vendor_id,
                                    uint16_t device_id,
                                    uint8_t base_class,
                                    uint8_t sub_class,
                                    uint8_t prog_if,
                                    uint8_t header_type)
{
    size_t function_index = ipf_460gx_function_index(dev, function);
    uint8_t *config = ipf_460gx_config(s, dev, function);
    uint8_t *mask = ipf_460gx_write_mask(s, dev, function);
    uint8_t *clear = ipf_460gx_w1c(s, dev, function);

    s->present[function_index] = true;
    memset(config, 0, PCI_CONFIG_SPACE_SIZE);
    memset(mask, 0xff, PCI_CONFIG_SPACE_SIZE);
    memset(clear, 0, PCI_CONFIG_SPACE_SIZE);

    ipf_460gx_set_ro(s, dev, function, PCI_VENDOR_ID, 2, vendor_id);
    ipf_460gx_set_ro(s, dev, function, PCI_DEVICE_ID, 2, device_id);
    ipf_460gx_set_ro(s, dev, function, PCI_REVISION_ID, 1, 0);
    ipf_460gx_set_ro(s, dev, function, PCI_CLASS_PROG, 1, prog_if);
    ipf_460gx_set_ro(s, dev, function, PCI_CLASS_DEVICE, 2,
                     (base_class << 8) | sub_class);
    ipf_460gx_set_ro(s, dev, function, PCI_HEADER_TYPE, 1, header_type);
}

static void ipf_460gx_init_sac(IA64IPF460GXState *s)
{
    /* SECTID/DEDTID/FSETID: bit 7 RW, bit 6 W1C, bits 5:0 RO. */
    ipf_460gx_set(s, IPF_460GX_DEV_SAC, 0, 0x80, 1, 0, 0x80, 0x40);
    ipf_460gx_set(s, IPF_460GX_DEV_SAC, 0, 0x81, 1, 0, 0x80, 0x40);
    ipf_460gx_set(s, IPF_460GX_DEV_SAC, 0, 0x82, 1, 0, 0x80, 0x40);

    ipf_460gx_set_ro(s, IPF_460GX_DEV_SAC, 0, 0xc0, 8,
                     UINT64_C(0x8080808080808080));

    ipf_460gx_set_w1c(s, IPF_460GX_DEV_SAC, 1, 0x40, 4, 0,
                      UINT32_MAX);
    ipf_460gx_set_w1c(s, IPF_460GX_DEV_SAC, 1, 0x44, 4, 0,
                      UINT32_MAX);
    ipf_460gx_set_rw(s, IPF_460GX_DEV_SAC, 1, 0x80, 1, 0, 0x3f);

    for (unsigned int i = 0; i < 6; i++) {
        uint16_t pmd = 0x90 + i * 8;
        uint16_t pmc = 0xd0 + i * 8;

        ipf_460gx_set_rw(s, IPF_460GX_DEV_SAC, 2, pmd, 8, 0,
                         UINT64_C(0x000000ffffffffff));
        ipf_460gx_set_rw(s, IPF_460GX_DEV_SAC, 2, pmc, 8, 0,
                         UINT64_C(0x000001ffffffffff));
    }
}

static void ipf_460gx_init_wxb(IA64IPF460GXState *s, unsigned int dev)
{
    ipf_460gx_set_rw(s, dev, 0, 0x40, 2, 0x00ff, 0x00ff);
    ipf_460gx_set_w1c(s, dev, 0, 0x44, 1, 0, 0xab);
    ipf_460gx_set_rw(s, dev, 0, 0x45, 2, 0x8040, 0xb800);
}

static void ipf_460gx_init_sdc(IA64IPF460GXState *s)
{
    unsigned int dev = IPF_460GX_DEV_SDC;

    ipf_460gx_set_ro(s, dev, 0, 0x40, 8, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x48, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x49, 2, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x50, 8, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x58, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x59, 2, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x60, 8, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x68, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x69, 2, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x70, 8, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x78, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x79, 2, 0);

    ipf_460gx_set_w1c(s, dev, 0, 0x80, 4, 0, UINT32_MAX);
    ipf_460gx_set_w1c(s, dev, 0, 0x84, 4, 0, UINT32_MAX);

    ipf_460gx_set_ro(s, dev, 0, 0x88, 4, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x8c, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x8d, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0x8e, 1, 0);

    ipf_460gx_set_rw(s, dev, 0, 0x98, 3, 0, 0x01ffff);
    ipf_460gx_set_rw(s, dev, 0, 0x9c, 3, 0, 0x01ffff);
    ipf_460gx_set_rw(s, dev, 0, 0xa0, 8, 0,
                     UINT64_C(0x000000ffffffffff));
    ipf_460gx_set_rw(s, dev, 0, 0xa8, 8, 0,
                     UINT64_C(0x000000ffffffffff));

    ipf_460gx_set_rw(s, dev, 0, 0xc8, 1, 0, 0xff);
    ipf_460gx_set_rw(s, dev, 0, 0xc9, 1, 0, 0xff);
    ipf_460gx_set_rw(s, dev, 0, 0xca, 1, 0, 0xff);
    ipf_460gx_set_rw(s, dev, 0, 0xcb, 1, 0, 0xff);

    ipf_460gx_set_ro(s, dev, 0, 0xd0, 8, 0);
    ipf_460gx_set_ro(s, dev, 0, 0xd8, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0xd9, 2, 0);
    ipf_460gx_set_ro(s, dev, 0, 0xe0, 8, 0);
    ipf_460gx_set_ro(s, dev, 0, 0xe8, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0xe9, 2, 0);
    ipf_460gx_set_ro(s, dev, 0, 0xf0, 8, 0);
    ipf_460gx_set_ro(s, dev, 0, 0xf8, 1, 0);
    ipf_460gx_set_ro(s, dev, 0, 0xf9, 2, 0);
}

static void ipf_460gx_init_mac(IA64IPF460GXState *s, unsigned int dev,
                               unsigned int function)
{
    ipf_460gx_set_ro(s, dev, function, 0x98, 1, 0);
    ipf_460gx_set_ro(s, dev, function, 0x9c, 3, 0);
}

static void ipf_460gx_init_ihpc(IA64IPF460GXState *s, unsigned int dev)
{
    ipf_460gx_set_rw(s, dev, 1, PCI_COMMAND, 2, 0, 0x0142);
    ipf_460gx_set(s, dev, 1, PCI_STATUS, 2, 0x0200, 0, 0xc000);
    ipf_460gx_set_rw(s, dev, 1, PCI_CACHE_LINE_SIZE, 1, 0, 0xff);
    ipf_460gx_set_rw(s, dev, 1, PCI_LATENCY_TIMER, 1, 0, 0xff);
    ipf_460gx_set_rw(s, dev, 1, PCI_INTERRUPT_LINE, 1, 0xff, 0xff);
    ipf_460gx_set_ro(s, dev, 1, PCI_INTERRUPT_PIN, 1, 1);
    ipf_460gx_set_rw(s, dev, 1, 0x40, 2, 0, 0x00ff);
    ipf_460gx_set_rw(s, dev, 1, 0x42, 2, 0x0002, 0xf080);
    ipf_460gx_set_ro(s, dev, 1, 0x44, 2, 0);
    ipf_460gx_set_w1c(s, dev, 1, 0x48, 1, 0, 0x3f);
    ipf_460gx_set_w1c(s, dev, 1, 0x49, 1, 0, 0x3f);
    ipf_460gx_set_ro(s, dev, 1, 0x4a, 1, 0);
    ipf_460gx_set_rw(s, dev, 1, 0x50, 4, 0, 0x000000fc);
    ipf_460gx_set_rw(s, dev, 1, 0x54, 4, 0, UINT32_MAX);
}

static void ipf_460gx_apply_sac_strap(IA64IPF460GXState *s)
{
    ipf_460gx_set_ro(s, IPF_460GX_DEV_SAC, 0, 0x44, 2,
                     s->sac_strap);
}

static void ipf_460gx_reset_state(IA64IPF460GXState *s)
{
    memset(s->present, 0, sizeof(s->present));
    memset(s->config, 0, sizeof(s->config));
    memset(s->write_mask, 0, sizeof(s->write_mask));
    memset(s->w1c, 0, sizeof(s->w1c));

    s->config_address = 0;
    s->cb0 = 0;
    s->cc0 = 0;

    ipf_460gx_init_function(s, IPF_460GX_DEV_SAC, 0,
                            PCI_VENDOR_ID_INTEL, IPF_460GX_DEVICE_ID_SAC,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0x80);
    ipf_460gx_init_function(s, IPF_460GX_DEV_SAC, 1,
                            PCI_VENDOR_ID_INTEL, IPF_460GX_DEVICE_ID_SAC,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0);
    ipf_460gx_init_function(s, IPF_460GX_DEV_SAC, 2,
                            PCI_VENDOR_ID_INTEL, IPF_460GX_DEVICE_ID_SAC,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0);
    ipf_460gx_init_sac(s);

    ipf_460gx_init_function(s, IPF_460GX_DEV_SDC, 0,
                            PCI_VENDOR_ID_INTEL, IPF_460GX_DEVICE_ID_SDC,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0);
    ipf_460gx_init_sdc(s);

    /* Function zero remains the real root-bus device at slot 2. */
    ipf_460gx_init_function(s, IPF_460GX_DEV_GXB, 1,
                            PCI_VENDOR_ID_INTEL,
                            IPF_460GX_DEVICE_ID_GXB_FN1,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0);
    ipf_460gx_set_ro(s, IPF_460GX_DEV_GXB, 1, PCI_REVISION_ID, 1, 4);
    ipf_460gx_init_function(s, IPF_460GX_DEV_GXB, 2,
                            PCI_VENDOR_ID_INTEL,
                            IPF_460GX_DEVICE_ID_GXB_FN2,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0);
    ipf_460gx_set_ro(s, IPF_460GX_DEV_GXB, 2, PCI_REVISION_ID, 1, 4);

    ipf_460gx_init_function(s, IPF_460GX_DEV_MAC, 0,
                            PCI_VENDOR_ID_INTEL, IPF_460GX_DEVICE_ID_MAC,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0x80);
    ipf_460gx_init_function(s, IPF_460GX_DEV_MAC, 1,
                            PCI_VENDOR_ID_INTEL, IPF_460GX_DEVICE_ID_MDC,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0);
    ipf_460gx_init_mac(s, IPF_460GX_DEV_MAC, 0);
    ipf_460gx_init_mac(s, IPF_460GX_DEV_MAC, 1);

    ipf_460gx_init_function(s, IPF_460GX_DEV_MDC, 0,
                            PCI_VENDOR_ID_INTEL, IPF_460GX_DEVICE_ID_MAC,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0x80);
    ipf_460gx_init_function(s, IPF_460GX_DEV_MDC, 1,
                            PCI_VENDOR_ID_INTEL, IPF_460GX_DEVICE_ID_MDC,
                            PCI_CLASS_BRIDGE_HOST >> 8,
                            PCI_CLASS_BRIDGE_HOST & 0xff, 0, 0);
    ipf_460gx_init_mac(s, IPF_460GX_DEV_MDC, 0);
    ipf_460gx_init_mac(s, IPF_460GX_DEV_MDC, 1);

    for (unsigned int dev = 16; dev <= 23; dev++) {
        ipf_460gx_init_function(s, dev, 0, PCI_VENDOR_ID_INTEL,
                                IPF_460GX_DEVICE_ID_WXB,
                                PCI_CLASS_BRIDGE_PCI >> 8,
                                PCI_CLASS_BRIDGE_PCI & 0xff, 0, 0x81);
        ipf_460gx_init_function(s, dev, 1, PCI_VENDOR_ID_INTEL,
                                IPF_460GX_DEVICE_ID_IHPC,
                                0x08, 0x04, 0, 0);
        ipf_460gx_init_wxb(s, dev);
        ipf_460gx_init_ihpc(s, dev);
    }

    pci_set_long(ipf_460gx_config(s, IPF_460GX_DEV_SAC, 0) + 0x70,
                 UINT32_MAX);
    ipf_460gx_apply_sac_strap(s);
}

static uint32_t ipf_460gx_read_config(IA64IPF460GXState *s,
                                      unsigned int dev,
                                      unsigned int function,
                                      uint16_t reg, unsigned int size)
{
    uint8_t *config = ipf_460gx_config(s, dev, function);
    uint32_t value = 0;

    for (unsigned int i = 0; i < size; i++) {
        uint16_t offset = reg + i;
        uint8_t byte = offset < PCI_CONFIG_SPACE_SIZE ?
                       config[offset] : 0xff;

        value |= (uint32_t)byte << (i * 8);
    }
    return value;
}

static void ipf_460gx_write_config(IA64IPF460GXState *s,
                                   unsigned int dev,
                                   unsigned int function,
                                   uint16_t reg, unsigned int size,
                                   uint32_t value)
{
    uint8_t *config = ipf_460gx_config(s, dev, function);
    uint8_t *write_mask = ipf_460gx_write_mask(s, dev, function);
    uint8_t *w1c = ipf_460gx_w1c(s, dev, function);

    for (unsigned int i = 0; i < size; i++) {
        uint16_t offset = reg + i;
        uint8_t byte;
        uint8_t current;
        uint8_t mask;

        if (offset >= PCI_CONFIG_SPACE_SIZE) {
            break;
        }
        byte = value >> (i * 8);
        current = config[offset];
        mask = write_mask[offset] & ~w1c[offset];
        current &= ~(byte & w1c[offset]);
        current = (current & ~mask) | (byte & mask);
        config[offset] = current;
    }
}

static void ipf_460gx_trace_special(unsigned int dev,
                                    unsigned int function,
                                    uint16_t reg, unsigned int size,
                                    uint32_t value, bool is_write)
{
    static int enabled = -1;
    uint64_t pc = 0;

    if (enabled == -1) {
        enabled = getenv("QEMU_IPF_TRACE_PCI_SPECIAL") ? 1 : 0;
    }
    if (!enabled || dev != IPF_460GX_DEV_MDC ||
        !((function <= 1 && (reg == 0x90 || reg == 0x94)) ||
          (function >= 4 && reg == 0x48))) {
        return;
    }
    if (current_cpu) {
        CPUIA64State *env = cpu_env(current_cpu);

        pc = env->ip;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "ipf pci special %s dev=%u fn=%u reg=0x%02x "
                  "size=%u val=0x%08x pc=%016" PRIx64 "\n",
                  is_write ? "wr" : "rd", dev, function, reg, size,
                  value, pc);
}

void ia64_ipf_460gx_set_config_address(IA64IPF460GXState *s,
                                        uint32_t value)
{
    s->config_address = value;
}

bool ia64_ipf_460gx_config_data(IA64IPF460GXState *s, bool is_write,
                                uint32_t port, unsigned int size,
                                uint32_t *value)
{
    uint32_t config_address;
    unsigned int bus;
    unsigned int dev;
    unsigned int function;
    uint16_t reg;
    bool cse_bus;
    bool root_bus;

    if (port < 0xcfc || port > 0xcff) {
        return false;
    }

    config_address = s->config_address;
    if (!(config_address & 0x80000000U)) {
        return false;
    }

    bus = (config_address >> 16) & 0xff;
    dev = (config_address >> 11) & 0x1f;
    function = (config_address >> 8) & 0x7;
    cse_bus = bus == IPF_460GX_CSE_BUS;
    root_bus = bus == 0;

    if (!cse_bus && !root_bus) {
        return false;
    }
    if (!ipf_460gx_present(s, dev, function)) {
        if (root_bus) {
            return false;
        }
        if (!is_write && value) {
            *value = size == 1 ? 0xff :
                     size == 2 ? 0xffff : UINT32_MAX;
        }
        return true;
    }

    reg = (config_address & 0xfc) + (port & 3);
    if (is_write) {
        uint32_t written = value ? *value : 0;

        ipf_460gx_write_config(s, dev, function, reg, size, written);
        ipf_460gx_trace_special(dev, function, reg, size, written, true);
    } else if (value) {
        *value = ipf_460gx_read_config(s, dev, function, reg, size);
        ipf_460gx_trace_special(dev, function, reg, size, *value, false);
    }
    return true;
}

void ia64_ipf_460gx_set_sac_strap(IA64IPF460GXState *s, uint16_t value)
{
    s->sac_strap = value;
    ipf_460gx_apply_sac_strap(s);
}

static uint32_t ipf_460gx_mmio_read_register(IA64IPF460GXState *s,
                                             hwaddr offset)
{
    switch (offset) {
    case IPF_460GX_MMIO_REG_CB0:
        return s->cb0 | (1U << 7);
    case IPF_460GX_MMIO_REG_CC0:
        return s->cc0 | (1U << 7);
    default:
        return 0;
    }
}

static void ipf_460gx_mmio_write_register(IA64IPF460GXState *s,
                                          hwaddr offset, uint32_t value,
                                          uint32_t mask)
{
    switch (offset) {
    case IPF_460GX_MMIO_REG_CB0:
        s->cb0 = (s->cb0 & ~mask) | (value & mask);
        if (s->cb0 & 1) {
            s->cb0 |= 2;
        } else {
            s->cb0 &= ~2U;
        }
        break;
    case IPF_460GX_MMIO_REG_CC0:
        s->cc0 = (s->cc0 & ~mask) | (value & mask);
        break;
    default:
        break;
    }
}

static uint64_t ipf_460gx_mmio_read(void *opaque, hwaddr offset,
                                    unsigned int size)
{
    IA64IPF460GXState *s = opaque;
    uint32_t reg;
    uint64_t mask;
    unsigned int shift;

    if (size < 1 || size > 4) {
        return 0;
    }
    reg = ipf_460gx_mmio_read_register(s, offset & ~3ULL);
    shift = (offset & 3) * 8;
    mask = size == 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    return (reg >> shift) & mask;
}

static void ipf_460gx_mmio_write(void *opaque, hwaddr offset,
                                 uint64_t data, unsigned int size)
{
    IA64IPF460GXState *s = opaque;
    uint32_t mask;
    uint32_t value;
    unsigned int shift;

    if (size < 1 || size > 4) {
        return;
    }
    shift = (offset & 3) * 8;
    mask = size == 4 ? UINT32_MAX : (1U << (size * 8)) - 1;
    value = data & mask;
    if (shift) {
        mask <<= shift;
        value <<= shift;
    }
    ipf_460gx_mmio_write_register(s, offset & ~3ULL, value, mask);
}

static const MemoryRegionOps ipf_460gx_mmio_ops = {
    .read = ipf_460gx_mmio_read,
    .write = ipf_460gx_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void ipf_460gx_reset(DeviceState *dev)
{
    ipf_460gx_reset_state(IA64_IPF_460GX(dev));
}

static void ipf_460gx_init(Object *obj)
{
    IA64IPF460GXState *s = IA64_IPF_460GX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &ipf_460gx_mmio_ops, s,
                          "ia64-ipf-460gx", IPF_460GX_MMIO_SIZE);
    memory_region_init_alias(&s->mmio_alias, obj, "ia64-ipf-460gx.alias",
                             &s->mmio, 0, IPF_460GX_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_mmio(sbd, &s->mmio_alias);
    ipf_460gx_reset_state(s);
}

static const VMStateDescription vmstate_ipf_460gx = {
    .name = TYPE_IA64_IPF_460GX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(config_address, IA64IPF460GXState),
        VMSTATE_UINT32(cb0, IA64IPF460GXState),
        VMSTATE_UINT32(cc0, IA64IPF460GXState),
        VMSTATE_UINT16(sac_strap, IA64IPF460GXState),
        VMSTATE_UINT8_ARRAY(config, IA64IPF460GXState,
                            IPF_460GX_CONFIG_BYTES),
        VMSTATE_END_OF_LIST()
    },
};

static void ipf_460gx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ipf_460gx_reset);
    dc->vmsd = &vmstate_ipf_460gx;
    dc->desc = "IA-64 IPF 460GX firmware chipset";
    dc->user_creatable = false;
}

static const TypeInfo ipf_460gx_info = {
    .name = TYPE_IA64_IPF_460GX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IA64IPF460GXState),
    .instance_init = ipf_460gx_init,
    .class_init = ipf_460gx_class_init,
};

static void ipf_460gx_register_types(void)
{
    type_register_static(&ipf_460gx_info);
}

type_init(ipf_460gx_register_types)
