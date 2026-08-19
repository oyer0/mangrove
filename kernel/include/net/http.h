#pragma once

#include <net/net.h>

#define HTTP_BODY_MAX 32768U
#define HTTP_HOST_MAX 128U
#define HTTP_PATH_MAX 512U
#define HTTP_CONTENT_TYPE_MAX 96U

typedef enum {
    HTTP_STATUS_SUCCESS = 0,
    HTTP_STATUS_INVALID_URL,
    HTTP_STATUS_UNSUPPORTED_SCHEME,
    HTTP_STATUS_DNS_FAILURE,
    HTTP_STATUS_TCP_FAILURE,
    HTTP_STATUS_TIMEOUT,
    HTTP_STATUS_MALFORMED,
    HTTP_STATUS_TOO_LARGE,
    HTTP_STATUS_UNSUPPORTED_ENCODING,
    HTTP_STATUS_NO_RESPONSE
} http_result_t;

typedef struct {
    u16 port;
    char hostname[HTTP_HOST_MAX];
    char path[HTTP_PATH_MAX];
} http_url_t;

typedef struct {
    u8 major;
    u8 minor;
    u16 status_code;
    u8 body[HTTP_BODY_MAX];
    usize body_length;
    char content_type[HTTP_CONTENT_TYPE_MAX];
    bool chunked;
    bool connection_close;
} http_response_t;

typedef struct {
    http_response_t *response;
    u8 state;
    u8 line[1024];
    usize line_length;
    usize content_length;
    usize body_received;
    usize chunk_remaining;
    bool have_content_length;
    bool have_transfer_encoding;
    bool saw_response;
    u8 chunk_crlf;
} http_parser_t;

bool http_parse_url(const char *url, http_url_t *parsed);
void http_parser_init(http_parser_t *parser, http_response_t *response);
bool http_parser_feed(http_parser_t *parser, const u8 *data, usize length);
bool http_parser_finish(http_parser_t *parser);
bool http_parser_complete(const http_parser_t *parser);

bool http_get(net_device_t *device, const char *url,
              http_response_t *response, http_result_t *result_out);
