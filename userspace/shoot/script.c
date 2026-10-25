/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "script.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/path.h"
#include "language.h"

#define SCRIPT_READ_BUFFER_CAPACITY 128U

static const usize invalid_statement = (usize)-1;

static void set_error(const char **out_error, const char *error)
{
    if (out_error) *out_error = error;
}

bool shell_script_init(shell_script_t *script)
{
    if (!script) return false;
    memset(script, 0, sizeof(*script));
    return true;
}

void shell_script_destroy(shell_script_t *script)
{
    if (!script) return;
    free(script->statements);
    free(script->source);
    script->statements = NULL;
    script->source = NULL;
    script->count = 0;
    script->capacity = 0;
    script->source_length = 0;
    script->source_capacity = 0;
    script->depth = 0;
}

static bool reserve_statements(shell_script_t *script, usize required)
{
    usize capacity;
    shell_statement_t *replacement;

    if (!script || script->capacity >= required) return true;
    capacity = script->capacity ? script->capacity : 32U;
    while (capacity < required) {
        if (capacity > (usize)-1 / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    if (capacity > (usize)-1 / sizeof(*script->statements)) return false;
    replacement = (shell_statement_t *)realloc(
        script->statements, capacity * sizeof(*replacement));
    if (!replacement) return false;
    script->statements = replacement;
    script->capacity = capacity;
    return true;
}

static bool reserve_source(shell_script_t *script, usize required)
{
    usize capacity;
    char *replacement;

    if (!script || script->source_capacity >= required) return true;
    capacity = script->source_capacity ? script->source_capacity : 256U;
    while (capacity < required) {
        if (capacity > (usize)-1 / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    if (capacity < required) return false;
    replacement = (char *)realloc(script->source, capacity);
    if (!replacement) return false;
    script->source = replacement;
    script->source_capacity = capacity;
    return true;
}

static bool reserve_line(char **line, usize *capacity, usize required)
{
    usize next;
    char *replacement;

    if (!line || !capacity) return false;
    if (*capacity >= required) return true;
    next = *capacity ? *capacity : 128U;
    while (next < required) {
        if (next > (usize)-1 / 2U) {
            next = required;
            break;
        }
        next *= 2U;
    }
    if (next < required) return false;
    replacement = (char *)realloc(*line, next);
    if (!replacement) return false;
    *line = replacement;
    *capacity = next;
    return true;
}

static const char *statement_text(const shell_script_t *script,
                                 const shell_statement_t *statement)
{
    if (!script || !statement || !script->source ||
        statement->text_offset > script->source_length ||
        statement->text_length > script->source_length -
            statement->text_offset) return "";
    return script->source + statement->text_offset;
}

static bool keyword_has_only_comment(const char *rest)
{
    return !rest || !*rest || (rest[0] == '/' && rest[1] == '/');
}

bool shell_script_append(shell_script_t *script, const char *line,
                         usize line_number, const char **out_error)
{
    shell_statement_t *statement;
    const char *rest;
    shell_statement_kind_t kind = SHELL_STATEMENT_COMMAND;
    usize index;
    usize open_index = invalid_statement;
    usize line_length;
    usize source_required;

    set_error(out_error, NULL);
    if (!script || !line) {
        set_error(out_error, "invalid script state");
        return false;
    }
    if (shell_line_is_blank_or_comment(line)) return true;
    if (script->count == (usize)-1) {
        set_error(out_error, "script is too large");
        return false;
    }
    line_length = strlen(line);
    if (line_length > (usize)-1 - script->source_length - 1U) {
        set_error(out_error, "script is too large");
        return false;
    }
    source_required = script->source_length + line_length + 1U;
    if (shell_line_keyword(line, "if", &rest)) {
        kind = SHELL_STATEMENT_IF;
    } else if (shell_line_keyword(line, "while", &rest)) {
        kind = SHELL_STATEMENT_WHILE;
    } else if (shell_line_keyword(line, "else", &rest)) {
        if (!keyword_has_only_comment(rest)) {
            set_error(out_error, "else does not accept a condition");
            return false;
        }
        kind = SHELL_STATEMENT_ELSE;
    } else if (shell_line_keyword(line, "end", &rest)) {
        if (!keyword_has_only_comment(rest)) {
            set_error(out_error, "end does not accept arguments");
            return false;
        }
        kind = SHELL_STATEMENT_END;
    }
    if (kind == SHELL_STATEMENT_IF || kind == SHELL_STATEMENT_WHILE) {
        if (script->depth == SHOOT_MAX_SCRIPT_NESTING) {
            set_error(out_error, "script nesting limit exceeded");
            return false;
        }
    } else if (kind == SHELL_STATEMENT_ELSE) {
        if (!script->depth) {
            set_error(out_error, "else has no matching if");
            return false;
        }
        open_index = script->open_blocks[script->depth - 1U];
        if (script->statements[open_index].kind != SHELL_STATEMENT_IF ||
            script->statements[open_index].matching != invalid_statement) {
            set_error(out_error, "else has no matching if");
            return false;
        }
    } else if (kind == SHELL_STATEMENT_END && !script->depth) {
        set_error(out_error, "end has no matching block");
        return false;
    }

    if (!reserve_statements(script, script->count + 1U) ||
        !reserve_source(script, source_required)) {
        set_error(out_error, "not enough memory");
        return false;
    }
    index = script->count;
    statement = &script->statements[index];
    statement->kind = kind;
    statement->line_number = line_number;
    statement->matching = invalid_statement;
    statement->text_offset = script->source_length;
    statement->text_length = line_length;
    memcpy(script->source + script->source_length, line, line_length + 1U);
    script->source_length = source_required;
    script->count++;
    if (kind == SHELL_STATEMENT_IF || kind == SHELL_STATEMENT_WHILE) {
        script->open_blocks[script->depth++] = index;
    } else if (kind == SHELL_STATEMENT_ELSE) {
        script->statements[open_index].matching = index;
    } else if (kind == SHELL_STATEMENT_END) {
        open_index = script->open_blocks[script->depth - 1U];
        usize matching = script->statements[open_index].matching;

        if (script->statements[open_index].kind == SHELL_STATEMENT_IF &&
            matching != invalid_statement &&
            script->statements[matching].kind == SHELL_STATEMENT_ELSE)
            script->statements[matching].matching = index;
        else
            script->statements[open_index].matching = index;
        script->depth--;
    }
    return true;
}

bool shell_script_complete(const shell_script_t *script)
{
    return script && script->depth == 0;
}

static void script_error(const char *source, usize line_number,
                         const char *message)
{
    if (source && *source)
        printf("Shoot: %s:%llu: %s.\n", source,
               (unsigned long long)line_number,
               message);
    else
        printf("Shoot: line %llu: %s.\n",
               (unsigned long long)line_number, message);
}

static const char *statement_condition(const shell_script_t *script,
                                      const shell_statement_t *statement)
{
    const char *rest = NULL;
    const char *text = statement_text(script, statement);

    if (!statement) return NULL;
    if (statement->kind == SHELL_STATEMENT_IF)
        (void)shell_line_keyword(text, "if", &rest);
    else if (statement->kind == SHELL_STATEMENT_WHILE)
        (void)shell_line_keyword(text, "while", &rest);
    return rest;
}

static bool execute_range(shell_script_t *script, shell_state_t *state,
                          usize first, usize last, const char *source)
{
    usize index = first;

    while (index < last) {
        shell_statement_t *statement = &script->statements[index];

        if (statement->kind == SHELL_STATEMENT_COMMAND) {
            if (!shell_execute_line(state, statement_text(script, statement),
                                    source,
                                    statement->line_number, true)) return false;
            index++;
            continue;
        }
        if (statement->kind == SHELL_STATEMENT_IF) {
            bool condition;
            usize branch;
            usize end;

            if (statement->matching == invalid_statement) {
                script_error(source, statement->line_number,
                             "missing end");
                return false;
            }
            if (!shell_evaluate_condition(state,
                                          statement_condition(script,
                                                              statement),
                                          &condition)) {
                script_error(source, statement->line_number,
                             "malformed condition");
                return false;
            }
            branch = statement->matching;
            if (script->statements[branch].kind == SHELL_STATEMENT_ELSE) {
                end = script->statements[branch].matching;
                if (end == invalid_statement) {
                    script_error(source, statement->line_number,
                                 "missing end");
                    return false;
                }
                if (condition) {
                    if (!execute_range(script, state, index + 1U, branch,
                                       source)) return false;
                } else if (!execute_range(script, state, branch + 1U, end,
                                          source)) return false;
                index = end + 1U;
            } else {
                if (condition &&
                    !execute_range(script, state, index + 1U, branch,
                                   source)) return false;
                index = branch + 1U;
            }
            continue;
        }
        if (statement->kind == SHELL_STATEMENT_WHILE) {
            bool condition;
            usize end = statement->matching;

            if (end == invalid_statement) {
                script_error(source, statement->line_number,
                             "missing end");
                return false;
            }
            for (;;) {
                if (!shell_evaluate_condition(state,
                                              statement_condition(script,
                                                                  statement),
                                              &condition)) {
                    script_error(source, statement->line_number,
                                 "malformed condition");
                    return false;
                }
                if (!condition) break;
                if (!execute_range(script, state, index + 1U, end, source))
                    return false;
            }
            index = end + 1U;
            continue;
        }
        script_error(source, statement->line_number,
                     statement->kind == SHELL_STATEMENT_ELSE ?
                         "unexpected else" : "unexpected end");
        return false;
    }
    return true;
}

bool shell_script_execute(shell_script_t *script, shell_state_t *state,
                          const char *source)
{
    if (!script || !state || !shell_script_complete(script)) return false;
    return execute_range(script, state, 0, script->count, source);
}

static bool script_read_file(shell_script_t *script, const char *path,
                            bool *out_reported_error)
{
    mg_result_t result;
    mg_handle_t handle;
    char buffer[SCRIPT_READ_BUFFER_CAPACITY];
    char *line = NULL;
    usize line_capacity = 0;
    usize line_length = 0;
    usize line_number = 1;
    const char *error = NULL;
    bool success = false;
    static const char empty_line[] = "";

    if (out_reported_error) *out_reported_error = false;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) return false;
    handle = (mg_handle_t)result;
    for (;;) {
        result = object_read(handle, buffer, sizeof(buffer));
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result < 0) {
            goto done;
        }
        for (usize index = 0; index < (usize)result; index++) {
            char value = buffer[index];

            if (value == '\n') {
                const char *source_line = empty_line;
                if (line_length) {
                    line[line_length] = '\0';
                    source_line = line;
                }
                if (!shell_script_append(script, source_line, line_number,
                                         &error)) {
                    script_error(path, line_number,
                                 error ? error : "invalid script statement");
                    if (out_reported_error) *out_reported_error = true;
                    goto done;
                }
                line_length = 0;
                if (line_number == (usize)-1) {
                    script_error(path, line_number, "script is too large");
                    if (out_reported_error) *out_reported_error = true;
                    goto done;
                }
                line_number++;
            } else if (value != '\r') {
                if (line_length > (usize)-1 - 2U ||
                    !reserve_line(&line, &line_capacity, line_length + 2U)) {
                    script_error(path, line_number, "not enough memory");
                    if (out_reported_error) *out_reported_error = true;
                    goto done;
                }
                line[line_length++] = value;
            }
        }
    }
    if (line_length) {
        line[line_length] = '\0';
        if (!shell_script_append(script, line, line_number, &error)) {
            script_error(path, line_number,
                         error ? error : "invalid script statement");
            if (out_reported_error) *out_reported_error = true;
            goto done;
        }
    }
    success = true;
done:
    (void)handle_close(handle);
    free(line);
    if (!success) return false;
    if (!shell_script_complete(script)) {
        usize open_index = script->open_blocks[script->depth - 1U];
        script_error(path, script->statements[open_index].line_number,
                     "missing end");
        if (out_reported_error) *out_reported_error = true;
        return false;
    }
    return true;
}

int shell_run_script(const char *path)
{
    shell_state_t state;
    shell_script_t script = {0};
    COMMAND_PATH_AUTO char *expanded = NULL;
    bool reported_error = false;
    bool success;

    if (!path || !*path ||
        !command_expand_home_path_alloc(path, true, &expanded)) {
        printf("Shoot: cannot read script \"%s\".\n", path ? path : "");
        return 1;
    }
    if (!shell_state_init(&state) || !shell_script_init(&script)) {
        printf("Shoot: unable to initialize script execution.\n");
        shell_script_destroy(&script);
        shell_state_destroy(&state);
        return 1;
    }
    if (!script_read_file(&script, expanded, &reported_error)) {
        if (!reported_error)
            printf("Shoot: cannot read script \"%s\".\n", path);
        shell_script_destroy(&script);
        shell_state_destroy(&state);
        return 1;
    }
    success = shell_script_execute(&script, &state, path);
    shell_script_destroy(&script);
    shell_state_destroy(&state);
    return success ? 0 : 1;
}
