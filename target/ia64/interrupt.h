/*
 * IA-64 local SAPIC interrupt arbitration
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_IA64_INTERRUPT_H
#define TARGET_IA64_INTERRUPT_H

#include <stdbool.h>
#include <stdint.h>

#define IA64_INTERRUPT_BITMAP_WORDS 4
#define IA64_INTERRUPT_VECTOR_MIN 16
#define IA64_INTERRUPT_SPURIOUS_VECTOR 15

#define IA64_TPR_MIC_SHIFT 4
#define IA64_TPR_MIC_MASK (0xfULL << IA64_TPR_MIC_SHIFT)
#define IA64_TPR_MMI (1ULL << 16)
#define IA64_TPR_WRITABLE_MASK (IA64_TPR_MIC_MASK | IA64_TPR_MMI)
#define IA64_TPR_RESET IA64_TPR_MMI

int ia64_interrupt_highest(const uint64_t bitmap[IA64_INTERRUPT_BITMAP_WORDS]);
uint64_t ia64_interrupt_sanitize_tpr(uint64_t value);
int ia64_interrupt_next(
    const uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS],
    const uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS],
    uint64_t tpr);
bool ia64_interrupt_deposit(
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS], unsigned int vector);
unsigned int ia64_interrupt_accept(
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS],
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS],
    uint64_t tpr);
int ia64_interrupt_eoi(
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS]);

#endif
