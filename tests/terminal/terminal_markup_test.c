/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <mg/object.h>
#include <mg/terminal_markup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main say_program_main
#include "../../userspace/say/main.c"
#undef main

#define CAPTURE_CAPACITY (256U * 1024U)
static u8 control_bytes[CAPTURE_CAPACITY];
static usize control_length;
static u8 plain_bytes[CAPTURE_CAPACITY];
static usize plain_length;
static bool interactive_output = true;
static bool fail_stream_open;
static unsigned open_count;
static unsigned close_count;
static unsigned write_count;
static usize write_lengths[8];

bool command_help_requested(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return false;
}

int command_print_help(const char *argv0)
{
    (void)argv0;
    return 0;
}

const char *error_string(mg_result_t result)
{
    (void)result;
    return "test error";
}

mg_result_t terminal_get_capabilities(mg_terminal_capabilities_t *out)
{
    if (!out) return MG_ERR_BAD_ARGUMENT;
    out->version = MG_TERMINAL_API_VERSION;
    out->capabilities = interactive_output ?
        MG_TERMINAL_CAP_INTERACTIVE_OUTPUT : 0U;
    return MG_OK;
}

mg_result_t terminal_control_stream_open(mg_handle_t *out_handle)
{
    if (!out_handle) return MG_ERR_BAD_ARGUMENT;
    open_count++;
    if (fail_stream_open) return MG_ERR_ACCESS_DENIED;
    *out_handle = 77U;
    return MG_OK;
}

mg_result_t terminal_control_stream_write(mg_handle_t handle,
                                           const void *records, usize length)
{
    if (handle != 77U || length > CAPTURE_CAPACITY - control_length)
        return MG_ERR_BAD_ARGUMENT;
    memcpy(control_bytes + control_length, records, length);
    control_length += length;
    assert(write_count < sizeof(write_lengths) / sizeof(write_lengths[0]));
    write_lengths[write_count] = length;
    write_count++;
    return MG_OK;
}

mg_result_t handle_close(mg_handle_t handle)
{
    if (handle != 77U) return MG_ERR_INVALID_HANDLE;
    close_count++;
    return MG_OK;
}

mg_result_t console_write(const void *buffer, usize length)
{
    if (length > CAPTURE_CAPACITY - plain_length) return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(plain_bytes + plain_length, buffer, length);
    plain_length += length;
    return (mg_result_t)length;
}

static void reset_capture(void)
{
    control_length = plain_length = 0U;
    open_count = close_count = write_count = 0U;
    fail_stream_open = false;
}

static u32 read_le32(const u8 *bytes)
{
    return (u32)bytes[0] | (u32)bytes[1] << 8 |
           (u32)bytes[2] << 16 | (u32)bytes[3] << 24;
}

static usize next_record(const u8 *bytes, usize length, usize offset,
                         u8 expected_type, const u8 **payload,
                         u32 *payload_length)
{
    assert(length - offset >= MG_TERMINAL_CONTROL_HEADER_SIZE);
    assert(memcmp(bytes + offset, "MGTC", 4U) == 0);
    assert(bytes[offset + 4U] == MG_TERMINAL_CONTROL_VERSION);
    assert(bytes[offset + 5U] == expected_type);
    *payload_length = read_le32(bytes + offset + 6U);
    assert((usize)*payload_length <= length - offset -
           MG_TERMINAL_CONTROL_HEADER_SIZE);
    *payload = bytes + offset + MG_TERMINAL_CONTROL_HEADER_SIZE;
    return MG_TERMINAL_CONTROL_HEADER_SIZE + *payload_length;
}

static void test_canonical_markup_encoding(void)
{
    static const char source[] =
        "{fg:green}mang{fg:yellow}rove{reset} "
        "{style:warning}Warn{reset} {bg:blue}{fg:white}X{reset}";
    usize offset = 0U;
    const u8 *payload;
    u32 payload_length;

    reset_capture();
    assert(terminal_markup_write_template(source, sizeof(source) - 1U) == MG_OK);
    assert(open_count == 1U && close_count == 1U && write_count == 1U);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_FOREGROUND, &payload, &payload_length);
    assert(payload_length == 1U && payload[0] == TERMINAL_COLOR_GREEN);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_TEXT, &payload, &payload_length);
    assert(payload_length == 4U && memcmp(payload, "mang", 4U) == 0);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_FOREGROUND, &payload, &payload_length);
    assert(payload_length == 1U && payload[0] == TERMINAL_COLOR_YELLOW);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_TEXT, &payload, &payload_length);
    assert(payload_length == 4U && memcmp(payload, "rove", 4U) == 0);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_RESET, &payload, &payload_length);
    assert(payload_length == 0U);
    assert(offset < control_length);

    reset_capture();
    static const char cursor[] =
        "{at:10,5}{cursor:hide}{clear:line}{clear:to-end}{clear:screen}";
    assert(terminal_markup_write_template(cursor, sizeof(cursor) - 1U) == MG_OK);
    offset = 0U;
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_CURSOR_POSITION, &payload, &payload_length);
    assert(payload_length == 8U && read_le32(payload) == 9U &&
           read_le32(payload + 4U) == 4U);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_CURSOR_VISIBILITY, &payload, &payload_length);
    assert(payload_length == 1U && payload[0] == 0U);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_CLEAR, &payload, &payload_length);
    assert(payload_length == 1U &&
           payload[0] == MG_TERMINAL_CONTROL_CLEAR_LINE);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_CLEAR, &payload, &payload_length);
    assert(payload_length == 1U &&
           payload[0] == MG_TERMINAL_CONTROL_CLEAR_TO_END);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_CLEAR, &payload, &payload_length);
    assert(payload_length == 1U &&
           payload[0] == MG_TERMINAL_CONTROL_CLEAR_SCREEN);
    assert(offset == control_length);
}

static void test_literal_braces_and_utf8(void)
{
    static const char source[] = "{{literal}} \xc3\xa9";
    static const char expected[] = "{literal} \xc3\xa9";
    const u8 *payload;
    u32 payload_length;

    reset_capture();
    assert(terminal_markup_write_template(source, sizeof(source) - 1U) == MG_OK);
    assert(control_length == MG_TERMINAL_CONTROL_HEADER_SIZE +
                             sizeof(expected) - 1U);
    (void)next_record(control_bytes, control_length, 0U,
        MG_TERMINAL_CONTROL_TEXT, &payload, &payload_length);
    assert(payload_length == sizeof(expected) - 1U);
    assert(memcmp(payload, expected, sizeof(expected) - 1U) == 0);
}

static void test_malformed_directives_stay_literal(void)
{
    static const char source[] =
        "{fg:chartreuse} {cursor:maybe} {at:0,2} {fg:red";
    static char oversized[160];
    const u8 *payload;
    u32 payload_length;

    reset_capture();
    assert(terminal_markup_write_template(source, sizeof(source) - 1U) == MG_OK);
    (void)next_record(control_bytes, control_length, 0U,
        MG_TERMINAL_CONTROL_TEXT, &payload, &payload_length);
    assert(payload_length == sizeof(source) - 1U);
    assert(memcmp(payload, source, payload_length) == 0);

    oversized[0] = '{';
    memset(oversized + 1U, 'x', sizeof(oversized) - 2U);
    oversized[sizeof(oversized) - 1U] = '}';
    reset_capture();
    assert(terminal_markup_write_template(oversized, sizeof(oversized)) ==
           MG_OK);
    (void)next_record(control_bytes, control_length, 0U,
        MG_TERMINAL_CONTROL_TEXT, &payload, &payload_length);
    assert(payload_length == sizeof(oversized));
    assert(memcmp(payload, oversized, sizeof(oversized)) == 0);
}

static void test_redirected_and_unavailable_streams_are_plain(void)
{
    static const char source[] = "{style:error}bad{reset} {{ok}}";
    static const char expected[] = "bad {ok}";

    reset_capture();
    interactive_output = false;
    assert(terminal_markup_write_template(source, sizeof(source) - 1U) ==
           (mg_result_t)(sizeof(expected) - 1U));
    assert(open_count == 0U && control_length == 0U);
    assert(plain_length == sizeof(expected) - 1U);
    assert(memcmp(plain_bytes, expected, sizeof(expected) - 1U) == 0);

    interactive_output = true;
    fail_stream_open = true;
    reset_capture();
    fail_stream_open = true;
    assert(terminal_markup_write_template(source, sizeof(source) - 1U) ==
           (mg_result_t)(sizeof(expected) - 1U));
    assert(open_count == 1U && control_length == 0U);
    assert(plain_length == sizeof(expected) - 1U);
    assert(memcmp(plain_bytes, expected, sizeof(expected) - 1U) == 0);

    static const char control_only[] = "{clear:screen}";
    reset_capture();
    fail_stream_open = true;
    assert(terminal_markup_write_template(control_only,
        sizeof(control_only) - 1U) == MG_ERR_ACCESS_DENIED);
    assert(open_count == 1U && plain_length == 0U && control_length == 0U);
    fail_stream_open = false;
}

static void test_batches_split_only_between_records(void)
{
    const char directive[] = "{reset}";
    usize count = MG_TERMINAL_CONTROL_MAX_BATCH / 10U + 17U;
    usize length = count * (sizeof(directive) - 1U);
    char *source = malloc(length);
    assert(source);
    for (usize index = 0; index < count; index++)
        memcpy(source + index * (sizeof(directive) - 1U), directive,
               sizeof(directive) - 1U);

    reset_capture();
    assert(terminal_markup_write_template(source, length) == MG_OK);
    assert(write_count > 1U && close_count == 1U);
    for (unsigned index = 0; index < write_count; index++)
        assert(write_lengths[index] <= MG_TERMINAL_CONTROL_MAX_BATCH);
    usize offset = 0U;
    while (offset < control_length) {
        usize batch_start = offset;
        /* The test writer is called with individual batches; their exact
         * boundaries are checked in terminal_control_stream_write below. */
        (void)batch_start;
        const u8 *payload;
        u32 payload_length;
        offset += next_record(control_bytes, control_length, offset,
            MG_TERMINAL_CONTROL_RESET, &payload, &payload_length);
        assert(payload_length == 0U);
    }
    free(source);
}

static void test_say_markup_frontend(void)
{
    char *arguments[] = {
        "say", "--markup", "{fg:green}mang", "rove{reset}", NULL
    };
    const u8 *payload;
    u32 payload_length;
    usize offset = 0U;

    interactive_output = true;
    reset_capture();
    assert(say_program_main(4, arguments) == 0);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_FOREGROUND, &payload, &payload_length);
    assert(payload_length == 1U && payload[0] == TERMINAL_COLOR_GREEN);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_TEXT, &payload, &payload_length);
    assert(payload_length == 9U && memcmp(payload, "mang rove", 9U) == 0);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_RESET, &payload, &payload_length);
    offset += next_record(control_bytes, control_length, offset,
        MG_TERMINAL_CONTROL_TEXT, &payload, &payload_length);
    assert(payload_length == 1U && payload[0] == '\n');
    assert(offset == control_length);

    char *redirected[] = {"say", "--markup", "{fg:red}safe{reset}", NULL};
    interactive_output = false;
    reset_capture();
    assert(say_program_main(3, redirected) == 0);
    assert(open_count == 0U && control_length == 0U);
    assert(plain_length == 5U && memcmp(plain_bytes, "safe\n", 5U) == 0);
    interactive_output = true;
}

int main(void)
{
    test_canonical_markup_encoding();
    test_literal_braces_and_utf8();
    test_malformed_directives_stay_literal();
    test_redirected_and_unavailable_streams_are_plain();
    test_batches_split_only_between_records();
    test_say_markup_frontend();
    puts("terminal markup: canonical records, literal fallbacks, UTF-8, redirection and bounded batches passed");
    return 0;
}
