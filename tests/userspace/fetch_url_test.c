/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../userspace/fetch/fetch_url.h"

static void expect_url(const char *text, const char *host, u16 port,
                       const char *path, bool https)
{
    fetch_url_t url;
    assert(fetch_parse_url(text, &url) == FETCH_URL_PARSE_OK);
    assert(!strcmp(url.host, host));
    assert(url.port == port);
    assert(!strcmp(url.path, path));
    assert(url.https == https);
    fetch_url_destroy(&url);
}

static void expect_redirect(const fetch_url_t *base, const char *location,
                            const char *host, u16 port, const char *path,
                            bool https)
{
    fetch_url_t url;
    assert(fetch_url_resolve_redirect(base, location, &url) ==
           FETCH_URL_PARSE_OK);
    assert(!strcmp(url.host, host));
    assert(url.port == port);
    assert(url.https == https);
    if (strcmp(url.path, path))
        fprintf(stderr, "redirect %s -> got %s, expected %s\n",
                location, url.path, path);
    assert(!strcmp(url.path, path));
    fetch_url_destroy(&url);
}

int main(void)
{
    fetch_url_t url;
    char *filename = NULL;

    expect_url("http://example.com", "example.com", 80U, "/", false);
    expect_url("HTTP://example.com", "example.com", 80U, "/", false);
    expect_url("example.com", "example.com", 80U, "/", false);
    expect_url("example.com:8080", "example.com", 8080U, "/", false);
    expect_url("https://example.com", "example.com", 443U, "/", true);
    expect_url("HTTPS://example.com:8443/a", "example.com", 8443U,
               "/a", true);
    expect_url("example.com/files/page.txt?download=1#section",
               "example.com", 80U, "/files/page.txt?download=1", false);
    expect_url("http://192.0.2.25", "192.0.2.25", 80U, "/", false);

    assert(fetch_parse_url("https://192.0.2.25/a", &url) ==
           FETCH_URL_PARSE_OK);
    assert(url.https && !strcmp(url.host, "192.0.2.25") && url.port == 443U);
    fetch_url_destroy(&url);
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

    assert(fetch_parse_url("http://example.com/a/b/file.txt?download=1",
                           &url) == FETCH_URL_PARSE_OK);
    assert(fetch_url_filename(&url, &filename));
    assert(!strcmp(filename, "file.txt"));
    free(filename);
    filename = NULL;
    fetch_url_destroy(&url);
    assert(fetch_parse_url("example.com", &url) == FETCH_URL_PARSE_OK);
    assert(fetch_url_filename(&url, &filename));
    assert(!strcmp(filename, "index.html"));
    free(filename);
    fetch_url_destroy(&url);

    assert(fetch_parse_url("http://example.com/a/b/index", &url) ==
           FETCH_URL_PARSE_OK);
    expect_redirect(&url, "../next?q=1", "example.com", 80U,
                    "/a/next?q=1", false);
    expect_redirect(&url, "./other", "example.com", 80U,
                    "/a/b/other", false);
    expect_redirect(&url, "/root/path", "example.com", 80U,
                    "/root/path", false);
    expect_redirect(&url, "?new=1", "example.com", 80U,
                    "/a/b/index?new=1", false);
    expect_redirect(&url, "#section", "example.com", 80U,
                    "/a/b/index", false);
    expect_redirect(&url, "//other.example/path", "other.example", 80U,
                    "/path", false);
    expect_redirect(&url, "http://other.example/path", "other.example", 80U,
                    "/path", false);
    expect_redirect(&url, "/a://b", "example.com", 80U, "/a://b", false);
    fetch_url_t loop_target;
    assert(fetch_url_resolve_redirect(&url, "/a/b/index", &loop_target) ==
           FETCH_URL_PARSE_OK);
    assert(fetch_url_equal(&url, &loop_target));
    fetch_url_destroy(&loop_target);
    fetch_url_destroy(&url);

    assert(fetch_parse_url("https://example.com/a/b/index", &url) ==
           FETCH_URL_PARSE_OK);
    fetch_url_t plain_url;
    assert(fetch_parse_url("http://example.com/a/b/index", &plain_url) ==
           FETCH_URL_PARSE_OK);
    assert(!fetch_url_equal(&url, &plain_url));
    fetch_url_destroy(&plain_url);
    expect_redirect(&url, "../next?q=1", "example.com", 443U,
                    "/a/next?q=1", true);
    expect_redirect(&url, "//other.example/path", "other.example", 443U,
                    "/path", true);
    expect_redirect(&url, "http://other.example/path", "other.example", 80U,
                    "/path", false);
    fetch_url_t secure_redirect;
    assert(fetch_url_resolve_redirect(&url, "https://other.example/path",
                                      &secure_redirect) == FETCH_URL_PARSE_OK);
    assert(fetch_url_redirect_allowed(&url, &secure_redirect));
    fetch_url_destroy(&secure_redirect);
    assert(fetch_url_resolve_redirect(&url, "http://other.example/path",
                                      &secure_redirect) == FETCH_URL_PARSE_OK);
    assert(!fetch_url_redirect_allowed(&url, &secure_redirect));
    fetch_url_destroy(&secure_redirect);
    fetch_url_destroy(&url);

    assert(fetch_parse_url("http://example.com", &url) == FETCH_URL_PARSE_OK);
    assert(fetch_url_resolve_redirect(&url, "https://example.com/secure",
                                      &secure_redirect) == FETCH_URL_PARSE_OK);
    assert(fetch_url_redirect_allowed(&url, &secure_redirect));
    fetch_url_destroy(&secure_redirect);
    fetch_url_destroy(&url);
    puts("fetch URLs: HTTP/HTTPS schemes, ports and redirect policy passed");
    return 0;
}
