/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

typedef struct fetch_http_decoder fetch_http_decoder_t;

typedef mg_result_t (*fetch_http_body_sink_fn)(void *context,
                                                const u8 *bytes,
                                                usize length);

typedef enum fetch_http_event {
    FETCH_HTTP_NEED_MORE,
    FETCH_HTTP_HEADERS_READY,
    FETCH_HTTP_COMPLETE,
    FETCH_HTTP_MALFORMED,
    FETCH_HTTP_SINK_FAILED,
} fetch_http_event_t;

typedef struct fetch_http_headers {
    u32 status;
    bool has_content_length;
    u64 content_length;
    bool chunked;
    const char *location;
    const char *content_type;
} fetch_http_headers_t;

/* The decoder allocates a bounded 16 KiB header workspace. It never buffers
 * the entity body; successful body spans are passed directly to sink. */
fetch_http_decoder_t *fetch_http_decoder_create(fetch_http_body_sink_fn sink,
                                                 void *context);
void fetch_http_decoder_destroy(fetch_http_decoder_t *decoder);

/* Feed may stop at the header boundary or the framed end of the body. The
 * caller must preserve and feed any unconsumed bytes after HEADERS_READY. */
fetch_http_event_t fetch_http_decoder_feed(fetch_http_decoder_t *decoder,
                                           const u8 *bytes, usize length,
                                           usize *out_consumed);
fetch_http_event_t fetch_http_decoder_eof(fetch_http_decoder_t *decoder);
const fetch_http_headers_t *fetch_http_decoder_headers(
    const fetch_http_decoder_t *decoder);
u64 fetch_http_decoder_body_bytes(const fetch_http_decoder_t *decoder);
bool fetch_http_decoder_complete(const fetch_http_decoder_t *decoder);
