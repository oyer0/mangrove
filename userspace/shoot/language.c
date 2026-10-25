/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "language.h"

#include <mangrove.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/path.h"

static const char *skip_spaces(const char *text)
{
    while (text && (*text == ' ' || *text == '\t')) text++;
    return text;
}

static bool identifier_start(char value)
{
    return (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') || value == '_';
}

static bool identifier_character(char value)
{
    return identifier_start(value) || (value >= '0' && value <= '9');
}

static bool grow_capacity(usize current, usize required, usize element_size,
                          usize *out_capacity)
{
    usize capacity = current ? current : 32U;

    if (!out_capacity || !element_size ||
        required > (usize)-1 / element_size) return false;
    while (capacity < required) {
        if (capacity > (usize)-1 / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    if (capacity > (usize)-1 / element_size) capacity = required;
    if (capacity < required || capacity > (usize)-1 / element_size)
        return false;
    *out_capacity = capacity;
    return true;
}

static bool command_reserve_storage(shell_command_t *command,
                                   usize required, bool with_flags)
{
    usize capacity;

    if (!command) return false;
    if (command->storage_capacity < required) {
        char *replacement;

        if (!grow_capacity(command->storage_capacity, required, 1U,
                           &capacity)) {
            command->allocation_failed = true;
            return false;
        }
        replacement = (char *)realloc(command->storage, capacity);
        if (!replacement) {
            command->allocation_failed = true;
            return false;
        }
        command->storage = replacement;
        command->storage_capacity = capacity;
    }
    if (with_flags && command->glob_flags_capacity < required) {
        u8 *replacement;

        if (!grow_capacity(command->glob_flags_capacity, required, 1U,
                           &capacity)) {
            command->allocation_failed = true;
            return false;
        }
        replacement = (u8 *)realloc(command->glob_flags, capacity);
        if (!replacement) {
            command->allocation_failed = true;
            return false;
        }
        command->glob_flags = replacement;
        command->glob_flags_capacity = capacity;
    }
    return true;
}

static bool command_reserve_arguments(shell_command_t *command,
                                     usize required)
{
    usize capacity;

    if (!command) return false;
    if (command->argument_capacity >= required) return true;
    if (!grow_capacity(command->argument_capacity, required,
                       sizeof(*command->parsed_arguments), &capacity)) {
        command->allocation_failed = true;
        return false;
    }
    const char **arguments = (const char **)realloc(
        command->parsed_arguments, capacity * sizeof(*arguments));
    if (!arguments) {
        command->allocation_failed = true;
        return false;
    }
    command->parsed_arguments = arguments;
    usize *offsets = (usize *)realloc(
        command->argument_offsets, capacity * sizeof(*offsets));
    if (!offsets) {
        command->allocation_failed = true;
        return false;
    }
    command->argument_offsets = offsets;
    const u8 **flags = (const u8 **)realloc(
        command->argument_glob_flags, capacity * sizeof(*flags));
    if (!flags) {
        command->allocation_failed = true;
        return false;
    }
    command->argument_glob_flags = flags;
    bool *home_expand = (bool *)realloc(
        command->argument_home_expand, capacity * sizeof(*home_expand));
    if (!home_expand) {
        command->allocation_failed = true;
        return false;
    }
    command->argument_home_expand = home_expand;
    command->argument_capacity = capacity;
    command->arguments = command->parsed_arguments;
    return true;
}

void shell_command_destroy(shell_command_t *command)
{
    if (!command) return;
    free(command->parsed_arguments);
    free(command->argument_offsets);
    free(command->argument_glob_flags);
    free(command->argument_home_expand);
    free(command->storage);
    free(command->glob_flags);
    memset(command, 0, sizeof(*command));
}

static bool append_character(shell_command_t *command, char value,
                             bool glob_active, bool track_glob)
{
    usize required;
    bool with_flags = track_glob;

    if (!command || command->storage_length > (usize)-1 - 2U) return false;
    required = command->storage_length + 2U;
    if (!command_reserve_storage(command, required, with_flags)) return false;
    command->storage[command->storage_length] = value;
    if (with_flags)
        command->glob_flags[command->storage_length] =
            glob_active ? 1U : 0U;
    command->storage_length++;
    return true;
}

static bool append_text(shell_command_t *command, const char *text,
                        bool glob_active, bool track_glob)
{
    usize length;
    usize required;
    bool with_flags = track_glob;

    if (!command || !text) return false;
    length = strlen(text);
    if (length > (usize)-1 - command->storage_length - 1U) return false;
    required = command->storage_length + length + 1U;
    if (!command_reserve_storage(command, required, with_flags)) return false;
    memcpy(command->storage + command->storage_length, text, length);
    if (with_flags)
        memset(command->glob_flags + command->storage_length,
               glob_active ? 1 : 0, length);
    command->storage_length += length;
    return true;
}

static const char *variable_value(const shell_state_t *state,
                                  const char *name, usize length)
{
    if (!state || !name || !length) return "";
    for (usize index = 0; index < state->variable_count; index++) {
        const shell_variable_t *variable = &state->variables[index];
        if (strlen(variable->name) == length &&
            !strncmp(variable->name, name, length)) return variable->value;
    }
    return "";
}

static bool append_variable(const shell_state_t *state, const char **cursor,
                            shell_command_t *command, bool glob_active,
                            bool track_glob)
{
    const char *start;
    usize length = 0;

    if (!state || !cursor || !*cursor || **cursor != '$' ||
        !command) return false;
    start = ++*cursor;
    if (!identifier_start(*start))
        return append_character(command, '$', glob_active, track_glob);
    while (identifier_character(start[length])) length++;
    if (!append_text(command, variable_value(state, start, length),
                     glob_active, track_glob)) return false;
    *cursor = start + length;
    return true;
}

static bool operator_at(const char *text)
{
    return text && (*text == '<' || *text == '>' ||
                    (*text == '=' && text[1] == '='));
}

static bool parse_word(const shell_state_t *state, const char **cursor,
                       shell_command_t *command,
                       const char **out_word, bool *out_home_expand,
                       bool stop_operators, bool track_glob)
{
    const char *source;
    usize start;
    char quote = '\0';
    bool saw_character = false;

    if (!cursor || !*cursor || !command || !out_word ||
        !out_home_expand) return false;
    source = *cursor;
    start = command->storage_length;
    *out_home_expand = source[0] != '\'';
    while (*source) {
        char value = *source;

        if (quote) {
            if (value == quote) {
                quote = '\0';
                source++;
                saw_character = true;
                continue;
            }
            if (quote == '"' && value == '$') {
                if (!append_variable(state, &source, command, false,
                                     track_glob))
                    return false;
                saw_character = true;
                continue;
            }
        } else {
            if (value == '\\') {
                source++;
                if (!*source) return false;
                if (command->storage_length == start)
                    *out_home_expand = false;
                if (!append_character(command, *source, false, track_glob))
                    return false;
                source++;
                saw_character = true;
                continue;
            }
            if (value == '\'' || value == '"') {
                quote = value;
                source++;
                saw_character = true;
                continue;
            }
            if (value == ' ' || value == '\t' || value == '>' ||
                (stop_operators && operator_at(source)) ||
                (value == '/' && source[1] == '/')) break;
            if (value == '$') {
                if (!append_variable(state, &source, command, true,
                                     track_glob))
                    return false;
                saw_character = true;
                continue;
            }
        }
        if (!append_character(command, value, track_glob && quote == '\0',
                              track_glob))
            return false;
        source++;
        saw_character = true;
    }
    if (quote || !saw_character || command->storage_length == (usize)-1)
        return false;
    if (!command_reserve_storage(command, command->storage_length + 1U,
                                 track_glob)) return false;
    command->storage[command->storage_length] = '\0';
    if (track_glob)
        command->glob_flags[command->storage_length] = 0;
    command->storage_length++;
    *cursor = source;
    *out_word = command->storage + start;
    return true;
}

shell_parse_result_t shell_parse_command(shell_state_t *state,
                                          const char *line,
                                          shell_command_t *command)
{
    const char *cursor;
    bool name_seen = false;
    bool output_seen = false;
    bool output_home_expand = false;

    if (!line || !command) return SHELL_PARSE_EMPTY;
    memset(command, 0, sizeof(*command));
    cursor = skip_spaces(line);
    if (!cursor || *cursor == '\0' ||
        (cursor[0] == '/' && cursor[1] == '/'))
        return SHELL_PARSE_EMPTY;
    command->name = NULL;
    command->argument_count = 0;
    command->output_path = NULL;
    command->output_append = false;

    while (*cursor) {
        const char *word;

        if (cursor[0] == '/' && cursor[1] == '/') break;
        if (*cursor == '>') {
            bool append = cursor[1] == '>';

            if (output_seen) return SHELL_PARSE_INVALID_SYNTAX;
            output_seen = true;
            cursor += append ? 2 : 1;
            cursor = skip_spaces(cursor);
            if (!cursor || !*cursor || (cursor[0] == '/' && cursor[1] == '/'))
                return SHELL_PARSE_INVALID_SYNTAX;
            if (!parse_word(state, &cursor, command, &word,
                            &output_home_expand, false, false))
                return command->allocation_failed ? SHELL_PARSE_NO_MEMORY :
                    SHELL_PARSE_INVALID_SYNTAX;
            if (!word[0]) return SHELL_PARSE_INVALID_SYNTAX;
            command->output_path_offset =
                (usize)(word - command->storage);
            command->output_append = append;
            cursor = skip_spaces(cursor);
            continue;
        }

        usize word_start = command->storage_length;
        bool home_expand = false;
        if (!parse_word(state, &cursor, command, &word,
                        name_seen ? &home_expand : &output_home_expand,
                        false, name_seen))
            return command->allocation_failed ? SHELL_PARSE_NO_MEMORY :
                SHELL_PARSE_INVALID_SYNTAX;
        if (!name_seen) {
            command->name_offset = (usize)(word - command->storage);
            name_seen = true;
        } else {
            if (command->argument_count == (usize)-1 ||
                !command_reserve_arguments(command,
                                           command->argument_count + 1U))
                return SHELL_PARSE_NO_MEMORY;
            command->argument_offsets[command->argument_count] = word_start;
            command->argument_home_expand[command->argument_count] =
                home_expand;
            command->argument_count++;
            command->arguments = command->parsed_arguments;
        }
        cursor = skip_spaces(cursor);
    }
    if (!name_seen) return SHELL_PARSE_INVALID_SYNTAX;
    command->name = command->storage + command->name_offset;
    if (output_seen)
        command->output_path = command->storage +
            command->output_path_offset;
    command->output_home_expand = output_home_expand;
    for (usize index = 0; index < command->argument_count; index++) {
        command->parsed_arguments[index] = command->storage +
            command->argument_offsets[index];
        command->argument_glob_flags[index] = command->glob_flags +
            command->argument_offsets[index];
    }
    return SHELL_PARSE_OK;
}

static bool parse_signed_integer(const char *text, i64 *out)
{
    const char *cursor = text;
    bool negative = false;
    u64 limit;
    u64 magnitude = 0;

    if (!text || !*text || !out) return false;
    if (*cursor == '+' || *cursor == '-') {
        negative = *cursor == '-';
        cursor++;
    }
    if (!*cursor) return false;
    limit = negative ? (1ULL << 63) : ((1ULL << 63) - 1ULL);
    while (*cursor) {
        u64 digit;

        if (*cursor < '0' || *cursor > '9') return false;
        digit = (u64)(*cursor - '0');
        if (magnitude > (limit - digit) / 10ULL) return false;
        magnitude = magnitude * 10ULL + digit;
        cursor++;
    }
    if (negative) {
        *out = magnitude == (1ULL << 63) ? (i64)(1ULL << 63) :
            -(i64)magnitude;
    } else {
        *out = (i64)magnitude;
    }
    return true;
}

static bool arithmetic_result(i64 left, char operation, i64 right,
                              i64 *out)
{
    if (!out) return false;
    if (operation == '+') {
        if ((right > 0 && left > (i64)((1ULL << 63) - 1ULL) - right) ||
            (right < 0 && left < (i64)(1ULL << 63) - right)) return false;
        *out = left + right;
        return true;
    }
    if (operation == '-') {
        if ((right > 0 && left < (i64)(1ULL << 63) + right) ||
            (right < 0 && left > (i64)((1ULL << 63) - 1ULL) + right))
            return false;
        *out = left - right;
        return true;
    }
    return false;
}

static bool set_variable(shell_state_t *state, const char *name,
                         usize name_length, const char *value)
{
    usize value_length;
    usize index;
    char *name_copy = NULL;
    char *value_copy;

    if (!state || !name || !name_length || !value) return false;
    value_length = strlen(value);
    if (name_length == (usize)-1 || value_length == (usize)-1) return false;
    for (index = 0; index < state->variable_count; index++) {
        const char *existing = state->variables[index].name;
        if (strlen(existing) == name_length &&
            !memcmp(existing, name, name_length)) break;
    }
    value_copy = (char *)malloc(value_length + 1U);
    if (!value_copy) return false;
    memcpy(value_copy, value, value_length + 1U);
    if (index == state->variable_count) {
        usize capacity = state->variable_capacity ?
            state->variable_capacity : 16U;

        name_copy = (char *)malloc(name_length + 1U);
        if (!name_copy) {
            free(value_copy);
            return false;
        }
        memcpy(name_copy, name, name_length);
        name_copy[name_length] = '\0';
        if (state->variable_count == (usize)-1) {
            free(name_copy);
            free(value_copy);
            return false;
        }
        usize required = state->variable_count + 1U;
        while (capacity < required) {
            if (capacity > (usize)-1 / 2U) {
                capacity = required;
                break;
            }
            capacity *= 2U;
        }
        if (capacity > (usize)-1 / sizeof(*state->variables)) {
            free(name_copy);
            free(value_copy);
            return false;
        }
        if (capacity != state->variable_capacity) {
            shell_variable_t *replacement = (shell_variable_t *)realloc(
                state->variables, capacity * sizeof(*replacement));
            if (!replacement) {
                free(name_copy);
                free(value_copy);
                return false;
            }
            state->variables = replacement;
            state->variable_capacity = capacity;
        }
        state->variables[index].name = name_copy;
        state->variable_count++;
    } else {
        free(state->variables[index].value);
    }
    state->variables[index].value = value_copy;
    return true;
}

shell_assignment_result_t shell_try_assignment(shell_state_t *state,
                                                const char *line)
{
    shell_command_t parsed = {0};
    usize value_offsets[3];
    const char *cursor;
    usize name_length = 0;
    usize value_count = 0;
    const char *name_start;

    if (!state || !line) return SHELL_ASSIGNMENT_NOT_PRESENT;
    cursor = skip_spaces(line);
    if (!cursor || !*cursor) return SHELL_ASSIGNMENT_NOT_PRESENT;
    name_start = cursor;
    while (*cursor && *cursor != '=' && *cursor != ' ' && *cursor != '\t')
        cursor++;
    name_length = (usize)(cursor - name_start);
    cursor = skip_spaces(cursor);
    if (*cursor != '=' || cursor[1] == '=')
        return SHELL_ASSIGNMENT_NOT_PRESENT;
    if (!name_length || !identifier_start(name_start[0]))
        return SHELL_ASSIGNMENT_INVALID;
    for (usize index = 1; index < name_length; index++)
        if (!identifier_character(name_start[index]))
            return SHELL_ASSIGNMENT_INVALID;
    cursor = skip_spaces(cursor + 1);
    while (*cursor && !(cursor[0] == '/' && cursor[1] == '/')) {
        const char *word;
        bool home_expand;

        if (value_count == sizeof(value_offsets) / sizeof(value_offsets[0])) {
            shell_command_destroy(&parsed);
            return SHELL_ASSIGNMENT_INVALID;
        }
        value_offsets[value_count] = parsed.storage_length;
        if (!parse_word(state, &cursor, &parsed, &word, &home_expand,
                        false, false)) {
            bool allocation_failed = parsed.allocation_failed;
            shell_command_destroy(&parsed);
            return allocation_failed ? SHELL_ASSIGNMENT_NO_MEMORY :
                SHELL_ASSIGNMENT_INVALID;
        }
        (void)word;
        value_count++;
        cursor = skip_spaces(cursor);
        if (*cursor == '>') {
            shell_command_destroy(&parsed);
            return SHELL_ASSIGNMENT_INVALID;
        }
    }
    if (value_count == 0) {
        shell_command_destroy(&parsed);
        return SHELL_ASSIGNMENT_INVALID;
    }
    const char *first_value = parsed.storage + value_offsets[0];
    if (value_count == 1) {
        bool success = set_variable(state, name_start, name_length,
                                    first_value);
        shell_command_destroy(&parsed);
        return success ? SHELL_ASSIGNMENT_OK : SHELL_ASSIGNMENT_NO_MEMORY;
    }
    if (value_count != 3) {
        shell_command_destroy(&parsed);
        return SHELL_ASSIGNMENT_INVALID;
    }
    const char *operation_text = parsed.storage + value_offsets[1];
    const char *third_value = parsed.storage + value_offsets[2];
    if (strlen(operation_text) != 1 ||
        (operation_text[0] != '+' && operation_text[0] != '-')) {
        shell_command_destroy(&parsed);
        return SHELL_ASSIGNMENT_INVALID;
    }
    {
        i64 left;
        i64 right;
        i64 result;
        char output[32];

        if (!parse_signed_integer(first_value, &left) ||
            !parse_signed_integer(third_value, &right) ||
            !arithmetic_result(left, operation_text[0], right, &result) ||
            snprintf(output, sizeof(output), "%lld", (long long)result) < 0) {
            shell_command_destroy(&parsed);
            return SHELL_ASSIGNMENT_INVALID;
        }
        bool success = set_variable(state, name_start, name_length, output);
        shell_command_destroy(&parsed);
        if (!success) return SHELL_ASSIGNMENT_NO_MEMORY;
    }
    return SHELL_ASSIGNMENT_OK;
}

static bool parse_condition_operator(const char **cursor, char *operation)
{
    const char *source;

    if (!cursor || !*cursor || !operation) return false;
    source = skip_spaces(*cursor);
    if (source[0] == '=' && source[1] == '=') {
        *operation = '=';
        *cursor = source + 2;
        return true;
    }
    if ((source[0] == '<' || source[0] == '>') && source[1] == '=') {
        *operation = source[0] == '<' ? 'L' : 'G';
        *cursor = source + 2;
        return true;
    }
    if (source[0] == '<' || source[0] == '>') {
        *operation = source[0];
        *cursor = source + 1;
        return true;
    }
    return false;
}

static bool condition_tail(const char *cursor)
{
    cursor = skip_spaces(cursor);
    return !*cursor || (cursor[0] == '/' && cursor[1] == '/');
}

bool shell_evaluate_condition(shell_state_t *state, const char *text,
                              bool *out_value)
{
    shell_command_t parsed = {0};
    const char *cursor = text;
    const char *left;
    const char *right;
    bool home_expand;
    char operation;
    usize left_offset;
    usize right_offset;
    bool success = false;

    if (!state || !text || !out_value) return false;
    if (!parse_word(state, &cursor, &parsed, &left, &home_expand,
                    true, false)) goto done;
    left_offset = (usize)(left - parsed.storage);
    cursor = skip_spaces(cursor);
    if (!strcmp(parsed.storage + left_offset, "exists")) {
        COMMAND_PATH_AUTO char *expanded = NULL;
        mg_path_info_t info;

        if (!parse_word(state, &cursor, &parsed, &right, &home_expand,
                        true, false)) goto done;
        right_offset = (usize)(right - parsed.storage);
        if (!condition_tail(cursor) ||
            !command_expand_home_path_alloc(parsed.storage + right_offset,
                                            home_expand, &expanded))
            goto done;
        *out_value = path_info(expanded, &info) == MG_OK;
        success = true;
        goto done;
    }
    if (!parse_condition_operator(&cursor, &operation)) goto done;
    {
        i64 left_number;
        i64 right_number;

        cursor = skip_spaces(cursor);
        if (!parse_word(state, &cursor, &parsed, &right, &home_expand,
                        true, false)) goto done;
        right_offset = (usize)(right - parsed.storage);
        left = parsed.storage + left_offset;
        right = parsed.storage + right_offset;
        if (!condition_tail(cursor)) goto done;
        if (operation == '=') {
            *out_value = !strcmp(left, right);
            success = true;
            goto done;
        }
        if (!parse_signed_integer(left, &left_number) ||
            !parse_signed_integer(right, &right_number)) goto done;
        switch (operation) {
        case '<': *out_value = left_number < right_number; break;
        case 'L': *out_value = left_number <= right_number; break;
        case '>': *out_value = left_number > right_number; break;
        case 'G': *out_value = left_number >= right_number; break;
        default: goto done;
        }
        success = true;
    }
done:
    shell_command_destroy(&parsed);
    return success;
}

bool shell_line_keyword(const char *line, const char *keyword,
                        const char **out_rest)
{
    const char *cursor;
    usize length;

    if (!line || !keyword) return false;
    cursor = skip_spaces(line);
    length = strlen(keyword);
    if (strncmp(cursor, keyword, length) != 0 ||
        (cursor[length] && cursor[length] != ' ' && cursor[length] != '\t' &&
         !(cursor[length] == '/' && cursor[length + 1] == '/')))
        return false;
    if (out_rest) *out_rest = skip_spaces(cursor + length);
    return true;
}

bool shell_line_is_blank_or_comment(const char *line)
{
    const char *cursor = skip_spaces(line);
    return !cursor || !*cursor || (cursor[0] == '/' && cursor[1] == '/');
}
