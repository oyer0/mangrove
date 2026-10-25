/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mg/clipboard.h>
#include <mg/memory.h>
#include <mg/session_service.h>
#include "../../userspace/clipd/clipboard.h"

static clip_state_t state;
static u64 now, active_sessions = 6, calls, mapped_bytes, peak_bytes, wire_bytes;
static mg_ipc_requester_t caller = {.pid = 10, .uid = 100, .session_id = 1};
static mg_handle_t incarnation = 1;
static int fail_map_after = -1, fail_malloc_after = -1, fail_ipc_after = -1;
static u32 drop_reply_operation;
#if !defined(__APPLE__)
void *__real_malloc(size_t size);
#endif
void *__wrap_malloc(size_t size) {
    if (fail_malloc_after == 0) { fail_malloc_after = -1; return NULL; }
    if (fail_malloc_after > 0) fail_malloc_after--;
    return __real_malloc(size);
}
mg_result_t memory_map(usize size, void **out) {
    assert(size == CLIPD_BLOCK_BYTES);
    if (fail_map_after == 0) { fail_map_after = -1; return MG_ERR_NO_MEMORY; }
    if (fail_map_after > 0) fail_map_after--;
    *out = __real_malloc(size);
    if (!*out) return MG_ERR_NO_MEMORY;
    mapped_bytes += size;
    if (mapped_bytes > peak_bytes) peak_bytes = mapped_bytes;
    return MG_OK;
}
mg_result_t memory_unmap(void *address) {
    free(address); mapped_bytes -= CLIPD_BLOCK_BYTES; return MG_OK;
}
mg_result_t session_query(mg_session_id_t id, mg_session_status_t *status) {
    if (id > 2 || !(active_sessions & (1ULL << id))) return MG_ERR_NOT_FOUND;
    *status = (mg_session_status_t){.id = id, .uid = id == 1 ? 100 : 200,
                                   .state = MG_SESSION_STATE_ACTIVE};
    return MG_OK;
}
mg_result_t service_lookup(const char *name, mg_handle_t *endpoint) {
    assert(!strcmp(name, MG_CLIPBOARD_ENDPOINT)); *endpoint = incarnation; return MG_OK;
}
mg_result_t handle_close(mg_handle_t handle) { (void)handle; return MG_OK; }
mg_result_t ipc_request(mg_handle_t endpoint, const mg_ipc_message_t *message,
                         mg_ipc_message_t *reply) {
    if (endpoint != incarnation) return MG_ERR_SERVICE_UNAVAILABLE;
    if (fail_ipc_after == 0) { fail_ipc_after = -1; return MG_ERR_CONNECTION_CLOSED; }
    if (fail_ipc_after > 0) fail_ipc_after--;
    assert(message->version == MG_IPC_PROTOCOL_VERSION &&
           message->type == MG_CLIPBOARD_REQUEST &&
           message->payload_length >= MG_CLIPBOARD_CONTROL_BYTES &&
           message->payload_length <= sizeof(mg_clipboard_request_t));
    mg_clipboard_request_t request = {0};
    memcpy(&request, message->payload, message->payload_length);
    assert(message->payload_length == MG_CLIPBOARD_CONTROL_BYTES +
        (request.operation == MG_CLIPBOARD_WRITE ? request.count : 0));
    mg_ipc_received_t received = {.message = *message, .requester = caller};
    clip_dispatch(&state, &received, reply, now);
    wire_bytes += message->payload_length + reply->payload_length;
    calls++;
    if (drop_reply_operation == request.operation) {
        drop_reply_operation=0;return MG_ERR_CONNECTION_CLOSED;
    }
    return MG_OK;
}
static void expect_text(const char *expected, usize size) {
    char *text = NULL; usize length = 999;
    assert(clipboard_get_text(size, &text, &length) == MG_OK);
    assert(length == size && !memcmp(text, expected, size) && !text[size]); free(text);
}
static void reset(void) {
    active_sessions = 0;
    assert(clip_collect(&state, now, 100000));
    assert(!mapped_bytes);
    clip_init(&state, 64U * 1024U * 1024U);
    active_sessions = 6; caller = (mg_ipc_requester_t){.pid=10,.uid=100,.session_id=1};
}
static void test_atomic_and_sessions(void) {
    assert(clipboard_query(&(mg_clipboard_info_t){0}) == MG_ERR_NOT_FOUND);
    const char original[] = " hello\n\xF0\x9F\x8C\xB1\t\0";
    assert(clipboard_set_text(original, sizeof(original)-1) == MG_OK);
    mg_clipboard_transfer_t read, write;
    assert(clipboard_read_begin(MG_CLIPBOARD_TEXT, &read) == MG_OK);
    assert(clipboard_publish_begin(MG_CLIPBOARD_TEXT, 3, &write) == MG_OK);
    assert(clipboard_write(&write,"new",3)==MG_OK);
    caller.pid = 12;
    expect_text(original,sizeof(original)-1); /* uncommitted data is invisible */
    caller.pid = 10;
    assert(clipboard_publish_commit(&write)==MG_OK);
    char bytes[128]; usize length;
    assert(clipboard_read(&read,bytes,sizeof(bytes),&length)==MG_OK);
    assert(length==sizeof(original)-1 && !memcmp(bytes,original,length));
    assert(clipboard_transfer_close(&read)==MG_OK);
    caller.pid = 11; /* source process no longer participates */
    expect_text("new",3);
    assert(clipboard_read_begin("image/png",&read)==MG_ERR_UNSUPPORTED);
    caller.session_id=2;caller.uid=200;
    assert(clipboard_get_text(100,(char **)&(char *){0},&length)==MG_ERR_NOT_FOUND);
    assert(clipboard_set_text("other",5)==MG_OK);
    caller.session_id=1;caller.uid=100;expect_text("new",3);
    caller.uid=200;
    assert(clipboard_clear()==MG_ERR_ACCESS_DENIED);
    caller.uid=100;caller.session_id=0;
    assert(clipboard_clear()==MG_ERR_ACCESS_DENIED);
    caller.session_id=1;caller.system_service=1;
    assert(clipboard_clear()==MG_ERR_ACCESS_DENIED);
    caller.system_service=0;
    assert(clipboard_set_text("",0)==MG_OK);expect_text("",0);
    assert(clipboard_clear()==MG_OK);
    assert(clipboard_query(&(mg_clipboard_info_t){0})==MG_ERR_NOT_FOUND);
    reset();
}
static void test_failures(void) {
    assert(clipboard_set_text("saved",5)==MG_OK);
    mg_clipboard_transfer_t transfer;
    assert(clipboard_publish_begin(MG_CLIPBOARD_TEXT,4,&transfer)==MG_OK);
    assert(clipboard_write(&transfer,"part",4)==MG_OK);
    assert(clipboard_transfer_close(&transfer)==MG_OK);expect_text("saved",5);
    assert(clipboard_publish_begin(MG_CLIPBOARD_TEXT,4,&transfer)==MG_OK);
    assert(clipboard_write(&transfer,"pa",2)==MG_OK);
    assert(clipboard_publish_commit(&transfer)==MG_ERR_BAD_ARGUMENT);
    assert(clipboard_transfer_close(&transfer)==MG_OK);expect_text("saved",5);
    drop_reply_operation=MG_CLIPBOARD_COMMIT;
    assert(clipboard_set_text("complete",8)==MG_ERR_CONNECTION_CLOSED);
    expect_text("complete",8); /* lost confirmation never exposes a partial item */
    assert(clipboard_set_text("saved",5)==MG_OK);
    assert(clipboard_set_text("\xC0\x80",2)==MG_ERR_BAD_ARGUMENT);expect_text("saved",5);
    assert(clipboard_set_text("\xE2\x82",2)==MG_ERR_BAD_ARGUMENT);expect_text("saved",5);
    fail_map_after=0;
    assert(clipboard_set_text("fail",4)==MG_ERR_NO_MEMORY);expect_text("saved",5);
    char *unchanged=(char *)1;usize unchanged_size=9;
    fail_malloc_after=0;
    assert(clipboard_get_text(100,&unchanged,&unchanged_size)==MG_ERR_NO_MEMORY);
    assert(unchanged==(char *)1 && unchanged_size==9);
    assert(clipboard_get_text(1,&unchanged,&unchanged_size)==MG_ERR_BUFFER_TOO_SMALL);
    fail_ipc_after=2;
    assert(clipboard_set_text("lost",4)==MG_ERR_CONNECTION_CLOSED);
    expect_text("saved",5);
    assert(clipboard_publish_begin(MG_CLIPBOARD_TEXT,4,&transfer)==MG_OK);
    u64 token=transfer.token;
    caller.pid++;
    mg_clipboard_request_t request={.version=1,.operation=MG_CLIPBOARD_COMMIT,.token=token};
    mg_clipboard_response_t response;
    clip_handle(&state,&caller,&request,&response,now);
    assert(response.result==MG_ERR_STALE_GENERATION);
    caller.pid--;
    now+=CLIPD_IDLE_MS;
    assert(clip_collect(&state,now,100));
    assert(clipboard_write(&transfer,"lost",4)==MG_ERR_STALE_GENERATION);
    (void)clipboard_transfer_close(&transfer);expect_text("saved",5);
    assert(clipboard_read_begin(MG_CLIPBOARD_TEXT,&transfer)==MG_OK);
    active_sessions &= ~(1ULL<<1);
    assert(clip_collect(&state,now,100));
    assert(clipboard_read(&transfer,&unchanged_size,1,&unchanged_size)==MG_ERR_ACCESS_DENIED);
    (void)clipboard_transfer_close(&transfer);
    active_sessions|=1ULL<<1;
    assert(clipboard_query(&(mg_clipboard_info_t){0})==MG_ERR_NOT_FOUND);
    assert(clipboard_set_text("restart",7)==MG_OK);
    assert(clipboard_read_begin(MG_CLIPBOARD_TEXT,&transfer)==MG_OK);
    reset();incarnation++;
    assert(clipboard_read(&transfer,&unchanged_size,1,&unchanged_size)==MG_ERR_SERVICE_UNAVAILABLE);
    (void)clipboard_transfer_close(&transfer);
}
static void test_capacity_and_protocol(void) {
    mg_clipboard_transfer_t transfers[CLIPD_TRANSFERS], rejected;
    for(u32 i=0;i<CLIPD_TRANSFERS;i++) {
        caller.pid=100+i;
        assert(clipboard_publish_begin(MG_CLIPBOARD_TEXT,0,&transfers[i])==MG_OK);
    }
    caller.pid=999;
    assert(clipboard_publish_begin(MG_CLIPBOARD_TEXT,0,&rejected)==MG_ERR_BUSY);
    for(u32 i=0;i<CLIPD_TRANSFERS;i++) {
        caller.pid=100+i;assert(clipboard_transfer_close(&transfers[i])==MG_OK);
    }
    caller.pid=10;
    assert(clipboard_set_text("old",3)==MG_OK);
    mg_clipboard_info_t info;
    assert(clipboard_query_format(1,&info)==MG_ERR_NOT_FOUND);
    assert(clipboard_publish_begin(MG_CLIPBOARD_TEXT,3,&rejected)==MG_OK);
    mg_clipboard_request_t request={.version=1,.operation=MG_CLIPBOARD_WRITE,
        .token=rejected.token,.offset=1,.count=1};
    mg_clipboard_response_t response;
    clip_handle(&state,&caller,&request,&response,now);
    assert(response.result==MG_ERR_BAD_ARGUMENT);
    assert(clipboard_write(&rejected,"new",3)==MG_ERR_STALE_GENERATION);
    (void)clipboard_transfer_close(&rejected);expect_text("old",3);
    request.version=0;
    clip_handle(&state,&caller,&request,&response,now);
    assert(response.result==MG_ERR_PROTOCOL);
    mg_ipc_received_t malformed={.requester=caller};
    malformed.message=(mg_ipc_message_t){.version=1,.type=MG_CLIPBOARD_REQUEST,
                                        .payload_length=MG_CLIPBOARD_CONTROL_BYTES-1};
    mg_ipc_message_t reply;
    clip_dispatch(&state,&malformed,&reply,now);
    memcpy(&response,reply.payload,reply.payload_length);
    assert(response.result==MG_ERR_PROTOCOL);
    request=(mg_clipboard_request_t){.version=1,.operation=MG_CLIPBOARD_WRITE,
                                    .count=MG_CLIPBOARD_CHUNK+1};
    malformed.message.payload_length=MG_CLIPBOARD_CONTROL_BYTES;
    memcpy(malformed.message.payload,&request,MG_CLIPBOARD_CONTROL_BYTES);
    clip_dispatch(&state,&malformed,&reply,now);
    memcpy(&response,reply.payload,reply.payload_length);
    assert(response.result==MG_ERR_PROTOCOL);expect_text("old",3);
    /* A small resource budget is exhaustion, not corruption. */
    reset();state.budget=CLIPD_BLOCK_BYTES;state.session_limit=CLIPD_BLOCK_BYTES;
    assert(clipboard_set_text("old",3)==MG_OK);
    assert(clipboard_set_text("new",3)==MG_ERR_NO_MEMORY);expect_text("old",3);
    assert(clipboard_clear()==MG_OK);assert(clip_drain(&state,1));
    assert(clipboard_set_text("recovered",9)==MG_OK);expect_text("recovered",9);
    reset();
}
static void test_large(void) {
    const usize length=8U*1024U*1024U;
    char *text=__real_malloc(length);assert(text);
    /* Deliberately split four-byte UTF-8 sequences at transport/block edges. */
    const unsigned char pattern[]={0xF0,0x9F,0x8C,0xB1,'\n',' ','\t',0};
    for(usize i=0;i<length;i++)text[i]=(i<5 || i>=length-3) ? 'x' : pattern[(i-5)%sizeof(pattern)];
    clock_t start=clock();u64 before=calls, before_wire=wire_bytes;
    assert(clipboard_set_text(text,length)==MG_OK);
    expect_text(text,length);
    assert(wire_bytes-before_wire == 2U*length + 2U*MG_CLIPBOARD_CONTROL_BYTES*(calls-before));
    double elapsed=(double)(clock()-start)/CLOCKS_PER_SEC;
    printf("clipboard 8 MiB round trip: %.3f s, %llu requests, %llu bulk bytes peak, %llu payload bytes\n",
        elapsed,(unsigned long long)(calls-before),(unsigned long long)peak_bytes,(unsigned long long)(wire_bytes-before_wire));
    /* Allocate at least two blocks, then fail at each bounded allocation. */
    for(int stage=0;stage<3;stage++) {
        fail_map_after=stage;
        assert(clipboard_set_text(text,256U*1024U)==MG_ERR_NO_MEMORY);
        assert(clip_collect(&state,now,100));
        expect_text(text,length);
    }
    char *unchanged=(char *)1;usize unchanged_size=9;
    fail_ipc_after=2; /* begin and first read succeed; next read fails */
    assert(clipboard_get_text(length,&unchanged,&unchanged_size)==MG_ERR_CONNECTION_CLOSED);
    assert(unchanged==(char *)1 && unchanged_size==9);
    mg_clipboard_transfer_t writer;
    assert(clipboard_publish_begin(MG_CLIPBOARD_TEXT,state.session_limit+1,&writer)==MG_ERR_NO_MEMORY);
    free(text);reset();
}
int main(void) {
    clip_init(&state,64U*1024U*1024U);
    test_atomic_and_sessions();test_failures();test_capacity_and_protocol();test_large();
    puts("clipboard: atomic items, pinned reads, session isolation, UTF-8, expiry, restart and allocation recovery passed");
}
