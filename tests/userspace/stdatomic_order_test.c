/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../../libc/include/stdatomic.h"

/* Observe the actual success/failure arguments reaching the compiler builtin.
 * Macro self-suppression leaves the final invocation as the real builtin. */
static int seen_success, seen_failure;
#define __atomic_compare_exchange_n(p, e, d, w, s, f) ({ \
    int test_success = (s), test_failure = (f); \
    seen_success = test_success; seen_failure = test_failure; \
    __atomic_compare_exchange_n((p), (e), (d), (w), \
                                test_success, test_failure); \
})

int main(void)
{
    const memory_order orders[] = {memory_order_relaxed, memory_order_consume,
        memory_order_acquire, memory_order_release, memory_order_acq_rel,
        memory_order_seq_cst};
    const int failure[] = {__ATOMIC_RELAXED, __ATOMIC_CONSUME,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED, __ATOMIC_ACQUIRE, __ATOMIC_SEQ_CST};
    const long values[8] = {0};
    volatile _Atomic(const long *) pointer = values + 2;
    for (unsigned i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
        assert(atomic_fetch_add_explicit(&pointer, 1, orders[i]) == values + 2);
        assert(seen_success == (int)orders[i] && seen_failure == failure[i]);
        assert(atomic_fetch_sub_explicit(&pointer, 1, orders[i]) == values + 3);
        assert(seen_success == (int)orders[i] && seen_failure == failure[i]);
    }
    assert(atomic_fetch_add(&pointer, -1) == values + 2);
    assert(seen_success == __ATOMIC_SEQ_CST && seen_failure == __ATOMIC_SEQ_CST);
    assert(atomic_fetch_sub(&pointer, -1) == values + 1);
    assert(seen_success == __ATOMIC_SEQ_CST && seen_failure == __ATOMIC_SEQ_CST);
    puts("pointer atomic order arguments and typed old values: PASS");
}
