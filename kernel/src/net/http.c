#include <net/dns.h>
#include <net/http.h>
#include <net/net.h>
#include <net/tcp.h>
#include <timer.h>

#define HTTP_REQUEST_MAX 2048U
#define HTTP_WAIT_TICKS 15000U

static bool http_ipv4_literal(const char *text, net_ipv4_t *address)
{
    usize index = 0;
    if (!text || !address) return false;
    for (u32 octet = 0; octet < 4; octet++) {
        usize value = 0;
        usize digits = 0;
        while (text[index] >= '0' && text[index] <= '9') {
            if (value > 25U || (value == 25U && text[index] > '5')) return false;
            value = value * 10U + (usize)(text[index++] - '0');
            if (++digits > 3) return false;
        }
        if (!digits || value > 255U) return false;
        address->octet[octet] = (u8)value;
        if (octet != 3) {
            if (text[index++] != '.') return false;
        } else if (text[index]) return false;
    }
    return true;
}

static bool http_append(char *buffer, usize capacity, usize *length,
                        const char *text)
{
    usize text_length = 0;
    while (text[text_length]) text_length++;
    if (*length > capacity || text_length > capacity - *length) return false;
    for (usize i = 0; i < text_length; i++) buffer[*length + i] = text[i];
    *length += text_length;
    return true;
}

static bool http_append_dec(char *buffer, usize capacity, usize *length, u16 value)
{
    char digits[6];
    usize count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value = (u16)(value / 10U);
    } while (value);
    if (*length > capacity || count > capacity - *length) return false;
    while (count) buffer[(*length)++] = digits[--count];
    return true;
}

static bool http_build_request(const http_url_t *url, char *request,
                               usize capacity, usize *length)
{
    *length = 0;
    if (!http_append(request, capacity, length, "GET ") ||
        !http_append(request, capacity, length, url->path) ||
        !http_append(request, capacity, length, " HTTP/1.1\r\nHost: ") ||
        !http_append(request, capacity, length, url->hostname)) return false;
    if (url->port != 80 &&
        (!http_append(request, capacity, length, ":") ||
         !http_append_dec(request, capacity, length, url->port))) return false;
    return http_append(request, capacity, length,
                       "\r\nUser-Agent: Mangrove/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n");
}

bool http_get(net_device_t *device, const char *url,
              http_response_t *response, http_result_t *result_out)
{
    http_url_t parsed;
    net_ipv4_t address;
    tcp_connection_t *connection = 0;
    tcp_status_t tcp_status;
    dns_status_t dns_status;
    http_parser_t parser;
    char request[HTTP_REQUEST_MAX];
    usize request_length;
    u8 receive[1024];
    u64 start;
    http_result_t result = HTTP_STATUS_INVALID_URL;
    if (result_out) *result_out = result;
    if (!device || !response || !url) return false;
    if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' &&
        url[4] == 's' && url[5] == ':' && url[6] == '/' && url[7] == '/') {
        result = HTTP_STATUS_UNSUPPORTED_SCHEME;
        if (result_out) *result_out = result;
        return false;
    }
    if (!http_parse_url(url, &parsed)) return false;
    if (http_ipv4_literal(parsed.hostname, &address)) {
        /* The literal parser is deliberately strict; malformed numeric names
         * fall through to DNS only when they contain alphabetic characters. */
    } else if (!dns_resolve_a(device, parsed.hostname, &address, &dns_status)) {
        result = HTTP_STATUS_DNS_FAILURE;
        if (result_out) *result_out = result;
        return false;
    }
    if (!http_build_request(&parsed, request, sizeof(request), &request_length)) {
        result = HTTP_STATUS_INVALID_URL;
        if (result_out) *result_out = result;
        return false;
    }
    if (!tcp_connect(device, address, parsed.port, &connection, &tcp_status)) {
        result = tcp_status == TCP_STATUS_TIMEOUT ? HTTP_STATUS_TIMEOUT : HTTP_STATUS_TCP_FAILURE;
        if (result_out) *result_out = result;
        return false;
    }
    http_parser_init(&parser, response);
    if (!tcp_send(connection, request, request_length, &tcp_status)) {
        (void)tcp_close(connection, &tcp_status);
        result = HTTP_STATUS_TCP_FAILURE;
        if (result_out) *result_out = result;
        return false;
    }
    start = timer_ticks();
    while (!http_parser_complete(&parser) && timer_ticks() - start < HTTP_WAIT_TICKS) {
        usize count = tcp_receive_bytes(connection, receive, sizeof(receive));
        if (count && !http_parser_feed(&parser, receive, count)) {
            (void)tcp_close(connection, &tcp_status);
            result = parser.response->body_length >= HTTP_BODY_MAX ?
                     HTTP_STATUS_TOO_LARGE : HTTP_STATUS_MALFORMED;
            if (result_out) *result_out = result;
            return false;
        }
        if (!count && tcp_connection_state(connection) == TCP_STATE_CLOSE_WAIT) {
            if (!http_parser_finish(&parser)) {
                (void)tcp_close(connection, &tcp_status);
                result = HTTP_STATUS_MALFORMED;
                if (result_out) *result_out = result;
                return false;
            }
        }
        if (!http_parser_complete(&parser)) __asm__ volatile("hlt");
    }
    if (!http_parser_complete(&parser)) {
        (void)tcp_close(connection, &tcp_status);
        result = HTTP_STATUS_TIMEOUT;
        if (result_out) *result_out = result;
        return false;
    }
    (void)tcp_close(connection, &tcp_status);
    result = HTTP_STATUS_SUCCESS;
    if (result_out) *result_out = result;
    return true;
}
