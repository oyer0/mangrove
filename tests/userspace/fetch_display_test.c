/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../userspace/fetch/fetch_display.h"

typedef struct capture {
    char *text;
    usize length;
    usize capacity;
} capture_t;

static void capture_write(void *opaque, const char *text, usize length)
{
    capture_t *capture = (capture_t *)opaque;
    if (!capture || !text || capture->length == (usize)-1 ||
        length > (usize)-1 - capture->length - 1U)
        return;
    usize needed = capture->length + length + 1U;
    if (needed > capture->capacity) {
        usize next = capture->capacity ? capture->capacity : 128U;
        while (next < needed) {
            if (next > (usize)-1 / 2U) return;
            next *= 2U;
        }
        char *grown = (char *)realloc(capture->text, next);
        if (!grown) return;
        capture->text = grown;
        capture->capacity = next;
    }
    memcpy(capture->text + capture->length, text, length);
    capture->length += length;
    capture->text[capture->length] = '\0';
}

static void test_lifecycle_and_authentication_order(void)
{
    capture_t output = {0};
    fetch_display_t display;
    fetch_url_t url = {.host = "secure.example", .path = "/manual",
                       .port = 443U, .https = true};
    mg_ipv4_addr_t address = {{192U, 0U, 2U, 24U}};
    fetch_http_headers_t headers = {.status = 200U,
        .has_content_length = true, .content_length = 1536U,
        .content_type = "text/html; charset=utf-8"};

    fetch_display_init(&display, false, false, 0U, capture_write, &output);
    fetch_display_requested(&display, &url);
    fetch_display_resolving(&display, url.host, false);
    fetch_display_resolved(&display, url.host, &address);
    fetch_display_connected(&display, url.host, url.port, &address);
    fetch_display_tls_starting(&display, url.host);
    assert(output.text && !strstr(output.text, "authenticated"));
    assert(strstr(output.text, "TLS 1.3: verifying secure.example..."));
    fetch_display_tls_verified(&display);
    fetch_display_http_status(&display, headers.status);
    fetch_display_response_details(&display, &headers, "manual");
    fetch_display_complete(&display, "manual", 1536U, 1200U, true);

    assert(strstr(output.text, "Requested URL: https://secure.example/manual"));
    assert(strstr(output.text, "DNS: secure.example -> 192.0.2.24"));
    assert(strstr(output.text, "TCP: connected to secure.example:443"));
    assert(!strstr(output.text, "Connecting TCP"));
    assert(strstr(output.text,
                  "TLS 1.3: authenticated (certificate chain and hostname verified)"));
    assert(strstr(output.text, "HTTP: status 200"));
    assert(strstr(output.text, "HTTP: Content-Length: 1.5 KiB"));
    assert(strstr(output.text, "HTTP: Content-Type: text/html; charset=utf-8"));
    assert(strstr(output.text, "Destination: manual"));
    assert(strstr(output.text,
                  "Download complete: manual (1.5 KiB saved; response transfer phase 1.2 s"));
    assert(!strchr(output.text, '\r') && !strstr(output.text, "\033["));
    free(output.text);
}

static void test_redirect_and_in_place_progress(void)
{
    capture_t output = {0};
    fetch_display_t display;
    fetch_url_t target = {.host = "cdn.example", .path = "/asset.bin",
                          .port = 443U, .https = true};

    fetch_display_init(&display, false, true, 80U, capture_write, &output);
    fetch_display_redirect(&display, 302U, &target);
    usize redirect_length = output.length;
    fetch_display_body_start(&display, true, 8192U, 100U);
    fetch_display_progress(&display, 4096U, 599U);
    assert(output.length == redirect_length); /* Strict 500 ms cadence. */
    fetch_display_progress(&display, 4096U, 600U);
    assert(output.text && output.text[redirect_length] == '\r');
    assert(strstr(output.text, "[ 50%]"));
    usize first_length = output.length;
    fetch_display_progress(&display, 8192U, 1100U);
    assert(output.length > first_length && strstr(output.text, "[100%]"));
    fetch_display_finish_progress(&display);
    assert(output.text[output.length - 1U] == '\n');
    assert(strstr(output.text,
                  "HTTP: redirect 302 -> https://cdn.example/asset.bin"));
    assert(!strstr(output.text, "HTTP: status 302"));
    free(output.text);
}

static void test_unknown_length_quiet_and_small_summary(void)
{
    capture_t output = {0};
    fetch_display_t display;

    fetch_display_init(&display, false, true, 80U, capture_write, &output);
    fetch_display_body_start(&display, false, 0U, 10U);
    fetch_display_progress(&display, 2048U, 510U);
    assert(output.text && strstr(output.text, "[  ?]"));
    assert(strstr(output.text, "2.0 KiB received"));
    fetch_display_finish_progress(&display);
    free(output.text);

    output = (capture_t){0};
    fetch_display_init(&display, false, false, 0U, capture_write, &output);
    fetch_display_complete(&display, "tiny", 13U, 3U, true);
    assert(output.text && strstr(output.text,
        "13 B saved; response transfer phase 3 ms"));
    assert(!strstr(output.text, "KiB/s"));
    free(output.text);

    output = (capture_t){0};
    fetch_display_init(&display, true, true, 80U, capture_write, &output);
    fetch_display_requested(&display, &(fetch_url_t){
        .host = "example.com", .path = "/", .port = 80U});
    fetch_display_complete(&display, "quiet", 99U, 20U, true);
    assert(output.length == 0U);
    free(output.text);
}

static void test_progress_error_transition_and_noninteractive_output(void)
{
    capture_t output = {0};
    fetch_display_t display;

    fetch_display_init(&display, false, true, 80U, capture_write, &output);
    fetch_display_body_start(&display, true, 8192U, 10U);
    fetch_display_progress(&display, 4096U, 510U);
    assert(output.text && strchr(output.text, '\r'));
    fetch_display_finish_progress(&display);
    static const char error_line[] = "fetch: transfer failed\n";
    capture_write(&output, error_line, sizeof(error_line) - 1U);
    assert(strstr(output.text, "\nfetch: transfer failed\n"));
    free(output.text);

    output = (capture_t){0};
    fetch_display_init(&display, false, false, 0U, capture_write, &output);
    fetch_display_body_start(&display, true, 8192U, 10U);
    fetch_display_progress(&display, 4096U, 510U);
    assert(!output.length || !strchr(output.text, '\r'));
    free(output.text);
}

static void test_progress_never_wraps_terminal_width(void)
{
    capture_t output = {0};
    fetch_display_t display;

    fetch_display_init(&display, false, true, 40U, capture_write, &output);
    fetch_display_body_start(&display, true, 8192U, 0U);
    fetch_display_progress(&display, 4096U, 500U);
    assert(output.text && strchr(output.text, '\r'));
    assert(!strchr(output.text, '\n'));
    assert(output.length <= 40U);
    usize fitting_length = output.length;
    usize rendered_length = fitting_length - 1U;

    /* A line that fills every cell would leave the terminal at its right
     * margin. Skip that update so finishing progress cannot trigger a wrap. */
    output = (capture_t){0};
    fetch_display_init(&display, false, true, (u32)rendered_length,
                       capture_write, &output);
    fetch_display_body_start(&display, true, 8192U, 0U);
    fetch_display_progress(&display, 4096U, 500U);
    assert(output.length == 0U);
    free(output.text);

    /* A larger amount/rate would wrap at 40 columns; retain the last complete
     * progress line rather than writing a partial or wrapped update. */
    output = (capture_t){0};
    fetch_display_init(&display, false, true, 40U, capture_write, &output);
    fetch_display_body_start(&display, true, 8192U, 0U);
    fetch_display_progress(&display, 4096U, 500U);
    assert(output.length == fitting_length);
    fetch_display_progress(&display, 4ULL * 1024U * 1024U * 1024U * 1024U,
                           1000U);
    assert(output.length == fitting_length);
    fetch_display_finish_progress(&display);
    assert(output.text[output.length - 1U] == '\n');
    free(output.text);

    output = (capture_t){0};
    fetch_display_init(&display, false, true, 40U, capture_write, &output);
    fetch_display_body_start(&display, true, ~(u64)0, 0U);
    fetch_display_progress(&display, 4096U, 500U);
    assert(output.length == 0U);
    free(output.text);
}

static void test_explicit_timing_sections(void)
{
    capture_t output = {0};
    fetch_display_t display;
    fetch_display_timings_t timings = {
        .dns_ms = 12U, .connect_ms = 34U, .tls_context_ms = 56U,
        .tls_handshake_ms = 78U, .first_byte_ms = 90U,
        .transfer_ms = 123U, .receive_ms = 45U, .write_ms = 6U,
        .publish_ms = 7U, .clock_available = true,
        .transfer_clock_available = true,
    };

    fetch_display_init(&display, false, false, 0U, capture_write, &output);
    fetch_display_timings(&display, &timings);
    assert(output.text && strstr(output.text, "Timing details"));
    assert(strstr(output.text, "DNS 12; TCP connect 34"));
    assert(strstr(output.text, "TLS handshake/authentication 78"));
    assert(strstr(output.text, "Response transfer 123 ms"));
    assert(strstr(output.text, "Receive wait 45"));
    assert(strstr(output.text, "publish 7"));
    assert(!strchr(output.text, '\r') && !strstr(output.text, "KiB/s"));
    free(output.text);

    output = (capture_t){0};
    fetch_display_init(&display, true, false, 0U, capture_write, &output);
    fetch_display_timings(&display, &timings);
    assert(output.length == 0U);
    free(output.text);
}

static void test_long_url_not_truncated(void)
{
    capture_t output = {0};
    fetch_display_t display;
    char *path = (char *)malloc(2049U);
    assert(path);
    path[0] = '/';
    memset(path + 1U, 'a', 2047U);
    path[2048U] = '\0';
    fetch_url_t url = {.host = "example.com", .path = path, .port = 80U};
    fetch_display_init(&display, false, false, 0U, capture_write, &output);
    fetch_display_requested(&display, &url);
    assert(output.length > 2048U);
    assert(output.text[output.length - 1U] == '\n');
    assert(strstr(output.text, path));
    free(path);
    free(output.text);
}

int main(void)
{
    test_lifecycle_and_authentication_order();
    test_redirect_and_in_place_progress();
    test_unknown_length_quiet_and_small_summary();
    test_progress_error_transition_and_noninteractive_output();
    test_progress_never_wraps_terminal_width();
    test_explicit_timing_sections();
    test_long_url_not_truncated();
    puts("fetch display: lifecycle, auth ordering, metadata, progress and quiet output passed");
    return 0;
}
