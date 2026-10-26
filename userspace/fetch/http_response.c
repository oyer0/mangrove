/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http_response.h"

#include <stdlib.h>
#include <string.h>

#define FETCH_HTTP_HEADER_MAX (16U * 1024U)
#define FETCH_HTTP_CHUNK_LINE_MAX 1024U
#define FETCH_HTTP_INFORMATIONAL_MAX 8U

typedef enum fetch_http_body_state {
    HTTP_BODY_HEADERS,
    HTTP_BODY_LENGTH,
    HTTP_BODY_CHUNK_SIZE,
    HTTP_BODY_CHUNK_DATA,
    HTTP_BODY_CHUNK_CRLF,
    HTTP_BODY_TRAILERS,
    HTTP_BODY_CLOSE,
    HTTP_BODY_DONE,
    HTTP_BODY_FAILED,
} fetch_http_body_state_t;

struct fetch_http_decoder {
    u8 header[FETCH_HTTP_HEADER_MAX];
    usize header_length;
    u8 delimiter_state;
    u8 chunk_line[FETCH_HTTP_CHUNK_LINE_MAX];
    usize chunk_line_length;
    usize trailer_bytes;
    u64 length_remaining;
    u64 chunk_remaining;
    u64 body_bytes;
    u8 chunk_crlf_state;
    u8 informational_count;
    bool headers_ready;
    bool headers_reported;
    fetch_http_body_state_t state;
    fetch_http_headers_t headers;
    fetch_http_body_sink_fn sink;
    void *context;
};

static bool ascii_equal_ci(const u8 *left, const char *right, usize length)
{
    for (usize index = 0; index < length; index++) {
        u8 a = left[index];
        u8 b = (u8)right[index];

        if (a >= 'A' && a <= 'Z') a = (u8)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (u8)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

static bool parse_decimal_u64(const u8 *text, usize length, u64 *out_value)
{
    u64 value = 0;

    if (!text || !length || !out_value) return false;
    for (usize index = 0; index < length; index++) {
        u64 digit;

        if (text[index] < '0' || text[index] > '9') return false;
        digit = (u64)(text[index] - '0');
        if (value > (~(u64)0 - digit) / 10ULL) return false;
        value = value * 10ULL + digit;
    }
    *out_value = value;
    return true;
}

static bool header_name_byte(u8 value)
{
    if ((value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z')) return true;
    switch (value) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~': return true;
        default: return false;
    }
}

static bool line_end(const u8 *bytes, usize length, usize start,
                     usize *out_end)
{
    for (usize index = start; index + 1U < length; index++) {
        if (bytes[index] == '\r' && bytes[index + 1U] == '\n') {
            *out_end = index;
            return true;
        }
        if (bytes[index] == '\n') return false;
    }
    return false;
}

static bool parse_status_line(const u8 *line, usize length, u32 *out_status)
{
    if (!line || !out_status || length < 12U ||
        memcmp(line, "HTTP/1.", 7U) != 0 ||
        (line[7] != '0' && line[7] != '1') || line[8] != ' ' ||
        line[9] < '0' || line[9] > '9' ||
        line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' ||
        (length > 12U && line[12] != ' ')) return false;
    *out_status = (u32)(line[9] - '0') * 100U +
                  (u32)(line[10] - '0') * 10U +
                  (u32)(line[11] - '0');
    return *out_status >= 100U && *out_status <= 599U;
}

static bool parse_headers(fetch_http_decoder_t *decoder)
{
    usize status_end;
    usize position;
    usize location_start = 0;
    usize location_length = 0;
    usize content_type_start = 0;
    usize content_type_length = 0;
    bool have_location = false;
    bool have_content_type = false;
    bool have_length = false;
    bool have_transfer_encoding = false;
    bool chunked = false;
    u64 content_length = 0;
    u32 status;

    if (!line_end(decoder->header, decoder->header_length, 0U, &status_end) ||
        !parse_status_line(decoder->header, status_end, &status)) return false;
    position = status_end + 2U;
    while (position + 1U < decoder->header_length) {
        usize end;
        usize colon;
        usize value_start;
        usize value_end;

        if (!line_end(decoder->header, decoder->header_length, position, &end))
            return false;
        if (end == position) break;
        colon = position;
        while (colon < end && decoder->header[colon] != ':') {
            if (!header_name_byte(decoder->header[colon])) return false;
            colon++;
        }
        if (colon == position || colon == end) return false;
        value_start = colon + 1U;
        while (value_start < end &&
               (decoder->header[value_start] == ' ' ||
                decoder->header[value_start] == '\t')) value_start++;
        value_end = end;
        while (value_end > value_start &&
               (decoder->header[value_end - 1U] == ' ' ||
                decoder->header[value_end - 1U] == '\t')) value_end--;
        for (usize index = value_start; index < value_end; index++) {
            u8 value = decoder->header[index];
            if ((value < 0x20U && value != '\t') || value == 0x7fU)
                return false;
        }
        if (ascii_equal_ci(decoder->header + position, "Content-Length",
                           colon - position)) {
            u64 parsed;
            if (!parse_decimal_u64(decoder->header + value_start,
                                   value_end - value_start, &parsed) ||
                (have_length && parsed != content_length)) return false;
            content_length = parsed;
            have_length = true;
        } else if (ascii_equal_ci(decoder->header + position,
                                  "Transfer-Encoding", colon - position)) {
            if (have_transfer_encoding ||
                value_end - value_start != 7U ||
                !ascii_equal_ci(decoder->header + value_start, "chunked",
                                value_end - value_start)) return false;
            have_transfer_encoding = true;
            chunked = true;
        } else if (ascii_equal_ci(decoder->header + position, "Location",
                                  colon - position)) {
            if (have_location || value_start == value_end) return false;
            location_start = value_start;
            location_length = value_end - value_start;
            have_location = true;
        } else if (ascii_equal_ci(decoder->header + position, "Content-Type",
                                  colon - position)) {
            if (have_content_type || value_start == value_end) return false;
            content_type_start = value_start;
            content_type_length = value_end - value_start;
            have_content_type = true;
        }
        position = end + 2U;
    }

    if (have_length && have_transfer_encoding) return false;
    decoder->headers.status = status;
    decoder->headers.has_content_length = have_length;
    decoder->headers.content_length = content_length;
    decoder->headers.chunked = chunked;
    decoder->headers.location = NULL;
    decoder->headers.content_type = NULL;
    if (have_location) {
        decoder->header[location_start + location_length] = '\0';
        decoder->headers.location = (const char *)decoder->header + location_start;
    }
    if (have_content_type) {
        decoder->header[content_type_start + content_type_length] = '\0';
        decoder->headers.content_type =
            (const char *)decoder->header + content_type_start;
    }

    if (status >= 100U && status < 200U && status != 101U) {
        if (++decoder->informational_count > FETCH_HTTP_INFORMATIONAL_MAX)
            return false;
        decoder->header_length = 0;
        decoder->delimiter_state = 0;
        decoder->headers = (fetch_http_headers_t){0};
        return true;
    }

    decoder->headers_ready = true;
    if (status == 101U || status == 204U || status == 304U ||
        (have_length && content_length == 0U)) {
        decoder->state = HTTP_BODY_DONE;
    } else if (chunked) {
        decoder->state = HTTP_BODY_CHUNK_SIZE;
    } else if (have_length) {
        decoder->length_remaining = content_length;
        decoder->state = HTTP_BODY_LENGTH;
    } else {
        decoder->state = HTTP_BODY_CLOSE;
    }
    return true;
}

static bool sink_bytes(fetch_http_decoder_t *decoder, const u8 *bytes,
                       usize length)
{
    if (!length) return true;
    if ((u64)length > ~(u64)0 - decoder->body_bytes ||
        !decoder->sink ||
        decoder->sink(decoder->context, bytes, length) != MG_OK) return false;
    decoder->body_bytes += (u64)length;
    return true;
}

static bool parse_chunk_size(const u8 *line, usize length, u64 *out_size)
{
    usize index = 0;
    u64 size = 0;
    bool have_digit = false;

    while (index < length && line[index] != ';') {
        u8 byte = line[index++];
        u64 digit;

        if (byte >= '0' && byte <= '9') digit = (u64)(byte - '0');
        else if (byte >= 'a' && byte <= 'f') digit = (u64)(byte - 'a' + 10U);
        else if (byte >= 'A' && byte <= 'F') digit = (u64)(byte - 'A' + 10U);
        else return false;
        if (size > (~(u64)0 - digit) / 16ULL) return false;
        size = size * 16ULL + digit;
        have_digit = true;
    }
    if (!have_digit) return false;
    if (index < length) {
        index++;
        if (index == length) return false;
        for (; index < length; index++) {
            u8 value = line[index];
            if ((value < 0x20U && value != '\t') || value == 0x7fU)
                return false;
        }
    }
    *out_size = size;
    return true;
}

static bool complete_chunk_line(fetch_http_decoder_t *decoder,
                                bool trailer)
{
    usize length;

    if (decoder->chunk_line_length < 2U ||
        decoder->chunk_line[decoder->chunk_line_length - 2U] != '\r' ||
        decoder->chunk_line[decoder->chunk_line_length - 1U] != '\n')
        return false;
    length = decoder->chunk_line_length - 2U;
    decoder->chunk_line_length = 0;
    if (!trailer) {
        if (!parse_chunk_size(decoder->chunk_line, length,
                              &decoder->chunk_remaining)) return false;
        decoder->state = decoder->chunk_remaining
            ? HTTP_BODY_CHUNK_DATA : HTTP_BODY_TRAILERS;
        return true;
    }
    if (!length) {
        decoder->state = HTTP_BODY_DONE;
        return true;
    }
    usize colon = 0;
    for (; colon < length; colon++) {
        if (decoder->chunk_line[colon] == ':') break;
        if (!header_name_byte(decoder->chunk_line[colon])) return false;
    }
    if (!colon || colon == length) return false;
    for (usize index = colon + 1U; index < length; index++) {
        u8 value = decoder->chunk_line[index];
        if ((value < 0x20U && value != '\t') || value == 0x7fU)
            return false;
    }
    return true;
}

fetch_http_decoder_t *fetch_http_decoder_create(fetch_http_body_sink_fn sink,
                                                 void *context)
{
    fetch_http_decoder_t *decoder;

    if (!sink) return NULL;
    decoder = (fetch_http_decoder_t *)calloc(1U, sizeof(*decoder));
    if (!decoder) return NULL;
    decoder->sink = sink;
    decoder->context = context;
    decoder->state = HTTP_BODY_HEADERS;
    return decoder;
}

void fetch_http_decoder_destroy(fetch_http_decoder_t *decoder)
{
    free(decoder);
}

static fetch_http_event_t decoder_failure(fetch_http_decoder_t *decoder,
                                          fetch_http_event_t event,
                                          usize consumed,
                                          usize *out_consumed)
{
    decoder->state = HTTP_BODY_FAILED;
    if (out_consumed) *out_consumed = consumed;
    return event;
}

fetch_http_event_t fetch_http_decoder_feed(fetch_http_decoder_t *decoder,
                                           const u8 *bytes, usize length,
                                           usize *out_consumed)
{
    usize offset = 0;

    if (out_consumed) *out_consumed = 0;
    if (!decoder || (!bytes && length) || !out_consumed ||
        decoder->state == HTTP_BODY_FAILED)
        return FETCH_HTTP_MALFORMED;
    if (decoder->state == HTTP_BODY_DONE) return FETCH_HTTP_COMPLETE;

    if (decoder->headers_ready && !decoder->headers_reported) {
        decoder->headers_reported = true;
        return FETCH_HTTP_HEADERS_READY;
    }

    while (offset < length) {
        if (decoder->state == HTTP_BODY_HEADERS) {
            u8 byte = bytes[offset++];
            static const u8 delimiter[] = {'\r', '\n', '\r', '\n'};
            if (decoder->header_length == sizeof(decoder->header))
                return decoder_failure(decoder, FETCH_HTTP_MALFORMED,
                                       offset, out_consumed);
            decoder->header[decoder->header_length++] = byte;
            if (byte == delimiter[decoder->delimiter_state]) {
                decoder->delimiter_state++;
            } else {
                decoder->delimiter_state = byte == '\r' ? 1U : 0U;
            }
            if (decoder->delimiter_state == sizeof(delimiter)) {
                decoder->delimiter_state = 0;
                if (!parse_headers(decoder))
                    return decoder_failure(decoder, FETCH_HTTP_MALFORMED,
                                           offset, out_consumed);
                if (decoder->headers_ready) {
                    decoder->headers_reported = true;
                    *out_consumed = offset;
                    return FETCH_HTTP_HEADERS_READY;
                }
            }
            continue;
        }
        if (decoder->state == HTTP_BODY_LENGTH) {
            usize amount = length - offset;
            if ((u64)amount > decoder->length_remaining)
                amount = (usize)decoder->length_remaining;
            if (!sink_bytes(decoder, bytes + offset, amount))
                return decoder_failure(decoder, FETCH_HTTP_SINK_FAILED,
                                       offset, out_consumed);
            offset += amount;
            decoder->length_remaining -= (u64)amount;
            if (!decoder->length_remaining) decoder->state = HTTP_BODY_DONE;
            continue;
        }
        if (decoder->state == HTTP_BODY_CLOSE) {
            usize amount = length - offset;
            if (!sink_bytes(decoder, bytes + offset, amount))
                return decoder_failure(decoder, FETCH_HTTP_SINK_FAILED,
                                       offset, out_consumed);
            offset += amount;
            continue;
        }
        if (decoder->state == HTTP_BODY_CHUNK_SIZE ||
            decoder->state == HTTP_BODY_TRAILERS) {
            u8 byte = bytes[offset++];
            bool trailers = decoder->state == HTTP_BODY_TRAILERS;
            if (trailers && ++decoder->trailer_bytes > FETCH_HTTP_HEADER_MAX)
                return decoder_failure(decoder, FETCH_HTTP_MALFORMED,
                                       offset, out_consumed);
            if (decoder->chunk_line_length == sizeof(decoder->chunk_line))
                return decoder_failure(decoder, FETCH_HTTP_MALFORMED,
                                       offset, out_consumed);
            decoder->chunk_line[decoder->chunk_line_length++] = byte;
            if (byte == '\n' &&
                !complete_chunk_line(decoder, trailers))
                return decoder_failure(decoder, FETCH_HTTP_MALFORMED,
                                       offset, out_consumed);
            continue;
        }
        if (decoder->state == HTTP_BODY_CHUNK_DATA) {
            usize amount = length - offset;
            if ((u64)amount > decoder->chunk_remaining)
                amount = (usize)decoder->chunk_remaining;
            if (!sink_bytes(decoder, bytes + offset, amount))
                return decoder_failure(decoder, FETCH_HTTP_SINK_FAILED,
                                       offset, out_consumed);
            offset += amount;
            decoder->chunk_remaining -= (u64)amount;
            if (!decoder->chunk_remaining) {
                decoder->state = HTTP_BODY_CHUNK_CRLF;
                decoder->chunk_crlf_state = 0;
            }
            continue;
        }
        if (decoder->state == HTTP_BODY_CHUNK_CRLF) {
            u8 expected = decoder->chunk_crlf_state ? '\n' : '\r';
            if (bytes[offset++] != expected)
                return decoder_failure(decoder, FETCH_HTTP_MALFORMED,
                                       offset, out_consumed);
            if (++decoder->chunk_crlf_state == 2U) {
                decoder->chunk_crlf_state = 0;
                decoder->state = HTTP_BODY_CHUNK_SIZE;
            }
            continue;
        }
        break;
    }
    *out_consumed = offset;
    return decoder->state == HTTP_BODY_DONE
        ? FETCH_HTTP_COMPLETE : FETCH_HTTP_NEED_MORE;
}

fetch_http_event_t fetch_http_decoder_eof(fetch_http_decoder_t *decoder)
{
    if (!decoder || decoder->state == HTTP_BODY_FAILED)
        return FETCH_HTTP_MALFORMED;
    if (decoder->state == HTTP_BODY_DONE) return FETCH_HTTP_COMPLETE;
    if (decoder->state == HTTP_BODY_CLOSE && decoder->headers_ready) {
        decoder->state = HTTP_BODY_DONE;
        return FETCH_HTTP_COMPLETE;
    }
    decoder->state = HTTP_BODY_FAILED;
    return FETCH_HTTP_MALFORMED;
}

const fetch_http_headers_t *fetch_http_decoder_headers(
    const fetch_http_decoder_t *decoder)
{
    return decoder && decoder->headers_ready ? &decoder->headers : NULL;
}

u64 fetch_http_decoder_body_bytes(const fetch_http_decoder_t *decoder)
{
    return decoder ? decoder->body_bytes : 0;
}

bool fetch_http_decoder_complete(const fetch_http_decoder_t *decoder)
{
    return decoder && decoder->state == HTTP_BODY_DONE;
}
