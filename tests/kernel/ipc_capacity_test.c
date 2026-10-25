/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/src/ipc.c"

static int fail_kmalloc_after = -1;

void *kmalloc(usize size)
{
    if (fail_kmalloc_after == 0) {
        fail_kmalloc_after = -1;
        return NULL;
    }
    if (fail_kmalloc_after > 0) fail_kmalloc_after--;
    return malloc(size);
}

void kfree(void *pointer) { free(pointer); }
void spinlock_init(spinlock_t *lock) { lock->value = 0; }
u64 spin_lock_irqsave(spinlock_t *lock) { (void)lock; return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{ (void)lock; (void)flags; }
void spin_lock(spinlock_t *lock) { (void)lock; }
void spin_unlock(spinlock_t *lock) { (void)lock; }
void object_release(kernel_object_t *object) { (void)object; }
bool scheduler_unblock(kernel_thread_t *thread) { (void)thread; return true; }

static void test_endpoint_registry(void)
{
    ipc_endpoint_t endpoints[24] = {0};
    endpoint_list = NULL;
    for (usize index = 0; index < sizeof(endpoints) / sizeof(endpoints[0]);
         index++) {
        (void)snprintf(endpoints[index].name, sizeof(endpoints[index].name),
                       "service%02llu", (unsigned long long)index);
        endpoints[index].live = true;
        endpoints[index].next = endpoint_list;
        endpoint_list = &endpoints[index];
    }
    for (usize index = 0; index < sizeof(endpoints) / sizeof(endpoints[0]);
         index++) {
        char name[MG_IPC_SERVICE_NAME_MAX];
        (void)snprintf(name, sizeof(name), "service%02llu",
                       (unsigned long long)index);
        assert(endpoint_find(name) != NULL);
    }
    assert(endpoint_find("missing") == NULL);
    endpoint_list = NULL;
}

static void test_request_queue_growth(void)
{
    ipc_endpoint_t endpoint = {0};
    ipc_request_t *requests[128] = {0};
    ipc_request_t *failed;

    fail_kmalloc_after = 0;
    failed = request_allocate();
    assert(failed == NULL && fail_kmalloc_after == -1);
    for (usize index = 0; index < 128U; index++) {
        requests[index] = request_allocate();
        assert(requests[index] != NULL);
        requests[index]->id = index + 1U;
        requests[index]->endpoint = &endpoint;
        request_link_locked(requests[index]);
        assert(endpoint_enqueue_request(&endpoint, requests[index]));
    }
    assert(endpoint.active_request_head == requests[0] &&
           endpoint.request_head == requests[0]);
    assert(endpoint.request_tail == requests[127]);

    endpoint_remove_request(&endpoint, requests[7]);
    endpoint_remove_request(&endpoint, requests[63]);
    requests[7]->client_active = false;
    requests[63]->client_active = false;
    request_maybe_free(requests[7]);
    request_maybe_free(requests[63]);
    requests[7] = requests[63] = NULL;

    for (usize expected = 1U; expected <= 128U; expected++) {
        ipc_delivery_t delivery;
        if (expected == 8U || expected == 64U) continue;
        assert(endpoint_dequeue(&endpoint, &delivery));
        assert(!delivery.is_event && delivery.request->id == expected);
        delivery.request->delivery_active = false;
        delivery.request->client_active = false;
        request_maybe_free(delivery.request);
    }
    assert(!endpoint_dequeue(&endpoint, &(ipc_delivery_t){0}));
    assert(!endpoint.active_request_head && !endpoint.active_request_tail);
    request_reclaim_retired();
}

static void test_bounded_interrupt_events_and_order(void)
{
    ipc_endpoint_t endpoint = {0};
    ipc_delivery_t delivery;

    for (usize sequence = 1U; sequence <= 24U; sequence++) {
        if (sequence % 3U == 0) {
            mg_event_t event = {.sequence = sequence};
            assert(endpoint_enqueue_event(&endpoint, &event));
        } else {
            ipc_request_t *request = request_allocate();
            assert(request);
            request->id = sequence;
            request->endpoint = &endpoint;
            request_link_locked(request);
            assert(endpoint_enqueue_request(&endpoint, request));
        }
    }
    assert(endpoint.event_count == IPC_EVENT_QUEUE_CAPACITY);
    {
        mg_event_t overflow = {.sequence = 25U};
        assert(!endpoint_enqueue_event(&endpoint, &overflow));
        endpoint.event_overflow = true;
    }
    for (usize sequence = 1U; sequence <= 24U; sequence++) {
        assert(endpoint_dequeue(&endpoint, &delivery));
        if (sequence % 3U == 0) {
            assert(delivery.is_event &&
                   delivery.event.sequence == sequence);
        } else {
            assert(!delivery.is_event && delivery.request->id == sequence);
            delivery.request->client_active = false;
            if (sequence == 1U) {
                request_maybe_free(delivery.request);
                assert(delivery.request->allocated);
                delivery.request->delivery_active = false;
                request_maybe_free(delivery.request);
            } else {
                delivery.request->delivery_active = false;
                request_maybe_free(delivery.request);
            }
        }
    }
    assert(!endpoint.request_head && endpoint.event_count == 0U);
    assert(endpoint.event_overflow);
    endpoint.event_overflow = false;
    request_reclaim_retired();
}

static void test_process_exit_unlinks_pending_request(void)
{
    ipc_endpoint_t endpoint = {0};
    process_t client = {0};
    process_t owner = {0};
    ipc_request_t *request = request_allocate();
    assert(request);
    endpoint.live = true;
    endpoint.owner = &owner;
    endpoint_list = &endpoint;
    request->id = 1U;
    request->endpoint = &endpoint;
    request->client_process = &client;
    request->client_active = true;
    request_link_locked(request);
    assert(endpoint_enqueue_request(&endpoint, request));
    client.ipc_outstanding = (struct ipc_request *)request;

    ipc_process_exit(&client);

    assert(!client.ipc_outstanding && !endpoint.request_head &&
           !endpoint.request_tail && !endpoint.active_request_head &&
           !endpoint.active_request_tail && !retired_requests);
    endpoint_list = NULL;
}

static void test_sequence_order_across_wrap(void)
{
    ipc_endpoint_t endpoint = {0};
    ipc_request_t *before_wrap = request_allocate();
    ipc_request_t *after_wrap = request_allocate();
    mg_event_t event = {.sequence = ~(u64)0};
    ipc_delivery_t delivery;
    assert(before_wrap && after_wrap);

    before_wrap->id = ~(u64)0 - 1U;
    after_wrap->id = 1U; /* request ID zero is skipped */
    before_wrap->endpoint = after_wrap->endpoint = &endpoint;
    request_link_locked(before_wrap);
    request_link_locked(after_wrap);
    assert(endpoint_enqueue_request(&endpoint, before_wrap));
    assert(endpoint_enqueue_event(&endpoint, &event));
    assert(endpoint_enqueue_request(&endpoint, after_wrap));

    assert(endpoint_dequeue(&endpoint, &delivery) &&
           delivery.request == before_wrap);
    before_wrap->delivery_active = false;
    request_maybe_free(before_wrap);
    assert(endpoint_dequeue(&endpoint, &delivery) && delivery.is_event &&
           delivery.event.sequence == event.sequence);
    assert(endpoint_dequeue(&endpoint, &delivery) &&
           delivery.request == after_wrap);
    after_wrap->delivery_active = false;
    request_maybe_free(after_wrap);
    assert(!endpoint_dequeue(&endpoint, &delivery) &&
           !endpoint.active_request_head && !endpoint.active_request_tail);
    request_reclaim_retired();
}

static void test_endpoint_exit_fails_queued_requests(void)
{
    ipc_endpoint_t endpoint = {0};
    process_t owner = {0};
    process_t client = {0};
    ipc_request_t *request = request_allocate();
    assert(request);
    endpoint.live = true;
    endpoint.owner = &owner;
    endpoint_list = &endpoint;
    request->id = 1U;
    request->endpoint = &endpoint;
    request->client_process = &client;
    request->client_active = true;
    request_link_locked(request);
    assert(endpoint_enqueue_request(&endpoint, request));
    client.ipc_outstanding = (struct ipc_request *)request;

    ipc_process_exit(&owner);

    assert(!endpoint.live && !endpoint_list && !endpoint.active_request_head &&
           !endpoint.request_head && !request->endpoint && request->allocated &&
           request->state == IPC_REQUEST_FAILED && request->client_active);
    request->client_active = false;
    request->client_process = NULL;
    client.ipc_outstanding = NULL;
    request_maybe_free(request);
    request_reclaim_retired();
    assert(!retired_requests);
}

int main(void)
{
    test_endpoint_registry();
    test_request_queue_growth();
    test_bounded_interrupt_events_and_order();
    test_process_exit_unlinks_pending_request();
    test_sequence_order_across_wrap();
    test_endpoint_exit_fails_queued_requests();
    puts("IPC dynamic endpoint/request queues and bounded event overflow passed");
    return 0;
}
