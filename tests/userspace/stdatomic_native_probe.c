/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdatomic.h>
#include <stdint.h>

static atomic_uint state = ATOMIC_VAR_INIT(1U);
static _Thread_local atomic_uint thread_state = ATOMIC_VAR_INIT(0U);

int main(void)
{
    unsigned expected = 1U;
    if (!atomic_compare_exchange_strong_explicit(
            &state, &expected, 2U, memory_order_acq_rel,
            memory_order_acquire))
        return 1;
    atomic_store_explicit(&thread_state, 3U, memory_order_release);
    return atomic_load_explicit(&thread_state, memory_order_acquire) == 3U &&
           atomic_load(&state) == 2U ? 0 : 2;
}
