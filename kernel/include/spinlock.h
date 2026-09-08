/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/*
 * A spinlock protects short, non-sleeping critical sections.  Do not sleep,
 * block, perform long device I/O, or wait indefinitely for another CPU while
 * holding one.  A scheduler lock must not span the context-switch assembly
 * handoff.  Interrupt handlers sharing data with normal kernel code must use
 * the IRQ-save form when required.  Lock ordering is established by callers;
 * acquire locks in a stable outer-to-inner order and release them in reverse.
 */
typedef struct spinlock
{
    volatile u32 value;
} spinlock_t;

void spinlock_init(spinlock_t *lock);
bool spin_try_lock(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* The returned flags must be passed unchanged to the matching unlock. */
u64 spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags);
