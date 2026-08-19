#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <net/http.h>

static void feed_fragments(http_parser_t *parser, const char *text, size_t split)
{
    size_t length = strlen(text);
    assert(http_parser_feed(parser, (const unsigned char *)text, split));
    assert(http_parser_feed(parser, (const unsigned char *)text + split, length - split));
}

int main(void)
{
    http_url_t url;
    http_response_t response;
    http_parser_t parser;
    const char *body = "hello from the web";
    const char *chunked =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n"
        "5\r\nhello\r\n" "9;foo=bar\r\n from web\r\n" "0\r\nX-Trailer: yes\r\n\r\n";
    assert(http_parse_url("http://example.com/", &url));
    assert(url.port == 80 && strcmp(url.hostname, "example.com") == 0 && strcmp(url.path, "/") == 0);
    assert(http_parse_url("http://example.com:8080/a/b", &url));
    assert(url.port == 8080 && strcmp(url.path, "/a/b") == 0);
    assert(!http_parse_url("https://example.com/", &url));

    http_parser_init(&parser, &response);
    feed_fragments(&parser,
        "HTTP/1.1 200 OK\r\ncontent-length: 18\r\nContent-TYPE: text/plain\r\n\r\nhello from the web", 23);
    assert(http_parser_complete(&parser));
    assert(response.status_code == 200 && response.body_length == 18);
    assert(memcmp(response.body, body, 18) == 0);
    assert(strcmp(response.content_type, "text/plain") == 0);

    http_parser_init(&parser, &response);
    assert(http_parser_feed(&parser, (const unsigned char *)chunked, strlen(chunked)));
    assert(http_parser_complete(&parser));
    assert(response.chunked && response.body_length == 14);
    assert(memcmp(response.body, "hello from web", 14) == 0);

    http_parser_init(&parser, &response);
    assert(http_parser_feed(&parser,
        (const unsigned char *)"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort",
        strlen("HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort")));
    assert(!http_parser_finish(&parser));
    puts("HTTP tests passed");
    return 0;
}
