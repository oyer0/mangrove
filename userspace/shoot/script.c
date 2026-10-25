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
    script->statements = (shell_statement_t *)calloc(
        SHOOT_MAX_SCRIPT_STATEMENTS, sizeof(*script->statements));
    return script->statements != NULL;
}

void shell_script_destroy(shell_script_t *script)
{
    if (!script) return;
    free(script->statements);
    script->statements = NULL;
    script->count = 0;
    script->depth = 0;
}

static bool copy_statement(shell_statement_t *statement, const char *line,
                           usize line_number)
{
    usize length;

    if (!statement || !line) return false;
    length = strlen(line);
    if (length >= sizeof(statement->text)) return false;
    statement->line_number = line_number;
    statement->matching = invalid_statement;
    memcpy(statement->text, line, length + 1U);
    return true;
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

    set_error(out_error, NULL);
    if (!script || !script->statements || !line) {
        set_error(out_error, "invalid script state");
        return false;
    }
    if (shell_line_is_blank_or_comment(line)) return true;
    if (script->count == SHOOT_MAX_SCRIPT_STATEMENTS) {
        set_error(out_error, "script has too many statements");
        return false;
    }
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
        index = script->open_blocks[script->depth - 1U];
        if (script->statements[index].kind != SHELL_STATEMENT_IF ||
            script->statements[index].matching != invalid_statement) {
            set_error(out_error, "else has no matching if");
            return false;
        }
    } else if (kind == SHELL_STATEMENT_END && !script->depth) {
        set_error(out_error, "end has no matching block");
        return false;
    }

    index = script->count++;
    statement = &script->statements[index];
    statement->kind = kind;
    if (!copy_statement(statement, line, line_number)) {
        script->count--;
        set_error(out_error, "script line is too long");
        return false;
    }
    if (kind == SHELL_STATEMENT_IF || kind == SHELL_STATEMENT_WHILE) {
        script->open_blocks[script->depth++] = index;
    } else if (kind == SHELL_STATEMENT_ELSE) {
        script->statements[script->open_blocks[script->depth - 1U]].matching =
            index;
    } else if (kind == SHELL_STATEMENT_END) {
        usize open_index = script->open_blocks[--script->depth];
        usize matching = script->statements[open_index].matching;

        if (script->statements[open_index].kind == SHELL_STATEMENT_IF &&
            matching != invalid_statement &&
            script->statements[matching].kind == SHELL_STATEMENT_ELSE)
            script->statements[matching].matching = index;
        else
            script->statements[open_index].matching = index;
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
        printf("Shoot: %s:%u: %s.\n", source, (unsigned)line_number,
               message);
    else
        printf("Shoot: line %u: %s.\n", (unsigned)line_number, message);
}

static const char *statement_condition(const shell_statement_t *statement)
{
    const char *rest = NULL;

    if (!statement) return NULL;
    if (statement->kind == SHELL_STATEMENT_IF)
        (void)shell_line_keyword(statement->text, "if", &rest);
    else if (statement->kind == SHELL_STATEMENT_WHILE)
        (void)shell_line_keyword(statement->text, "while", &rest);
    return rest;
}

static bool execute_range(shell_script_t *script, shell_state_t *state,
                          usize first, usize last, const char *source)
{
    usize index = first;

    while (index < last) {
        shell_statement_t *statement = &script->statements[index];

        if (statement->kind == SHELL_STATEMENT_COMMAND) {
            if (!shell_execute_line(state, statement->text, source,
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
                                          statement_condition(statement),
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
                                              statement_condition(statement),
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
    char line[SHOOT_LINE_CAPACITY];
    usize line_length = 0;
    usize line_number = 1;
    const char *error = NULL;

    if (out_reported_error) *out_reported_error = false;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) return false;
    handle = (mg_handle_t)result;
    for (;;) {
        result = object_read(handle, buffer, sizeof(buffer));
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result < 0) {
            (void)handle_close(handle);
            return false;
        }
        for (usize index = 0; index < (usize)result; index++) {
            char value = buffer[index];

            if (line_number > SHOOT_MAX_SCRIPT_SOURCE_LINES) {
                script_error(path, line_number, "script is too long");
                if (out_reported_error) *out_reported_error = true;
                (void)handle_close(handle);
                return false;
            }
            if (value == '\n') {
                line[line_length] = '\0';
                if (!shell_script_append(script, line, line_number, &error)) {
                    script_error(path, line_number,
                                 error ? error : "invalid script statement");
                    if (out_reported_error) *out_reported_error = true;
                    (void)handle_close(handle);
                    return false;
                }
                line_length = 0;
                line_number++;
            } else if (value != '\r') {
                if (line_length + 1U >= sizeof(line)) {
                    script_error(path, line_number, "script line is too long");
                    if (out_reported_error) *out_reported_error = true;
                    (void)handle_close(handle);
                    return false;
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
            (void)handle_close(handle);
            return false;
        }
    }
    (void)handle_close(handle);
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
