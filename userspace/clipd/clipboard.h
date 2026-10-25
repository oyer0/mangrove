/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <mg/clipboard.h>
#include <mg/utf8.h>

#define CLIPD_SESSIONS 8U
#define CLIPD_TRANSFERS 32U
#define CLIPD_ITEMS (CLIPD_SESSIONS + CLIPD_TRANSFERS)
#define CLIPD_BLOCK_BYTES (64U * 1024U)
#define CLIPD_IDLE_MS 30000U

typedef struct clip_block clip_block_t;
struct clip_block {
    clip_block_t *next;
    usize used;
    u8 bytes[CLIPD_BLOCK_BYTES - sizeof(void *) - sizeof(usize)];
};
typedef struct {
    bool live;
    u32 references;
    u64 size, written, generation;
    clip_block_t *first, *last;
    mg_utf8_state_t utf8;
} clip_item_t;
typedef struct {
    u64 id;
    u32 uid;
    clip_item_t *item;
} clip_session_t;
typedef struct {
    u64 token, pid, session, deadline, offset;
    u32 uid;
    bool writing;
    clip_item_t *item;
    clip_block_t *cursor;
    usize cursor_offset;
} clip_transfer_t;
typedef struct {
    clip_session_t sessions[CLIPD_SESSIONS];
    clip_transfer_t transfers[CLIPD_TRANSFERS];
    clip_item_t items[CLIPD_ITEMS];
    clip_block_t *garbage_first, *garbage_last;
    u64 budget, session_limit, allocated, next_token, generation;
} clip_state_t;

void clip_init(clip_state_t *state, u64 budget);
void clip_dispatch(clip_state_t *state, const mg_ipc_received_t *received,
                    mg_ipc_message_t *reply, u64 now);
void clip_handle(clip_state_t *state, const mg_ipc_requester_t *caller,
                  const mg_clipboard_request_t *request,
                  mg_clipboard_response_t *response, u64 now);
/* Call periodically even with no incoming requests. Releases expired sessions
 * and idle transfers, and at most block_limit retired mappings. */
bool clip_drain(clip_state_t *state, u32 block_limit);
bool clip_collect(clip_state_t *state, u64 now, u32 block_limit);
