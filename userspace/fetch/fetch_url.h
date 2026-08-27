#pragma once

#include <mg/types.h>

#define FETCH_HOST_MAX 128U
#define FETCH_PATH_MAX 512U
#define FETCH_FILENAME_MAX 256U

typedef struct {
    char host[FETCH_HOST_MAX];
    char path[FETCH_PATH_MAX];
    u16 port;
} fetch_url_t;

typedef enum fetch_url_parse_result {
    FETCH_URL_PARSE_OK = 0,
    FETCH_URL_PARSE_HTTPS_UNSUPPORTED,
    FETCH_URL_PARSE_INVALID,
} fetch_url_parse_result_t;

fetch_url_parse_result_t fetch_parse_url(const char *text, fetch_url_t *url);
bool fetch_url_filename(const fetch_url_t *url, char *filename, usize capacity);
