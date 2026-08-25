/*
 * IA-64 local SAPIC interrupt arbitration
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "interrupt.h"
#include "qemu/host-utils.h"

int ia64_interrupt_highest(const uint64_t bitmap[IA64_INTERRUPT_BITMAP_WORDS])
{
    for (int word = IA64_INTERRUPT_BITMAP_WORDS - 1; word >= 0; word--) {
        uint64_t vectors = bitmap[word];

        if (word == 0) {
            vectors &= ~((1ULL << IA64_INTERRUPT_VECTOR_MIN) - 1);
        }
        if (vectors) {
            return word * 64 + 63 - clz64(vectors);
        }
    }

    return -1;
}

uint64_t ia64_interrupt_sanitize_tpr(uint64_t value)
{
    return value & IA64_TPR_WRITABLE_MASK;
}

int ia64_interrupt_next(
    const uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS],
    const uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS],
    uint64_t tpr)
{
    int pending_vector = ia64_interrupt_highest(pending);
    int in_service_vector;
    unsigned int minimum_class;

    if (pending_vector < 0) {
        return -1;
    }

    in_service_vector = ia64_interrupt_highest(in_service);
    if (in_service_vector >= pending_vector) {
        return -1;
    }

    tpr = ia64_interrupt_sanitize_tpr(tpr);
    if (tpr & IA64_TPR_MMI) {
        return -1;
    }

    minimum_class = (tpr & IA64_TPR_MIC_MASK) >> IA64_TPR_MIC_SHIFT;
    if (((unsigned int)pending_vector >> 4) <= minimum_class) {
        return -1;
    }

    return pending_vector;
}

bool ia64_interrupt_deposit(
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS], unsigned int vector)
{
    if (vector < IA64_INTERRUPT_VECTOR_MIN || vector > UINT8_MAX) {
        return false;
    }

    pending[vector >> 6] |= 1ULL << (vector & 63);
    return true;
}

unsigned int ia64_interrupt_accept(
    uint64_t pending[IA64_INTERRUPT_BITMAP_WORDS],
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS],
    uint64_t tpr)
{
    int vector = ia64_interrupt_next(pending, in_service, tpr);

    if (vector < 0) {
        return IA64_INTERRUPT_SPURIOUS_VECTOR;
    }

    pending[(unsigned int)vector >> 6] &= ~(1ULL << (vector & 63));
    in_service[(unsigned int)vector >> 6] |= 1ULL << (vector & 63);
    return vector;
}

int ia64_interrupt_eoi(
    uint64_t in_service[IA64_INTERRUPT_BITMAP_WORDS])
{
    int vector = ia64_interrupt_highest(in_service);

    if (vector >= 0) {
        in_service[(unsigned int)vector >> 6] &=
            ~(1ULL << (vector & 63));
    }
    return vector;
}
