/*
 * IA-64 I/O SAPIC device
 *
 * Copyright (c) 2026 Bryce Lanham
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IA64_IOSAPIC_H
#define HW_IA64_IOSAPIC_H

#include "hw/sysbus.h"
#include "hw/ia64/iosapic-core.h"
#include "target/ia64/cpu-qom.h"

#define TYPE_IA64_IOSAPIC "ia64-iosapic"
OBJECT_DECLARE_SIMPLE_TYPE(IA64IOSAPICState, IA64_IOSAPIC)

qemu_irq ia64_iosapic_get_irq(IA64IOSAPICState *s, unsigned int pin);
void ia64_iosapic_set_irq(void *opaque, int pin, int level);

#endif /* HW_IA64_IOSAPIC_H */
