/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <spinlock.h>
#include <cpu_relax.h>

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELAXED);
}

bool spin_try_lock(spinlock_t *lock)
{
    u32 expected = 0U;

    return __atomic_compare_exchange_n(&lock->value, &expected, 1U, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void spin_lock(spinlock_t *lock)
{
    for (;;) {
        while (__atomic_load_n(&lock->value, __ATOMIC_RELAXED) != 0U)
            cpu_relax();

        if (spin_try_lock(lock))
            return;
        cpu_relax();
    }
}

void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

static u64 spin_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void spin_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

u64 spin_lock_irqsave(spinlock_t *lock)
{
    u64 flags = spin_irq_save();

    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    spin_unlock(lock);
    spin_irq_restore(flags);
}
