#include "fetch_url.h"

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
