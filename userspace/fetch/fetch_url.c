#include "fetch_url.h"
#include <string.h>

static bool append(char *out, usize capacity, usize *used, char c)
{
    if (!out || !used || *used + 1 >= capacity) return false;
    out[(*used)++] = c; out[*used] = '\0'; return true;
}

static bool starts_with(const char *text, const char *prefix)
{
    usize i = 0;
    if (!text || !prefix) return false;
    while (prefix[i]) {
        if (text[i] != prefix[i]) return false;
        i++;
    }
    return true;
}

fetch_url_parse_result_t fetch_parse_url(const char *text, fetch_url_t *url)
{
    const char *cursor;
    usize used = 0;
    u32 port;
    fetch_url_parse_result_t result;

    if (!text || !url) return FETCH_URL_PARSE_INVALID;
    if (starts_with(text, "http://")) {
        cursor = text + 7;
        port = 80;
        result = FETCH_URL_PARSE_OK;
    } else if (starts_with(text, "https://")) {
        cursor = text + 8;
        port = 443;
        result = FETCH_URL_PARSE_HTTPS_UNSUPPORTED;
    } else {
        return FETCH_URL_PARSE_INVALID;
    }

    *url = (fetch_url_t){0};
    while (*cursor && *cursor != '/' && *cursor != ':') {
        if ((u8)*cursor <= 0x20 ||
            !append(url->host, sizeof(url->host), &used, *cursor++)) {
            return FETCH_URL_PARSE_INVALID;
        }
    }
    if (!used) return FETCH_URL_PARSE_INVALID;
    if (*cursor == ':') {
        u32 digits = 0; cursor++;
        port = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            port = port * 10U + (u32)(*cursor++ - '0');
            if (++digits > 5 || port == 0 || port > 65535U) {
                return FETCH_URL_PARSE_INVALID;
            }
        }
        if (!digits) return FETCH_URL_PARSE_INVALID;
    }
    url->port = (u16)port;
    if (!*cursor) {
        url->path[0] = '/';
        return result;
    }
    if (*cursor != '/') return FETCH_URL_PARSE_INVALID;
    used = 0;
    while (*cursor) {
        if (!append(url->path, sizeof(url->path), &used, *cursor++)) {
            return FETCH_URL_PARSE_INVALID;
        }
    }
    return used ? result : FETCH_URL_PARSE_INVALID;
}

bool fetch_url_filename(const fetch_url_t *url, char *filename, usize capacity)
{
    const char *path;
    usize path_length = 0;
    usize start;
    usize length;

    if (!url || !filename || capacity == 0) return false;

    path = url->path;
    while (path[path_length] && path[path_length] != '?' &&
           path[path_length] != '#') {
        path_length++;
    }

    /* A directory URL, including one with a query or fragment, names the
     * conventional index file rather than an empty path component. */
    if (path_length == 0 || path[path_length - 1] == '/') {
        if (capacity < sizeof("index.html")) return false;
        memcpy(filename, "index.html", sizeof("index.html"));
        return true;
    }

    start = path_length;
    while (start > 0 && path[start - 1] != '/') start--;
    length = path_length - start;
    if (length == 0 || length >= capacity) return false;
    if ((length == 1 && path[start] == '.') ||
        (length == 2 && path[start] == '.' && path[start + 1] == '.')) {
        return false;
    }

    for (usize i = 0; i < length; i++) {
        u8 c = (u8)path[start + i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return false;
        filename[i] = path[start + i];
    }
    filename[length] = '\0';
    return true;
}
