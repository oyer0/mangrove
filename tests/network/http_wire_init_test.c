/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>

#include <net/http.h>

#include "../../kernel/src/net/http_wire.c"

int main(void)
{
    http_parser_t parser;
    http_response_t response;

    memset(&parser, 0xa5, sizeof(parser));
    memset(&response, 0xa5, sizeof(response));
    http_parser_init(&parser, &response);
    assert(parser.response == &response);
    assert(parser.state == 0U);
    assert(parser.line_length == 0U && parser.content_length == 0U);
    assert(parser.body_received == 0U && parser.chunk_remaining == 0U);
    assert(!parser.have_content_length && !parser.have_transfer_encoding);
    assert(!parser.saw_response && parser.chunk_crlf == 0U);
    assert(response.major == 0U && response.minor == 0U);
    assert(response.status_code == 0U && response.body_length == 0U);
    assert(!response.chunked && !response.connection_close);
    assert(response.content_type[0] == '\0');
    for (usize index = 0; index < sizeof(response.body); index++)
        assert(response.body[index] == 0U);
    for (usize index = 0; index < sizeof(parser.line); index++)
        assert(parser.line[index] == 0U);
    return 0;
}
