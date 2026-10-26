/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fetch_url.h"

#include <stdlib.h>
#include <string.h>

static bool starts_with(const char *text, const char *prefix)
{
    usize index = 0;

    if (!text || !prefix) return false;
    while (prefix[index]) {
        if (text[index] != prefix[index]) return false;
        index++;
    }
    return true;
}

static bool starts_with_ci(const char *text, const char *prefix)
{
    usize index = 0;

    if (!text || !prefix) return false;
    while (prefix[index]) {
        char value = text[index];
        char expected = prefix[index];
        if (value >= 'A' && value <= 'Z') value = (char)(value + ('a' - 'A'));
        if (expected >= 'A' && expected <= 'Z')
            expected = (char)(expected + ('a' - 'A'));
        if (value != expected) return false;
        index++;
    }
    return true;
}

static bool explicit_scheme(const char *text)
{
    const char *colon;

    if (!text || !*text || text[0] == '/') return false;
    colon = strchr(text, ':');
    if (!colon || colon[1] != '/' || colon[2] != '/') return false;
    for (const char *cursor = text; cursor < colon; cursor++) {
        u8 value = (u8)*cursor;
        if (!((value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') ||
              (cursor != text && ((value >= '0' && value <= '9') ||
                                  value == '+' || value == '.' ||
                                  value == '-')))) return false;
    }
    return colon != text;
}

static bool ascii_equal_ci(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return *left == *right;
}

void fetch_url_destroy(fetch_url_t *url)
{
    if (!url) return;
    free(url->host);
    free(url->path);
    *url = (fetch_url_t){0};
}

static bool copy_range(const char *begin, usize length, char **out)
{
    char *copy;

    if (!begin || !out || length == (usize)-1) return false;
    copy = (char *)malloc(length + 1U);
    if (!copy) return false;
    memcpy(copy, begin, length);
    copy[length] = '\0';
    *out = copy;
    return true;
}

static bool valid_host(const char *host, usize length)
{
    usize label_length = 0;
    bool trailing_dot = length && host[length - 1U] == '.';

    if (!host || !length || length > 253U) return false;
    for (usize index = 0; index < length; index++) {
        u8 value = (u8)host[index];

        if (value == '.') {
            if (!label_length || label_length > 63U ||
                host[index - 1U] == '-') return false;
            label_length = 0;
        } else if ((value >= 'a' && value <= 'z') ||
                   (value >= 'A' && value <= 'Z') ||
                   (value >= '0' && value <= '9') || value == '-') {
            if (!label_length && value == '-') return false;
            label_length++;
        } else {
            return false;
        }
    }
    if (trailing_dot) return label_length == 0;
    return label_length && label_length <= 63U && host[length - 1U] != '-';
}

static bool valid_url_bytes(const char *text, usize length)
{
    for (usize index = 0; index < length; index++) {
        u8 value = (u8)text[index];
        if (value <= 0x20U || value == 0x7fU) return false;
    }
    return true;
}

fetch_url_parse_result_t fetch_parse_url(const char *text, fetch_url_t *url)
{
    const char *cursor;
    const char *authority;
    const char *authority_end;
    const char *path_start;
    const char *fragment;
    usize host_length;
    u32 port = 80U;
    bool https = false;

    if (!text || !url) return FETCH_URL_PARSE_INVALID;
    *url = (fetch_url_t){0};
    if (starts_with_ci(text, "https://")) {
        https = true;
        port = 443U;
        cursor = text + 8U;
    } else if (starts_with_ci(text, "http://")) {
        cursor = text + 7U;
    } else {
        /* Treat an explicit non-HTTP scheme as unsupported/invalid rather
         * than accidentally interpreting it as a hostname. A colon followed
         * by digits remains a bare-host port. */
        if (explicit_scheme(text))
            return FETCH_URL_PARSE_INVALID;
        cursor = text;
    }
    authority = cursor;
    authority_end = authority;
    while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
           *authority_end != '#' && *authority_end != ':') authority_end++;
    host_length = (usize)(authority_end - authority);
    if (!valid_host(authority, host_length)) return FETCH_URL_PARSE_INVALID;
    cursor = authority_end;
    if (*cursor == ':') {
        u32 digits = 0;
        port = 0;
        cursor++;
        while (*cursor >= '0' && *cursor <= '9') {
            if (++digits > 5U) return FETCH_URL_PARSE_INVALID;
            port = port * 10U + (u32)(*cursor++ - '0');
            if (!port || port > 65535U) return FETCH_URL_PARSE_INVALID;
        }
        if (!digits) return FETCH_URL_PARSE_INVALID;
    }
    if (*cursor && *cursor != '/' && *cursor != '?' && *cursor != '#')
        return FETCH_URL_PARSE_INVALID;
    fragment = strchr(cursor, '#');
    path_start = cursor;
    (void)fragment;
    usize path_length = fragment ? (usize)(fragment - path_start) : strlen(path_start);
    if (!valid_url_bytes(path_start, path_length)) return FETCH_URL_PARSE_INVALID;

    url->host = NULL;
    if (!copy_range(authority, host_length, &url->host))
        return FETCH_URL_PARSE_NO_MEMORY;
    url->port = (u16)port;
    url->https = https;
    if (!path_length || path_start[0] == '#') {
        url->path = (char *)malloc(2U);
        if (url->path) memcpy(url->path, "/", 2U);
    } else if (path_start[0] == '?') {
        usize query_length = path_length;
        url->path = (char *)malloc(query_length + 2U);
        if (url->path) {
            url->path[0] = '/';
            memcpy(url->path + 1U, path_start, query_length);
            url->path[query_length + 1U] = '\0';
        }
    } else {
        url->path = NULL;
        if (!copy_range(path_start, path_length, &url->path)) {
            fetch_url_destroy(url);
            return FETCH_URL_PARSE_NO_MEMORY;
        }
    }
    if (!url->path) {
        fetch_url_destroy(url);
        return FETCH_URL_PARSE_NO_MEMORY;
    }
    return FETCH_URL_PARSE_OK;
}

bool fetch_url_clone(const fetch_url_t *source, fetch_url_t *destination)
{
    if (!source || !destination || !source->host || !source->path)
        return false;
    *destination = (fetch_url_t){0};
    if (!copy_range(source->host, strlen(source->host), &destination->host) ||
        !copy_range(source->path, strlen(source->path), &destination->path)) {
        fetch_url_destroy(destination);
        return false;
    }
    destination->port = source->port;
    destination->https = source->https;
    return true;
}

bool fetch_url_equal(const fetch_url_t *left, const fetch_url_t *right)
{
    return left && right && left->host && right->host && left->path &&
        right->path && left->port == right->port &&
        left->https == right->https &&
        ascii_equal_ci(left->host, right->host) &&
        !strcmp(left->path, right->path);
}

bool fetch_url_redirect_allowed(const fetch_url_t *base,
                               const fetch_url_t *redirect)
{
    return base && redirect && base->host && redirect->host &&
           (!base->https || redirect->https);
}

static bool normalize_path(const char *path, usize length, char **out_path)
{
    typedef struct path_segment {
        usize offset;
        usize length;
    } path_segment_t;
    char *output;
    path_segment_t *segments;
    usize depth = 0;
    usize used = 1U;
    usize query_at = length;
    bool trailing_slash = false;
    usize path_end;
    usize cursor = 1U;

    if (!path || !out_path || !length || path[0] != '/') return false;
    for (usize index = 0; index < length; index++)
        if (path[index] == '?' && query_at == length) query_at = index;
    if (length > (usize)-1 - 2U ||
        length + 2U > (usize)-1 / sizeof(*segments))
        return false;
    output = (char *)malloc(length + 2U);
    segments = (path_segment_t *)malloc((length + 2U) * sizeof(*segments));
    if (!output || !segments) {
        free(output);
        free(segments);
        return false;
    }
    output[0] = '/';
    path_end = query_at;
    if (path_end > 1U && path[path_end - 1U] == '/') {
        trailing_slash = true;
        path_end--;
    }
    while (cursor < path_end) {
        usize start = cursor;
        usize segment_length;
        while (cursor < path_end && path[cursor] != '/') cursor++;
        segment_length = cursor - start;
        if (segment_length == 1U && path[start] == '.') {
            trailing_slash = true;
        } else if (segment_length == 2U && path[start] == '.' &&
                   path[start + 1U] == '.') {
            if (depth) depth--;
            trailing_slash = true;
        } else {
            segments[depth++] = (path_segment_t){start, segment_length};
            trailing_slash = false;
        }
        if (cursor < path_end) cursor++;
    }
    for (usize index = 0; index < depth; index++) {
        if (index) output[used++] = '/';
        memcpy(output + used, path + segments[index].offset,
               segments[index].length);
        used += segments[index].length;
    }
    if (trailing_slash && used > 1U) output[used++] = '/';
    if (query_at < length) {
        memcpy(output + used, path + query_at, length - query_at);
        used += length - query_at;
    }
    output[used] = '\0';
    free(segments);
    *out_path = output;
    return true;
}

static fetch_url_parse_result_t compose_url(const fetch_url_t *base,
                                             const char *path,
                                             fetch_url_t *out)
{
    usize host_length;
    usize path_length;
    char port_text[8];
    usize port_length = 0;
    u32 port;
    char *text;
    usize total;
    usize at = 0;
    const char *scheme;
    usize scheme_length;

    if (!base || !base->host || !path || !out) return FETCH_URL_PARSE_INVALID;
    host_length = strlen(base->host);
    path_length = strlen(path);
    port = base->port;
    if (port != (base->https ? 443U : 80U)) {
        char reversed[5];
        usize count = 0;
        do {
            reversed[count++] = (char)('0' + port % 10U);
            port /= 10U;
        } while (port && count < sizeof(reversed));
        while (count) port_text[port_length++] = reversed[--count];
    }
    scheme = base->https ? "https://" : "http://";
    scheme_length = base->https ? 8U : 7U;
    if (host_length > (usize)-1 - scheme_length)
        return FETCH_URL_PARSE_INVALID;
    total = scheme_length + host_length;
    if (port_length) {
        if (total > (usize)-1 - port_length - 1U)
            return FETCH_URL_PARSE_INVALID;
        total += port_length + 1U;
    }
    if (total > (usize)-1 - path_length) return FETCH_URL_PARSE_INVALID;
    total += path_length;
    if (total == (usize)-1) return FETCH_URL_PARSE_INVALID;
    text = (char *)malloc(total + 1U);
    if (!text) return FETCH_URL_PARSE_NO_MEMORY;
    memcpy(text + at, scheme, scheme_length); at += scheme_length;
    memcpy(text + at, base->host, host_length); at += host_length;
    if (port_length) {
        text[at++] = ':';
        memcpy(text + at, port_text, port_length); at += port_length;
    }
    memcpy(text + at, path, path_length); at += path_length;
    text[at] = '\0';
    fetch_url_parse_result_t result = fetch_parse_url(text, out);
    free(text);
    return result;
}

fetch_url_parse_result_t fetch_url_resolve_redirect(
    const fetch_url_t *base, const char *location, fetch_url_t *url)
{
    const char *fragment;
    usize location_length;
    char *path = NULL;
    fetch_url_parse_result_t result;

    if (!base || !base->host || !base->path || !location || !url)
        return FETCH_URL_PARSE_INVALID;
    *url = (fetch_url_t){0};
    if (starts_with_ci(location, "http://") || starts_with_ci(location, "https://") ||
        explicit_scheme(location)) return fetch_parse_url(location, url);
    if (starts_with(location, "//")) {
        usize length = strlen(location);
        char *absolute;
        const char *scheme = base->https ? "https:" : "http:";
        usize scheme_length = base->https ? 6U : 5U;
        if (length > (usize)-1 - scheme_length - 1U)
            return FETCH_URL_PARSE_INVALID;
        absolute = (char *)malloc(length + scheme_length + 1U);
        if (!absolute) return FETCH_URL_PARSE_NO_MEMORY;
        memcpy(absolute, scheme, scheme_length);
        memcpy(absolute + scheme_length, location, length + 1U);
        result = fetch_parse_url(absolute, url);
        free(absolute);
        return result;
    }
    location_length = strlen(location);
    fragment = memchr(location, '#', location_length);
    if (fragment) location_length = (usize)(fragment - location);
    if (!location_length) return fetch_url_clone(base, url)
        ? FETCH_URL_PARSE_OK : FETCH_URL_PARSE_NO_MEMORY;
    if (!valid_url_bytes(location, location_length)) return FETCH_URL_PARSE_INVALID;

    usize base_path_length = strlen(base->path);
    const char *base_query = strchr(base->path, '?');
    if (base_query) base_path_length = (usize)(base_query - base->path);
    usize merged_length;
    char *merged;

    if (location[0] == '/') {
        merged_length = location_length;
        merged = NULL;
        if (!copy_range(location, merged_length, &merged))
            return FETCH_URL_PARSE_NO_MEMORY;
    } else if (location[0] == '?') {
        if (base_path_length > (usize)-1 - location_length - 1U)
            return FETCH_URL_PARSE_INVALID;
        merged_length = base_path_length + location_length;
        merged = (char *)malloc(merged_length + 1U);
        if (!merged) return FETCH_URL_PARSE_NO_MEMORY;
        memcpy(merged, base->path, base_path_length);
        memcpy(merged + base_path_length, location, location_length);
        merged[merged_length] = '\0';
    } else {
        usize directory_length = base_path_length;
        while (directory_length && base->path[directory_length - 1U] != '/')
            directory_length--;
        if (directory_length > (usize)-1 - location_length - 1U)
            return FETCH_URL_PARSE_INVALID;
        merged_length = directory_length + location_length;
        merged = (char *)malloc(merged_length + 1U);
        if (!merged) return FETCH_URL_PARSE_NO_MEMORY;
        memcpy(merged, base->path, directory_length);
        memcpy(merged + directory_length, location, location_length);
        merged[merged_length] = '\0';
    }
    if (!normalize_path(merged, merged_length, &path)) {
        free(merged);
        return FETCH_URL_PARSE_NO_MEMORY;
    }
    free(merged);
    result = compose_url(base, path, url);
    free(path);
    return result;
}

bool fetch_url_filename(const fetch_url_t *url, char **filename)
{
    usize path_length = 0;
    usize start;
    usize length;

    if (!url || !url->path || !filename) return false;
    *filename = NULL;
    while (url->path[path_length] && url->path[path_length] != '?')
        path_length++;
    if (!path_length || url->path[path_length - 1U] == '/')
        return copy_range("index.html", sizeof("index.html") - 1U, filename);
    start = path_length;
    while (start && url->path[start - 1U] != '/') start--;
    length = path_length - start;
    if (!length || (length == 1U && url->path[start] == '.') ||
        (length == 2U && url->path[start] == '.' &&
         url->path[start + 1U] == '.')) return false;
    for (usize index = 0; index < length; index++) {
        u8 value = (u8)url->path[start + index];
        if (value == '/' || value == '\\' || value <= 0x20U ||
            value == 0x7fU) return false;
    }
    return copy_range(url->path + start, length, filename);
}
