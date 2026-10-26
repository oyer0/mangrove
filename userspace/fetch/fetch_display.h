/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "fetch_url.h"
#include "http_response.h"

#include <mg/net.h>
#include <mg/types.h>

typedef void (*fetch_display_write_fn)(void *context, const char *text,
                                       usize length);

typedef struct fetch_display {
    fetch_display_write_fn write;
    void *write_context;
    bool quiet;
    bool interactive_progress;
    bool progress_active;
    bool progress_has_length;
    u64 progress_length;
    u64 progress_last_time_ms;
    u64 progress_last_bytes;
    usize progress_line_length;
} fetch_display_t;

typedef struct fetch_display_timings {
    u64 dns_ms;
    u64 connect_ms;
    u64 tls_context_ms;
    u64 tls_handshake_ms;
    u64 first_byte_ms;
    u64 transfer_ms;
    u64 receive_ms;
    u64 write_ms;
    u64 output_check_ms;
    u64 file_setup_ms;
    u64 connection_close_ms;
    u64 file_close_ms;
    u64 publish_ms;
    u64 write_calls;
    u64 write_max_ms;
    u64 total_ms;
    bool clock_available;
    bool transfer_clock_available;
} fetch_display_timings_t;

void fetch_display_init(fetch_display_t *display, bool quiet,
                        bool interactive_progress,
                        fetch_display_write_fn write, void *write_context);
void fetch_display_requested(fetch_display_t *display,
                             const fetch_url_t *url);
void fetch_display_resolving(fetch_display_t *display, const char *host,
                             bool address_literal);
void fetch_display_resolved(fetch_display_t *display, const char *host,
                            const mg_ipv4_addr_t *address);
void fetch_display_connecting(fetch_display_t *display, const char *host,
                              u16 port);
void fetch_display_connected(fetch_display_t *display, const char *host,
                             u16 port, const mg_ipv4_addr_t *address);
void fetch_display_tls_starting(fetch_display_t *display, const char *host);
void fetch_display_tls_verified(fetch_display_t *display, const char *host);
void fetch_display_http_status(fetch_display_t *display, u32 status);
void fetch_display_redirect(fetch_display_t *display, u32 status,
                            const fetch_url_t *url);
void fetch_display_response_details(fetch_display_t *display,
                                    const fetch_http_headers_t *headers,
                                    const char *destination);
void fetch_display_body_start(fetch_display_t *display, bool has_length,
                              u64 content_length, u64 now_ms);
void fetch_display_progress(fetch_display_t *display, u64 bytes,
                            u64 now_ms);
void fetch_display_finish_progress(fetch_display_t *display);
void fetch_display_complete(fetch_display_t *display, const char *filename,
                            u64 bytes, u64 elapsed_ms, bool have_elapsed);
void fetch_display_timings(fetch_display_t *display,
                           const fetch_display_timings_t *timings);
