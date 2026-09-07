/* SPDX-License-Identifier: GPL-3.0-only */
#include <mangrove.h>
#include <mg/device_service.h>
#include <mg/error.h>
#include <mg/ipc.h>
#include <mg/log_service.h>
#include <stdio.h>
#include <string.h>

static bool category_valid(u32 category)
{
    return category >= MG_DEVICE_CATEGORY_PCI &&
           category <= MG_DEVICE_CATEGORY_INPUT;
}

static bool send_response(const mg_ipc_received_t *received,
                          const mg_device_response_t *response)
{
    mg_ipc_message_t message = {0};

    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_DEVICE_RESPONSE;
    message.payload_length = sizeof(*response);
    memcpy(message.payload, response, sizeof(*response));
    return ipc_reply(received->request, &message) == MG_OK;
}

static void handle_request(const mg_ipc_received_t *received)
{
    mg_device_request_t request;
    mg_device_response_t response = {0};
    mg_device_snapshot_request_t query = {0};
    u32 total = 0;
    u32 filter;
    u32 count;
    u64 snapshot_generation = 0;
    mg_result_t result;

    if (!received || received->message.type != MG_DEVICE_REQUEST ||
        received->message.payload_length != sizeof(request)) {
        response.result = MG_ERR_PROTOCOL;
        (void)send_response(received, &response);
        return;
    }
    memcpy(&request, received->message.payload, sizeof(request));
    if (request.version != MG_DEVICE_PROTOCOL_VERSION || request.limit == 0U ||
        request.limit > MG_DEVICE_RESPONSE_MAX ||
        (request.operation == MG_DEVICE_OP_LIST_CATEGORY &&
         !category_valid(request.category)) ||
        (request.operation == MG_DEVICE_OP_GET && !request.device_id) ||
        (request.operation != MG_DEVICE_OP_LIST_ALL &&
         request.operation != MG_DEVICE_OP_LIST_CATEGORY &&
         request.operation != MG_DEVICE_OP_GET &&
         request.operation != MG_DEVICE_OP_LIST_BLOCK)) {
        response.result = MG_ERR_BAD_ARGUMENT;
    } else {
        if (request.operation == MG_DEVICE_OP_LIST_ALL)
            filter = MG_DEVICE_CATEGORY_ALL;
        else if (request.operation == MG_DEVICE_OP_LIST_CATEGORY)
            filter = request.category;
        else if (request.operation == MG_DEVICE_OP_LIST_BLOCK)
            filter = MG_DEVICE_FILTER_BLOCKS;
        else
            filter = MG_DEVICE_CATEGORY_ALL;
        query.filter = filter;
        query.reserved = 0;
        query.device_id = request.operation == MG_DEVICE_OP_GET
            ? request.device_id : 0;
        query.offset = request.operation == MG_DEVICE_OP_GET ? 0 : request.offset;
        query.result_capacity = request.operation == MG_DEVICE_OP_GET ? 1U :
                                request.limit;
        query.result = response.devices;
        query.out_total = &total;
        query.snapshot_generation = request.snapshot_generation;
        query.out_snapshot_generation = &snapshot_generation;
        result = device_snapshot(&query);
        if (result < 0) {
            response.result = result;
        } else {
            count = (u32)result;
            response.result = MG_OK;
            response.count = count;
            response.total = total;
            response.next_offset = count && request.offset + count < total
                ? request.offset + count : 0;
            response.snapshot_generation = snapshot_generation;
        }
    }
    (void)send_response(received, &response);
}

static mg_result_t refresh_snapshot(void)
{
    mg_device_info_t snapshot[MG_DEVICE_RESPONSE_MAX];
    mg_device_snapshot_request_t query = {0};
    u32 total = 0;
    mg_result_t result;

    query.filter = MG_DEVICE_CATEGORY_ALL;
    query.result_capacity = MG_DEVICE_RESPONSE_MAX;
    query.result = snapshot;
    query.out_total = &total;
    result = device_snapshot(&query);
    /* The snapshot syscall returns the copied count on success, not MG_OK.
       Treat any non-negative count as a successful refresh so normal hotplug
       events are not incorrectly logged as failures. */
    return result < 0 ? result : MG_OK;
}

static void handle_event(const mg_ipc_received_t *received)
{
    mg_event_t event;

    if (!received || (received->delivery_kind != MG_IPC_DELIVERY_EVENT &&
                      received->delivery_kind !=
                          MG_IPC_DELIVERY_EVENT_OVERFLOW))
        return;
    if (received->message.payload_length != sizeof(event)) return;
    memcpy(&event, received->message.payload, sizeof(event));
    if (event.version != MG_EVENT_PROTOCOL_VERSION) return;
    /* Events are wakeup hints; rebuild the bounded snapshot so the daemon's
     * cached view is current even when several changes arrive together. */
    {
        mg_result_t result = refresh_snapshot();
        char message[MG_LOG_MESSAGE_MAX];
        const char *name = event.name[0] ? event.name : "device";
        const char *action = NULL;

        switch (event.type) {
            case MG_EVENT_DEVICE_ADDED:
                action = "device added";
                break;
            case MG_EVENT_DEVICE_REMOVED:
                action = "device removed";
                break;
            case MG_EVENT_BLOCK_ADDED:
                action = "block device added";
                break;
            case MG_EVENT_BLOCK_REMOVED:
                action = "block device removed";
                break;
            case MG_EVENT_USB_DEVICE_ADDED:
                action = "USB device attached";
                break;
            case MG_EVENT_USB_DEVICE_REMOVED:
                action = "USB device removed";
                break;
            case MG_EVENT_QUEUE_OVERFLOW:
                action = "device snapshot rebuilt after event overflow";
                break;
            default:
                break;
        }
        if (action) {
            if (result == MG_OK)
                snprintf(message, sizeof(message), "%s: %s", action, name);
            else
                snprintf(message, sizeof(message),
                         "%s: %s (%s)", action, name,
                         error_string(result));
            (void)mg_log_submit(result == MG_OK ? MG_LOG_INFO : MG_LOG_ERROR,
                                message);
        }
    }
}

int main(void)
{
    mg_handle_t endpoint;
    mg_ipc_received_t received;
    mg_result_t result = service_register("device", &endpoint);

    if (result != MG_OK) {
        printf("Deviced: endpoint registration failed: %s\n",
               error_string(result));
        process_exit(1);
    }
    result = ipc_event_subscribe(endpoint, MG_EVENT_CLASS_DEVICE |
                                 MG_EVENT_CLASS_BLOCK |
                                 MG_EVENT_CLASS_USB |
                                 MG_EVENT_CLASS_NETWORK);
    if (result != MG_OK) {
        (void)handle_close(endpoint);
        process_exit(1);
    }
    result = refresh_snapshot();
    if (result != MG_OK) {
        (void)mg_log_submit(MG_LOG_ERROR, "device snapshot initialization failed");
    }
    for (;;) {
        result = ipc_receive(endpoint, &received);
        if (result != MG_OK) {
            (void)handle_close(endpoint);
            process_exit(1);
        }
        if (received.delivery_kind == MG_IPC_DELIVERY_EVENT ||
            received.delivery_kind == MG_IPC_DELIVERY_EVENT_OVERFLOW)
            handle_event(&received);
        else {
            handle_request(&received);
            (void)handle_close(received.request);
        }
    }
}
