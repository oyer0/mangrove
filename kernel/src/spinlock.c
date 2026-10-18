/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <spinlock.h>
#include <cpu_relax.h>
#include <cpu.h>
#include <panic.h>

static void spinlock_depth_enter(void)
{
    cpu_local_t *cpu = cpu_current();

    if (cpu && __atomic_add_fetch(&cpu->spinlock_depth, 1U,
                                  __ATOMIC_ACQ_REL) == 0U)
        panic("spinlock: held-depth overflow");
}

static void spinlock_depth_leave(void)
{
    cpu_local_t *cpu = cpu_current();

    if (cpu && __atomic_sub_fetch(&cpu->spinlock_depth, 1U,
                                  __ATOMIC_ACQ_REL) == ~(u32)0)
        panic("spinlock: held-depth underflow");
}

u32 spinlock_depth_current(void)
{
    cpu_local_t *cpu = cpu_current();

    if (!cpu)
        return ~(u32)0;
    return __atomic_load_n(&cpu->spinlock_depth, __ATOMIC_ACQUIRE);
}

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELAXED);
}

bool spin_try_lock(spinlock_t *lock)
{
    u32 expected = 0U;
    bool acquired;

    /* Count before the compare-exchange so an interrupt cannot preempt in
     * the gap between successful acquisition and publishing the held state.
     * A failed try immediately removes its temporary count. */
    spinlock_depth_enter();
    acquired = __atomic_compare_exchange_n(
        &lock->value, &expected, 1U, false, __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED);
    if (!acquired)
        spinlock_depth_leave();
    return acquired;
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
    spinlock_depth_leave();
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
