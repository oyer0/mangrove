/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../userspace/fetch/fetch_url.h"

static void expect_http(const char *text, const char *host, u16 port,
                       const char *path, fetch_url_t *url)
{
    assert(url);
    assert(fetch_parse_url(text, url) == FETCH_URL_PARSE_OK);
    assert(!strcmp(url->host, host));
    assert(url->port == port);
    assert(!strcmp(url->path, path));
}

int main(void)
{
    fetch_url_t url;
    char filename[FETCH_FILENAME_MAX];

    expect_http("http://example.com", "example.com", 80U, "/", &url);
    expect_http("http://example.com/", "example.com", 80U, "/", &url);
    expect_http("http://example.com/files/page.txt?download=1",
                "example.com", 80U, "/files/page.txt?download=1", &url);
    expect_http("http://example.com:8080/path", "example.com", 8080U,
                "/path", &url);
    expect_http("http://192.0.2.25", "192.0.2.25", 80U, "/", &url);

    expect_http("http://example.com", "example.com", 80U, "/", &url);
    assert(fetch_url_filename(&url, filename, sizeof(filename)));
    assert(!strcmp(filename, "index.html"));
    expect_http("http://example.com/files/page.txt?download=1",
                "example.com", 80U, "/files/page.txt?download=1", &url);
    assert(fetch_url_filename(&url, filename, sizeof(filename)));
    assert(!strcmp(filename, "page.txt"));
    expect_http("http://example.com", "example.com", 80U, "/", &url);
    assert(fetch_url_filename(&url, filename, sizeof(filename)));
    assert(!strcmp(filename, "index.html"));

    assert(fetch_parse_url("https://example.com", &url) ==
           FETCH_URL_PARSE_HTTPS_UNSUPPORTED);
    assert(fetch_parse_url("ftp://example.com", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://", &url) == FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http:///path", &url) == FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com:", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com:0", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com:65536", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com:80suffix", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com bad", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com/path with-space", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com/path\twith-tab", &url) ==
           FETCH_URL_PARSE_INVALID);
    assert(fetch_parse_url("http://example.com/path\177", &url) ==
           FETCH_URL_PARSE_INVALID);
    puts("fetch URL parsing: default paths, explicit paths, hostnames, IPv4 and malformed URLs passed");
    return 0;
}
