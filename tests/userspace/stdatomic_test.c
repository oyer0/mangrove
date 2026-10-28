/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

/* Include the production header without replacing the host pthread headers. */
#include "../../libc/include/stdatomic.h"

#define WORKERS 6U
#define ITERATIONS 80000U

static atomic_uint counter = ATOMIC_VAR_INIT(0U);
static atomic_uint published = ATOMIC_VAR_INIT(0U);
static unsigned published_payload;

static void *increment_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < ITERATIONS; i++)
        (void)atomic_fetch_add_explicit(&counter, 1U, memory_order_relaxed);
    return NULL;
}

static void *publisher(void *unused)
{
    (void)unused;
    published_payload = 0x5a17c0deU;
    atomic_store_explicit(&published, 1U, memory_order_release);
    return NULL;
}

static void *observer(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&published, memory_order_acquire)) { }
    assert(published_payload == 0x5a17c0deU);
    return NULL;
}

int main(void)
{
    pthread_t workers[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++)
        assert(pthread_create(&workers[i], NULL, increment_worker, NULL) == 0);
    for (unsigned i = 0; i < WORKERS; i++)
        assert(pthread_join(workers[i], NULL) == 0);
    assert(atomic_load_explicit(&counter, memory_order_acquire) ==
           WORKERS * ITERATIONS);

    unsigned expected = WORKERS * ITERATIONS;
    assert(atomic_compare_exchange_strong_explicit(
        &counter, &expected, 7U, memory_order_acq_rel, memory_order_acquire));
    expected = 8U;
    assert(!atomic_compare_exchange_weak_explicit(
        &counter, &expected, 9U, memory_order_release, memory_order_relaxed));
    assert(expected == 7U);
    assert(atomic_exchange_explicit(&counter, 10U, memory_order_seq_cst) == 7U);
    assert(atomic_fetch_sub(&counter, 3U) == 10U);
    assert(atomic_fetch_or(&counter, 4U) == 7U);
    assert(atomic_fetch_xor(&counter, 3U) == 7U);
    assert(atomic_fetch_and(&counter, 6U) == 4U);
    assert(atomic_fetch_nand(&counter, 1U) == 4U);

    atomic_flag flag = ATOMIC_FLAG_INIT;
    assert(!atomic_flag_test_and_set_explicit(&flag, memory_order_acquire));
    assert(atomic_flag_test_and_set(&flag));
    atomic_flag_clear_explicit(&flag, memory_order_release);
    assert(!atomic_flag_test_and_set(&flag));

    atomic_store_explicit(&published, 0U, memory_order_relaxed);
    pthread_t reader, writer;
    assert(pthread_create(&reader, NULL, observer, NULL) == 0);
    assert(pthread_create(&writer, NULL, publisher, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0);
    assert(pthread_join(reader, NULL) == 0);

    assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2);
    assert(atomic_is_lock_free(&counter));
    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_acq_rel);
    puts("Mangrove C17 atomic tests passed");
    return 0;
}
