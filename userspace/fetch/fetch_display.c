/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fetch_display.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FETCH_PROGRESS_INTERVAL_MS 500U
#define FETCH_FORMAT_MAX 70000U

static void emit(fetch_display_t *display, const char *text, usize length)
{
    if (display && !display->quiet && display->write && text && length)
        display->write(display->write_context, text, length);
}

static void emit_line(fetch_display_t *display, const char *format, ...)
{
    va_list arguments;
    va_list measure;
    char local[512];
    char *buffer = local;
    int needed;

    if (!display || display->quiet || !display->write || !format) return;
    va_start(arguments, format);
    va_copy(measure, arguments);
    needed = vsnprintf(NULL, 0U, format, measure);
    va_end(measure);
    if (needed < 0 || (usize)needed >= FETCH_FORMAT_MAX) {
        va_end(arguments);
        return;
    }
    if ((usize)needed + 1U > sizeof(local)) {
        buffer = (char *)malloc((usize)needed + 1U);
        if (!buffer) {
            va_end(arguments);
            return;
        }
    }
    if (vsnprintf(buffer, (usize)needed + 1U, format, arguments) == needed) {
        emit(display, buffer, (usize)needed);
        emit(display, "\n", 1U);
    }
    if (buffer != local) free(buffer);
    va_end(arguments);
}

static void format_quantity(char *buffer, usize capacity, u64 value)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    u64 divisor = 1U;
    usize unit = 0U;

    while (unit + 1U < sizeof(units) / sizeof(units[0]) &&
           value / divisor >= 1024U) {
        divisor *= 1024U;
        unit++;
    }
    if (!unit) {
        (void)snprintf(buffer, capacity, "%llu B",
                       (unsigned long long)value);
        return;
    }
    u64 whole = value / divisor;
    u64 tenths = (value % divisor) / (divisor / 10U);
    (void)snprintf(buffer, capacity, "%llu.%llu %s",
                   (unsigned long long)whole,
                   (unsigned long long)tenths, units[unit]);
}

static u64 transfer_rate(u64 bytes, u64 milliseconds)
{
    if (!milliseconds) return 0U;
    u64 whole = bytes / milliseconds;
    u64 remainder = bytes % milliseconds;
    if (whole > ~(u64)0 / 1000U) return ~(u64)0;
    u64 rate = whole * 1000U;
    u64 fraction = 0U;
    u64 accumulator = 0U;
    for (u32 index = 0U; index < 1000U; index++) {
        if (accumulator >= milliseconds - remainder) {
            accumulator -= milliseconds - remainder;
            fraction++;
        } else {
            accumulator += remainder;
        }
    }
    return fraction > ~(u64)0 - rate ? ~(u64)0 : rate + fraction;
}

static void format_rate(char *buffer, usize capacity, u64 bytes_per_second)
{
    if (bytes_per_second < 1024U) {
        (void)snprintf(buffer, capacity, "%llu B/s",
                       (unsigned long long)bytes_per_second);
    } else {
        u64 kib = bytes_per_second / 1024U;
        u64 tenth = (bytes_per_second % 1024U) * 10U / 1024U;
        (void)snprintf(buffer, capacity, "%llu.%llu KiB/s",
                       (unsigned long long)kib,
                       (unsigned long long)tenth);
    }
}

static u32 percentage(u64 bytes, u64 total)
{
    u64 quotient;
    u64 remainder;
    u32 result = 0U;

    if (!total) return 100U;
    if (bytes >= total) return 100U;
    quotient = total / 100U;
    remainder = total % 100U;
    for (u32 candidate = 1U; candidate <= 100U; candidate++) {
        u64 threshold = quotient * candidate +
            (remainder * candidate + 99U) / 100U;
        if (bytes < threshold) break;
        result = candidate;
    }
    return result;
}

void fetch_display_init(fetch_display_t *display, bool quiet,
                        bool interactive_progress,
                        fetch_display_write_fn write, void *write_context)
{
    if (!display) return;
    *display = (fetch_display_t){
        .write = write,
        .write_context = write_context,
        .quiet = quiet,
        .interactive_progress = interactive_progress,
    };
}

void fetch_display_requested(fetch_display_t *display,
                             const fetch_url_t *url)
{
    if (!url || !url->host || !url->path) return;
    if (url->port == (url->https ? 443U : 80U))
        emit_line(display, "Requested URL: %s://%s%s%s", url->https ? "https" : "http",
                  url->host, url->path[0] == '/' ? "" : "/", url->path);
    else
        emit_line(display, "Requested URL: %s://%s:%u%s%s",
                  url->https ? "https" : "http", url->host,
                  (unsigned)url->port, url->path[0] == '/' ? "" : "/",
                  url->path);
}

void fetch_display_resolving(fetch_display_t *display, const char *host,
                             bool address_literal)
{
    if (address_literal)
        emit_line(display, "Address: %s (IPv4 literal; DNS not needed)", host);
    else
        emit_line(display, "Resolving %s...", host);
}

void fetch_display_resolved(fetch_display_t *display, const char *host,
                            const mg_ipv4_addr_t *address)
{
    if (!address) return;
    emit_line(display, "Resolved %s to %u.%u.%u.%u", host,
              (unsigned)address->octet[0], (unsigned)address->octet[1],
              (unsigned)address->octet[2], (unsigned)address->octet[3]);
}

void fetch_display_connecting(fetch_display_t *display, const char *host,
                              u16 port)
{
    emit_line(display, "Connecting TCP to %s:%u...", host,
              (unsigned)port);
}

void fetch_display_connected(fetch_display_t *display, const char *host,
                             u16 port, const mg_ipv4_addr_t *address)
{
    if (!address) return;
    emit_line(display, "TCP connected to %s:%u (%u.%u.%u.%u)", host,
              (unsigned)port, (unsigned)address->octet[0],
              (unsigned)address->octet[1], (unsigned)address->octet[2],
              (unsigned)address->octet[3]);
}

void fetch_display_tls_starting(fetch_display_t *display, const char *host)
{
    emit_line(display, "Negotiating TLS 1.3 and verifying %s...", host);
}

void fetch_display_tls_verified(fetch_display_t *display, const char *host)
{
    emit_line(display,
              "TLS 1.3 authenticated: certificate chain and hostname verified for %s",
              host);
}

void fetch_display_http_status(fetch_display_t *display, u32 status)
{
    emit_line(display, "HTTP response: %u", (unsigned)status);
}

void fetch_display_redirect(fetch_display_t *display, u32 status,
                            const fetch_url_t *url)
{
    if (!url || !url->host || !url->path) return;
    if (url->port == (url->https ? 443U : 80U))
        emit_line(display, "Redirect %u to %s://%s%s%s", (unsigned)status,
                  url->https ? "https" : "http", url->host,
                  url->path[0] == '/' ? "" : "/", url->path);
    else
        emit_line(display, "Redirect %u to %s://%s:%u%s%s",
                  (unsigned)status, url->https ? "https" : "http",
                  url->host, (unsigned)url->port,
                  url->path[0] == '/' ? "" : "/", url->path);
}

void fetch_display_response_details(fetch_display_t *display,
                                    const fetch_http_headers_t *headers,
                                    const char *destination)
{
    char size[48];
    if (!headers) return;
    if (headers->has_content_length) {
        format_quantity(size, sizeof(size), headers->content_length);
        emit_line(display, "Content-Length: %s", size);
    } else if (headers->chunked) {
        emit_line(display, "Response size: not declared (chunked transfer)");
    } else {
        emit_line(display, "Response size: not declared (connection-close delimited)");
    }
    if (headers->content_type)
        emit_line(display, "Content-Type: %s", headers->content_type);
    if (destination)
        emit_line(display, "Destination: %s", destination);
}

void fetch_display_body_start(fetch_display_t *display, bool has_length,
                              u64 content_length, u64 now_ms)
{
    if (!display) return;
    display->progress_has_length = has_length;
    display->progress_length = content_length;
    display->progress_last_time_ms = now_ms;
    display->progress_last_bytes = 0U;
    display->progress_line_length = 0U;
}

void fetch_display_progress(fetch_display_t *display, u64 bytes,
                            u64 now_ms)
{
    char amount[48], total[48], speed[48], line[192];
    int length;
    if (!display || display->quiet || !display->interactive_progress ||
        now_ms < display->progress_last_time_ms ||
        now_ms - display->progress_last_time_ms < FETCH_PROGRESS_INTERVAL_MS)
        return;
    format_quantity(amount, sizeof(amount), bytes);
    format_rate(speed, sizeof(speed), transfer_rate(
        bytes - (bytes >= display->progress_last_bytes
                     ? display->progress_last_bytes : bytes),
        now_ms - display->progress_last_time_ms));
    if (display->progress_has_length) {
        format_quantity(total, sizeof(total), display->progress_length);
        length = snprintf(line, sizeof(line), "[%3u%%] %s / %s  %s",
                          percentage(bytes, display->progress_length), amount,
                          total, speed);
    } else {
        length = snprintf(line, sizeof(line), "[  ?] %s received  %s",
                          amount, speed);
    }
    if (length < 0 || (usize)length >= sizeof(line)) return;
    emit(display, "\r", 1U);
    emit(display, line, (usize)length);
    if ((usize)length < display->progress_line_length) {
        usize padding = display->progress_line_length - (usize)length;
        while (padding--) emit(display, " ", 1U);
    }
    display->progress_line_length = (usize)length > display->progress_line_length
        ? (usize)length : display->progress_line_length;
    display->progress_active = true;
    display->progress_last_time_ms = now_ms;
    display->progress_last_bytes = bytes;
}

void fetch_display_finish_progress(fetch_display_t *display)
{
    if (!display || !display->progress_active) return;
    emit(display, "\n", 1U);
    display->progress_active = false;
    display->progress_line_length = 0U;
}

void fetch_display_complete(fetch_display_t *display, const char *filename,
                            u64 bytes, u64 elapsed_ms, bool have_elapsed)
{
    char amount[48];
    char duration[48];
    char speed[48];
    format_quantity(amount, sizeof(amount), bytes);
    if (!have_elapsed) {
        emit_line(display, "Download complete: %s (%s saved)",
                  filename ? filename : "output", amount);
        return;
    }
    if (elapsed_ms >= 1000U)
        (void)snprintf(duration, sizeof(duration), "%llu.%llu s",
                       (unsigned long long)(elapsed_ms / 1000U),
                       (unsigned long long)((elapsed_ms % 1000U) / 100U));
    else
        (void)snprintf(duration, sizeof(duration), "%llu ms",
                       (unsigned long long)elapsed_ms);
    if (bytes >= 1024U && elapsed_ms >= 100U) {
        format_rate(speed, sizeof(speed), transfer_rate(bytes, elapsed_ms));
        emit_line(display, "Download complete: %s (%s saved in %s, %s)",
                  filename ? filename : "output", amount, duration, speed);
    } else {
        emit_line(display, "Download complete: %s (%s saved in %s)",
                  filename ? filename : "output", amount, duration);
    }
}

void fetch_display_timings(fetch_display_t *display,
                           const fetch_display_timings_t *timings)
{
    if (!timings) return;
    emit_line(display, "Timing details (milliseconds):");
    if (timings->clock_available) {
        emit_line(display, "  DNS %llu; TCP connect %llu; TLS context/trust store %llu; TLS handshake/authentication %llu; first byte %llu; total %llu",
                  (unsigned long long)timings->dns_ms,
                  (unsigned long long)timings->connect_ms,
                  (unsigned long long)timings->tls_context_ms,
                  (unsigned long long)timings->tls_handshake_ms,
                  (unsigned long long)timings->first_byte_ms,
                  (unsigned long long)timings->total_ms);
        if (timings->transfer_clock_available)
            emit_line(display, "  Response transfer %llu ms",
                      (unsigned long long)timings->transfer_ms);
        else
            emit_line(display, "  Response transfer timing unavailable");
        emit_line(display, "  Receive wait %llu; file setup %llu; file writes %llu (%llu calls, maximum %llu); connection close %llu; file close %llu; publish %llu; output check %llu",
                  (unsigned long long)timings->receive_ms,
                  (unsigned long long)timings->file_setup_ms,
                  (unsigned long long)timings->write_ms,
                  (unsigned long long)timings->write_calls,
                  (unsigned long long)timings->write_max_ms,
                  (unsigned long long)timings->connection_close_ms,
                  (unsigned long long)timings->file_close_ms,
                  (unsigned long long)timings->publish_ms,
                  (unsigned long long)timings->output_check_ms);
    } else {
        emit_line(display, "  Monotonic timing is unavailable; stage durations were not measured.");
    }
}
