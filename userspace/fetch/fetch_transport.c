/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fetch_transport.h"

#include <mg/error.h>

#include <stddef.h>
#include <stdint.h>

mg_result_t fetch_transport_prepare_tls(fetch_transport_state_t *state)
{
    mg_result_t result;

    if (!state) return MG_ERR_BAD_ARGUMENT;
    if (state->tls_context) return MG_OK;
    if (!state->tls_library_initialized) {
        result = mgtls_init();
        if (result != MG_OK) return result;
        state->tls_library_initialized = true;
    }
    result = mgtls_context_create(&state->tls_context);
    if (result != MG_OK) state->tls_context = NULL;
    return result;
}

mg_result_t fetch_transport_connect_tcp(const fetch_url_t *url,
                                       const mg_ipv4_addr_t *address,
                                       u32 timeout_ms,
                                       fetch_transport_t *transport)
{
    if (!url || !address || !transport || transport->open)
        return MG_ERR_BAD_ARGUMENT;
    *transport = (fetch_transport_t){0};
    mg_net_endpoint_t endpoint = {
        .address = *address,
        .port = url->port,
        .reserved = 0,
    };
    mg_result_t result = mg_stream_connect(&endpoint, timeout_ms,
                                           &transport->stream);
    if (result != MG_OK) return result;
    transport->open = true;
    return MG_OK;
}

mg_result_t fetch_transport_start_tls(fetch_transport_state_t *state,
                                      const char *server_name,
                                      u32 timeout_ms,
                                      fetch_transport_t *transport)
{
    mg_result_t result;
    mg_handle_t stream;

    if (!state || !server_name || !transport || !transport->open ||
        transport->secure || !transport->stream)
        return MG_ERR_BAD_ARGUMENT;
    result = fetch_transport_prepare_tls(state);
    if (result != MG_OK) return result;
    stream = transport->stream;
    transport->stream = 0;
    transport->open = false;
    result = mgtls_session_connect_stream(state->tls_context, server_name,
                                          stream, timeout_ms,
                                          &transport->tls_session);
    if (result != MG_OK) {
        transport->tls_session = NULL;
        return result;
    }
    if (!transport->tls_session) return MG_ERR_PROTOCOL;
    transport->secure = true;
    transport->open = true;
    return MG_OK;
}

mg_result_t fetch_transport_send(fetch_transport_t *transport,
                                 const void *data, usize length,
                                 u32 timeout_ms)
{
    if (!transport || !transport->open || (!data && length))
        return MG_ERR_BAD_ARGUMENT;
    if (!transport->secure)
        return mg_stream_send(transport->stream, data, length, timeout_ms);

    usize transferred = 0;
    mg_result_t result = mgtls_session_send(transport->tls_session, data,
                                            length, timeout_ms, &transferred);
    if (result != MG_OK) return result;
    if (transferred > length || transferred > (usize)INT64_MAX)
        return MG_ERR_PROTOCOL;
    return (mg_result_t)transferred;
}

mg_result_t fetch_transport_receive(fetch_transport_t *transport,
                                    void *data, usize capacity,
                                    u32 timeout_ms)
{
    if (!transport || !transport->open || (!data && capacity))
        return MG_ERR_BAD_ARGUMENT;
    if (!transport->secure)
        return mg_stream_receive(transport->stream, data, capacity,
                                 timeout_ms);

    usize transferred = 0;
    mg_result_t result = mgtls_session_receive(transport->tls_session, data,
                                               capacity, timeout_ms,
                                               &transferred);
    if (result != MG_OK) return result;
    if (transferred > capacity || transferred > (usize)INT64_MAX)
        return MG_ERR_PROTOCOL;
    return (mg_result_t)transferred;
}

mg_result_t fetch_transport_close(fetch_transport_t *transport)
{
    mg_result_t result;

    if (!transport || !transport->open) return MG_ERR_BAD_ARGUMENT;
    result = transport->secure ?
        mgtls_session_close(transport->tls_session) :
        mg_stream_close(transport->stream);
    if (result != MG_ERR_BUSY)
        *transport = (fetch_transport_t){0};
    return result;
}

mg_result_t fetch_transport_state_cleanup(fetch_transport_state_t *state)
{
    mg_result_t result;

    if (!state) return MG_ERR_BAD_ARGUMENT;
    if (state->tls_context) {
        result = mgtls_context_destroy(state->tls_context);
        if (result != MG_OK) return result;
        state->tls_context = NULL;
    }
    if (state->tls_library_initialized) {
        result = mgtls_cleanup();
        if (result != MG_OK) return result;
        state->tls_library_initialized = false;
    }
    return MG_OK;
}
