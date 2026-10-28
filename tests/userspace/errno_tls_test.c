/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

int *__errno_location(void);

static pthread_barrier_t shared_barrier;
static int *addresses[2];

static void barrier_wait(void)
{
    int result = pthread_barrier_wait(&shared_barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *worker(void *opaque)
{
    intptr_t index = (intptr_t)opaque;
    int *cell = __errno_location();
    addresses[index] = cell;
    *cell = 100 + (int)index;
    barrier_wait();
    /* Keep the workers parked while main verifies their initial cells. */
    barrier_wait();
    for (unsigned i = 0; i < 10000; i++) {
        assert(__errno_location() == cell);
        assert(*cell == 100 + (int)index);
        *cell += 1;
        *cell -= 1;
    }
    barrier_wait();
    assert(*cell == 100 + (int)index);
    return NULL;
}

int main(void)
{
    pthread_t threads[2];
    assert(pthread_barrier_init(&shared_barrier, NULL, 3) == 0);
    assert(pthread_create(&threads[0], NULL, worker, (void *)0) == 0);
    assert(pthread_create(&threads[1], NULL, worker, (void *)1) == 0);
    barrier_wait();
    assert(addresses[0] && addresses[1] && addresses[0] != addresses[1]);
    assert(*addresses[0] == 100 && *addresses[1] == 101);
    barrier_wait();
    barrier_wait();
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    assert(pthread_barrier_destroy(&shared_barrier) == 0);
    puts("Mangrove errno TLS isolation across host threads PASS");
    return 0;
}
