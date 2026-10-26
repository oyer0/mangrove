/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <types.h>
typedef volatile u32 spinlock_t;
static inline void spinlock_init(spinlock_t *lock)
{ __atomic_store_n(lock, 0U, __ATOMIC_RELAXED); }
static inline u64 spin_lock_irqsave(spinlock_t *lock)
{ while (__atomic_exchange_n(lock, 1U, __ATOMIC_ACQUIRE)) { } return 0; }
static inline void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{ (void)flags; __atomic_store_n(lock, 0U, __ATOMIC_RELEASE); }
