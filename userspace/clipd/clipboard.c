/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "clipboard.h"
#include <mg/memory.h>
#include <mg/session.h>
#include <mg/session_service.h>
#include <string.h>

void clip_init(clip_state_t *state, u64 budget)
{
    memset(state, 0, sizeof(*state));
    state->budget = budget;
    state->session_limit = budget / 2U;
    state->next_token = 1;
}

static void item_release(clip_state_t *state, clip_item_t *item)
{
    if (!item || --item->references) return;
    if (item->first) {
        if (state->garbage_last) state->garbage_last->next = item->first;
        else state->garbage_first = item->first;
        state->garbage_last = item->last;
    }
    memset(item, 0, sizeof(*item));
}

static void transfer_release(clip_state_t *state, clip_transfer_t *transfer)
{
    item_release(state, transfer->item);
    memset(transfer, 0, sizeof(*transfer));
}

static bool session_active(u64 id, u32 uid)
{
    mg_session_status_t status;
    return id && session_query(id, &status) == MG_OK &&
        status.id == id && status.uid == uid &&
        status.state == MG_SESSION_STATE_ACTIVE;
}

bool clip_collect(clip_state_t *state, u64 now, u32 block_limit)
{
    for (u32 i = 0; i < CLIPD_SESSIONS; i++) {
        clip_session_t *session = &state->sessions[i];
        if (!session->id || session_active(session->id, session->uid)) continue;
        for (u32 j = 0; j < CLIPD_TRANSFERS; j++)
            if (state->transfers[j].token &&
                state->transfers[j].session == session->id)
                transfer_release(state, &state->transfers[j]);
        item_release(state, session->item);
        memset(session, 0, sizeof(*session));
    }
    for (u32 i = 0; i < CLIPD_TRANSFERS; i++)
        if (state->transfers[i].token && now >= state->transfers[i].deadline)
            transfer_release(state, &state->transfers[i]);
    return clip_drain(state, block_limit);
}

bool clip_drain(clip_state_t *state, u32 block_limit)
{
    while (block_limit-- && state->garbage_first) {
        clip_block_t *block = state->garbage_first;
        clip_block_t *next = block->next;
        if (memory_unmap(block) != MG_OK) return false;
        state->garbage_first = next;
        if (!next) state->garbage_last = NULL;
        state->allocated -= CLIPD_BLOCK_BYTES;
    }
    return true;
}

static clip_session_t *get_session(clip_state_t *state, u64 id, u32 uid)
{
    clip_session_t *empty = NULL;
    for (u32 i = 0; i < CLIPD_SESSIONS; i++) {
        clip_session_t *session = &state->sessions[i];
        if (session->id == id) return session->uid == uid ? session : NULL;
        if (!session->id) empty = session;
    }
    if (empty) { empty->id = id; empty->uid = uid; }
    return empty;
}

static u64 deadline(u64 now)
{
    return now > ~(u64)0 - CLIPD_IDLE_MS ? ~(u64)0 : now + CLIPD_IDLE_MS;
}

static mg_result_t begin(clip_state_t *state, clip_session_t *session,
                          const mg_ipc_requester_t *caller,
                          const mg_clipboard_request_t *request,
                          mg_clipboard_response_t *response, u64 now)
{
    bool writing = request->operation == MG_CLIPBOARD_WRITE_BEGIN;
    clip_transfer_t *transfer = NULL;
    if (strcmp(request->format, MG_CLIPBOARD_TEXT)) return MG_ERR_UNSUPPORTED;
    for (u32 i = 0; i < CLIPD_TRANSFERS; i++) {
        clip_transfer_t *entry = &state->transfers[i];
        if (entry->token && entry->pid == caller->pid && entry->writing == writing)
            return MG_ERR_BUSY;
        if (!entry->token) transfer = entry;
    }
    if (!transfer || !state->next_token) return MG_ERR_BUSY;
    clip_item_t *item = session->item;
    if (writing) {
        if (request->size > state->session_limit) return MG_ERR_NO_MEMORY;
        item = NULL;
        for (u32 i = 0; i < CLIPD_ITEMS; i++)
            if (!state->items[i].live) { item = &state->items[i]; break; }
        if (!item) return MG_ERR_BUSY;
        *item = (clip_item_t){.live = true, .size = request->size};
        mg_utf8_state_reset(&item->utf8);
    } else if (!item) return MG_ERR_NOT_FOUND;
    item->references++;
    *transfer = (clip_transfer_t){.token = state->next_token++, .pid = caller->pid,
        .session = caller->session_id, .uid = caller->uid, .writing = writing,
        .deadline = deadline(now), .item = item, .cursor = item->first};
    response->token = transfer->token;
    response->size = item->size;
    response->generation = item->generation;
    return MG_OK;
}

static mg_result_t append(clip_state_t *state, clip_transfer_t *transfer,
                           const mg_clipboard_request_t *request)
{
    clip_item_t *item = transfer->item;
    if (!request->count || request->count > MG_CLIPBOARD_CHUNK ||
        request->offset != item->written ||
        request->count > item->size - item->written) return MG_ERR_BAD_ARGUMENT;
    mg_utf8_state_t utf8 = item->utf8;
    for (u32 i = 0; i < request->count; i++) {
        mg_utf8_feed_result_t feed = mg_utf8_state_feed(&utf8, request->data[i]);
        if (feed.status == MG_UTF8_INVALID) return MG_ERR_BAD_ARGUMENT;
    }
    u32 copied = 0;
    while (copied < request->count) {
        clip_block_t *block = item->last;
        if (!block || block->used == sizeof(block->bytes)) {
            void *address;
            if (state->allocated > state->budget ||
                state->budget - state->allocated < CLIPD_BLOCK_BYTES)
                return MG_ERR_NO_MEMORY;
            mg_result_t result = memory_map(CLIPD_BLOCK_BYTES, &address);
            if (result != MG_OK) return result;
            block = address;
            block->used = 0;
            block->next = NULL;
            if (item->last) item->last->next = block;
            else item->first = block;
            item->last = block;
            state->allocated += CLIPD_BLOCK_BYTES;
        }
        usize amount = request->count - copied;
        if (amount > sizeof(block->bytes) - block->used)
            amount = sizeof(block->bytes) - block->used;
        memcpy(block->bytes + block->used, request->data + copied, amount);
        block->used += amount;
        copied += amount;
    }
    item->written += request->count;
    item->utf8 = utf8;
    return MG_OK;
}

void clip_handle(clip_state_t *state, const mg_ipc_requester_t *caller,
                  const mg_clipboard_request_t *request,
                  mg_clipboard_response_t *response, u64 now)
{
    memset(response, 0, sizeof(*response));
    response->version = MG_CLIPBOARD_VERSION;
    response->result = MG_ERR_PROTOCOL;
    if (request->version != MG_CLIPBOARD_VERSION || request->reserved ||
        !memchr(request->format, 0, sizeof(request->format))) return;
    response->result = MG_ERR_ACCESS_DENIED;
    if (!caller->pid || caller->system_service ||
        !session_active(caller->session_id, caller->uid)) return;
    clip_session_t *session = get_session(state, caller->session_id, caller->uid);
    if (!session) { response->result = MG_ERR_BUSY; return; }
    mg_result_t result = MG_OK;
    clip_item_t *item = session->item;
    if (request->operation == MG_CLIPBOARD_QUERY) {
        if (!item) result = MG_ERR_NOT_FOUND;
        else if (request->offset) result = MG_ERR_NOT_FOUND;
        else {
            response->formats = 1;
            response->size = item->size;
            response->generation = item->generation;
            memcpy(response->format, MG_CLIPBOARD_TEXT, sizeof(MG_CLIPBOARD_TEXT));
        }
    } else if (request->operation == MG_CLIPBOARD_CLEAR) {
        item_release(state, item);
        session->item = NULL;
    } else if (request->operation == MG_CLIPBOARD_WRITE_BEGIN ||
               request->operation == MG_CLIPBOARD_READ_BEGIN) {
        result = begin(state, session, caller, request, response, now);
    } else {
        clip_transfer_t *transfer = NULL;
        for (u32 i = 0; i < CLIPD_TRANSFERS; i++) {
            clip_transfer_t *entry = &state->transfers[i];
            if (entry->token && entry->token == request->token &&
                entry->pid == caller->pid && entry->session == caller->session_id &&
                entry->uid == caller->uid) { transfer = entry; break; }
        }
        if (!transfer) { response->result = MG_ERR_STALE_GENERATION; return; }
        if (now >= transfer->deadline) {
            transfer_release(state, transfer);
            response->result = MG_ERR_TIMEOUT;
            return;
        }
        transfer->deadline = deadline(now);
        item = transfer->item;
        switch (request->operation) {
            case MG_CLIPBOARD_CANCEL:
            case MG_CLIPBOARD_READ_END:
                transfer_release(state, transfer);
                break;
            case MG_CLIPBOARD_WRITE:
                result = transfer->writing ? append(state, transfer, request)
                                           : MG_ERR_BAD_ARGUMENT;
                if (result != MG_OK && transfer->writing)
                    transfer_release(state, transfer);
                break;
            case MG_CLIPBOARD_COMMIT:
                if (!transfer->writing || item->written != item->size ||
                    !mg_utf8_state_is_initial(&item->utf8)) result = MG_ERR_BAD_ARGUMENT;
                else if (state->generation == ~(u64)0) result = MG_ERR_NO_SPACE;
                else {
                    item->generation = ++state->generation;
                    item->references++; /* committed session owns a reference */
                    item_release(state, session->item);
                    session->item = item;
                }
                transfer_release(state, transfer);
                break;
            case MG_CLIPBOARD_READ:
                if (transfer->writing || !request->count ||
                    request->count > MG_CLIPBOARD_CHUNK ||
                    request->offset != transfer->offset) { result = MG_ERR_BAD_ARGUMENT; break; }
                while (response->count < request->count && transfer->offset < item->size) {
                    clip_block_t *block = transfer->cursor;
                    usize amount = block->used - transfer->cursor_offset;
                    if (amount > request->count - response->count)
                        amount = request->count - response->count;
                    memcpy(response->data + response->count,
                           block->bytes + transfer->cursor_offset, amount);
                    response->count += amount;
                    transfer->offset += amount;
                    transfer->cursor_offset += amount;
                    if (transfer->cursor_offset == block->used) {
                        transfer->cursor = block->next;
                        transfer->cursor_offset = 0;
                    }
                }
                break;
            default: result = MG_ERR_BAD_ARGUMENT; break;
        }
    }
    response->result = result;
}

void clip_dispatch(clip_state_t *state, const mg_ipc_received_t *received,
                    mg_ipc_message_t *reply, u64 now)
{
    mg_clipboard_response_t response = {.version = MG_CLIPBOARD_VERSION,
                                        .result = MG_ERR_PROTOCOL};
    if (received->delivery_kind == MG_IPC_DELIVERY_REQUEST &&
        received->message.version == MG_IPC_PROTOCOL_VERSION &&
        received->message.type == MG_CLIPBOARD_REQUEST &&
        received->message.payload_length >= MG_CLIPBOARD_CONTROL_BYTES &&
        received->message.payload_length <= sizeof(mg_clipboard_request_t)) {
        mg_clipboard_request_t request = {0};
        memcpy(&request, received->message.payload, received->message.payload_length);
        u32 extra = request.operation == MG_CLIPBOARD_WRITE ? request.count : 0;
        if (extra <= MG_CLIPBOARD_CHUNK && received->message.payload_length ==
            MG_CLIPBOARD_CONTROL_BYTES + extra)
            clip_handle(state, &received->requester, &request, &response, now);
    }
    *reply = (mg_ipc_message_t){.version = MG_IPC_PROTOCOL_VERSION,
        .type = MG_CLIPBOARD_RESPONSE,
        .payload_length = MG_CLIPBOARD_CONTROL_BYTES + response.count};
    memcpy(reply->payload, &response, reply->payload_length);
}
