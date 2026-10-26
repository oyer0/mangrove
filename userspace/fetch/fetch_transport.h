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
mg_result_t fetch_transport_open(fetch_transport_state_t *state,
                                 const fetch_url_t *url,
                                 const mg_ipv4_addr_t *address,
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
