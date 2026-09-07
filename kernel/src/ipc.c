/* SPDX-License-Identifier: GPL-3.0-only */
#include <ipc.h>

#include <heap.h>
#include <mangrove_errors.h>
#include <object.h>
#include <scheduler.h>
#include <service.h>
#include <string.h>
#include <timer.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef enum {
    IPC_REQUEST_FREE = 0,
    IPC_REQUEST_PENDING,
    IPC_REQUEST_DELIVERED,
    IPC_REQUEST_REPLIED,
    IPC_REQUEST_FAILED,
} ipc_request_state_t;

typedef struct ipc_request ipc_request_t;
typedef struct ipc_endpoint ipc_endpoint_t;

struct ipc_request {
    bool allocated;
    u64 id;
    ipc_request_state_t state;
    ipc_endpoint_t *endpoint;
    process_t *client_process;
    kernel_thread_t *client_thread;
    bool client_active;
    bool client_blocked;
    bool context_live;
    bool authorization_claimed;
    u64 requester_pid;
    mg_session_id_t requester_session_id;
    u32 requester_service_id;
    bool requester_system_service;
    u16 request_type;
    u32 request_length;
    u8 request_payload[MG_IPC_MESSAGE_PAYLOAD_MAX];
    u16 reply_type;
    u32 reply_length;
    u8 reply_payload[MG_IPC_MESSAGE_PAYLOAD_MAX];
    process_credentials_t requester_credentials;
    char requester_name[sizeof(((mg_ipc_requester_t *)0)->process_name)];
};

typedef struct {
    bool is_event;
    ipc_request_t *request;
    mg_event_t event;
} ipc_delivery_t;

struct ipc_endpoint {
    kernel_object_t object;
    char name[MG_IPC_SERVICE_NAME_MAX];
    process_t *owner;
    u32 service_id;
    bool live;
    ipc_delivery_t queue[IPC_MAX_QUEUE_DEPTH];
    u32 queue_head;
    u32 queue_tail;
    u32 queue_count;
    u32 event_classes;
    bool event_overflow;
    kernel_thread_t *receiver_waiter;
    ipc_endpoint_t *next;
};

typedef struct {
    kernel_object_t object;
    ipc_request_t *request;
    process_t *owner;
} ipc_request_object_t;

static ipc_endpoint_t *endpoint_list;
static ipc_request_t request_pool[IPC_MAX_REQUESTS];
static u32 endpoint_count;
static u64 next_request_id;

/* Event publishers may run from hardware interrupt context.  Keep endpoint
 * queue/list transitions atomic with those publishers, and keep the waiter
 * registration adjacent to the scheduler block so an event cannot arrive in
 * the small check-to-sleep window. */
static u64 ipc_irq_save(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void ipc_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static ipc_endpoint_t *endpoint_from_object(kernel_object_t *object)
{
    return (ipc_endpoint_t *)((u8 *)object -
                              __builtin_offsetof(ipc_endpoint_t, object));
}

static ipc_request_object_t *request_object_from_object(kernel_object_t *object)
{
    return (ipc_request_object_t *)((u8 *)object -
        __builtin_offsetof(ipc_request_object_t, object));
}

static bool ipc_name_valid(const char *name)
{
    usize length = 0;

    if (!name) return false;
    while (length < MG_IPC_SERVICE_NAME_MAX && name[length] != '\0') {
        char value = name[length++];
        if (length == 1U) {
            if (value < 'a' || value > 'z') return false;
        } else if (!((value >= 'a' && value <= 'z') ||
                     (value >= '0' && value <= '9') ||
                     value == '_' || value == '-')) {
            return false;
        }
    }
    return length != 0 && length < MG_IPC_SERVICE_NAME_MAX;
}

static int ipc_message_validate(const mg_ipc_message_t *message)
{
    if (!message || message->type == 0 ||
        message->payload_length > MG_IPC_MESSAGE_PAYLOAD_MAX)
        return MG_ERR_BAD_ARGUMENT;
    if (message->version != MG_IPC_PROTOCOL_VERSION)
        return MG_ERR_PROTOCOL;
    return MG_OK;
}

static ipc_request_t *request_allocate(void)
{
    for (u32 index = 0; index < IPC_MAX_REQUESTS; index++) {
        if (!request_pool[index].allocated) {
            memset(&request_pool[index], 0, sizeof(request_pool[index]));
            request_pool[index].allocated = true;
            request_pool[index].state = IPC_REQUEST_PENDING;
            return &request_pool[index];
        }
    }
    return NULL;
}

static void request_free(ipc_request_t *request)
{
    if (!request || !request->allocated) return;
    memset(request, 0, sizeof(*request));
}

static void request_maybe_free(ipc_request_t *request)
{
    if (!request || !request->allocated || request->client_active ||
        request->context_live) return;
    request_free(request);
}

static void request_wake_client(ipc_request_t *request)
{
    kernel_thread_t *thread;

    if (!request || !request->client_active || !request->client_blocked ||
        !request->client_thread)
        return;
    thread = request->client_thread;
    request->client_blocked = false;
    request->client_thread = NULL;
    if (thread->state == THREAD_STATE_BLOCKED &&
        !scheduler_unblock(thread)) {
        /* Keep the waiter associated with the request if the scheduler could
         * not enqueue it.  This is defensive; a normal wakeup succeeds. */
        request->client_blocked = true;
        request->client_thread = thread;
    }
}

static void request_fail(ipc_request_t *request)
{
    if (!request || !request->allocated ||
        request->state == IPC_REQUEST_REPLIED ||
        request->state == IPC_REQUEST_FAILED) return;
    request->state = IPC_REQUEST_FAILED;
    request_wake_client(request);
    request_maybe_free(request);
}

static void request_context_destroy(kernel_object_t *object)
{
    ipc_request_object_t *context = request_object_from_object(object);
    ipc_request_t *request;

    if (!context) return;
    request = context->request;
    if (request && request->allocated) {
        request->context_live = false;
        if (request->state == IPC_REQUEST_PENDING ||
            request->state == IPC_REQUEST_DELIVERED)
            request_fail(request);
        request_maybe_free(request);
    }
    kfree(context);
}

static void endpoint_destroy(kernel_object_t *object)
{
    ipc_endpoint_t *endpoint = endpoint_from_object(object);

    if (!endpoint) return;
    if (endpoint_count) endpoint_count--;
    kfree(endpoint);
}

static bool endpoint_allowed_for_process(process_t *process,
                                         const char *name,
                                         u32 *service_id)
{
    const kernel_service_definition_t *definition;

    if (!process || !process->system_service ||
        !service_definition_lookup(process->service_id, &definition) ||
        !definition->endpoint_name ||
        strcmp(definition->endpoint_name, name) != 0) return false;
    if (service_id) *service_id = process->service_id;
    return true;
}

static ipc_endpoint_t *endpoint_find(const char *name)
{
    for (ipc_endpoint_t *endpoint = endpoint_list; endpoint;
         endpoint = endpoint->next) {
        if (endpoint->live && strcmp(endpoint->name, name) == 0)
            return endpoint;
    }
    return NULL;
}

static bool endpoint_enqueue_request(ipc_endpoint_t *endpoint,
                                     ipc_request_t *request)
{
    if (!endpoint || !request || endpoint->queue_count >= IPC_MAX_QUEUE_DEPTH)
        return false;
    endpoint->queue[endpoint->queue_tail].is_event = false;
    endpoint->queue[endpoint->queue_tail].request = request;
    memset(&endpoint->queue[endpoint->queue_tail].event, 0,
           sizeof(endpoint->queue[endpoint->queue_tail].event));
    endpoint->queue_tail = (endpoint->queue_tail + 1U) % IPC_MAX_QUEUE_DEPTH;
    endpoint->queue_count++;
    return true;
}

static bool endpoint_enqueue_event(ipc_endpoint_t *endpoint,
                                   const mg_event_t *event)
{
    if (!endpoint || !event || endpoint->queue_count >= IPC_MAX_QUEUE_DEPTH)
        return false;
    endpoint->queue[endpoint->queue_tail].is_event = true;
    endpoint->queue[endpoint->queue_tail].request = NULL;
    endpoint->queue[endpoint->queue_tail].event = *event;
    endpoint->queue_tail = (endpoint->queue_tail + 1U) % IPC_MAX_QUEUE_DEPTH;
    endpoint->queue_count++;
    return true;
}

static bool endpoint_dequeue(ipc_endpoint_t *endpoint,
                             ipc_delivery_t *out_delivery)
{
    if (!endpoint || !out_delivery || endpoint->queue_count == 0)
        return false;
    *out_delivery = endpoint->queue[endpoint->queue_head];
    memset(&endpoint->queue[endpoint->queue_head], 0,
           sizeof(endpoint->queue[endpoint->queue_head]));
    endpoint->queue_head = (endpoint->queue_head + 1U) % IPC_MAX_QUEUE_DEPTH;
    endpoint->queue_count--;
    return true;
}

static void endpoint_remove_request(ipc_endpoint_t *endpoint,
                                    ipc_request_t *request)
{
    u32 offset;

    if (!endpoint || !request || endpoint->queue_count == 0) return;
    for (offset = 0; offset < endpoint->queue_count; offset++) {
        u32 index = (endpoint->queue_head + offset) % IPC_MAX_QUEUE_DEPTH;
        if (endpoint->queue[index].is_event ||
            endpoint->queue[index].request != request) continue;
        while (offset + 1U < endpoint->queue_count) {
            u32 next = (index + 1U) % IPC_MAX_QUEUE_DEPTH;
            endpoint->queue[index] = endpoint->queue[next];
            index = next;
            offset++;
        }
        memset(&endpoint->queue[index], 0, sizeof(endpoint->queue[index]));
        endpoint->queue_tail = (endpoint->queue_tail + IPC_MAX_QUEUE_DEPTH - 1U) %
                               IPC_MAX_QUEUE_DEPTH;
        endpoint->queue_count--;
        return;
    }
}

static void endpoint_unregister(ipc_endpoint_t *endpoint)
{
    ipc_endpoint_t **cursor;
    u64 saved_flags;

    if (!endpoint) return;
    saved_flags = ipc_irq_save();
    if (!endpoint->live) {
        ipc_irq_restore(saved_flags);
        return;
    }
    endpoint->live = false;
    if (endpoint->receiver_waiter) endpoint->receiver_waiter = NULL;
    cursor = &endpoint_list;
    while (*cursor && *cursor != endpoint) cursor = &(*cursor)->next;
    if (*cursor == endpoint) *cursor = endpoint->next;
    endpoint->next = NULL;

    for (u32 index = 0; index < IPC_MAX_REQUESTS; index++) {
        ipc_request_t *request = &request_pool[index];
        if (request->allocated && request->endpoint == endpoint) {
            request->endpoint = NULL;
            request_fail(request);
        }
    }
    /* The initial object reference is the registry's reference.  Client
     * handles keep a dead endpoint object alive only as a stale handle. */
    object_release(&endpoint->object);
    ipc_irq_restore(saved_flags);
}

static void request_fill_received(const ipc_request_t *request,
                                  mg_ipc_received_t *received,
                                  mg_handle_t context_handle)
{
    memset(received, 0, sizeof(*received));
    received->message.version = MG_IPC_PROTOCOL_VERSION;
    received->message.type = request->request_type;
    received->message.payload_length = request->request_length;
    received->message.request_id = request->id;
    memcpy(received->message.payload, request->request_payload,
           request->request_length);
    received->requester.pid = request->requester_pid;
    received->requester.uid = request->requester_credentials.uid;
    received->requester.role = request->requester_credentials.role;
    received->requester.session_id = request->requester_session_id;
    received->requester.service_id = request->requester_service_id;
    received->requester.service_privileges =
        request->requester_credentials.service_privileges;
    received->requester.system_service = request->requester_system_service;
    memcpy(received->requester.process_name, request->requester_name,
           sizeof(received->requester.process_name));
    received->request = context_handle;
    received->delivery_kind = MG_IPC_DELIVERY_REQUEST;
}

bool ipc_init(void)
{
    endpoint_list = NULL;
    endpoint_count = 0;
    next_request_id = 1;
    memset(request_pool, 0, sizeof(request_pool));
    return true;
}

int ipc_service_register(process_t *process, const char *name,
                         process_handle_t *out_handle)
{
    ipc_endpoint_t *endpoint;
    process_handle_t handle;
    u32 service_id;
    u64 saved_flags;

    if (!process || !out_handle || !ipc_name_valid(name))
        return MG_ERR_BAD_ARGUMENT;
    if (!endpoint_allowed_for_process(process, name, &service_id))
        return MG_ERR_PRIVILEGE_REQUIRED;
    endpoint = (ipc_endpoint_t *)kmalloc(sizeof(*endpoint));
    if (!endpoint) return MG_ERR_NO_MEMORY;
    memset(endpoint, 0, sizeof(*endpoint));
    object_init(&endpoint->object, OBJECT_TYPE_IPC_ENDPOINT,
                endpoint_destroy);
    strncpy(endpoint->name, name, sizeof(endpoint->name) - 1U);
    endpoint->name[sizeof(endpoint->name) - 1U] = '\0';
    endpoint->owner = process;
    endpoint->service_id = service_id;
    endpoint->live = true;
    saved_flags = ipc_irq_save();
    if (endpoint_find(name)) {
        ipc_irq_restore(saved_flags);
        kfree(endpoint);
        return MG_ERR_ALREADY_EXISTS;
    }
    if (endpoint_count >= IPC_MAX_ENDPOINTS) {
        ipc_irq_restore(saved_flags);
        kfree(endpoint);
        return MG_ERR_NO_MEMORY;
    }
    endpoint->next = endpoint_list;
    endpoint_list = endpoint;
    endpoint_count++;
    ipc_irq_restore(saved_flags);

    if (!process_handle_install(process, &endpoint->object,
                                OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE,
                                &handle)) {
        endpoint_unregister(endpoint);
        return MG_ERR_NO_MEMORY;
    }
    *out_handle = handle;
    return MG_OK;
}

int ipc_service_lookup(process_t *process, const char *name,
                       process_handle_t *out_handle)
{
    ipc_endpoint_t *endpoint;
    process_handle_t handle;
    u64 saved_flags;

    if (!process || !out_handle || !ipc_name_valid(name))
        return MG_ERR_BAD_ARGUMENT;
    saved_flags = ipc_irq_save();
    endpoint = endpoint_find(name);
    if (!endpoint) {
        ipc_irq_restore(saved_flags);
        return MG_ERR_SERVICE_UNAVAILABLE;
    }
    if (!process_handle_install(process, &endpoint->object,
                                OBJECT_RIGHT_WRITE, &handle)) {
        ipc_irq_restore(saved_flags);
        return MG_ERR_NO_MEMORY;
    }
    ipc_irq_restore(saved_flags);
    *out_handle = handle;
    return MG_OK;
}

int ipc_kernel_request(process_t *process, process_handle_t endpoint_handle,
                       const mg_ipc_message_t *message,
                       mg_ipc_message_t *reply)
{
    kernel_object_t *object;
    ipc_endpoint_t *endpoint;
    ipc_request_t *request;
    process_credentials_t credentials;
    kernel_thread_t *waiter;
    u64 saved_flags;
    int result = MG_OK;

    if (!process || !message || !reply) return MG_ERR_BAD_ARGUMENT;
    result = ipc_message_validate(message);
    if (result != MG_OK) return result;
    if (process->ipc_outstanding) return MG_ERR_BUSY;
    object = process_handle_lookup(process, endpoint_handle,
                                   OBJECT_TYPE_IPC_ENDPOINT,
                                   OBJECT_RIGHT_WRITE);
    if (!object) return MG_ERR_INVALID_HANDLE;
    endpoint = endpoint_from_object(object);
    if (!endpoint->live) return MG_ERR_SERVICE_UNAVAILABLE;
    if (!process_get_credentials(process, &credentials))
        return MG_ERR_ACCESS_DENIED;
    request = request_allocate();
    if (!request) return MG_ERR_NO_MEMORY;
    request->endpoint = endpoint;
    request->client_process = process;
    request->client_thread = thread_current();
    request->client_active = true;
    request->requester_pid = process->pid;
    request->requester_session_id = process->session_id;
    request->requester_service_id = process->service_id;
    request->requester_system_service = process->system_service;
    request->request_type = message->type;
    request->request_length = message->payload_length;
    request->requester_credentials = credentials;
    strncpy(request->requester_name, process->name,
            sizeof(request->requester_name) - 1U);
    request->requester_name[sizeof(request->requester_name) - 1U] = '\0';
    memcpy(request->request_payload, message->payload,
           message->payload_length);
    process->ipc_outstanding = request;

    saved_flags = ipc_irq_save();
    if (!endpoint->live) {
        ipc_irq_restore(saved_flags);
        process->ipc_outstanding = NULL;
        request->client_active = false;
        request->client_process = NULL;
        request->client_thread = NULL;
        request_free(request);
        return MG_ERR_SERVICE_UNAVAILABLE;
    }
    if (endpoint->queue_count >= IPC_MAX_QUEUE_DEPTH) {
        ipc_irq_restore(saved_flags);
        process->ipc_outstanding = NULL;
        request->client_active = false;
        request->client_process = NULL;
        request->client_thread = NULL;
        request_free(request);
        return MG_ERR_QUEUE_FULL;
    }
    request->id = next_request_id++;
    if (request->id == 0) request->id = next_request_id++;
    if (!endpoint_enqueue_request(endpoint, request)) {
        ipc_irq_restore(saved_flags);
        process->ipc_outstanding = NULL;
        request->client_active = false;
        request->client_process = NULL;
        request->client_thread = NULL;
        request_free(request);
        return MG_ERR_QUEUE_FULL;
    }
    waiter = endpoint->receiver_waiter;
    endpoint->receiver_waiter = NULL;
    if (waiter && !scheduler_unblock(waiter)) {
        endpoint->receiver_waiter = waiter;
    }
    ipc_irq_restore(saved_flags);

    while (request->state == IPC_REQUEST_PENDING ||
           request->state == IPC_REQUEST_DELIVERED) {
        request->client_blocked = true;
        if (!scheduler_block()) {
            request->client_blocked = false;
            request_fail(request);
            result = MG_ERR_BUSY;
            break;
        }
        request->client_blocked = false;
    }
    if (result == MG_OK) {
        if (request->state == IPC_REQUEST_REPLIED) {
            memset(reply, 0, sizeof(*reply));
            reply->version = MG_IPC_PROTOCOL_VERSION;
            reply->type = request->reply_type;
            reply->payload_length = request->reply_length;
            reply->request_id = request->id;
            memcpy(reply->payload, request->reply_payload,
                   request->reply_length);
        } else {
            result = MG_ERR_SERVICE_UNAVAILABLE;
        }
    }
    if (process->ipc_outstanding == request) process->ipc_outstanding = NULL;
    request->client_active = false;
    request->client_blocked = false;
    request->client_process = NULL;
    request->client_thread = NULL;
    request_maybe_free(request);
    return result;
}

static int ipc_kernel_receive_internal(process_t *process,
                                       process_handle_t endpoint_handle,
                                       mg_ipc_received_t *received,
                                       bool wait, bool timed,
                                       u32 timeout_ms)
{
    kernel_object_t *object;
    ipc_endpoint_t *endpoint;
    ipc_request_t *request;
    ipc_request_object_t *context;
    process_handle_t context_handle;
    kernel_thread_t *self;
    ipc_delivery_t delivery = {0};
    bool overflow = false;
    u64 deadline = 0;

    if (!process || !received) return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup(process, endpoint_handle,
                                   OBJECT_TYPE_IPC_ENDPOINT,
                                   OBJECT_RIGHT_READ);
    if (!object) return MG_ERR_INVALID_HANDLE;
    endpoint = endpoint_from_object(object);
    if (!endpoint->live) return MG_ERR_SERVICE_UNAVAILABLE;
    if (endpoint->owner != process) return MG_ERR_ACCESS_DENIED;
    self = thread_current();
    if (!self) return MG_ERR_BUSY;
    if (timed) {
        if (timeout_ms == 0) return MG_ERR_TIMEOUT;
        deadline = timer_uptime_ms();
        deadline = deadline > (~(u64)0 - (u64)timeout_ms) ?
            ~(u64)0 : deadline + (u64)timeout_ms;
    }

    for (;;) {
        u64 saved_flags = ipc_irq_save();

        if (!endpoint->live) {
            ipc_irq_restore(saved_flags);
            return MG_ERR_SERVICE_UNAVAILABLE;
        }
        if (endpoint->event_overflow && endpoint->queue_count == 0) {
            endpoint->event_overflow = false;
            overflow = true;
            ipc_irq_restore(saved_flags);
            break;
        }
        if (endpoint->queue_count != 0) {
            bool dequeued = endpoint_dequeue(endpoint, &delivery);
            ipc_irq_restore(saved_flags);
            if (!dequeued) return MG_ERR_SERVICE_UNAVAILABLE;
            break;
        }
        if (!wait) {
            ipc_irq_restore(saved_flags);
            return MG_ERR_WOULD_BLOCK;
        }
        if (endpoint->receiver_waiter && endpoint->receiver_waiter != self) {
            ipc_irq_restore(saved_flags);
            return MG_ERR_BUSY;
        }
        endpoint->receiver_waiter = self;
        if (timed) {
            u64 now = timer_uptime_ms();
            u64 remaining = now >= deadline ? 0 : deadline - now;
            if (remaining == 0) {
                if (endpoint->receiver_waiter == self)
                    endpoint->receiver_waiter = NULL;
                ipc_irq_restore(saved_flags);
                return MG_ERR_TIMEOUT;
            }
            if (!scheduler_sleep(remaining)) {
                if (endpoint->receiver_waiter == self)
                    endpoint->receiver_waiter = NULL;
                ipc_irq_restore(saved_flags);
                return MG_ERR_BUSY;
            }
        } else if (!scheduler_block()) {
            if (endpoint->receiver_waiter == self)
                endpoint->receiver_waiter = NULL;
            ipc_irq_restore(saved_flags);
            return MG_ERR_BUSY;
        }
        ipc_irq_restore(saved_flags);
    }

    if (overflow) {
        mg_event_t event = {0};
        u64 saved_flags = ipc_irq_save();
        event.version = MG_EVENT_PROTOCOL_VERSION;
        event.type = MG_EVENT_QUEUE_OVERFLOW;
        event.flags = MG_EVENT_FLAG_OVERFLOW;
        event.event_class = endpoint->event_classes;
        event.sequence = next_request_id++;
        ipc_irq_restore(saved_flags);
        memset(received, 0, sizeof(*received));
        received->message.version = MG_IPC_PROTOCOL_VERSION;
        received->message.type = MG_IPC_EVENT_MESSAGE;
        received->message.payload_length = sizeof(event);
        received->message.request_id = event.sequence;
        memcpy(received->message.payload, &event, sizeof(event));
        received->delivery_kind = MG_IPC_DELIVERY_EVENT_OVERFLOW;
        return MG_OK;
    }

    if (delivery.is_event) {
        memset(received, 0, sizeof(*received));
        received->message.version = MG_IPC_PROTOCOL_VERSION;
        received->message.type = MG_IPC_EVENT_MESSAGE;
        received->message.payload_length = sizeof(delivery.event);
        received->message.request_id = delivery.event.sequence;
        memcpy(received->message.payload, &delivery.event,
               sizeof(delivery.event));
        received->delivery_kind = MG_IPC_DELIVERY_EVENT;
        return MG_OK;
    }
    request = delivery.request;
    if (!request || request->state != IPC_REQUEST_PENDING)
        return MG_ERR_SERVICE_UNAVAILABLE;
    request->state = IPC_REQUEST_DELIVERED;
    context = (ipc_request_object_t *)kmalloc(sizeof(*context));
    if (!context) {
        request_fail(request);
        return MG_ERR_NO_MEMORY;
    }
    memset(context, 0, sizeof(*context));
    object_init(&context->object, OBJECT_TYPE_IPC_REQUEST,
                request_context_destroy);
    context->request = request;
    context->owner = process;
    request->context_live = true;
    if (!process_handle_install(process, &context->object,
                                OBJECT_RIGHT_WRITE, &context_handle)) {
        request->context_live = false;
        object_release(&context->object);
        request_fail(request);
        return MG_ERR_NO_MEMORY;
    }
    object_release(&context->object);
    request_fill_received(request, received, context_handle);
    return MG_OK;
}

int ipc_kernel_receive(process_t *process, process_handle_t endpoint_handle,
                       mg_ipc_received_t *received)
{
    return ipc_kernel_receive_internal(process, endpoint_handle, received,
                                       true, false, 0);
}

int ipc_kernel_receive_timed(process_t *process, process_handle_t endpoint_handle,
                             mg_ipc_received_t *received, u32 timeout_ms)
{
    return ipc_kernel_receive_internal(process, endpoint_handle, received,
                                       true, true, timeout_ms);
}

int ipc_kernel_try_receive(process_t *process, process_handle_t endpoint_handle,
                           mg_ipc_received_t *received)
{
    return ipc_kernel_receive_internal(process, endpoint_handle, received,
                                       false, false, 0);
}

int ipc_kernel_reply(process_t *process, process_handle_t request_handle,
                     const mg_ipc_message_t *message)
{
    kernel_object_t *object;
    ipc_request_object_t *context;
    ipc_request_t *request;
    int validation_result;

    if (!process || !message) return MG_ERR_BAD_ARGUMENT;
    validation_result = ipc_message_validate(message);
    if (validation_result != MG_OK) return validation_result;
    object = process_handle_lookup(process, request_handle,
                                   OBJECT_TYPE_IPC_REQUEST,
                                   OBJECT_RIGHT_WRITE);
    if (!object) return MG_ERR_INVALID_HANDLE;
    context = request_object_from_object(object);
    if (!context || context->owner != process || !context->request)
        return MG_ERR_ACCESS_DENIED;
    request = context->request;
    if (!request->allocated || !request->context_live ||
        request->state != IPC_REQUEST_DELIVERED)
        return MG_ERR_SERVICE_UNAVAILABLE;
    request->reply_type = message->type;
    request->reply_length = message->payload_length;
    memcpy(request->reply_payload, message->payload,
           message->payload_length);
    request->state = IPC_REQUEST_REPLIED;
    request_wake_client(request);
    return MG_OK;
}

bool ipc_request_context_claim(process_t *service,
                               process_handle_t request_handle,
                               process_credentials_t *credentials,
                               char *requester_name,
                               usize requester_name_size)
{
    kernel_object_t *object;
    ipc_request_object_t *context;
    ipc_request_t *request;

    if (!service || service != process_current() || !credentials ||
        !requester_name ||
        requester_name_size == 0 || !service->system_service)
        return false;
    object = process_handle_lookup(service, request_handle,
                                   OBJECT_TYPE_IPC_REQUEST,
                                   OBJECT_RIGHT_WRITE);
    if (!object) return false;
    context = request_object_from_object(object);
    if (!context || context->owner != service || !context->request)
        return false;
    request = context->request;
    if (!request->allocated || !request->context_live ||
        request->state != IPC_REQUEST_DELIVERED ||
        request->authorization_claimed ||
        strlen(request->requester_name) >= requester_name_size ||
        !identity_credentials_valid(&request->requester_credentials))
        return false;
    *credentials = request->requester_credentials;
    strcpy(requester_name, request->requester_name);
    request->authorization_claimed = true;
    return true;
}

bool ipc_request_context_origin(process_t *service,
                                process_handle_t request_handle,
                                u32 expected_service_id,
                                u64 *origin_pid)
{
    kernel_object_t *object;
    ipc_request_object_t *context;
    ipc_request_t *request;

    if (!service || service != process_current() || !service->system_service ||
        !origin_pid || !expected_service_id)
        return false;
    object = process_handle_lookup(service, request_handle,
                                   OBJECT_TYPE_IPC_REQUEST,
                                   OBJECT_RIGHT_WRITE);
    if (!object) return false;
    context = request_object_from_object(object);
    if (!context || context->owner != service || !context->request)
        return false;
    request = context->request;
    if (!request->allocated || !request->context_live ||
        request->state != IPC_REQUEST_DELIVERED ||
        !request->requester_system_service ||
        request->requester_service_id != expected_service_id)
        return false;
    *origin_pid = request->requester_pid;
    return true;
}

static bool event_subscription_allowed(const ipc_endpoint_t *endpoint,
                                       u32 event_classes)
{
    u32 allowed = 0;

    if (!endpoint || !endpoint->owner || !endpoint->owner->system_service)
        return false;
    switch (endpoint->service_id) {
        case MG_SERVICE_DEVICED:
            if (!identity_credentials_has_privilege(
                    &endpoint->owner->credentials,
                    IDENTITY_PRIVILEGE_MANAGE_DEVICES))
                return false;
            allowed = MG_EVENT_CLASS_DEVICE | MG_EVENT_CLASS_BLOCK |
                      MG_EVENT_CLASS_USB | MG_EVENT_CLASS_NETWORK;
            break;
        case MG_SERVICE_VOLUMED:
            if (!identity_credentials_has_privilege(
                    &endpoint->owner->credentials,
                    IDENTITY_PRIVILEGE_MANAGE_DEVICES))
                return false;
            allowed = MG_EVENT_CLASS_DEVICE | MG_EVENT_CLASS_BLOCK |
                      MG_EVENT_CLASS_USB;
            break;
        case MG_SERVICE_NETWORKD:
            if (!identity_credentials_has_privilege(
                    &endpoint->owner->credentials,
                    IDENTITY_PRIVILEGE_MANAGE_NETWORK))
                return false;
            allowed = MG_EVENT_CLASS_NETWORK;
            break;
        default:
            return false;
    }
    return event_classes != 0 && (event_classes & ~allowed) == 0;
}

static void endpoint_discard_events(ipc_endpoint_t *endpoint)
{
    ipc_delivery_t retained[IPC_MAX_QUEUE_DEPTH];
    u32 retained_count = 0;

    if (!endpoint) return;
    for (u32 offset = 0; offset < endpoint->queue_count; offset++) {
        u32 index = (endpoint->queue_head + offset) % IPC_MAX_QUEUE_DEPTH;
        if (endpoint->queue[index].is_event) continue;
        if (retained_count < IPC_MAX_QUEUE_DEPTH)
            retained[retained_count++] = endpoint->queue[index];
    }
    memset(endpoint->queue, 0, sizeof(endpoint->queue));
    for (u32 index = 0; index < retained_count; index++)
        endpoint->queue[index] = retained[index];
    endpoint->queue_head = 0;
    endpoint->queue_tail = retained_count % IPC_MAX_QUEUE_DEPTH;
    endpoint->queue_count = retained_count;
    endpoint->event_overflow = false;
}

int ipc_kernel_event_subscribe(process_t *process,
                               process_handle_t endpoint_handle,
                               u32 event_classes)
{
    kernel_object_t *object;
    ipc_endpoint_t *endpoint;
    u64 saved_flags;

    if (!process || !event_classes) return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup(process, endpoint_handle,
                                   OBJECT_TYPE_IPC_ENDPOINT,
                                   OBJECT_RIGHT_READ);
    if (!object) return MG_ERR_INVALID_HANDLE;
    endpoint = endpoint_from_object(object);
    if (!endpoint->live || endpoint->owner != process ||
        !event_subscription_allowed(endpoint, event_classes))
        return MG_ERR_PRIVILEGE_REQUIRED;
    saved_flags = ipc_irq_save();
    endpoint_discard_events(endpoint);
    endpoint->event_classes = event_classes;
    ipc_irq_restore(saved_flags);
    return MG_OK;
}

int ipc_kernel_event_unsubscribe(process_t *process,
                                 process_handle_t endpoint_handle)
{
    kernel_object_t *object;
    ipc_endpoint_t *endpoint;
    u64 saved_flags;

    if (!process) return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup(process, endpoint_handle,
                                   OBJECT_TYPE_IPC_ENDPOINT,
                                   OBJECT_RIGHT_READ);
    if (!object) return MG_ERR_INVALID_HANDLE;
    endpoint = endpoint_from_object(object);
    if (!endpoint->live || endpoint->owner != process)
        return MG_ERR_ACCESS_DENIED;
    saved_flags = ipc_irq_save();
    endpoint_discard_events(endpoint);
    endpoint->event_classes = 0;
    ipc_irq_restore(saved_flags);
    return MG_OK;
}

void ipc_publish_event(u32 event_class, u16 type, u64 resource_id,
                       const char *name)
{
    mg_event_t event = {0};
    u64 saved_flags;

    if (!event_class || !type) return;
    event.version = MG_EVENT_PROTOCOL_VERSION;
    event.type = type;
    event.event_class = event_class;
    event.resource_id = resource_id;
    if (name)
        strncpy(event.name, name, sizeof(event.name) - 1U);

    saved_flags = ipc_irq_save();
    event.sequence = next_request_id++;
    if (event.sequence == 0) event.sequence = next_request_id++;
    for (ipc_endpoint_t *endpoint = endpoint_list; endpoint;
         endpoint = endpoint->next) {
        kernel_thread_t *waiter;
        if (!endpoint->live || !(endpoint->event_classes & event_class))
            continue;
        if (!endpoint_enqueue_event(endpoint, &event)) {
            /* State snapshots are authoritative.  A subsequent overflow
             * marker tells the service to rebuild its view instead of
             * allowing an unbounded queue or silently corrupting it. */
            endpoint->event_overflow = true;
            continue;
        }
        waiter = endpoint->receiver_waiter;
        endpoint->receiver_waiter = NULL;
        if (waiter && !scheduler_unblock(waiter))
            endpoint->receiver_waiter = waiter;
    }
    ipc_irq_restore(saved_flags);
}

void ipc_process_exit(process_t *process)
{
    ipc_endpoint_t *endpoint;

    if (!process) return;
    endpoint = endpoint_list;
    while (endpoint) {
        ipc_endpoint_t *next = endpoint->next;
        if (endpoint->owner == process) endpoint_unregister(endpoint);
        endpoint = next;
    }
    for (u32 index = 0; index < IPC_MAX_REQUESTS; index++) {
        ipc_request_t *request = &request_pool[index];
        if (request->allocated && request->client_process == process) {
            if (request->state == IPC_REQUEST_PENDING && request->endpoint) {
                endpoint_remove_request(request->endpoint, request);
                request->endpoint = NULL;
            }
            request->client_active = false;
            request->client_blocked = false;
            request->client_thread = NULL;
            request->client_process = NULL;
            if (process->ipc_outstanding == request)
                process->ipc_outstanding = NULL;
            request_fail(request);
            request_maybe_free(request);
        }
    }
    process->ipc_outstanding = NULL;
}
