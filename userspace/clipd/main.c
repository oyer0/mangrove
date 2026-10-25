/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "clipboard.h"
#include <mangrove.h>
#include <mg/inspection.h>
#include <mg/object.h>
#include <mg/process.h>
#include <mg/time.h>
#include <stdio.h>
#include <string.h>

static clip_state_t state;
int main(void)
{
    mg_handle_t endpoint;
    mg_system_memory_info_t memory;
    if (memory_info(&memory) != MG_OK ||
        service_register(MG_CLIPBOARD_ENDPOINT, &endpoint) != MG_OK) {
        fprintf(stderr, "clipd: clipboard service initialization failed.\n");
        return 1;
    }
    /* Resource policy for this daemon incarnation, not a protocol size cap.
     * Half of currently free RAM bounds total mapped bulk storage; one item
     * may use half that budget, leaving room for atomic replacement. */
    clip_init(&state, memory.physical_free_bytes / 2U);
    u64 last_collection = uptime_ms();
    for (;;) {
        mg_ipc_received_t received;
        mg_result_t result = ipc_receive_timed(endpoint, &received, 100U);
        u64 now = uptime_ms();
        if (result == MG_ERR_SHUTDOWN) return 0;
        if (result != MG_OK && result != MG_ERR_TIMEOUT) return 1;
        if (now - last_collection >= 250U) {
            if (!clip_collect(&state, now, 0)) return 1;
            last_collection = now;
        }
        if (!clip_drain(&state, 8)) return 1;
        if (result != MG_OK || received.delivery_kind != MG_IPC_DELIVERY_REQUEST)
            continue;
        mg_ipc_message_t reply;
        clip_dispatch(&state, &received, &reply, now);
        (void)ipc_reply(received.request, &reply);
        (void)handle_close(received.request);
    }
}
