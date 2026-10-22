/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <assert.h>
#include <errno.h>
#include <pthread.h>

#ifdef __APPLE__
/* Darwin lacks pthread_barrier. A reusable generation barrier still runs
 * actual host threads; it does not substitute sequential fixture execution.
 * Callers join all participants before destroy; cancellation is not used. */
#define HOST_BARRIER_SERIAL_THREAD (-1)
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned count, waiting, generation;
} host_barrier_t;
static inline int host_barrier_init(host_barrier_t *b, const void *attr,
                                     unsigned count)
{
    if (attr || !count) return EINVAL;
    int result = pthread_mutex_init(&b->mutex, NULL);
    if (result) return result;
    result = pthread_cond_init(&b->condition, NULL);
    if (result) { pthread_mutex_destroy(&b->mutex); return result; }
    b->count = count; b->waiting = 0; b->generation = 0;
    return 0;
}
static inline int host_barrier_wait(host_barrier_t *b)
{
    assert(!pthread_mutex_lock(&b->mutex));
    unsigned generation = b->generation;
    int result = 0;
    if (++b->waiting == b->count) {
        b->waiting = 0; b->generation++;
        assert(!pthread_cond_broadcast(&b->condition));
        result = HOST_BARRIER_SERIAL_THREAD;
    } else {
        while (generation == b->generation)
            assert(!pthread_cond_wait(&b->condition, &b->mutex));
    }
    assert(!pthread_mutex_unlock(&b->mutex));
    return result;
}
static inline int host_barrier_destroy(host_barrier_t *b)
{
    int result = pthread_cond_destroy(&b->condition);
    if (result) return result;
    return pthread_mutex_destroy(&b->mutex);
}
#else
/* Retain the native Linux primitive and its existing behavior. */
#define host_barrier_t pthread_barrier_t
#define host_barrier_init pthread_barrier_init
#define host_barrier_wait pthread_barrier_wait
#define host_barrier_destroy pthread_barrier_destroy
#define HOST_BARRIER_SERIAL_THREAD PTHREAD_BARRIER_SERIAL_THREAD
#endif
