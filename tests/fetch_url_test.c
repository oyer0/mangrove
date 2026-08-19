#include <assert.h>
#include <string.h>
#include "userspace/fetch/fetch_url.h"

int main(void)
{
    fetch_url_t url;
    assert(fetch_parse_url("http://example.com/", &url) ==
           FETCH_URL_PARSE_OK);
    assert(strcmp(url.host, "example.com") == 0 && strcmp(url.path, "/") == 0 && url.port == 80);
    assert(fetch_parse_url("http://example.com:8080/a/b", &url) ==
           FETCH_URL_PARSE_OK);
    assert(url.port == 8080 && strcmp(url.path, "/a/b") == 0);
    assert(fetch_parse_url("https://xoyaa.com", &url) ==
           FETCH_URL_PARSE_HTTPS_UNSUPPORTED);
    assert(strcmp(url.host, "xoyaa.com") == 0 && strcmp(url.path, "/") == 0 && url.port == 443);
    assert(fetch_parse_url("https:///bad", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("https:/xoyaa.com", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http:///bad", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com:0/", &url) ==
           FETCH_URL_PARSE_INVALID);
    return 0;
}
