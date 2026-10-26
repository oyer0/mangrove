/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "fetch_url.h"

#include <mg/net.h>
#include <mg/tls.h>

typedef struct fetch_transport_state {
    mgtls_context_t *tls_context;
    bool tls_library_initialized;
} fetch_transport_state_t;

typedef struct fetch_transport {
    mg_handle_t stream;
    mgtls_session_t *tls_session;
    bool secure;
    bool open;
} fetch_transport_t;

mg_result_t fetch_transport_prepare_tls(fetch_transport_state_t *state);
/* Open the TCP stream to a resolved IPv4 endpoint. For HTTPS, prepare the
 * trust context before opening TCP and then call fetch_transport_start_tls(). */
mg_result_t fetch_transport_connect_tcp(const fetch_url_t *url,
                                       const mg_ipv4_addr_t *address,
                                       u32 timeout_ms,
                                       fetch_transport_t *transport);
/* Upgrade an open stream to authenticated TLS 1.3. mgTLS consumes the stream
 * on both success and failure. */
mg_result_t fetch_transport_start_tls(fetch_transport_state_t *state,
                                      const char *server_name,
                                      u32 timeout_ms,
                                      fetch_transport_t *transport);
mg_result_t fetch_transport_send(fetch_transport_t *transport,
                                 const void *data, usize length,
                                 u32 timeout_ms);
mg_result_t fetch_transport_receive(fetch_transport_t *transport,
                                    void *data, usize capacity,
                                    u32 timeout_ms);
/* A busy close retains the transport so the caller can retry. */
mg_result_t fetch_transport_close(fetch_transport_t *transport);
mg_result_t fetch_transport_state_cleanup(fetch_transport_state_t *state);
