/* SPDX-License-Identifier: GPL-3.0-only */
#include <mangrove.h>
#include <mg/log_service.h>
#include <mg/terminal.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"

#define LOGV_SOURCE_WIDTH_DEFAULT 12U
#define LOGV_SOURCE_WIDTH_NARROW  8U
#define LOGV_DEFAULT_COLUMNS      80U
#define LOGV_MAX_BOOT_HEADINGS    64U

typedef mg_result_t (*log_record_callback_t)(const mg_log_record_t *record,
                                             void *context);

typedef struct {
    u64 boot_id;
    bool anchor_found;
    mg_mangrove_time_t anchor;
} boot_heading_t;

typedef struct {
    boot_heading_t entries[LOGV_MAX_BOOT_HEADINGS];
    usize count;
} boot_heading_table_t;

typedef struct {
    bool have_boot;
    bool heading_printed;
    u64 boot_id;
    const boot_heading_table_t *headings;
    u32 terminal_columns;
} print_context_t;

static const char *severity_name(u32 severity)
{
    switch (severity) {
        case MG_LOG_DEBUG: return "DEBUG";
        case MG_LOG_INFO: return "INFO";
        case MG_LOG_WARNING: return "WARN";
        case MG_LOG_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static mg_result_t request_log(const mg_log_request_t *payload,
                               mg_log_response_t *response)
{
    mg_handle_t endpoint = 0;
    mg_ipc_message_t request = {0};
    mg_ipc_message_t reply = {0};
    mg_result_t result;

    result = service_lookup("log", &endpoint);
    if (result != MG_OK) return result;
    request.version = MG_IPC_PROTOCOL_VERSION;
    request.type = MG_LOG_REQUEST;
    request.payload_length = sizeof(*payload);
    memcpy(request.payload, payload, sizeof(*payload));
    result = ipc_request(endpoint, &request, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_LOG_RESPONSE ||
        reply.payload_length != sizeof(*response)) return MG_ERR_PROTOCOL;
    memcpy(response, reply.payload, sizeof(*response));
    return response->result;
}

static bool record_valid(const mg_log_record_t *record)
{
    if (!record || !record->source_length ||
        record->source_length >= MG_LOG_SOURCE_MAX ||
        !record->message_length ||
        record->message_length >= MG_LOG_MESSAGE_MAX ||
        record->source[record->source_length] != '\0' ||
        record->message[record->message_length] != '\0' ||
        record->severity > MG_LOG_ERROR) return false;
    return true;
}

/* Derive the boot's realtime anchor from a record stamped with both clocks.
 * This keeps calendar time out of individual record presentation while still
 * allowing a useful heading when a later record is the first synchronized
 * one. */
static bool record_boot_anchor(const mg_log_record_t *record,
                               mg_mangrove_time_t *out)
{
    const i64 i64_max = (i64)0x7fffffffffffffffLL;
    const i64 i64_min = (i64)(-0x7fffffffffffffffLL - 1LL);
    u64 elapsed_seconds;
    u64 elapsed_milliseconds;
    i64 seconds;
    u32 nanoseconds;

    if (!record || !out || !record->realtime_available ||
        record->realtime.nanoseconds >= MG_TIME_NANOSECONDS_PER_SEC)
        return false;
    elapsed_seconds = record->monotonic_ms / 1000U;
    elapsed_milliseconds = record->monotonic_ms % 1000U;
    if (elapsed_seconds > (u64)i64_max ||
        record->realtime.seconds < i64_min + (i64)elapsed_seconds)
        return false;

    seconds = record->realtime.seconds - (i64)elapsed_seconds;
    nanoseconds = record->realtime.nanoseconds -
                  (u32)(elapsed_milliseconds * 1000000U);
    if (record->realtime.nanoseconds <
        (u32)(elapsed_milliseconds * 1000000U)) {
        if (seconds == i64_min) return false;
        seconds--;
        nanoseconds = record->realtime.nanoseconds +
                      MG_TIME_NANOSECONDS_PER_SEC -
                      (u32)(elapsed_milliseconds * 1000000U);
    }
    out->seconds = seconds;
    out->nanoseconds = nanoseconds;
    out->reserved = 0;
    return true;
}

static mg_result_t scan_logs(const char *source, bool current_boot,
                             log_record_callback_t callback, void *context,
                             bool *out_printed)
{
    mg_log_request_t request = {0};
    mg_log_response_t response;
    mg_result_t result;
    u32 offset = 0;
    bool printed = false;

    request.version = MG_LOG_PROTOCOL_VERSION;
    request.operation = current_boot ? MG_LOG_OP_QUERY_BOOT : MG_LOG_OP_QUERY;
    request.severity = MG_LOG_SEVERITY_ANY;
    if (source)
        strncpy(request.source, source, sizeof(request.source) - 1U);
    for (;;) {
        request.offset = offset;
        result = request_log(&request, &response);
        if (result != MG_OK) return result;
        if (response.count > MG_LOG_QUERY_MAX) return MG_ERR_PROTOCOL;
        for (u32 index = 0; index < response.count; index++) {
            if (!record_valid(&response.records[index]))
                return MG_ERR_PROTOCOL;
            printed = true;
            if (callback) {
                result = callback(&response.records[index], context);
                if (result != MG_OK) return result;
            }
        }
        if (!response.next_offset) break;
        if (response.next_offset <= offset) return MG_ERR_PROTOCOL;
        offset = response.next_offset;
    }
    if (out_printed) *out_printed = printed;
    return MG_OK;
}

static boot_heading_t *find_boot_heading(boot_heading_table_t *table,
                                         u64 boot_id, bool create)
{
    if (!table || !boot_id) return NULL;
    for (usize index = 0; index < table->count; index++)
        if (table->entries[index].boot_id == boot_id)
            return &table->entries[index];
    if (!create || table->count >= LOGV_MAX_BOOT_HEADINGS) return NULL;
    table->entries[table->count].boot_id = boot_id;
    table->entries[table->count].anchor_found = false;
    table->count++;
    return &table->entries[table->count - 1U];
}

static const boot_heading_t *find_boot_heading_const(
    const boot_heading_table_t *table, u64 boot_id)
{
    if (!table || !boot_id) return NULL;
    for (usize index = 0; index < table->count; index++)
        if (table->entries[index].boot_id == boot_id)
            return &table->entries[index];
    return NULL;
}

static mg_result_t collect_boot_heading(const mg_log_record_t *record,
                                        void *context)
{
    boot_heading_table_t *table = (boot_heading_table_t *)context;
    boot_heading_t *heading = find_boot_heading(table, record->boot_id, true);

    if (heading && !heading->anchor_found &&
        record_boot_anchor(record, &heading->anchor))
        heading->anchor_found = true;
    return MG_OK;
}

static u32 source_width_for_terminal(u32 columns)
{
    return columns < 48U ? LOGV_SOURCE_WIDTH_NARROW :
                           LOGV_SOURCE_WIDTH_DEFAULT;
}

static usize bounded_source(const mg_log_record_t *record, char *out,
                            usize capacity, u32 width)
{
    usize length;
    usize copy_length;

    if (!record || !out || !capacity || !width) return 0;
    length = record->source_length;
    if (length <= width) copy_length = length;
    else copy_length = width > 1U ? width - 1U : 1U;
    if (copy_length + 1U > capacity) copy_length = capacity - 1U;
    memcpy(out, record->source, copy_length);
    if (length > width && copy_length) out[copy_length - 1U] = '~';
    out[copy_length] = '\0';
    return copy_length;
}

static usize format_timestamp(u64 milliseconds, char *out, usize capacity)
{
    u64 hours = milliseconds / 3600000U;
    u64 minutes = (milliseconds / 60000U) % 60U;
    u64 seconds = (milliseconds / 1000U) % 60U;
    u64 fraction = milliseconds % 1000U;
    int length;

    if (!out || !capacity) return 0;
    length = snprintf(out, capacity, "+%02llu:%02llu:%02llu.%03llu",
                      hours, minutes, seconds, fraction);
    return length < 0 ? 0U : (usize)length;
}

static usize append_spaces(char *out, usize capacity, usize count)
{
    usize actual = count;

    if (!out || !capacity) return 0;
    if (actual >= capacity) actual = capacity - 1U;
    memset(out, ' ', actual);
    out[actual] = '\0';
    return actual;
}

static usize format_prefix(const mg_log_record_t *record, u32 source_width,
                           char *out, usize capacity)
{
    char timestamp[32];
    char source[MG_LOG_SOURCE_MAX];
    usize source_length;
    usize length;
    int formatted;

    if (!record || !out || !capacity) return 0;
    if (!format_timestamp(record->monotonic_ms, timestamp,
                          sizeof(timestamp))) return 0;
    (void)bounded_source(record, source, sizeof(source), source_width);
    formatted = snprintf(out, capacity, "%s %-6s ", timestamp,
                         severity_name(record->severity));
    if (formatted < 0 || (usize)formatted >= capacity) return 0;
    length = (usize)formatted;
    source_length = strlen(source);
    if (source_length > source_width) source_length = source_width;
    if (length + source_length >= capacity) return 0;
    memcpy(out + length, source, source_length);
    length += source_length;
    if (source_width > source_length) {
        usize padding = source_width - source_length;
        if (length + padding >= capacity) return 0;
        memset(out + length, ' ', padding);
        length += padding;
    }
    out[length] = '\0';
    return length;
}

/* Mangrove's console currently treats each valid decoded scalar as one
 * terminal cell.  Malformed bytes are consumed as one replacement-width
 * unit, which keeps a damaged record bounded and prevents UTF-8 splits. */
static usize next_utf8(const char *text, usize length, usize offset,
                       usize *sequence)
{
    u8 first;
    usize expected = 1U;
    u32 codepoint;

    if (!text || offset >= length || !sequence) return 0;
    first = (u8)text[offset];
    codepoint = first;
    if (first >= 0xc2U && first <= 0xdfU) {
        expected = 2U;
        codepoint = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
        expected = 3U;
        codepoint = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        expected = 4U;
        codepoint = first & 0x07U;
    } else if (first >= 0x80U) {
        *sequence = 1U;
        return 1U;
    }
    if (offset + expected > length) {
        *sequence = 1U;
        return 1U;
    }
    for (usize index = 1U; index < expected; index++) {
        u8 byte = (u8)text[offset + index];
        if ((byte & 0xc0U) != 0x80U) {
            *sequence = 1U;
            return 1U;
        }
        codepoint = (codepoint << 6) | (byte & 0x3fU);
    }
    if ((expected == 2U && codepoint < 0x80U) ||
        (expected == 3U && codepoint < 0x800U) ||
        (expected == 4U && codepoint < 0x10000U) ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
        codepoint > 0x10ffffU) {
        *sequence = 1U;
        return 1U;
    }
    *sequence = expected;
    return expected;
}

static bool write_bytes(const char *text, usize length)
{
    return console_write(text, length) == (mg_result_t)length;
}

static mg_result_t print_record(const mg_log_record_t *record,
                                void *context)
{
    print_context_t *print = (print_context_t *)context;
    char prefix[96];
    char continuation[96];
    const char *message = record->message;
    usize message_length = record->message_length;
    usize offset = 0;
    u32 source_width = source_width_for_terminal(print->terminal_columns);
    usize prefix_length;
    usize message_columns;

    if (!print->have_boot || print->boot_id != record->boot_id) {
        mg_calendar_time_t calendar;
        mg_mangrove_time_t heading_anchor;
        const boot_heading_t *heading = find_boot_heading_const(
            print->headings, record->boot_id);
        bool have_anchor = heading && heading->anchor_found;

        if (print->heading_printed && !write_bytes("\n", 1U))
            return MG_ERR_IO;
        print->have_boot = true;
        print->boot_id = record->boot_id;
        print->heading_printed = true;
        if (have_anchor) heading_anchor = heading->anchor;
        else have_anchor = record_boot_anchor(record, &heading_anchor);
        if (have_anchor &&
            mangrove_time_to_calendar(&heading_anchor, &calendar) == MG_OK) {
            if (printf("Boot %04d-%02u-%02u %02u:%02u:%02u UTC\n",
                       calendar.year, (u32)calendar.month, (u32)calendar.day,
                       (u32)calendar.hour, (u32)calendar.minute,
                       (u32)calendar.second) < 0)
                return MG_ERR_IO;
        } else if (printf("Boot %08llx\n", record->boot_id & 0xffffffffU) < 0) {
            return MG_ERR_IO;
        }
    }

    prefix_length = format_prefix(record, source_width, prefix,
                                   sizeof(prefix));
    if (!prefix_length) return MG_ERR_PROTOCOL;
    message_columns = print->terminal_columns > prefix_length
        ? print->terminal_columns - prefix_length : 1U;
    (void)append_spaces(continuation, sizeof(continuation), prefix_length);

    if (!message_length) {
        if (!write_bytes(prefix, prefix_length) ||
            !write_bytes("\n", 1U)) return MG_ERR_IO;
        return MG_OK;
    }
    while (offset < message_length) {
        usize start = offset;
        usize display = 0;
        usize sequence;
        bool first_line = start == 0;

        while (offset < message_length && display < message_columns) {
            usize consumed = next_utf8(message, message_length, offset,
                                       &sequence);
            if (!consumed) return MG_ERR_PROTOCOL;
            offset += consumed;
            display++;
        }
        if (first_line && !write_bytes(prefix, prefix_length))
            return MG_ERR_IO;
        if (!first_line && !write_bytes(continuation, prefix_length))
            return MG_ERR_IO;
        if (!write_bytes(message + start, offset - start) ||
            !write_bytes("\n", 1U)) return MG_ERR_IO;
    }
    return MG_OK;
}

static int query_logs(const char *source, bool current_boot)
{
    boot_heading_table_t headings = {0};
    print_context_t print = {0};
    mg_result_t result;
    bool printed = false;
    bool batched = false;
    mg_terminal_size_t size = {0};

    result = scan_logs(source, current_boot, collect_boot_heading, &headings,
                       &printed);
    if (result != MG_OK) {
        printf("Log service returned malformed or unavailable data.\n");
        return 1;
    }
    if (terminal_get_size(&size) == MG_OK && size.columns)
        print.terminal_columns = size.columns;
    else
        print.terminal_columns = LOGV_DEFAULT_COLUMNS;

    if (!printed) {
        if (current_boot) printf("No records for the current boot.\n");
        else if (source) printf("No records for %s.\n", source);
        else printf("No retained system records.\n");
        return 0;
    }
    if (console_begin_transaction() == MG_OK) batched = true;
    print.headings = &headings;
    result = scan_logs(source, current_boot, print_record, &print, &printed);
    if (batched && console_end_transaction() != MG_OK) result = MG_ERR_IO;
    if (result != MG_OK) {
        printf("Log service returned malformed or unavailable data.\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc == 1) return query_logs(NULL, false);
    if (argc == 2 && !strcmp(argv[1], "boot"))
        return query_logs(NULL, true);
    if (argc == 2) return query_logs(argv[1], false);
    command_usage_error(argv[0], "logv [boot|source]",
                        argc > 1 ? argv[1] : NULL);
    return 1;
}
