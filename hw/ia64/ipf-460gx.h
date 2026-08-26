/*
 * IA-64 IPF 460GX firmware-visible chipset device
 *
 * Copyright (c) 2026 Bryce Lanham
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_IA64_IPF_460GX_H
#define HW_IA64_IPF_460GX_H

#include "hw/sysbus.h"

#define TYPE_IA64_IPF_460GX "ia64-ipf-460gx"
OBJECT_DECLARE_SIMPLE_TYPE(IA64IPF460GXState, IA64_IPF_460GX)

void ia64_ipf_460gx_set_config_address(IA64IPF460GXState *s,
                                        uint32_t value);
bool ia64_ipf_460gx_config_data(IA64IPF460GXState *s, bool is_write,
                                uint32_t port, unsigned int size,
                                uint32_t *value);
void ia64_ipf_460gx_set_sac_strap(IA64IPF460GXState *s, uint16_t value);

#endif /* HW_IA64_IPF_460GX_H */
