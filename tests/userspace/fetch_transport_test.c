/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../userspace/fetch/fetch_transport.h"

static mg_result_t stream_connect_result;
static mg_result_t stream_send_result;
static mg_result_t stream_receive_result;
static mg_result_t stream_close_result;
static mg_result_t tls_init_result;
static mg_result_t tls_context_result;
static mg_result_t tls_connect_result;
static mg_result_t tls_send_result;
static mg_result_t tls_receive_result;
static mg_result_t tls_close_result;
static mg_result_t tls_context_destroy_result;
static mg_result_t tls_cleanup_result;
static usize tls_send_count;
static usize tls_receive_count;
static unsigned int stream_connect_calls;
static unsigned int stream_send_calls;
static unsigned int stream_receive_calls;
static unsigned int stream_close_calls;
static unsigned int tls_init_calls;
static unsigned int tls_context_calls;
static unsigned int tls_connect_calls;
static unsigned int tls_send_calls;
static unsigned int tls_receive_calls;
static unsigned int tls_close_calls;
static unsigned int tls_context_destroy_calls;
static unsigned int tls_cleanup_calls;
static const char *tls_connect_host;
static u16 tls_connect_port;

static void reset(void)
{
    stream_connect_result = MG_OK;
    stream_send_result = 3;
    stream_receive_result = 4;
    stream_close_result = MG_OK;
    tls_init_result = MG_OK;
    tls_context_result = MG_OK;
    tls_connect_result = MG_OK;
    tls_send_result = MG_OK;
    tls_receive_result = MG_OK;
    tls_close_result = MG_OK;
    tls_context_destroy_result = MG_OK;
    tls_cleanup_result = MG_OK;
    tls_send_count = 7U;
    tls_receive_count = 5U;
    stream_connect_calls = stream_send_calls = stream_receive_calls = 0U;
    stream_close_calls = tls_init_calls = tls_context_calls = 0U;
    tls_connect_calls = tls_send_calls = tls_receive_calls = 0U;
    tls_close_calls = tls_context_destroy_calls = tls_cleanup_calls = 0U;
    tls_connect_host = NULL;
    tls_connect_port = 0U;
}

mg_result_t mg_stream_connect(const mg_net_endpoint_t *endpoint,
                              u32 timeout_ms, mg_handle_t *out_handle)
{
    assert(endpoint && endpoint->port == 80U && timeout_ms == 123U);
    ++stream_connect_calls;
    if (stream_connect_result == MG_OK) *out_handle = 42;
    return stream_connect_result;
}

mg_result_t mg_stream_send(mg_handle_t handle, const void *data,
                           usize length, u32 timeout_ms)
{
    assert(handle == 42 && data && length == 12U && timeout_ms == 456U);
    ++stream_send_calls;
    return stream_send_result;
}

mg_result_t mg_stream_receive(mg_handle_t handle, void *data,
                              usize capacity, u32 timeout_ms)
{
    assert(handle == 42 && data && capacity == 32U && timeout_ms == 789U);
    ++stream_receive_calls;
    return stream_receive_result;
}

mg_result_t mg_stream_close(mg_handle_t handle)
{
    assert(handle == 42);
    ++stream_close_calls;
    return stream_close_result;
}

mg_result_t mgtls_init(void)
{
    ++tls_init_calls;
    return tls_init_result;
}

mg_result_t mgtls_context_create(mgtls_context_t **out_context)
{
    assert(out_context);
    ++tls_context_calls;
    if (tls_context_result == MG_OK)
        *out_context = (mgtls_context_t *)(uintptr_t)1U;
    else
        *out_context = NULL;
    return tls_context_result;
}

mg_result_t mgtls_session_connect(mgtls_context_t *context,
                                  const char *server_name, u16 port,
                                  u32 timeout_ms,
                                  mgtls_session_t **out_session)
{
    assert(context == (mgtls_context_t *)(uintptr_t)1U);
    assert(timeout_ms == 123U && out_session);
    ++tls_connect_calls;
    tls_connect_host = server_name;
    tls_connect_port = port;
    if (tls_connect_result == MG_OK)
        *out_session = (mgtls_session_t *)(uintptr_t)2U;
    else
        *out_session = NULL;
    return tls_connect_result;
}

mg_result_t mgtls_session_send(mgtls_session_t *session, const void *data,
                               usize length, u32 timeout_ms,
                               usize *out_transferred)
{
    assert(session == (mgtls_session_t *)(uintptr_t)2U);
    assert(data && length == 12U && timeout_ms == 456U && out_transferred);
    ++tls_send_calls;
    *out_transferred = tls_send_count;
    return tls_send_result;
}

mg_result_t mgtls_session_receive(mgtls_session_t *session, void *data,
                                  usize capacity, u32 timeout_ms,
                                  usize *out_transferred)
{
    assert(session == (mgtls_session_t *)(uintptr_t)2U);
    assert(data && capacity == 32U && timeout_ms == 789U && out_transferred);
    ++tls_receive_calls;
    *out_transferred = tls_receive_count;
    return tls_receive_result;
}

mg_result_t mgtls_session_close(mgtls_session_t *session)
{
    assert(session == (mgtls_session_t *)(uintptr_t)2U);
    ++tls_close_calls;
    return tls_close_result;
}

mg_result_t mgtls_context_destroy(mgtls_context_t *context)
{
    assert(context == (mgtls_context_t *)(uintptr_t)1U);
    ++tls_context_destroy_calls;
    return tls_context_destroy_result;
}

mg_result_t mgtls_cleanup(void)
{
    ++tls_cleanup_calls;
    return tls_cleanup_result;
}

static void test_http_keeps_native_transport(void)
{
    fetch_transport_state_t state = {0};
    fetch_transport_t transport = {0};
    fetch_url_t url = {.host = "example.test", .path = "/", .port = 80U};
    mg_ipv4_addr_t address = {{192U, 0U, 2U, 1U}};

    reset();
    assert(fetch_transport_open(&state, &url, &address, 123U,
                                &transport) == MG_OK);
    assert(transport.open && !transport.secure && stream_connect_calls == 1U);
    assert(tls_init_calls == 0U && tls_connect_calls == 0U);
    assert(fetch_transport_send(&transport, "hello world!", 12U, 456U) == 3);
    assert(fetch_transport_receive(&transport, (void *)"", 32U, 789U) == 4);
    assert(fetch_transport_close(&transport) == MG_OK);
    assert(!transport.open && stream_close_calls == 1U);
    assert(fetch_transport_state_cleanup(&state) == MG_OK);
    assert(tls_cleanup_calls == 0U);
}

static void test_https_uses_authenticated_session_and_partial_io(void)
{
    fetch_transport_state_t state = {0};
    fetch_transport_t transport = {0};
    fetch_url_t url = {.host = "secure.example", .path = "/", .port = 443U,
                       .https = true};
    usize received;

    reset();
    assert(fetch_transport_prepare_tls(&state) == MG_OK);
    assert(fetch_transport_open(&state, &url, NULL, 123U, &transport) == MG_OK);
    assert(transport.open && transport.secure && tls_init_calls == 1U &&
           tls_context_calls == 1U && tls_connect_calls == 1U);
    assert(!stream_connect_calls);
    assert(!strcmp(tls_connect_host, "secure.example") &&
           tls_connect_port == 443U);
    assert(fetch_transport_send(&transport, "hello world!", 12U, 456U) == 7);
    assert(fetch_transport_receive(&transport, &received, 32U, 789U) == 5);
    assert(tls_send_calls == 1U && tls_receive_calls == 1U);
    assert(fetch_transport_close(&transport) == MG_OK);
    assert(!transport.open && tls_close_calls == 1U && !stream_close_calls);
    assert(fetch_transport_state_cleanup(&state) == MG_OK);
    assert(tls_context_destroy_calls == 1U && tls_cleanup_calls == 1U);
}

static void test_security_failures_never_fall_back(void)
{
    fetch_transport_state_t state = {0};
    fetch_transport_t transport = {0};
    fetch_url_t url = {.host = "bad.example", .path = "/", .port = 443U,
                       .https = true};

    reset();
    tls_context_result = MG_ERR_TIME_UNAVAILABLE;
    assert(fetch_transport_open(&state, &url, NULL, 123U, &transport) ==
           MG_ERR_TIME_UNAVAILABLE);
    assert(!transport.open && !tls_connect_calls && !stream_connect_calls);
    assert(fetch_transport_state_cleanup(&state) == MG_OK);
    assert(tls_cleanup_calls == 1U);

    reset();
    tls_connect_result = MG_ERR_CERTIFICATE_INVALID;
    assert(fetch_transport_open(&state, &url, NULL, 123U, &transport) ==
           MG_ERR_CERTIFICATE_INVALID);
    assert(!transport.open && tls_connect_calls == 1U &&
           !stream_connect_calls);
    assert(fetch_transport_state_cleanup(&state) == MG_OK);
    assert(tls_context_destroy_calls == 1U && tls_cleanup_calls == 1U);

    reset();
    tls_init_result = MG_ERR_ENTROPY_UNAVAILABLE;
    assert(fetch_transport_open(&state, &url, NULL, 123U, &transport) ==
           MG_ERR_ENTROPY_UNAVAILABLE);
    assert(!transport.open && tls_context_calls == 0U &&
           tls_connect_calls == 0U && !stream_connect_calls);
    assert(fetch_transport_state_cleanup(&state) == MG_OK);
    assert(tls_cleanup_calls == 0U);

    reset();
    tls_context_result = MG_ERR_TRUST_STORE_INVALID;
    assert(fetch_transport_open(&state, &url, NULL, 123U, &transport) ==
           MG_ERR_TRUST_STORE_INVALID);
    assert(!transport.open && tls_init_calls == 1U &&
           tls_context_calls == 1U && !tls_connect_calls &&
           !stream_connect_calls);
    assert(fetch_transport_state_cleanup(&state) == MG_OK);
    assert(!tls_context_destroy_calls && tls_cleanup_calls == 1U);
}

static void test_interrupt_and_retryable_close(void)
{
    fetch_transport_state_t state = {0};
    fetch_transport_t transport = {0};
    fetch_url_t url = {.host = "secure.example", .path = "/", .port = 443U,
                       .https = true};

    reset();
    assert(fetch_transport_open(&state, &url, NULL, 123U, &transport) == MG_OK);
    tls_receive_result = MG_ERR_INTERRUPTED;
    assert(fetch_transport_receive(&transport, (void *)"", 32U, 789U) ==
           MG_ERR_INTERRUPTED);
    tls_close_result = MG_ERR_BUSY;
    assert(fetch_transport_close(&transport) == MG_ERR_BUSY);
    assert(transport.open);
    tls_close_result = MG_OK;
    assert(fetch_transport_close(&transport) == MG_OK);
    assert(!transport.open);
    assert(fetch_transport_state_cleanup(&state) == MG_OK);

    reset();
    state = (fetch_transport_state_t){0};
    transport = (fetch_transport_t){0};
    assert(fetch_transport_open(&state, &url, NULL, 123U, &transport) == MG_OK);
    tls_close_result = MG_ERR_INTERRUPTED;
    assert(fetch_transport_close(&transport) == MG_ERR_INTERRUPTED);
    assert(!transport.open);
    assert(fetch_transport_state_cleanup(&state) == MG_OK);
}

int main(void)
{
    test_http_keeps_native_transport();
    test_https_uses_authenticated_session_and_partial_io();
    test_security_failures_never_fall_back();
    test_interrupt_and_retryable_close();
    puts("fetch transport: HTTP compatibility, authenticated TLS routing, partial I/O, failures and cleanup passed");
    return 0;
}
