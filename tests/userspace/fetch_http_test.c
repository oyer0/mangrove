/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../userspace/fetch/http_response.h"

typedef struct test_sink {
    u8 *bytes;
    usize length;
    bool fail;
} test_sink_t;

static mg_result_t sink_append(void *context, const u8 *bytes, usize length)
{
    test_sink_t *sink = (test_sink_t *)context;
    u8 *grown;

    if (sink->fail) return MG_ERR_IO;
    if (length > (usize)-1 - sink->length) return MG_ERR_NO_MEMORY;
    grown = (u8 *)realloc(sink->bytes, sink->length + length);
    if (!grown && length) return MG_ERR_NO_MEMORY;
    sink->bytes = grown;
    if (length) memcpy(sink->bytes + sink->length, bytes, length);
    sink->length += length;
    return MG_OK;
}

static fetch_http_event_t feed_fragmented(fetch_http_decoder_t *decoder,
                                           const u8 *bytes, usize length,
                                           usize fragment)
{
    usize offset = 0;

    while (offset < length) {
        usize end = length - offset < fragment
            ? length : offset + fragment;
        while (offset < end) {
            usize consumed = 0;
            fetch_http_event_t event = fetch_http_decoder_feed(
                decoder, bytes + offset, end - offset, &consumed);
            offset += consumed;
            if (event == FETCH_HTTP_MALFORMED ||
                event == FETCH_HTTP_SINK_FAILED) return event;
            if (event == FETCH_HTTP_COMPLETE) return event;
            if (event == FETCH_HTTP_NEED_MORE && consumed == 0)
                return FETCH_HTTP_MALFORMED;
        }
    }
    return FETCH_HTTP_NEED_MORE;
}

static void test_content_length_and_no_extra_read(void)
{
    static const u8 response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloNEXT";
    test_sink_t sink = {0};
    fetch_http_decoder_t *decoder = fetch_http_decoder_create(sink_append,
                                                              &sink);
    usize consumed = 0;
    usize header_length = sizeof(response) - 1U - strlen("helloNEXT");

    assert(decoder);
    assert(fetch_http_decoder_feed(decoder, response, sizeof(response) - 1U,
                                   &consumed) == FETCH_HTTP_HEADERS_READY);
    assert(consumed == header_length);
    const fetch_http_headers_t *headers = fetch_http_decoder_headers(decoder);
    assert(headers && headers->status == 200U && headers->has_content_length);
    assert(headers->content_length == 5U && !headers->chunked);
    assert(fetch_http_decoder_feed(decoder, response + consumed,
                                   sizeof(response) - 1U - consumed,
                                   &consumed) == FETCH_HTTP_COMPLETE);
    assert(consumed == 5U);
    assert(sink.length == 5U && !memcmp(sink.bytes, "hello", 5U));
    assert(fetch_http_decoder_body_bytes(decoder) == 5U);
    assert(fetch_http_decoder_complete(decoder));
    free(sink.bytes);
    fetch_http_decoder_destroy(decoder);
}

static void test_fragmented_headers_and_chunked_body(void)
{
    static const u8 response[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "X-Test: fragmented\r\n\r\n"
        "4;sample=yes\r\nWiki\r\n5\r\npedia\r\n0\r\n"
        "X-Trailer: done\r\n\r\n";
    test_sink_t sink = {0};
    fetch_http_decoder_t *decoder = fetch_http_decoder_create(sink_append,
                                                              &sink);
    fetch_http_event_t event;

    assert(decoder);
    event = feed_fragmented(decoder, response, sizeof(response) - 1U, 1U);
    assert(event == FETCH_HTTP_COMPLETE);
    const fetch_http_headers_t *headers = fetch_http_decoder_headers(decoder);
    assert(headers && headers->status == 200U && headers->chunked);
    assert(!headers->has_content_length);
    assert(sink.length == 9U && !memcmp(sink.bytes, "Wikipedia", 9U));
    free(sink.bytes);
    fetch_http_decoder_destroy(decoder);
}

static void test_close_delimited_and_interim_response(void)
{
    static const u8 response[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.0 200 OK\r\n\r\nclose body";
    test_sink_t sink = {0};
    fetch_http_decoder_t *decoder = fetch_http_decoder_create(sink_append,
                                                              &sink);

    assert(decoder);
    assert(feed_fragmented(decoder, response, sizeof(response) - 1U, 3U) ==
           FETCH_HTTP_NEED_MORE);
    const fetch_http_headers_t *headers = fetch_http_decoder_headers(decoder);
    assert(headers && headers->status == 200U && !headers->has_content_length);
    assert(sink.length == strlen("close body"));
    assert(!memcmp(sink.bytes, "close body", sink.length));
    assert(fetch_http_decoder_eof(decoder) == FETCH_HTTP_COMPLETE);
    free(sink.bytes);
    fetch_http_decoder_destroy(decoder);
}

static void test_redirect_and_error_status_headers(void)
{
    static const u8 redirect[] =
        "HTTP/1.1 302 Found\r\nLocation: ../next?q=1\r\n"
        "Content-Length: 100\r\n\r\n";
    static const u8 error[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    test_sink_t sink = {0};
    fetch_http_decoder_t *decoder = fetch_http_decoder_create(sink_append,
                                                              &sink);
    usize consumed = 0;

    assert(decoder);
    assert(fetch_http_decoder_feed(decoder, redirect, sizeof(redirect) - 1U,
                                   &consumed) == FETCH_HTTP_HEADERS_READY);
    const fetch_http_headers_t *headers = fetch_http_decoder_headers(decoder);
    assert(headers && headers->status == 302U);
    assert(headers->location && !strcmp(headers->location, "../next?q=1"));
    assert(sink.length == 0U);
    fetch_http_decoder_destroy(decoder);

    decoder = fetch_http_decoder_create(sink_append, &sink);
    assert(decoder);
    assert(fetch_http_decoder_feed(decoder, error, sizeof(error) - 1U,
                                   &consumed) == FETCH_HTTP_HEADERS_READY);
    headers = fetch_http_decoder_headers(decoder);
    assert(headers && headers->status == 404U &&
           fetch_http_decoder_complete(decoder));
    fetch_http_decoder_destroy(decoder);
}

static void test_large_streaming_body(void)
{
    const usize body_length = 192U * 1024U;
    char header[96];
    int header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n",
        (unsigned)body_length);
    test_sink_t sink = {0};
    fetch_http_decoder_t *decoder = fetch_http_decoder_create(sink_append,
                                                              &sink);
    u8 input[4096];

    assert(header_length > 0 && (usize)header_length < sizeof(header));
    assert(decoder);
    usize consumed = 0;
    assert(fetch_http_decoder_feed(decoder, (const u8 *)header,
                                   (usize)header_length, &consumed) ==
           FETCH_HTTP_HEADERS_READY);
    assert(consumed == (usize)header_length);
    for (usize offset = 0; offset < body_length; ) {
        usize amount = body_length - offset < sizeof(input)
            ? body_length - offset : sizeof(input);
        memset(input, (int)(offset / sizeof(input)), amount);
        fetch_http_event_t event = fetch_http_decoder_feed(
            decoder, input, amount, &consumed);
        assert(consumed == amount);
        assert(event == (offset + amount == body_length
            ? FETCH_HTTP_COMPLETE : FETCH_HTTP_NEED_MORE));
        offset += amount;
    }
    assert(sink.length == body_length);
    for (usize offset = 0; offset < body_length; offset += sizeof(input))
        assert(sink.bytes[offset] == (u8)(offset / sizeof(input)));
    free(sink.bytes);
    fetch_http_decoder_destroy(decoder);
}

static void test_truncated_malformed_and_sink_failure(void)
{
    static const u8 truncated[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc";
    static const u8 bad_chunk[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "z\r\n";
    static const u8 conflicting[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    static const u8 malformed_trailer[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\nBad Name: value\r\n\r\n";
    test_sink_t sink = {0};
    fetch_http_decoder_t *decoder = fetch_http_decoder_create(sink_append,
                                                              &sink);

    assert(decoder);
    assert(feed_fragmented(decoder, truncated, sizeof(truncated) - 1U, 2U) ==
           FETCH_HTTP_NEED_MORE);
    assert(fetch_http_decoder_eof(decoder) == FETCH_HTTP_MALFORMED);
    fetch_http_decoder_destroy(decoder);

    decoder = fetch_http_decoder_create(sink_append, &sink);
    assert(decoder);
    assert(feed_fragmented(decoder, bad_chunk, sizeof(bad_chunk) - 1U, 2U) ==
           FETCH_HTTP_MALFORMED);
    fetch_http_decoder_destroy(decoder);

    decoder = fetch_http_decoder_create(sink_append, &sink);
    assert(decoder);
    assert(feed_fragmented(decoder, conflicting,
                           sizeof(conflicting) - 1U, 4U) ==
           FETCH_HTTP_MALFORMED);
    fetch_http_decoder_destroy(decoder);

    decoder = fetch_http_decoder_create(sink_append, &sink);
    assert(decoder);
    assert(feed_fragmented(decoder, malformed_trailer,
                           sizeof(malformed_trailer) - 1U, 3U) ==
           FETCH_HTTP_MALFORMED);
    fetch_http_decoder_destroy(decoder);

    static const u8 body[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
    sink.fail = true;
    decoder = fetch_http_decoder_create(sink_append, &sink);
    assert(decoder);
    usize consumed = 0;
    assert(fetch_http_decoder_feed(decoder, body, sizeof(body) - 1U,
                                   &consumed) == FETCH_HTTP_HEADERS_READY);
    assert(fetch_http_decoder_feed(decoder, body + consumed,
                                   sizeof(body) - 1U - consumed,
                                   &consumed) == FETCH_HTTP_SINK_FAILED);
    fetch_http_decoder_destroy(decoder);
}

int main(void)
{
    test_content_length_and_no_extra_read();
    test_fragmented_headers_and_chunked_body();
    test_close_delimited_and_interim_response();
    test_redirect_and_error_status_headers();
    test_large_streaming_body();
    test_truncated_malformed_and_sink_failure();
    puts("fetch HTTP framing: fragmented, length, chunked, close-delimited, large and failure cases passed");
    return 0;
}
