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
typedef _Atomic(int *) atomic_int_pointer;
static atomic_int_pointer selected_pointer;
static unsigned pointer_object_evaluations;
static unsigned pointer_operand_evaluations;
static unsigned pointer_order_evaluations;

static atomic_int_pointer *select_pointer(void)
{
    pointer_object_evaluations++;
    return &selected_pointer;
}

static ptrdiff_t select_pointer_operand(void)
{
    pointer_operand_evaluations++;
    return 2;
}

static memory_order select_pointer_order(void)
{
    pointer_order_evaluations++;
    return memory_order_acq_rel;
}

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

    {
        int values[8] = {0};
        _Atomic(int *) ints = values;
        int *old_int = atomic_fetch_add_explicit(
            &ints, 1, memory_order_relaxed);
        assert(old_int == &values[0]);
        assert(atomic_load_explicit(&ints, memory_order_acquire) ==
               &values[1]);
        old_int = atomic_fetch_sub_explicit(&ints, 1, memory_order_release);
        assert(old_int == &values[1]);
        assert(atomic_load(&ints) == &values[0]);

        old_int = atomic_fetch_add_explicit(&ints, 3, memory_order_acq_rel);
        assert(old_int == &values[0]);
        assert(atomic_load(&ints) == &values[3]);
        old_int = atomic_fetch_sub_explicit(&ints, 2, memory_order_seq_cst);
        assert(old_int == &values[3]);
        assert(atomic_load(&ints) == &values[1]);

        unsigned char bytes[4] = {0};
        _Atomic(unsigned char *) chars = bytes;
        unsigned char *old_char = atomic_fetch_add_explicit(
            &chars, 2, memory_order_acquire);
        assert(old_char == &bytes[0]);
        assert(atomic_load(&chars) == &bytes[2]);
        old_char = atomic_fetch_sub_explicit(
            &chars, 1, memory_order_consume);
        assert(old_char == &bytes[2]);
        assert(atomic_load(&chars) == &bytes[1]);

        struct wide_element { uint64_t words[3]; } wide[3] = {{{0}}};
        _Atomic(struct wide_element *) wide_pointer = wide;
        struct wide_element *old_wide = atomic_fetch_add_explicit(
            &wide_pointer, 1, memory_order_relaxed);
        assert(old_wide == &wide[0]);
        assert(atomic_load(&wide_pointer) == &wide[1]);
        old_wide = atomic_fetch_sub(&wide_pointer, 1);
        assert(old_wide == &wide[1]);
        assert(atomic_load(&wide_pointer) == &wide[0]);

        signed char narrow = 0;
        atomic_schar narrow_atomic = ATOMIC_VAR_INIT(3);
        assert(atomic_fetch_add(&narrow_atomic, 2) == 3);
        assert(atomic_fetch_sub_explicit(
                   &narrow_atomic, 1, memory_order_relaxed) == 5);
        narrow = atomic_load(&narrow_atomic);
        assert(narrow == 4);

        selected_pointer = values;
        pointer_object_evaluations = 0;
        pointer_operand_evaluations = 0;
        pointer_order_evaluations = 0;
        old_int = atomic_fetch_add_explicit(
            select_pointer(), select_pointer_operand(),
            select_pointer_order());
        assert(old_int == &values[0]);
        assert(atomic_load(&selected_pointer) == &values[2]);
        assert(pointer_object_evaluations == 1U);
        assert(pointer_operand_evaluations == 1U);
        assert(pointer_order_evaluations == 1U);
        pointer_object_evaluations = 0;
        pointer_operand_evaluations = 0;
        pointer_order_evaluations = 0;
        old_int = atomic_fetch_sub_explicit(
            select_pointer(), select_pointer_operand(),
            select_pointer_order());
        assert(old_int == &values[2]);
        assert(atomic_load(&selected_pointer) == &values[0]);
        assert(pointer_object_evaluations == 1U);
        assert(pointer_operand_evaluations == 1U);
        assert(pointer_order_evaluations == 1U);
    }

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
