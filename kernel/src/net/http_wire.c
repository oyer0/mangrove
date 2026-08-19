#include <net/http.h>

enum {
    HTTP_PARSE_STATUS = 0,
    HTTP_PARSE_HEADERS,
    HTTP_PARSE_BODY_LENGTH,
    HTTP_PARSE_BODY_CLOSE,
    HTTP_PARSE_CHUNK_SIZE,
    HTTP_PARSE_CHUNK_DATA,
    HTTP_PARSE_CHUNK_CRLF,
    HTTP_PARSE_TRAILERS,
    HTTP_PARSE_COMPLETE,
    HTTP_PARSE_ERROR
};

static usize http_length(const char *s)
{
    usize n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static bool http_equal(const char *a, usize length, const char *b)
{
    usize i;
    if (http_length(b) != length) return false;
    for (i = 0; i < length; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

static bool http_copy_text(char *out, usize capacity, const u8 *input, usize length)
{
    if (!out || capacity == 0 || length >= capacity) return false;
    for (usize i = 0; i < length; i++) out[i] = (char)input[i];
    out[length] = '\0';
    return true;
}

static void http_trim(const u8 **start, usize *length)
{
    while (*length && ((*start)[0] == ' ' || (*start)[0] == '\t')) {
        (*start)++;
        (*length)--;
    }
    while (*length && ((*start)[*length - 1] == ' ' || (*start)[*length - 1] == '\t'))
        (*length)--;
}

static bool http_parse_decimal(const u8 *p, usize n, usize *value)
{
    usize result = 0;
    if (!n) return false;
    for (usize i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9' || result > (usize)-1 / 10U) return false;
        result = result * 10U + (usize)(p[i] - '0');
    }
    *value = result;
    return true;
}

static bool http_parse_hex(const u8 *p, usize n, usize *value)
{
    usize result = 0;
    bool digit = false;
    for (usize i = 0; i < n; i++) {
        u8 c = p[i];
        usize v;
        if (c == ';') break;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10U;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10U;
        else if (c == ' ' || c == '\t') continue;
        else return false;
        if (result > ((usize)-1 - v) / 16U) return false;
        result = result * 16U + v;
        digit = true;
    }
    if (!digit) return false;
    *value = result;
    return true;
}

static bool http_parse_status(http_parser_t *parser)
{
    const u8 *line = parser->line;
    usize n = parser->line_length;
    usize code;
    if (n < 12 || line[0] != 'H' || line[1] != 'T' || line[2] != 'T' ||
        line[3] != 'P' || line[4] != '/' || line[6] != '.' || line[8] != ' ' ||
        line[5] < '0' || line[5] > '9' || line[7] < '0' || line[7] > '9') return false;
    if (!http_parse_decimal(line + 9, 3, &code) || code < 100 || code > 999) return false;
    parser->response->major = (u8)(line[5] - '0');
    parser->response->minor = (u8)(line[7] - '0');
    parser->response->status_code = (u16)code;
    parser->saw_response = true;
    return true;
}

static bool http_parse_header(http_parser_t *parser)
{
    const u8 *line = parser->line;
    usize n = parser->line_length;
    usize colon = 0;
    const u8 *value;
    usize value_length;
    for (; colon < n && line[colon] != ':'; colon++) {}
    if (!colon || colon == n) return false;
    value = line + colon + 1;
    value_length = n - colon - 1;
    http_trim(&value, &value_length);
    if (http_equal((const char *)line, colon, "content-length")) {
        usize length;
        if (parser->have_content_length || !http_parse_decimal(value, value_length, &length)) return false;
        parser->have_content_length = true;
        parser->content_length = length;
    } else if (http_equal((const char *)line, colon, "transfer-encoding")) {
        if (parser->have_transfer_encoding) return false;
        parser->have_transfer_encoding = true;
        if (value_length >= 7 && http_equal((const char *)value, 7, "chunked"))
            parser->response->chunked = true;
        else return false;
    } else if (http_equal((const char *)line, colon, "connection")) {
        if (value_length >= 5 && http_equal((const char *)value, 5, "close"))
            parser->response->connection_close = true;
    } else if (http_equal((const char *)line, colon, "content-type")) {
        if (!http_copy_text(parser->response->content_type,
                            HTTP_CONTENT_TYPE_MAX, value, value_length)) return false;
    }
    return true;
}

static bool http_line_complete(http_parser_t *parser)
{
    parser->line[parser->line_length] = '\0';
    if (parser->state == HTTP_PARSE_STATUS) {
        parser->state = HTTP_PARSE_HEADERS;
        return http_parse_status(parser);
    }
    if (parser->state == HTTP_PARSE_HEADERS) {
        if (parser->line_length == 0) {
            if (parser->response->chunked) parser->state = HTTP_PARSE_CHUNK_SIZE;
            else if (parser->have_content_length) parser->state = HTTP_PARSE_BODY_LENGTH;
            else parser->state = HTTP_PARSE_BODY_CLOSE;
            return true;
        }
        return http_parse_header(parser);
    }
    if (parser->state == HTTP_PARSE_CHUNK_SIZE) {
        if (!http_parse_hex(parser->line, parser->line_length, &parser->chunk_remaining)) return false;
        parser->state = parser->chunk_remaining ? HTTP_PARSE_CHUNK_DATA : HTTP_PARSE_TRAILERS;
        return true;
    }
    if (parser->state == HTTP_PARSE_TRAILERS) {
        if (parser->line_length == 0) parser->state = HTTP_PARSE_COMPLETE;
        return true;
    }
    return false;
}

static bool http_append_body(http_parser_t *parser, const u8 *data, usize length)
{
    if (length > HTTP_BODY_MAX - parser->response->body_length) {
        parser->state = HTTP_PARSE_ERROR;
        return false;
    }
    for (usize i = 0; i < length; i++)
        parser->response->body[parser->response->body_length + i] = data[i];
    parser->response->body_length += length;
    return true;
}

bool http_parse_url(const char *url, http_url_t *parsed)
{
    const char *authority;
    const char *path;
    const char *colon = 0;
    usize host_length;
    usize port = 0;
    if (!url || !parsed || http_length(url) < 8) return false;
    if (url[0] != 'h' || url[1] != 't' || url[2] != 't' || url[3] != 'p' || url[4] != ':' || url[5] != '/' || url[6] != '/')
        return false;
    authority = url + 7;
    path = authority;
    while (*path && *path != '/') {
        if (*path == ':') colon = path;
        path++;
    }
    host_length = (usize)((colon ? colon : path) - authority);
    if (!host_length || host_length >= HTTP_HOST_MAX) return false;
    for (usize i = 0; i < host_length; i++) {
        char c = authority[i];
        if ((c == ':' && colon) || c == '/' || c == ' ' || c == '\t') return false;
        parsed->hostname[i] = c;
    }
    parsed->hostname[host_length] = '\0';
    if (colon) {
        const char *p = colon + 1;
        if (!*p || (path != p && path[-1] == ':')) return false;
        while (p < path) {
            if (*p < '0' || *p > '9' || port > 65535U / 10U) return false;
            port = port * 10U + (usize)(*p - '0');
            p++;
        }
        if (!port || port > 65535U) return false;
    } else port = 80;
    parsed->port = (u16)port;
    if (!*path) {
        parsed->path[0] = '/';
        parsed->path[1] = '\0';
        return true;
    }
    if ((usize)(http_length(path)) >= HTTP_PATH_MAX) return false;
    for (usize i = 0; path[i]; i++) parsed->path[i] = path[i];
    parsed->path[http_length(path)] = '\0';
    return true;
}

void http_parser_init(http_parser_t *parser, http_response_t *response)
{
    if (!parser || !response) return;
    *parser = (http_parser_t){0};
    *response = (http_response_t){0};
    parser->response = response;
    parser->state = HTTP_PARSE_STATUS;
}

bool http_parser_feed(http_parser_t *parser, const u8 *data, usize length)
{
    if (!parser || !data || parser->state == HTTP_PARSE_ERROR) return false;
    for (usize i = 0; i < length; i++) {
        u8 byte = data[i];
        if (parser->state == HTTP_PARSE_COMPLETE) return true;
        if (parser->state == HTTP_PARSE_BODY_LENGTH) {
            usize remaining = parser->content_length - parser->body_received;
            usize take = length - i < remaining ? length - i : remaining;
            if (!http_append_body(parser, data + i, take)) return false;
            parser->body_received += take;
            i += take - (take ? 1U : 0U);
            if (parser->body_received == parser->content_length) parser->state = HTTP_PARSE_COMPLETE;
            continue;
        }
        if (parser->state == HTTP_PARSE_BODY_CLOSE) {
            if (!http_append_body(parser, data + i, length - i)) return false;
            parser->body_received += length - i;
            return true;
        }
        if (parser->state == HTTP_PARSE_CHUNK_DATA) {
            usize take = length - i < parser->chunk_remaining ? length - i : parser->chunk_remaining;
            if (!http_append_body(parser, data + i, take)) return false;
            parser->chunk_remaining -= take;
            i += take - (take ? 1U : 0U);
            if (!parser->chunk_remaining) {
                parser->state = HTTP_PARSE_CHUNK_CRLF;
                parser->chunk_crlf = 0;
            }
            continue;
        }
        if (parser->state == HTTP_PARSE_CHUNK_CRLF) {
            if ((parser->chunk_crlf == 0 && byte != '\r') ||
                (parser->chunk_crlf == 1 && byte != '\n')) {
                parser->state = HTTP_PARSE_ERROR;
                return false;
            }
            parser->chunk_crlf++;
            if (parser->chunk_crlf == 2) parser->state = HTTP_PARSE_CHUNK_SIZE;
            continue;
        }
        if (byte == '\r') continue;
        if (byte == '\n') {
            if (!http_line_complete(parser)) {
                parser->state = HTTP_PARSE_ERROR;
                return false;
            }
            parser->line_length = 0;
        } else {
            if (parser->line_length + 1 >= sizeof(parser->line)) {
                parser->state = HTTP_PARSE_ERROR;
                return false;
            }
            parser->line[parser->line_length++] = byte;
        }
    }
    return parser->state != HTTP_PARSE_ERROR;
}

bool http_parser_finish(http_parser_t *parser)
{
    if (!parser || parser->state == HTTP_PARSE_ERROR) return false;
    if (parser->state == HTTP_PARSE_BODY_CLOSE) parser->state = HTTP_PARSE_COMPLETE;
    return parser->state == HTTP_PARSE_COMPLETE;
}

bool http_parser_complete(const http_parser_t *parser)
{
    return parser && parser->state == HTTP_PARSE_COMPLETE;
}
