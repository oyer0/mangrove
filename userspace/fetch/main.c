/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fetch_args.h"
#include "fetch_display.h"
#include "fetch_sink_buffer.h"
#include "fetch_transport.h"
#include "fetch_url.h"
#include "http_response.h"
#include "../common/help.h"

#define FETCH_IO_CHUNK 8192U
#define FETCH_IDLE_TIMEOUT_MS 10000U
#define FETCH_CONNECT_TIMEOUT_MS 5000U
#define FETCH_PROGRESS_INTERVAL_MS 500U
#define FETCH_MAX_REDIRECTS 8U

typedef struct fetch_url_list {
    fetch_url_t *items;
    usize count;
    usize capacity;
} fetch_url_list_t;

typedef struct fetch_metrics {
    u64 dns_ms, connect_ms, tls_context_ms, tls_handshake_ms;
    u64 first_byte_ms, receive_ms, write_ms;
    u64 output_check_ms, file_setup_ms, connection_close_ms;
    u64 file_close_ms, publish_ms;
    u64 write_calls, write_max_ms;
    u64 total_ms;
    u64 transfer_start_ms, transfer_ms;
    bool clock_available;
    bool transfer_clock_available;
} fetch_metrics_t;

typedef struct fetch_sink_context {
    mg_handle_t file;
    fetch_display_t *display;
    bool progress_clock_ready;
    mg_result_t failure;
    u64 bytes;
    u64 last_progress_ms;
    fetch_metrics_t *metrics;
    fetch_sink_buffer_t *buffer;
} fetch_sink_context_t;

static bool monotonic_ms(u64 *out)
{
    mg_monotonic_time_t time;
    if (!out || mg_clock_monotonic(&time) != MG_OK) return false;
    *out = time.milliseconds;
    return true;
}

static void stdout_write(void *context, const char *text, usize length)
{
    (void)context;
    if (!text || !length) return;
    (void)fwrite(text, 1U, length, stdout);
    (void)fflush(stdout);
}

static u64 duration_ms(u64 start, u64 end)
{
    return end >= start ? end - start : 0;
}

static void add_duration(u64 *total, u64 start, u64 end)
{
    u64 duration = duration_ms(start, end);
    *total = duration > ~(u64)0 - *total ? ~(u64)0 : *total + duration;
}

static char *copy_string(const char *text)
{
    usize length;
    char *copy;
    if (!text) return NULL;
    length = strlen(text);
    if (length == (usize)-1) return NULL;
    copy = (char *)malloc(length + 1U);
    if (copy) memcpy(copy, text, length + 1U);
    return copy;
}

static bool url_list_append(fetch_url_list_t *list, const char *text,
                            fetch_url_parse_result_t *out_status)
{
    fetch_url_t url;
    fetch_url_parse_result_t status = fetch_parse_url(text, &url);
    if (out_status) *out_status = status;
    if (status != FETCH_URL_PARSE_OK) return false;
    if (list->count == list->capacity) {
        usize capacity = list->capacity ? list->capacity * 2U : 8U;
        fetch_url_t *grown;
        if (capacity < list->capacity || capacity > (usize)-1 / sizeof(*grown)) {
            fetch_url_destroy(&url);
            if (out_status) *out_status = FETCH_URL_PARSE_NO_MEMORY;
            return false;
        }
        grown = (fetch_url_t *)realloc(list->items,
                                       capacity * sizeof(*grown));
        if (!grown) {
            fetch_url_destroy(&url);
            if (out_status) *out_status = FETCH_URL_PARSE_NO_MEMORY;
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = url;
    return true;
}

static void url_list_destroy(fetch_url_list_t *list)
{
    if (!list) return;
    for (usize index = 0; index < list->count; index++)
        fetch_url_destroy(&list->items[index]);
    free(list->items);
    *list = (fetch_url_list_t){0};
}

static bool line_append(char **line, usize *length, usize *capacity, char byte)
{
    if (*length == *capacity) {
        usize next = *capacity ? *capacity * 2U : 128U;
        char *grown;
        if (next < *capacity) return false;
        grown = (char *)realloc(*line, next);
        if (!grown) return false;
        *line = grown;
        *capacity = next;
    }
    (*line)[(*length)++] = byte;
    return true;
}

static bool finish_input_line(fetch_url_list_t *list, char *line,
                              usize length, u64 line_number)
{
    usize start = 0;
    usize end = length;
    fetch_url_parse_result_t status;
    char *text;

    while (start < end && (line[start] == ' ' || line[start] == '\t' ||
                           line[start] == '\r')) start++;
    while (end > start && (line[end - 1U] == ' ' || line[end - 1U] == '\t' ||
                           line[end - 1U] == '\r')) end--;
    if (start == end || line[start] == '#') return true;
    text = (char *)malloc(end - start + 1U);
    if (!text) {
        printf("fetch: not enough memory while reading URL list line %llu.\n",
               (unsigned long long)line_number);
        return false;
    }
    memcpy(text, line + start, end - start);
    text[end - start] = '\0';
    bool appended = url_list_append(list, text, &status);
    free(text);
    if (!appended) {
        printf("fetch: invalid URL on list line %llu%s.\n",
               (unsigned long long)line_number,
               status == FETCH_URL_PARSE_NO_MEMORY ? ": not enough memory" : "");
        return false;
    }
    return true;
}

static bool read_url_file(const char *path, fetch_url_list_t *list)
{
    mg_result_t opened = file_open(path, MG_OPEN_READ);
    mg_handle_t file;
    u8 block[FETCH_IO_CHUNK];
    char *line = NULL;
    usize length = 0;
    usize capacity = 0;
    u64 line_number = 1U;
    bool success = true;

    if (opened < 0) {
        printf("fetch: could not open URL list \"%s\": %s.\n", path,
               error_string(opened));
        return false;
    }
    file = (mg_handle_t)opened;
    for (;;) {
        mg_result_t got = object_read(file, block, sizeof(block));
        if (got == MG_ERR_END_OF_FILE || got == 0) break;
        if (got < 0 || (u64)got > sizeof(block)) {
            printf("fetch: could not read URL list \"%s\": %s.\n", path,
                   got < 0 ? error_string(got) : "invalid byte count");
            success = false;
            break;
        }
        for (usize index = 0; index < (usize)got; index++) {
            if (!block[index]) {
                printf("fetch: URL list \"%s\" contains a NUL byte.\n", path);
                success = false;
                break;
            }
            if (block[index] == '\n') {
                if (!finish_input_line(list, line, length, line_number++)) {
                    success = false;
                    break;
                }
                length = 0;
            } else if (!line_append(&line, &length, &capacity,
                                   (char)block[index])) {
                printf("fetch: not enough memory while reading \"%s\".\n",
                       path);
                success = false;
                break;
            }
        }
        if (!success) break;
    }
    if (success && length && !finish_input_line(list, line, length,
                                                 line_number)) success = false;
    free(line);
    mg_result_t closed = handle_close(file);
    if (closed < 0) {
        printf("fetch: could not close URL list \"%s\": %s.\n", path,
               error_string(closed));
        success = false;
    }
    return success;
}

static bool append_decimal(char *output, usize capacity, usize *used, u64 value)
{
    char reversed[20];
    usize digits = 0;
    do {
        reversed[digits++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    } while (value && digits < sizeof(reversed));
    if (digits > capacity - *used) return false;
    while (digits) output[(*used)++] = reversed[--digits];
    return true;
}

static bool build_temp_path(const char *destination, bool current_directory,
                            u64 attempt, char **out_path)
{
    const char *parent = "";
    usize parent_length = 0;
    char leaf[96];
    usize used = 0;
    u64 now = 0;
    usize total;
    char *path;

    if (!out_path) return false;
    *out_path = NULL;
    if (!current_directory) {
        const char *slash = strrchr(destination, '/');
        if (slash) {
            parent = destination;
            parent_length = (usize)(slash - destination) + 1U;
        }
    }
    (void)monotonic_ms(&now);
    memcpy(leaf, ".fetch-", 7U);
    used = 7U;
    if (!append_decimal(leaf, sizeof(leaf), &used, process_current_pid()) ||
        used >= sizeof(leaf)) return false;
    leaf[used++] = '-';
    if (!append_decimal(leaf, sizeof(leaf), &used, now) ||
        used >= sizeof(leaf)) return false;
    leaf[used++] = '-';
    if (!append_decimal(leaf, sizeof(leaf), &used, attempt) ||
        used > sizeof(leaf) - sizeof(".part")) return false;
    memcpy(leaf + used, ".part", sizeof(".part"));
    used += sizeof(".part") - 1U;
    if (parent_length > MG_PATH_MAX_BYTES ||
        used > MG_PATH_MAX_BYTES - parent_length) return false;
    total = parent_length + used;
    path = (char *)malloc(total + 1U);
    if (!path) return false;
    if (parent_length) memcpy(path, parent, parent_length);
    memcpy(path + parent_length, leaf, used);
    path[total] = '\0';
    *out_path = path;
    return true;
}

static mg_result_t create_temp_file(const char *destination,
                                    bool current_directory,
                                    char **out_path, mg_handle_t *out_handle)
{
    u64 attempt = 0;
    if (!out_path || !out_handle) return MG_ERR_BAD_ARGUMENT;
    *out_path = NULL;
    *out_handle = 0;
    for (;;) {
        char *candidate = NULL;
        mg_result_t created;
        mg_result_t opened;
        if (!build_temp_path(destination, current_directory, attempt,
                             &candidate)) return MG_ERR_NO_MEMORY;
        created = file_create(candidate);
        if (created == MG_ERR_ALREADY_EXISTS) {
            free(candidate);
            if (attempt == ~(u64)0) return MG_ERR_NO_SPACE;
            attempt++;
            continue;
        }
        if (created != MG_OK) {
            *out_path = candidate;
            return created;
        }
        opened = file_open(candidate, MG_OPEN_WRITE);
        if (opened < 0) {
            mg_result_t removed = path_remove(candidate);
            if (removed != MG_OK && removed != MG_ERR_NOT_FOUND)
                printf("fetch: could not remove temporary file \"%s\": %s.\n",
                       candidate, error_string(removed));
            *out_path = candidate;
            return opened;
        }
        *out_path = candidate;
        *out_handle = (mg_handle_t)opened;
        return MG_OK;
    }
}

static bool build_request(const fetch_url_t *url, char **out_request,
                          usize *out_length)
{
    static const char format[] =
        "GET %s HTTP/1.1\r\nHost: %s%s\r\n"
        "User-Agent: Mangrove\r\nAccept: */*\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
    char port[8] = {0};
    usize capacity;
    int count;
    char *request;

    if (!url || !url->host || !url->path || !out_request || !out_length)
        return false;
    if (url->port != (url->https ? 443U : 80U)) {
        count = snprintf(port, sizeof(port), ":%u", (unsigned)url->port);
        if (count < 0 || (usize)count >= sizeof(port)) return false;
    }
    usize host_length = strlen(url->host);
    usize path_length = strlen(url->path);
    usize overhead = sizeof(format) + 16U;
    if (path_length > (usize)-1 - overhead ||
        host_length > (usize)-1 - overhead - path_length)
        return false;
    capacity = host_length + path_length + overhead;
    request = (char *)malloc(capacity);
    if (!request) return false;
    count = snprintf(request, capacity, format, url->path, url->host, port);
    if (count < 0 || (usize)count >= capacity) {
        free(request);
        return false;
    }
    *out_request = request;
    *out_length = (usize)count;
    return true;
}

static mg_result_t file_sink_write(void *opaque, const u8 *bytes,
                                   usize length)
{
    fetch_sink_context_t *context = (fetch_sink_context_t *)opaque;
    u64 before = 0, after = 0;
    mg_result_t result;

    if (!context || (!bytes && length)) return MG_ERR_BAD_ARGUMENT;
    bool timed = monotonic_ms(&before);
    result = object_write_all(context->file, bytes, length);
    if (context->metrics && context->metrics->write_calls != ~(u64)0)
        context->metrics->write_calls++;
    if (timed && monotonic_ms(&after) && context->metrics) {
        u64 elapsed = duration_ms(before, after);
        add_duration(&context->metrics->write_ms, before, after);
        if (elapsed > context->metrics->write_max_ms)
            context->metrics->write_max_ms = elapsed;
    }
    if (result < 0 || (usize)result != length) {
        return result < 0 ? result : MG_ERR_IO;
    }
    return MG_OK;
}

static mg_result_t file_sink(void *opaque, const u8 *bytes, usize length)
{
    fetch_sink_context_t *context = (fetch_sink_context_t *)opaque;
    u64 after = 0;
    mg_result_t result;

    if (!context || (!bytes && length)) return MG_ERR_BAD_ARGUMENT;
    if ((u64)length > ~(u64)0 - context->bytes) {
        context->failure = MG_ERR_NO_SPACE;
        return context->failure;
    }
    result = fetch_sink_buffer_append(context->buffer, bytes, length);
    if (result != MG_OK) {
        context->failure = result;
        return result;
    }
    context->bytes += (u64)length;
    if (context->progress_clock_ready &&
        monotonic_ms(&after) &&
        after >= context->last_progress_ms &&
        after - context->last_progress_ms >= FETCH_PROGRESS_INTERVAL_MS) {
        fetch_display_progress(context->display, context->bytes, after);
        context->last_progress_ms = after;
    }
    return MG_OK;
}

static bool redirect_status(u32 status)
{
    return status == 301U || status == 302U || status == 303U ||
           status == 307U || status == 308U;
}

static bool output_path_absent(const char *path)
{
    mg_path_info_t info;
    mg_result_t result = path_info(path, &info);
    if (result == MG_ERR_NOT_FOUND) return true;
    if (result == MG_OK)
        printf("fetch: output \"%s\" already exists; it will not be overwritten.\n",
               path);
    else
        printf("fetch: could not inspect output \"%s\": %s.\n", path,
               error_string(result));
    return false;
}

static char *numbered_name(const char *base, u64 number)
{
    char suffix[24];
    int suffix_length = snprintf(suffix, sizeof(suffix), ".%llu",
                                 (unsigned long long)number);
    usize base_length = strlen(base);
    char *name;
    if (suffix_length < 0 || (usize)suffix_length >= sizeof(suffix) ||
        base_length > (usize)-1 - (usize)suffix_length ||
        base_length + (usize)suffix_length > MG_PATH_MAX_BYTES) return NULL;
    name = (char *)malloc(base_length + (usize)suffix_length + 1U);
    if (!name) return NULL;
    memcpy(name, base, base_length);
    memcpy(name + base_length, suffix, (usize)suffix_length + 1U);
    return name;
}

static mg_result_t publish_temp(const char *temporary, const char *destination,
                                bool automatic, char **out_published)
{
    u64 suffix = 0;
    *out_published = NULL;
    for (;;) {
        char *candidate = suffix ? numbered_name(destination, suffix) :
            copy_string(destination);
        mg_result_t moved;
        if (!candidate) return MG_ERR_NO_MEMORY;
        moved = path_move(temporary, candidate);
        if (moved == MG_OK) {
            *out_published = candidate;
            return MG_OK;
        }
        free(candidate);
        if (!automatic || moved != MG_ERR_ALREADY_EXISTS) return moved;
        if (suffix == ~(u64)0) return MG_ERR_NO_SPACE;
        suffix++;
    }
}

static int download_one(const fetch_url_t *initial, const char *explicit_output,
                        bool quiet, bool timings, bool interactive_progress)
{
    fetch_url_t current = {0};
    fetch_url_t visited[FETCH_MAX_REDIRECTS + 1U] = {{0}};
    usize visited_count = 0;
    usize redirect_count = 0;
    char *temporary = NULL;
    char *automatic_name = NULL;
    char *published = NULL;
    mg_handle_t file = 0;
    bool file_opened = false;
    bool file_created = false;
    bool complete = false;
    bool uncertain_publish = false;
    mg_result_t failure = MG_OK;
    fetch_metrics_t metrics = {0};
    fetch_display_t display;
    fetch_sink_context_t sink = {0};
    fetch_transport_state_t transport_state = {0};
    fetch_transport_t active_transport = {0};
    u64 total_start = 0;
    u64 total_end = 0;
    bool timings_printed = false;
    u64 fs_start = 0;
    u64 fs_end = 0;

    metrics.clock_available = monotonic_ms(&total_start);
    fetch_display_init(&display, quiet, interactive_progress, stdout_write,
                       NULL);
    if (!fetch_url_clone(initial, &current)) {
        printf("fetch: not enough memory to prepare URL.\n");
        return 1;
    }
    fetch_display_requested(&display, &current);
    if (explicit_output && monotonic_ms(&fs_start)) {
        bool absent = output_path_absent(explicit_output);
        if (monotonic_ms(&fs_end))
            add_duration(&metrics.output_check_ms, fs_start, fs_end);
        if (!absent) {
            fetch_url_destroy(&current);
            return 1;
        }
    } else if (explicit_output && !output_path_absent(explicit_output)) {
        fetch_url_destroy(&current);
        return 1;
    }
    if (monotonic_ms(&fs_start)) {
        failure = create_temp_file(explicit_output, explicit_output == NULL,
                                   &temporary, &file);
        if (monotonic_ms(&fs_end))
            add_duration(&metrics.file_setup_ms, fs_start, fs_end);
    } else {
        failure = create_temp_file(explicit_output, explicit_output == NULL,
                                   &temporary, &file);
    }
    if (failure < 0) {
        if (failure == MG_ERR_UNCERTAIN)
            printf("fetch: temporary-file creation at \"%s\" is uncertain; inspect it before retrying.\n",
                   temporary ? temporary : "unknown path");
        else
            printf("fetch: could not create temporary output%s%s: %s.\n",
                   temporary ? " \"" : "", temporary ? temporary : "",
                   error_string(failure));
        free(temporary);
        fetch_url_destroy(&current);
        return 1;
    }
    file_created = true;
    file_opened = true;
    sink.file = file;
    sink.display = &display;
    sink.metrics = &metrics;

    for (;;) {
        mg_ipv4_addr_t address;
        char *request = NULL;
        usize request_length = 0;
        fetch_http_decoder_t *decoder = NULL;
        fetch_url_t next_url = {0};
        bool follow = false;
        bool response_complete = false;
        bool response_started = false;
        u64 phase_start = 0;
        u64 phase_end = 0;
        u64 request_sent = 0;
        bool request_sent_clock = false;

        for (usize index = 0; index < visited_count; index++) {
            if (fetch_url_equal(&current, &visited[index])) {
                printf("fetch: redirect loop detected at %s%s.\n",
                       current.host, current.path);
                failure = MG_ERR_PROTOCOL;
                goto done;
            }
        }
        if (visited_count >= FETCH_MAX_REDIRECTS + 1U) {
            printf("fetch: redirect limit exceeded.\n");
            failure = MG_ERR_PROTOCOL;
            goto done;
        }
        if (!fetch_url_clone(&current, &visited[visited_count++])) {
            printf("fetch: not enough memory while following redirects.\n");
            failure = MG_ERR_NO_MEMORY;
            goto done;
        }
        sink.progress_clock_ready = false;
        if (current.https) {
            if (monotonic_ms(&phase_start)) {
                failure = fetch_transport_prepare_tls(&transport_state);
                if (monotonic_ms(&phase_end))
                    add_duration(&metrics.tls_context_ms, phase_start,
                                 phase_end);
            } else {
                failure = fetch_transport_prepare_tls(&transport_state);
            }
            if (failure < 0) {
                printf("fetch: could not prepare TLS trust and crypto state for %s: %s.\n",
                       current.host, error_string(failure));
                goto done;
            }
        }

        bool is_ip = mg_ipv4_parse(current.host, &address);
        fetch_display_resolving(&display, current.host, is_ip);

        if (!is_ip) {
            if (monotonic_ms(&phase_start)) {
                failure = mg_net_resolve_a(current.host, &address,
                                           MG_NET_TIMEOUT_DEFAULT);
                if (monotonic_ms(&phase_end))
                    add_duration(&metrics.dns_ms, phase_start, phase_end);
            } else {
                failure = mg_net_resolve_a(current.host, &address,
                                           MG_NET_TIMEOUT_DEFAULT);
            }
            if (failure < 0) {
                printf("fetch: DNS lookup failed for %s: %s.\n", current.host,
                       error_string(failure));
                goto done;
            }
        }
        if (!is_ip) fetch_display_resolved(&display, current.host, &address);
        if (!build_request(&current, &request, &request_length)) {
            printf("fetch: could not prepare HTTP request for %s.\n",
                   current.host);
            failure = MG_ERR_NO_MEMORY;
            goto done;
        }
        fetch_display_connecting(&display, current.host, current.port);
        if (monotonic_ms(&phase_start)) {
            failure = fetch_transport_connect_tcp(
                &current, &address, FETCH_CONNECT_TIMEOUT_MS,
                &active_transport);
            if (monotonic_ms(&phase_end))
                add_duration(&metrics.connect_ms, phase_start, phase_end);
        } else {
            failure = fetch_transport_connect_tcp(&current, &address,
                FETCH_CONNECT_TIMEOUT_MS, &active_transport);
        }
        if (failure < 0) {
            printf("fetch: TCP connection to %s:%u failed: %s.\n",
                   current.host, (unsigned)current.port,
                   error_string(failure));
            free(request);
            goto done;
        }
        fetch_display_connected(&display, current.host, current.port,
                                &address);
        if (current.https) {
            fetch_display_tls_starting(&display, current.host);
            if (monotonic_ms(&phase_start)) {
                failure = fetch_transport_start_tls(
                    &transport_state, current.host, FETCH_CONNECT_TIMEOUT_MS,
                    &active_transport);
                if (monotonic_ms(&phase_end))
                    add_duration(&metrics.tls_handshake_ms, phase_start,
                                 phase_end);
            } else {
                failure = fetch_transport_start_tls(
                    &transport_state, current.host, FETCH_CONNECT_TIMEOUT_MS,
                    &active_transport);
            }
            if (failure < 0) {
                printf("fetch: TLS 1.3 handshake/authentication for %s failed; no authenticated session was established: %s.\n",
                       current.host, error_string(failure));
                free(request);
                goto done;
            }
            fetch_display_tls_verified(&display, current.host);
        }
        usize request_offset = 0;
        mg_result_t sent = MG_OK;
        while (request_offset < request_length) {
            usize remaining = request_length - request_offset;
            sent = fetch_transport_send(&active_transport,
                                        request + request_offset, remaining,
                                        FETCH_CONNECT_TIMEOUT_MS);
            if (sent < 0) break;
            if (sent == 0 || (u64)sent > remaining) {
                sent = MG_ERR_IO;
                break;
            }
            request_offset += (usize)sent;
        }
        free(request);
        request = NULL;
        if (request_offset != request_length) {
            failure = sent < 0 ? sent : MG_ERR_IO;
            printf("fetch: request send to %s failed: %s.\n", current.host,
                   error_string(failure));
            mg_result_t closed = fetch_transport_close(&active_transport);
            if (closed < 0)
                printf("fetch: connection close after request failure failed: %s.\n",
                       error_string(closed));
            goto done;
        }
        request_sent_clock = monotonic_ms(&request_sent);
        decoder = fetch_http_decoder_create(file_sink, &sink);
        if (!decoder) {
            failure = MG_ERR_NO_MEMORY;
            (void)fetch_transport_close(&active_transport);
            printf("fetch: not enough memory for HTTP response state.\n");
            goto done;
        }

        u8 receive_buffer[FETCH_IO_CHUNK];
        for (;;) {
            u64 receive_start = 0;
            u64 receive_end = 0;
            bool receive_clock = monotonic_ms(&receive_start);
            mg_result_t received = fetch_transport_receive(
                &active_transport, receive_buffer, sizeof(receive_buffer),
                FETCH_IDLE_TIMEOUT_MS);
            bool receive_end_clock = monotonic_ms(&receive_end);
            if (response_started && receive_clock && receive_end_clock)
                add_duration(&metrics.receive_ms, receive_start, receive_end);
            if (received > (mg_result_t)sizeof(receive_buffer)) {
                failure = MG_ERR_IO;
                fetch_display_finish_progress(&display);
                printf("fetch: network returned an invalid receive length.\n");
                break;
            }
            if (received > 0) {
                usize offset = 0;
                if (!response_started) {
                    response_started = true;
                    if (monotonic_ms(&phase_end)) {
                        if (!metrics.first_byte_ms && request_sent_clock)
                            metrics.first_byte_ms = duration_ms(request_sent,
                                                                phase_end);
                        sink.last_progress_ms = phase_end;
                        sink.progress_clock_ready = true;
                    }
                }
                while (offset < (usize)received) {
                    usize consumed = 0;
                    fetch_http_event_t event = fetch_http_decoder_feed(
                        decoder, receive_buffer + offset,
                        (usize)received - offset, &consumed);
                    offset += consumed;
                    if (event == FETCH_HTTP_HEADERS_READY) {
                        const fetch_http_headers_t *headers =
                            fetch_http_decoder_headers(decoder);
                        if (!headers) {
                            failure = MG_ERR_PROTOCOL;
                            break;
                        }
                        fetch_display_http_status(&display, headers->status);
                        if (redirect_status(headers->status)) {
                            if (!headers->location) {
                                printf("fetch: HTTP status %u from %s has no Location header.\n",
                                       headers->status, current.host);
                                failure = MG_ERR_PROTOCOL;
                                break;
                            }
                            if (redirect_count >= FETCH_MAX_REDIRECTS) {
                                printf("fetch: redirect limit (%u) exceeded.\n",
                                       FETCH_MAX_REDIRECTS);
                                failure = MG_ERR_PROTOCOL;
                                break;
                            }
                            fetch_url_parse_result_t redirected =
                                fetch_url_resolve_redirect(&current,
                                    headers->location, &next_url);
                            if (redirected != FETCH_URL_PARSE_OK) {
                                printf("fetch: invalid redirect from %s%s.\n",
                                       current.host, current.path);
                                failure = redirected == FETCH_URL_PARSE_NO_MEMORY
                                    ? MG_ERR_NO_MEMORY : MG_ERR_PROTOCOL;
                                break;
                            }
                            if (!fetch_url_redirect_allowed(&current,
                                                            &next_url)) {
                                printf("fetch: refusing HTTPS-to-HTTP redirect downgrade to %s%s.\n",
                                       next_url.host, next_url.path);
                                fetch_url_destroy(&next_url);
                                failure = MG_ERR_PROTOCOL;
                                break;
                            }
                            fetch_display_redirect(&display, headers->status,
                                                  &next_url);
                            follow = true;
                            break;
                        }
                        if (headers->status < 200U || headers->status >= 300U) {
                            printf("fetch: HTTP request to %s failed with status %u.\n",
                                   current.host, headers->status);
                            failure = MG_ERR_PROTOCOL;
                            break;
                        }
                        if (!explicit_output && !automatic_name &&
                            !fetch_url_filename(&current, &automatic_name)) {
                            printf("fetch: final URL has no safe output filename.\n");
                            failure = MG_ERR_BAD_ARGUMENT;
                            break;
                        }
                        fetch_display_response_details(&display, headers,
                            explicit_output ? explicit_output : automatic_name);
                        u64 body_start = 0;
                        bool body_clock = monotonic_ms(&body_start);
                        metrics.transfer_clock_available = body_clock;
                        metrics.transfer_start_ms = body_start;
                        sink.last_progress_ms = body_start;
                        sink.progress_clock_ready = body_clock;
                        fetch_display_body_start(&display,
                            headers->has_content_length,
                            headers->content_length, body_start);
                        if (!fetch_http_decoder_complete(decoder) &&
                            !sink.buffer) {
                            sink.buffer = (fetch_sink_buffer_t *)malloc(
                                sizeof(*sink.buffer));
                            if (!sink.buffer) {
                                printf("fetch: not enough memory to buffer response output.\n");
                                failure = MG_ERR_NO_MEMORY;
                                break;
                            }
                            fetch_sink_buffer_init(sink.buffer,
                                                   file_sink_write, &sink);
                        }
                        if (fetch_http_decoder_complete(decoder)) {
                            response_complete = true;
                            break;
                        }
                        continue;
                    }
                    if (event == FETCH_HTTP_COMPLETE) {
                        response_complete = true;
                        break;
                    }
                    if (event == FETCH_HTTP_MALFORMED) {
                        fetch_display_finish_progress(&display);
                        printf("fetch: malformed or truncated HTTP response from %s.\n",
                               current.host);
                        failure = MG_ERR_PROTOCOL;
                        break;
                    }
                    if (event == FETCH_HTTP_SINK_FAILED) {
                        failure = sink.failure;
                        fetch_display_finish_progress(&display);
                        printf("fetch: writing temporary output failed: %s.\n",
                               error_string(failure));
                        break;
                    }
                    if (!consumed) {
                        fetch_display_finish_progress(&display);
                        printf("fetch: HTTP response decoder made no progress.\n");
                        failure = MG_ERR_PROTOCOL;
                        break;
                    }
                }
                if (follow || response_complete || failure < 0) break;
                continue;
            }
            if (received == MG_ERR_CONNECTION_CLOSED) {
                if (fetch_http_decoder_eof(decoder) == FETCH_HTTP_COMPLETE)
                    response_complete = true;
                else {
                    fetch_display_finish_progress(&display);
                    printf("fetch: connection closed before the HTTP response completed.\n");
                    failure = MG_ERR_PROTOCOL;
                }
                break;
            }
            failure = received < 0 ? received : MG_ERR_IO;
            fetch_display_finish_progress(&display);
            printf("fetch: response receive from %s failed: %s.\n",
                   current.host, error_string(failure));
            break;
        }
        fetch_display_finish_progress(&display);
        if (monotonic_ms(&phase_start)) {
            mg_result_t closed = fetch_transport_close(&active_transport);
            if (monotonic_ms(&phase_end))
                add_duration(&metrics.connection_close_ms, phase_start,
                             phase_end);
            if (closed < 0 && failure == MG_OK) {
                failure = closed;
                printf("fetch: connection close failed: %s.\n",
                       error_string(closed));
            }
        } else {
            mg_result_t closed = fetch_transport_close(&active_transport);
            if (closed < 0 && failure == MG_OK) failure = closed;
        }
        fetch_http_decoder_destroy(decoder);
        if (failure < 0) {
            fetch_url_destroy(&next_url);
            goto done;
        }
        if (follow) {
            fetch_url_destroy(&current);
            current = next_url;
            redirect_count++;
            continue;
        }
        if (response_complete) {
            if (sink.buffer) {
                failure = fetch_sink_buffer_flush(sink.buffer);
                if (failure != MG_OK) {
                    sink.failure = failure;
                    printf("fetch: writing temporary output failed: %s.\n",
                           error_string(failure));
                    goto done;
                }
                free(sink.buffer);
                sink.buffer = NULL;
            }
            u64 transfer_end = 0;
            if (metrics.transfer_clock_available && monotonic_ms(&transfer_end))
                metrics.transfer_ms = duration_ms(metrics.transfer_start_ms,
                                                  transfer_end);
            complete = true;
            break;
        }
        failure = MG_ERR_PROTOCOL;
        goto done;
    }

    if (!complete) {
        failure = MG_ERR_PROTOCOL;
        goto done;
    }
    failure = fetch_transport_state_cleanup(&transport_state);
    if (failure < 0) {
        printf("fetch: TLS cleanup failed: %s.\n", error_string(failure));
        goto done;
    }
    if (file_opened) {
        u64 file_close_start = 0;
        u64 file_close_end = 0;
        bool file_close_clock = monotonic_ms(&file_close_start);
        mg_result_t closed = handle_close(file);
        if (file_close_clock && monotonic_ms(&file_close_end))
            add_duration(&metrics.file_close_ms, file_close_start,
                         file_close_end);
        file_opened = false;
        if (closed < 0) {
            fetch_display_finish_progress(&display);
            printf("fetch: could not close temporary output \"%s\": %s.\n",
                   temporary, error_string(closed));
            failure = closed;
            goto done;
        }
    }
    if (!explicit_output && !automatic_name &&
        !fetch_url_filename(&current, &automatic_name)) {
        printf("fetch: final URL has no safe output filename.\n");
        failure = MG_ERR_BAD_ARGUMENT;
        goto done;
    }
    if (monotonic_ms(&fs_start)) {
        failure = publish_temp(temporary,
                               explicit_output ? explicit_output : automatic_name,
                               explicit_output == NULL, &published);
        if (monotonic_ms(&fs_end))
            add_duration(&metrics.publish_ms, fs_start, fs_end);
    } else {
        failure = publish_temp(temporary,
                           explicit_output ? explicit_output : automatic_name,
                           explicit_output == NULL, &published);
    }
    if (failure == MG_ERR_UNCERTAIN) {
        uncertain_publish = true;
        fetch_display_finish_progress(&display);
        printf("fetch: publishing \"%s\" had an uncertain result; inspect the output before retrying.\n",
               explicit_output ? explicit_output : automatic_name);
        goto done;
    }
    if (failure < 0) {
        fetch_display_finish_progress(&display);
        printf("fetch: could not publish \"%s\": %s.\n",
               explicit_output ? explicit_output : automatic_name,
               error_string(failure));
        goto done;
    }
    file_created = false;
    fetch_display_finish_progress(&display);
    fetch_display_complete(&display, published, sink.bytes,
                           metrics.transfer_ms,
                           metrics.transfer_clock_available);
    failure = MG_OK;

done:
    fetch_display_finish_progress(&display);
    if (active_transport.open) {
        mg_result_t closed = fetch_transport_close(&active_transport);
        if (closed < 0) {
            printf("fetch: connection cleanup failed: %s.\n",
                   error_string(closed));
            if (failure == MG_OK) failure = closed;
        }
    }
    {
        mg_result_t tls_cleanup =
            fetch_transport_state_cleanup(&transport_state);
        if (tls_cleanup < 0) {
            printf("fetch: TLS cleanup failed: %s.\n",
                   error_string(tls_cleanup));
            if (failure == MG_OK) failure = tls_cleanup;
        }
    }
    if (file_opened) {
        mg_result_t closed = handle_close(file);
        file_opened = false;
        if (failure == MG_OK && closed < 0) failure = closed;
    }
    if (file_created && !uncertain_publish && temporary) {
        mg_result_t removed = path_remove(temporary);
        if (removed != MG_OK && removed != MG_ERR_NOT_FOUND) {
            printf("fetch: could not remove incomplete temporary file \"%s\": %s.\n",
                   temporary, error_string(removed));
            if (failure == MG_OK) failure = removed;
        }
    }
    if (metrics.clock_available && monotonic_ms(&total_end))
        metrics.total_ms = duration_ms(total_start, total_end);
    if (timings && !timings_printed) {
        fetch_display_timings_t report = {
            .dns_ms = metrics.dns_ms,
            .connect_ms = metrics.connect_ms,
            .tls_context_ms = metrics.tls_context_ms,
            .tls_handshake_ms = metrics.tls_handshake_ms,
            .first_byte_ms = metrics.first_byte_ms,
            .transfer_ms = metrics.transfer_ms,
            .receive_ms = metrics.receive_ms,
            .write_ms = metrics.write_ms,
            .output_check_ms = metrics.output_check_ms,
            .file_setup_ms = metrics.file_setup_ms,
            .connection_close_ms = metrics.connection_close_ms,
            .file_close_ms = metrics.file_close_ms,
            .publish_ms = metrics.publish_ms,
            .write_calls = metrics.write_calls,
            .write_max_ms = metrics.write_max_ms,
            .total_ms = metrics.total_ms,
            .clock_available = metrics.clock_available,
            .transfer_clock_available = metrics.transfer_clock_available,
        };
        fetch_display_timings(&display, &report);
        timings_printed = true;
    }
    for (usize index = 0; index < visited_count; index++)
        fetch_url_destroy(&visited[index]);
    fetch_url_destroy(&current);
    free(temporary);
    free(automatic_name);
    free(published);
    free(sink.buffer);
    return failure < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    fetch_arguments_t arguments;
    fetch_url_list_t urls = {0};
    const char *bad_argument = NULL;
    fetch_arguments_result_t parsed;
    int status = 0;
    bool interactive_progress = false;

    if (command_help_requested(argc, argv)) return command_print_help(argv[0]);
    parsed = fetch_arguments_parse(argc, argv, &arguments, &bad_argument);
    if (parsed != FETCH_ARGUMENTS_OK) {
        if (parsed == FETCH_ARGUMENTS_DUPLICATE_OPTION)
            printf("fetch: option specified more than once: %s.\n",
                   bad_argument ? bad_argument : "option");
        else if (parsed == FETCH_ARGUMENTS_UNKNOWN_OPTION)
            printf("fetch: unknown option: %s.\n",
                   bad_argument ? bad_argument : "(unknown)");
        else if (parsed == FETCH_ARGUMENTS_MISSING_VALUE)
            printf("fetch: missing value for %s.\n",
                   bad_argument ? bad_argument : "option");
        else if (parsed == FETCH_ARGUMENTS_UNEXPECTED_VALUE)
            printf("fetch: unexpected value for %s.\n",
                   bad_argument ? bad_argument : "option");
        else if (parsed == FETCH_ARGUMENTS_NO_MEMORY)
            printf("fetch: not enough memory while preparing arguments.\n");
        else
            printf("fetch: invalid option or missing value near %s.\n",
                   bad_argument ? bad_argument : "command line");
        fetch_arguments_destroy(&arguments);
        return 1;
    }
    if (arguments.input_file) {
        if (!read_url_file(arguments.input_file, &urls)) {
            status = 1;
            goto done;
        }
    } else {
        for (usize index = 0; index < arguments.url_count; index++) {
            fetch_url_parse_result_t url_status;
            if (!url_list_append(&urls, arguments.urls[index], &url_status)) {
                printf("fetch: invalid URL \"%s\"%s.\n",
                       arguments.urls[index],
                       url_status == FETCH_URL_PARSE_NO_MEMORY
                           ? ": not enough memory" : "");
                status = 1;
                goto done;
            }
        }
    }
    if (!urls.count) {
        command_usage_error(argv[0],
            "fetch [-q] [--timings] [-o FILE] URL... | fetch [-q] [--timings] [-o FILE] -i URLS_FILE",
            NULL);
        status = 1;
        goto done;
    }
    if (arguments.output_path && urls.count != 1U) {
        printf("fetch: -o/--output requires exactly one URL.\n");
        status = 1;
        goto done;
    }
    if (arguments.output_path &&
        strlen(arguments.output_path) > MG_PATH_MAX_BYTES) {
        printf("fetch: output path is too long.\n");
        status = 1;
        goto done;
    }
    if (!arguments.quiet) {
        mg_terminal_capabilities_t capabilities = {0};
        mg_terminal_size_t size = {0};
        if (terminal_get_capabilities(&capabilities) == MG_OK &&
            (capabilities.capabilities &
             MG_TERMINAL_CAP_INTERACTIVE_OUTPUT) != 0U &&
            terminal_get_size(&size) == MG_OK && size.columns >= 40U)
            interactive_progress = true;
    }
    for (usize index = 0; index < urls.count; index++)
        if (download_one(&urls.items[index], arguments.output_path,
                         arguments.quiet, arguments.timings,
                         interactive_progress)) status = 1;

done:
    fetch_arguments_destroy(&arguments);
    url_list_destroy(&urls);
    return status;
}
