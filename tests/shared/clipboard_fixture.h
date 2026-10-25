/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Deterministic transport/session/mapping adapters. The client and service
 * implementation are linked unchanged into the editor production fixtures. */
#include <mg/memory.h>
#include <mg/session_service.h>
#include "../../userspace/clipd/clipboard.h"
static clip_state_t clipboard_fixture_state;
static bool clipboard_fixture_active = true;
static u32 clipboard_fixture_fail_operation;
static mg_ipc_requester_t clipboard_fixture_caller = {
    .pid = 10, .uid = 100, .session_id = 1
};
mg_result_t memory_map(usize size, void **out)
{
    *out = malloc(size);
    return *out ? MG_OK : MG_ERR_NO_MEMORY;
}
mg_result_t memory_unmap(void *pointer) { free(pointer); return MG_OK; }
mg_result_t session_query(mg_session_id_t id, mg_session_status_t *status)
{
    if (!clipboard_fixture_active || id != 1) return MG_ERR_NOT_FOUND;
    *status = (mg_session_status_t){.id=1,.uid=100,.state=MG_SESSION_STATE_ACTIVE};
    return MG_OK;
}
mg_result_t service_lookup(const char *name, mg_handle_t *endpoint)
{
    assert(!strcmp(name, MG_CLIPBOARD_ENDPOINT)); *endpoint=1;return MG_OK;
}
mg_result_t handle_close(mg_handle_t handle) { (void)handle; return MG_OK; }
mg_result_t ipc_request(mg_handle_t endpoint, const mg_ipc_message_t *message,
                         mg_ipc_message_t *reply)
{
    assert(endpoint == 1);
    mg_clipboard_request_t request = {0};
    memcpy(&request,message->payload,message->payload_length);
    if (request.operation == clipboard_fixture_fail_operation) {
        clipboard_fixture_fail_operation=0;
        return MG_ERR_SERVICE_UNAVAILABLE;
    }
    mg_ipc_received_t received={.message=*message,.requester=clipboard_fixture_caller};
    clip_dispatch(&clipboard_fixture_state,&received,reply,0);
    return MG_OK;
}
static void clipboard_fixture_reset(void)
{
    clipboard_fixture_active=false;
    assert(clip_collect(&clipboard_fixture_state,0,10000));
    clipboard_fixture_active=true;
    clip_init(&clipboard_fixture_state,64U*1024U*1024U);
    clipboard_fixture_caller.pid=10;
    clipboard_fixture_fail_operation=0;
}
