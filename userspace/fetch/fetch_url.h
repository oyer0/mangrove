/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>

typedef struct {
    char *host;
    char *path;
    u16 port;
    bool https;
} fetch_url_t;

typedef enum fetch_url_parse_result {
    FETCH_URL_PARSE_OK = 0,
    FETCH_URL_PARSE_INVALID,
    FETCH_URL_PARSE_NO_MEMORY,
} fetch_url_parse_result_t;

fetch_url_parse_result_t fetch_parse_url(const char *text, fetch_url_t *url);
void fetch_url_destroy(fetch_url_t *url);
bool fetch_url_clone(const fetch_url_t *source, fetch_url_t *destination);
bool fetch_url_equal(const fetch_url_t *left, const fetch_url_t *right);
/* HTTPS redirects may remain HTTPS, but never downgrade to HTTP. */
bool fetch_url_redirect_allowed(const fetch_url_t *base,
                               const fetch_url_t *redirect);
fetch_url_parse_result_t fetch_url_resolve_redirect(
    const fetch_url_t *base, const char *location, fetch_url_t *url);
bool fetch_url_filename(const fetch_url_t *url, char **filename);
