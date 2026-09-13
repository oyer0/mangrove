/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "language.h"

#include <mangrove.h>
#include <stdio.h>
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

static bool append_character(char *storage, usize *used, usize capacity,
                             char value)
{
    if (!storage || !used || *used + 1U >= capacity) return false;
    storage[(*used)++] = value;
    return true;
}

static bool append_text(char *storage, usize *used, usize capacity,
                        const char *text)
{
    usize length;

    if (!storage || !used || !text) return false;
    length = strlen(text);
    if (*used + length + 1U > capacity) return false;
    memcpy(storage + *used, text, length);
    *used += length;
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
                            char *storage, usize *used, usize capacity)
{
    const char *start;
    usize length = 0;

    if (!state || !cursor || !*cursor || **cursor != '$' ||
        !storage || !used) return false;
    start = ++*cursor;
    if (!identifier_start(*start))
        return append_character(storage, used, capacity, '$');
    while (identifier_character(start[length])) length++;
    if (!append_text(storage, used, capacity,
                     variable_value(state, start, length))) return false;
    *cursor = start + length;
    return true;
}

static bool operator_at(const char *text)
{
    return text && (*text == '<' || *text == '>' ||
                    (*text == '=' && text[1] == '='));
}

static bool parse_word(const shell_state_t *state, const char **cursor,
                       char *storage, usize capacity, usize *used,
                       const char **out_word, bool *out_home_expand,
                       bool stop_operators)
{
    const char *source;
    usize start;
    char quote = '\0';
    bool saw_character = false;

    if (!cursor || !*cursor || !storage || !used || !out_word ||
        !out_home_expand || *used >= capacity) return false;
    source = *cursor;
    start = *used;
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
                if (!append_variable(state, &source, storage, used,
                                     capacity)) return false;
                saw_character = true;
                continue;
            }
        } else {
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
                if (!append_variable(state, &source, storage, used,
                                     capacity)) return false;
                saw_character = true;
                continue;
            }
        }
        if (!append_character(storage, used, capacity, value)) return false;
        source++;
        saw_character = true;
    }
    if (quote || !saw_character || *used + 1U >= capacity) return false;
    storage[(*used)++] = '\0';
    *cursor = source;
    *out_word = storage + start;
    return true;
}

shell_parse_result_t shell_parse_command(shell_state_t *state,
                                          const char *line,
                                          shell_command_t *command)
{
    const char *cursor;
    usize storage_length = 0;

    if (!line || !command) return SHELL_PARSE_EMPTY;
    memset(command->storage, 0, sizeof(command->storage));
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

            if (command->output_path) return SHELL_PARSE_INVALID_SYNTAX;
            cursor += append ? 2 : 1;
            cursor = skip_spaces(cursor);
            if (!cursor || !*cursor || (cursor[0] == '/' && cursor[1] == '/'))
                return SHELL_PARSE_INVALID_SYNTAX;
            if (!parse_word(state, &cursor, command->storage,
                            sizeof(command->storage), &storage_length, &word,
                            &command->output_home_expand, false) ||
                !word[0]) return SHELL_PARSE_INVALID_SYNTAX;
            command->output_path = word;
            command->output_append = append;
            cursor = skip_spaces(cursor);
            continue;
        }

        if (command->name && command->argument_count == SHOOT_MAX_ARGUMENTS)
            return SHELL_PARSE_TOO_MANY_ARGUMENTS;
        if (!parse_word(state, &cursor, command->storage,
                        sizeof(command->storage), &storage_length, &word,
                        command->name ?
                            &command->argument_home_expand[
                                command->argument_count] :
                            &command->output_home_expand, false))
            return SHELL_PARSE_INVALID_SYNTAX;
        if (!command->name) {
            command->name = word;
        } else {
            command->arguments[command->argument_count++] = word;
        }
        cursor = skip_spaces(cursor);
    }
    return command->name ? SHELL_PARSE_OK : SHELL_PARSE_INVALID_SYNTAX;
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
                         const char *value)
{
    usize index;

    if (!state || !name || !value || strlen(name) >= SHOOT_VARIABLE_NAME_CAPACITY ||
        strlen(value) >= SHOOT_VARIABLE_VALUE_CAPACITY) return false;
    for (index = 0; index < state->variable_count; index++)
        if (!strcmp(state->variables[index].name, name)) break;
    if (index == state->variable_count) {
        if (index == SHOOT_MAX_VARIABLES) return false;
        state->variable_count++;
    }
    strcpy(state->variables[index].name, name);
    strcpy(state->variables[index].value, value);
    return true;
}

shell_assignment_result_t shell_try_assignment(shell_state_t *state,
                                                const char *line)
{
    char name[SHOOT_VARIABLE_NAME_CAPACITY];
    char storage[SHOOT_COMMAND_STORAGE_CAPACITY];
    const char *values[3];
    const char *cursor;
    usize name_length = 0;
    usize value_count = 0;
    usize used = 0;

    if (!state || !line) return SHELL_ASSIGNMENT_NOT_PRESENT;
    cursor = skip_spaces(line);
    if (!cursor || !identifier_start(*cursor))
        return SHELL_ASSIGNMENT_NOT_PRESENT;
    while (identifier_character(cursor[name_length])) name_length++;
    if (name_length >= sizeof(name)) {
        while (identifier_character(cursor[name_length])) name_length++;
        return SHELL_ASSIGNMENT_INVALID;
    }
    memcpy(name, cursor, name_length);
    name[name_length] = '\0';
    cursor += name_length;
    cursor = skip_spaces(cursor);
    if (*cursor != '=' || cursor[1] == '=')
        return SHELL_ASSIGNMENT_NOT_PRESENT;
    cursor = skip_spaces(cursor + 1);
    memset(storage, 0, sizeof(storage));
    while (*cursor && !(cursor[0] == '/' && cursor[1] == '/')) {
        const char *word;
        bool home_expand;

        if (value_count == sizeof(values) / sizeof(values[0]))
            return SHELL_ASSIGNMENT_INVALID;
        if (!parse_word(state, &cursor, storage, sizeof(storage), &used,
                        &word, &home_expand, false))
            return SHELL_ASSIGNMENT_INVALID;
        values[value_count++] = word;
        cursor = skip_spaces(cursor);
        if (*cursor == '>') return SHELL_ASSIGNMENT_INVALID;
    }
    if (value_count == 0) return SHELL_ASSIGNMENT_INVALID;
    if (value_count == 1)
        return set_variable(state, name, values[0])
            ? SHELL_ASSIGNMENT_OK : SHELL_ASSIGNMENT_INVALID;
    if (value_count != 3 || strlen(values[1]) != 1 ||
        (values[1][0] != '+' && values[1][0] != '-'))
        return SHELL_ASSIGNMENT_INVALID;
    {
        i64 left;
        i64 right;
        i64 result;
        char output[SHOOT_VARIABLE_VALUE_CAPACITY];

        if (!parse_signed_integer(values[0], &left) ||
            !parse_signed_integer(values[2], &right) ||
            !arithmetic_result(left, values[1][0], right, &result) ||
            snprintf(output, sizeof(output), "%lld", (long long)result) < 0 ||
            !set_variable(state, name, output))
            return SHELL_ASSIGNMENT_INVALID;
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
    char storage[SHOOT_COMMAND_STORAGE_CAPACITY];
    const char *cursor = text;
    const char *left;
    const char *right;
    bool home_expand;
    char operation;
    usize used = 0;

    if (!state || !text || !out_value) return false;
    memset(storage, 0, sizeof(storage));
    if (!parse_word(state, &cursor, storage, sizeof(storage),
                    &used, &left, &home_expand, true)) return false;
    cursor = skip_spaces(cursor);
    if (!strcmp(left, "exists")) {
        char expanded[256];
        mg_path_info_t info;

        used = strlen(left) + 1U;
        if (!parse_word(state, &cursor, storage, sizeof(storage), &used,
                        &right, &home_expand, true) ||
            !condition_tail(cursor) ||
            !command_expand_home_path(right, home_expand, expanded,
                                      sizeof(expanded))) return false;
        *out_value = path_info(expanded, &info) == MG_OK;
        return true;
    }
    if (!parse_condition_operator(&cursor, &operation)) return false;
    {
        i64 left_number;
        i64 right_number;

        used = strlen(left) + 1U;
        cursor = skip_spaces(cursor);
        if (!parse_word(state, &cursor, storage, sizeof(storage), &used,
                        &right, &home_expand, true) || !condition_tail(cursor))
            return false;
        if (operation == '=') {
            *out_value = !strcmp(left, right);
            return true;
        }
        if (!parse_signed_integer(left, &left_number) ||
            !parse_signed_integer(right, &right_number)) return false;
        switch (operation) {
        case '<': *out_value = left_number < right_number; break;
        case 'L': *out_value = left_number <= right_number; break;
        case '>': *out_value = left_number > right_number; break;
        case 'G': *out_value = left_number >= right_number; break;
        default: return false;
        }
    }
    return true;
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
