/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/terminal_markup.h>
#include <mg/object.h>
#include <mg/terminal.h>
#include <stdlib.h>
#include <string.h>

#define MARKUP_DIRECTIVE_MAX 128U

typedef struct {
    u8 type;
    u8 payload[8];
    u32 payload_length;
} markup_operation_t;

typedef struct {
    const u8 *text;
    usize text_length;
    usize consumed;
    u8 literal_brace;
    bool is_literal_brace;
    bool is_operation;
    markup_operation_t operation;
} markup_token_t;

static bool name_is(const u8 *text, usize length, const char *name)
{
    usize index = 0;
    while (name[index]) {
        if (index >= length || text[index] != (u8)name[index]) return false;
        index++;
    }
    return index == length;
}

static bool parse_unsigned(const u8 *text, usize length, u32 *out_value)
{
    u64 value = 0;
    if (!text || !length || !out_value) return false;
    for (usize index = 0; index < length; index++) {
        u8 byte = text[index];
        u32 digit;
        if (byte < '0' || byte > '9') return false;
        digit = byte - '0';
        if (value > (0xffffffffULL - digit) / 10ULL) return false;
        value = value * 10ULL + digit;
    }
    *out_value = (u32)value;
    return true;
}

static bool parse_color(const u8 *text, usize length, u8 *out_color)
{
    static const char *const names[TERMINAL_COLOR_COUNT] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan",
        "light-gray", "dark-gray", "bright-red", "bright-green",
        "bright-yellow", "bright-blue", "bright-magenta", "bright-cyan",
        "white"
    };

    if (!text || !out_color) return false;
    for (u8 color = 0; color < TERMINAL_COLOR_COUNT; color++) {
        if (name_is(text, length, names[color])) {
            *out_color = color;
            return true;
        }
    }
    return false;
}

static bool parse_style(const u8 *text, usize length, u8 *out_style)
{
    static const char *const names[TERMINAL_STYLE_COUNT] = {
        "default", "directory", "error", "warning", "success", "heading"
    };

    if (!text || !out_style) return false;
    for (u8 style = 0; style < TERMINAL_STYLE_COUNT; style++) {
        if (name_is(text, length, names[style])) {
            *out_style = style;
            return true;
        }
    }
    return false;
}

static void put_le32(u8 *output, u32 value)
{
    output[0] = (u8)value;
    output[1] = (u8)(value >> 8);
    output[2] = (u8)(value >> 16);
    output[3] = (u8)(value >> 24);
}

static bool parse_directive(const u8 *text, usize length,
                            markup_operation_t *operation)
{
    usize colon = 0;
    u8 value;

    if (!text || !operation || length < 2U) return false;
    while (colon < length && text[colon] != ':') colon++;

    if (colon == length) {
        if (!name_is(text, length, "reset")) return false;
        *operation = (markup_operation_t){
            .type = MG_TERMINAL_CONTROL_RESET,
            .payload_length = 0U,
        };
        return true;
    }

    const u8 *argument = text + colon + 1U;
    usize argument_length = length - colon - 1U;
    if (!argument_length) return false;

    if (name_is(text, colon, "fg") || name_is(text, colon, "bg")) {
        if (!parse_color(argument, argument_length, &value)) return false;
        *operation = (markup_operation_t){
            .type = name_is(text, colon, "fg") ?
                MG_TERMINAL_CONTROL_FOREGROUND :
                MG_TERMINAL_CONTROL_BACKGROUND,
            .payload = {value},
            .payload_length = 1U,
        };
        return true;
    }
    if (name_is(text, colon, "style")) {
        if (!parse_style(argument, argument_length, &value)) return false;
        *operation = (markup_operation_t){
            .type = MG_TERMINAL_CONTROL_STYLE,
            .payload = {value},
            .payload_length = 1U,
        };
        return true;
    }
    if (name_is(text, colon, "cursor")) {
        if (name_is(argument, argument_length, "show")) value = 1U;
        else if (name_is(argument, argument_length, "hide")) value = 0U;
        else return false;
        *operation = (markup_operation_t){
            .type = MG_TERMINAL_CONTROL_CURSOR_VISIBILITY,
            .payload = {value},
            .payload_length = 1U,
        };
        return true;
    }
    if (name_is(text, colon, "clear")) {
        if (name_is(argument, argument_length, "line"))
            value = MG_TERMINAL_CONTROL_CLEAR_LINE;
        else if (name_is(argument, argument_length, "to-end"))
            value = MG_TERMINAL_CONTROL_CLEAR_TO_END;
        else if (name_is(argument, argument_length, "screen"))
            value = MG_TERMINAL_CONTROL_CLEAR_SCREEN;
        else return false;
        *operation = (markup_operation_t){
            .type = MG_TERMINAL_CONTROL_CLEAR,
            .payload = {value},
            .payload_length = 1U,
        };
        return true;
    }
    if (name_is(text, colon, "at")) {
        usize comma = 0;
        u32 row;
        u32 column;
        while (comma < argument_length && argument[comma] != ',') comma++;
        if (comma == argument_length || !comma ||
            comma + 1U >= argument_length ||
            !parse_unsigned(argument, comma, &row) ||
            !parse_unsigned(argument + comma + 1U,
                            argument_length - comma - 1U, &column) ||
            row == 0U || column == 0U)
            return false;
        *operation = (markup_operation_t){
            .type = MG_TERMINAL_CONTROL_CURSOR_POSITION,
            .payload_length = 8U,
        };
        put_le32(operation->payload, row - 1U);
        put_le32(operation->payload + 4U, column - 1U);
        return true;
    }
    return false;
}

static markup_token_t next_token(const u8 *text, usize length, usize offset)
{
    markup_token_t token = {0};
    usize close;

    token.consumed = 1U;
    token.text = text + offset;
    token.text_length = 1U;
    if (text[offset] == '{' && offset + 1U < length &&
        text[offset + 1U] == '{') {
        token.consumed = 2U;
        token.text = NULL;
        token.text_length = 1U;
        token.literal_brace = '{';
        token.is_literal_brace = true;
        return token;
    }
    if (text[offset] == '}' && offset + 1U < length &&
        text[offset + 1U] == '}') {
        token.consumed = 2U;
        token.text = NULL;
        token.text_length = 1U;
        token.literal_brace = '}';
        token.is_literal_brace = true;
        return token;
    }
    if (text[offset] != '{') return token;

    close = offset + 1U;
    while (close < length && text[close] != '}') close++;
    if (close == length) {
        token.consumed = length - offset;
        token.text_length = token.consumed;
        return token;
    }

    token.consumed = close - offset + 1U;
    token.text_length = token.consumed;
    if (token.consumed > MARKUP_DIRECTIVE_MAX ||
        !parse_directive(text + offset + 1U, close - offset - 1U,
                         &token.operation))
        return token;

    token.text = NULL;
    token.text_length = 0U;
    token.is_operation = true;
    return token;
}

static bool add_size(usize *value, usize amount)
{
    if (amount > (usize)-1 - *value) return false;
    *value += amount;
    return true;
}

static bool measure_template(const u8 *text, usize length, bool encode,
                            usize *out_visible, usize *out_wire)
{
    usize visible = 0U;
    usize wire = 0U;
    usize pending_text = 0U;
    usize offset = 0U;

    while (offset < length) {
        markup_token_t token = next_token(text, length, offset);
        offset += token.consumed;
        if (token.is_operation) {
            if (encode && pending_text) {
                if (!add_size(&wire, MG_TERMINAL_CONTROL_HEADER_SIZE) ||
                    !add_size(&wire, pending_text))
                    return false;
                pending_text = 0U;
            }
            if (encode &&
                (!add_size(&wire, MG_TERMINAL_CONTROL_HEADER_SIZE) ||
                 !add_size(&wire, token.operation.payload_length)))
                return false;
            continue;
        }

        if (!add_size(&visible, token.text_length)) return false;
        if (!encode) continue;
        usize remaining = token.text_length;
        while (remaining) {
            if (pending_text == MG_TERMINAL_CONTROL_MAX_TEXT) {
                if (!add_size(&wire, MG_TERMINAL_CONTROL_HEADER_SIZE) ||
                    !add_size(&wire, pending_text))
                    return false;
                pending_text = 0U;
            }
            usize available = MG_TERMINAL_CONTROL_MAX_TEXT - pending_text;
            usize amount = remaining < available ? remaining : available;
            pending_text += amount;
            remaining -= amount;
        }
    }
    if (encode && pending_text &&
        (!add_size(&wire, MG_TERMINAL_CONTROL_HEADER_SIZE) ||
         !add_size(&wire, pending_text)))
        return false;
    if (out_visible) *out_visible = visible;
    if (out_wire) *out_wire = wire;
    return true;
}

static u8 token_text_byte(const markup_token_t *token, usize index)
{
    return token->is_literal_brace ? token->literal_brace : token->text[index];
}

static void write_record_header(u8 *output, u8 type, u32 payload_length)
{
    output[0] = 'M'; output[1] = 'G'; output[2] = 'T'; output[3] = 'C';
    output[4] = MG_TERMINAL_CONTROL_VERSION;
    output[5] = type;
    put_le32(output + 6U, payload_length);
}

typedef struct {
    u8 *buffer;
    usize capacity;
    usize length;
    usize text_record_start;
    u32 text_length;
} wire_builder_t;

static bool wire_close_text(wire_builder_t *builder)
{
    if (!builder->text_length) return true;
    put_le32(builder->buffer + builder->text_record_start + 6U,
             builder->text_length);
    builder->text_length = 0U;
    return true;
}

static bool wire_append_text(wire_builder_t *builder, u8 byte)
{
    if (builder->text_length == MG_TERMINAL_CONTROL_MAX_TEXT &&
        !wire_close_text(builder))
        return false;
    if (!builder->text_length) {
        if (builder->capacity - builder->length <
            MG_TERMINAL_CONTROL_HEADER_SIZE + 1U)
            return false;
        builder->text_record_start = builder->length;
        write_record_header(builder->buffer + builder->length,
                            MG_TERMINAL_CONTROL_TEXT, 0U);
        builder->length += MG_TERMINAL_CONTROL_HEADER_SIZE;
    }
    if (builder->length >= builder->capacity) return false;
    builder->buffer[builder->length++] = byte;
    builder->text_length++;
    return true;
}

static bool wire_append_operation(wire_builder_t *builder,
                                  const markup_operation_t *operation)
{
    usize record_size = MG_TERMINAL_CONTROL_HEADER_SIZE +
                        operation->payload_length;
    if (!wire_close_text(builder) ||
        record_size > builder->capacity - builder->length)
        return false;
    write_record_header(builder->buffer + builder->length,
                        operation->type, operation->payload_length);
    builder->length += MG_TERMINAL_CONTROL_HEADER_SIZE;
    if (operation->payload_length) {
        memcpy(builder->buffer + builder->length, operation->payload,
               operation->payload_length);
        builder->length += operation->payload_length;
    }
    return true;
}

static bool build_wire(const u8 *text, usize length, u8 *output,
                       usize capacity, usize *out_length)
{
    wire_builder_t builder = {output, capacity, 0U, 0U, 0U};
    usize offset = 0U;

    while (offset < length) {
        markup_token_t token = next_token(text, length, offset);
        offset += token.consumed;
        if (token.is_operation) {
            if (!wire_append_operation(&builder, &token.operation))
                return false;
        } else {
            for (usize index = 0; index < token.text_length; index++)
                if (!wire_append_text(&builder,
                        token_text_byte(&token, index)))
                    return false;
        }
    }
    if (!wire_close_text(&builder)) return false;
    *out_length = builder.length;
    return builder.length == capacity;
}

static bool build_visible_text(const u8 *text, usize length, u8 *output,
                               usize capacity)
{
    usize offset = 0U;
    usize used = 0U;

    while (offset < length) {
        markup_token_t token = next_token(text, length, offset);
        offset += token.consumed;
        if (token.is_operation) continue;
        if (token.text_length > capacity - used) return false;
        for (usize index = 0; index < token.text_length; index++)
            output[used++] = token_text_byte(&token, index);
    }
    return used == capacity;
}

static u32 read_le32(const u8 *bytes)
{
    return (u32)bytes[0] | (u32)bytes[1] << 8 |
           (u32)bytes[2] << 16 | (u32)bytes[3] << 24;
}

static mg_result_t write_wire_batches(mg_handle_t stream, const u8 *wire,
                                      usize length)
{
    usize offset = 0U;
    while (offset < length) {
        usize batch_start = offset;
        usize batch_length = 0U;
        while (offset < length) {
            usize remaining = length - offset;
            if (remaining < MG_TERMINAL_CONTROL_HEADER_SIZE)
                return MG_ERR_IO;
            usize record_length = MG_TERMINAL_CONTROL_HEADER_SIZE +
                                  (usize)read_le32(wire + offset + 6U);
            if (record_length > remaining ||
                record_length > MG_TERMINAL_CONTROL_MAX_BATCH)
                return MG_ERR_IO;
            if (batch_length && record_length >
                MG_TERMINAL_CONTROL_MAX_BATCH - batch_length)
                break;
            batch_length += record_length;
            offset += record_length;
        }
        if (!batch_length) return MG_ERR_IO;
        mg_result_t result = terminal_control_stream_write(
            stream, wire + batch_start, batch_length);
        if (result != MG_OK) return result;
    }
    return MG_OK;
}

mg_result_t terminal_markup_write_template(const void *source, usize length)
{
    const u8 *text = (const u8 *)source;
    mg_terminal_capabilities_t capabilities;
    bool interactive = false;
    usize visible_length;
    usize wire_length = 0U;
    u8 *output;
    mg_result_t result;

    if (length && !text) return MG_ERR_BAD_ARGUMENT;
    if (!length) return MG_OK;

    if (terminal_get_capabilities(&capabilities) == MG_OK &&
        (capabilities.capabilities & MG_TERMINAL_CAP_INTERACTIVE_OUTPUT))
        interactive = true;
    if (!measure_template(text, length, interactive, &visible_length,
                          &wire_length))
        return MG_ERR_BUFFER_TOO_SMALL;

    if (!interactive) {
        if (!visible_length) return MG_OK;
        output = (u8 *)malloc(visible_length);
        if (!output) return MG_ERR_NO_MEMORY;
        if (!build_visible_text(text, length, output, visible_length)) {
            free(output);
            return MG_ERR_IO;
        }
        result = console_write(output, visible_length);
        free(output);
        return result;
    }

    if (!wire_length) return MG_OK;
    output = (u8 *)malloc(wire_length);
    if (!output) return MG_ERR_NO_MEMORY;
    usize built_length = 0U;
    if (!build_wire(text, length, output, wire_length, &built_length)) {
        free(output);
        return MG_ERR_IO;
    }

    mg_handle_t stream;
    result = terminal_control_stream_open(&stream);
    if (result != MG_OK) {
        /* A capability/ownership transition can race the probe. Preserve the
         * safe visible text path, never apply controls through stdout. */
        free(output);
        if (!visible_length) return result;
        output = (u8 *)malloc(visible_length);
        if (!output) return MG_ERR_NO_MEMORY;
        if (build_visible_text(text, length, output, visible_length))
            result = console_write(output, visible_length);
        else
            result = MG_ERR_IO;
        free(output);
        return result;
    }
    result = write_wire_batches(stream, output, built_length);
    mg_result_t close_result = handle_close(stream);
    if (result == MG_OK && close_result != MG_OK) result = close_result;
    free(output);
    return result;
}
