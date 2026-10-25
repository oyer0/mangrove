/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "glob.h"

#include <mg/filesystem.h>
#include <mg/object.h>
#include <mg/utf8.h>
#include <stdlib.h>
#include <string.h>

typedef struct glob_component {
    usize start;
    usize length;
    bool has_pattern;
} glob_component_t;

typedef struct glob_context {
    const char *pattern;
    const u8 *flags;
    usize pattern_length;
    glob_component_t *components;
    usize component_count;
    bool trailing_slash;
    shell_expanded_arguments_t *output;
} glob_context_t;

typedef struct glob_walk_frame {
    char *prefix;
    mg_handle_t directory;
    bool opened;
    bool literal_done;
    bool follows_match;
} glob_walk_frame_t;

static bool decode_scalar(const char *text, usize length, usize offset,
                          u32 *out_scalar, usize *out_width)
{
    if (!text || !out_scalar || !out_width || offset >= length ||
        mg_utf8_decode_one((const u8 *)text + offset, length - offset,
                           out_scalar, out_width) != MG_UTF8_OK ||
        !*out_width) return false;
    return true;
}

static bool flag_at(const u8 *flags, usize offset)
{
    return flags && flags[offset] != 0;
}

static bool bracket_end(const char *pattern, const u8 *flags, usize length,
                        usize start, usize *out_end)
{
    usize cursor = start + 1U;
    bool items = false;
    u32 previous = 0;

    if (cursor < length && pattern[cursor] == '!' &&
        flag_at(flags, cursor)) cursor++;
    while (cursor < length) {
        u32 scalar;
        usize width;

        if (!decode_scalar(pattern, length, cursor, &scalar, &width))
            return false;
        if (scalar == ']' && flag_at(flags, cursor)) {
            if (!items) return false;
            *out_end = cursor + width;
            return true;
        }
        if (scalar == '-' && flag_at(flags, cursor) && items &&
            cursor + width < length &&
            !(pattern[cursor + width] == ']' &&
              flag_at(flags, cursor + width))) {
            usize next = cursor + width;
            u32 end_scalar;
            usize end_width;

            if (!decode_scalar(pattern, length, next, &end_scalar,
                               &end_width) || previous > end_scalar)
                return false;
            cursor = next + end_width;
            previous = end_scalar;
            continue;
        }
        cursor += width;
        previous = scalar;
        items = true;
    }
    return false;
}

static bool validate_component(const char *pattern, const u8 *flags,
                               usize length)
{
    for (usize cursor = 0; cursor < length;) {
        u32 scalar;
        usize width;

        if (!decode_scalar(pattern, length, cursor, &scalar, &width))
            return false;
        if (scalar == '[' && flag_at(flags, cursor)) {
            usize end;
            if (!bracket_end(pattern, flags, length, cursor, &end))
                return false;
            cursor = end;
        } else {
            cursor += width;
        }
    }
    return true;
}

static bool bracket_matches(const char *pattern, const u8 *flags,
                            usize length, usize start, u32 candidate,
                            usize *out_end)
{
    usize cursor = start + 1U;
    usize end;
    bool invert = false;
    bool matched = false;

    if (!bracket_end(pattern, flags, length, start, &end)) return false;
    if (cursor < end && pattern[cursor] == '!' && flag_at(flags, cursor)) {
        invert = true;
        cursor++;
    }
    while (cursor < end - 1U) {
        u32 first;
        usize first_width;

        if (!decode_scalar(pattern, end - 1U, cursor, &first, &first_width))
            return false;
        if (cursor + first_width < end - 1U &&
            pattern[cursor + first_width] == '-' &&
            flag_at(flags, cursor + first_width)) {
            usize last_offset = cursor + first_width + 1U;
            u32 last;
            usize last_width;

            if (!decode_scalar(pattern, end - 1U, last_offset, &last,
                               &last_width)) return false;
            if (candidate >= first && candidate <= last) matched = true;
            cursor = last_offset + last_width;
        } else {
            if (candidate == first) matched = true;
            cursor += first_width;
        }
    }
    *out_end = end;
    return invert ? !matched : matched;
}

static usize next_scalar(const char *text, usize length, usize offset)
{
    u32 scalar;
    usize width;

    if (!decode_scalar(text, length, offset, &scalar, &width))
        return length;
    return offset + width;
}

static bool component_matches(const char *pattern, const u8 *flags,
                              usize pattern_length, const char *name)
{
    usize p = 0;
    usize n = 0;
    usize name_length = strlen(name);
    usize star_pattern = (usize)-1;
    usize star_name = 0;

    /* Unix-style dot protection is applied to each component independently. */
    if (name[0] == '.' && (!pattern_length || pattern[0] != '.'))
        return false;
    while (n < name_length) {
        u32 pattern_scalar;
        u32 name_scalar;
        usize pattern_width;
        usize name_width;
        bool matched = false;

        if (p < pattern_length && pattern[p] == '*' && flag_at(flags, p)) {
            while (p < pattern_length && pattern[p] == '*' &&
                   flag_at(flags, p)) p++;
            star_pattern = p;
            star_name = n;
            continue;
        }
        if (p < pattern_length && pattern[p] == '?' && flag_at(flags, p)) {
            p++;
            n = next_scalar(name, name_length, n);
            continue;
        }
        if (p < pattern_length && pattern[p] == '[' && flag_at(flags, p)) {
            usize end;
            if (decode_scalar(name, name_length, n, &name_scalar,
                              &name_width) &&
                bracket_matches(pattern, flags, pattern_length, p,
                                name_scalar, &end)) {
                p = end;
                n += name_width;
                continue;
            }
        } else if (p < pattern_length &&
                   decode_scalar(pattern, pattern_length, p,
                                 &pattern_scalar, &pattern_width) &&
                   decode_scalar(name, name_length, n,
                                 &name_scalar, &name_width)) {
            matched = pattern_scalar == name_scalar;
            if (matched) {
                p += pattern_width;
                n += name_width;
                continue;
            }
        }
        if (star_pattern != (usize)-1 && star_name < name_length) {
            star_name = next_scalar(name, name_length, star_name);
            n = star_name;
            p = star_pattern;
            continue;
        }
        return false;
    }
    while (p < pattern_length && pattern[p] == '*' && flag_at(flags, p)) p++;
    return p == pattern_length;
}

static mg_result_t append_path_component(char **out_path, const char *prefix,
                                         const char *component,
                                         usize component_length)
{
    if (!out_path || !prefix || !component) return MG_ERR_BAD_ARGUMENT;
    *out_path = NULL;
    usize prefix_length = strlen(prefix);
    bool separator = prefix_length &&
        !(prefix_length == 1U && prefix[0] == '/');
    usize extra = separator ? 1U : 0U;
    if (prefix_length > MG_PATH_MAX_BYTES ||
        extra > MG_PATH_MAX_BYTES - prefix_length ||
        component_length > MG_PATH_MAX_BYTES - prefix_length - extra)
        return MG_ERR_NAME_TOO_LONG;
    usize needed = prefix_length + extra + component_length;
    char *output = (char *)malloc(needed + 1U);
    if (!output) return MG_ERR_NO_MEMORY;
    if (prefix_length) memcpy(output, prefix, prefix_length);
    usize offset = prefix_length;
    if (separator) output[offset++] = '/';
    memcpy(output + offset, component, component_length);
    output[needed] = '\0';
    *out_path = output;
    return MG_OK;
}

/* Budget includes argv[0], every pointer, and the trailing NULL. Metadata
 * grows geometrically; filenames retain independent ownership across sorting. */
static mg_result_t reserve_argument(shell_expanded_arguments_t *output,
                                     usize length)
{
    if (output->count >= MG_PROCESS_MAX_ARGUMENTS - 1U)
        return MG_ERR_BUFFER_TOO_SMALL;
    usize pointers = (output->count + 3U) * sizeof(uintptr_t);
    if (output->bytes > MG_PROCESS_ARGUMENT_BYTES_MAX - pointers ||
        length >= MG_PROCESS_ARGUMENT_BYTES_MAX - pointers - output->bytes)
        return MG_ERR_BUFFER_TOO_SMALL;
    if (output->count == output->capacity) {
        usize capacity = output->capacity ? output->capacity * 2U : 32U;
        if (capacity > MG_PROCESS_MAX_ARGUMENTS - 1U)
            capacity = MG_PROCESS_MAX_ARGUMENTS - 1U;
        const char **arguments = malloc(capacity * (sizeof(*arguments) + sizeof(bool)));
        if (!arguments) return MG_ERR_NO_MEMORY;
        bool *owned = (bool *)(arguments + capacity);
        if (output->count) {
            memcpy(arguments, output->arguments, output->count * sizeof(*arguments));
            memcpy(owned, output->owned, output->count * sizeof(*owned));
        }
        free(output->arguments);
        output->arguments = arguments;
        output->owned = owned;
        output->capacity = capacity;
    }
    return MG_OK;
}

static mg_result_t append_literal(shell_expanded_arguments_t *output,
                                  const char *word)
{
    usize length = strlen(word);
    mg_result_t result = reserve_argument(output, length);
    if (result_is_error(result)) return result;
    output->arguments[output->count] = word;
    output->owned[output->count++] = false;
    output->bytes += length + 1U;
    return MG_OK;
}

static mg_result_t append_result(glob_context_t *context, const char *path,
                                 bool require_directory,
                                 bool selected_from_directory)
{
    mg_path_info_t info;
    mg_result_t result = path_info(path, &info);
    usize length;
    char *copy;

    /* A directory entry selected by the pattern disappearing before its
     * metadata lookup is a concurrent mutation, not an unmatched pattern.
     * Fail closed so a literal wildcard-named file cannot be selected by the
     * unmatched-pattern fallback after a partial match raced away. */
    if (result == MG_ERR_NOT_FOUND)
        return selected_from_directory ? MG_ERR_BUSY : MG_OK;
    if (result_is_error(result)) return result;
    if (context->trailing_slash && info.type != MG_PATH_TYPE_DIRECTORY)
        return selected_from_directory ? MG_ERR_BUSY : MG_OK;
    if (require_directory && info.type != MG_PATH_TYPE_DIRECTORY)
        return MG_OK;
    usize path_length = strlen(path);
    length = path_length;
    if (context->trailing_slash) {
        if (length == MG_PATH_MAX_BYTES) return MG_ERR_NAME_TOO_LONG;
        length++;
    }
    if (length > MG_PATH_MAX_BYTES) return MG_ERR_NAME_TOO_LONG;
    result = reserve_argument(context->output, length);
    if (result_is_error(result)) return result;
    copy = (char *)malloc(length + 1U);
    if (!copy) return MG_ERR_NO_MEMORY;
    memcpy(copy, path, path_length);
    if (context->trailing_slash) copy[path_length] = '/';
    copy[length] = '\0';
    context->output->arguments[context->output->count] = copy;
    context->output->owned[context->output->count] = true;
    context->output->count++;
    context->output->bytes += length + 1U;
    return MG_OK;
}

static mg_result_t walk_components(glob_context_t *context,
                                   const char *initial_prefix)
{
    glob_walk_frame_t *frames;
    usize depth = 0;
    mg_result_t outcome = MG_OK;

    if (context->component_count == (usize)-1 ||
        context->component_count + 1U >
            (usize)-1 / sizeof(*frames)) return MG_ERR_NO_MEMORY;
    frames = (glob_walk_frame_t *)malloc(
        (context->component_count + 1U) * sizeof(*frames));
    if (!frames) return MG_ERR_NO_MEMORY;
    memset(frames, 0,
           (context->component_count + 1U) * sizeof(*frames));
    usize initial_length = strlen(initial_prefix);
    frames[0].prefix = (char *)malloc(initial_length + 1U);
    if (!frames[0].prefix) { free(frames); return MG_ERR_NO_MEMORY; }
    memcpy(frames[0].prefix, initial_prefix, initial_length + 1U);

    for (;;) {
        glob_walk_frame_t *frame = &frames[depth];
        if (depth == context->component_count) {
            outcome = append_result(context, frame->prefix, false, false);
            free(frame->prefix);
            frame->prefix = NULL;
            if (result_is_error(outcome) || depth == 0U) break;
            depth--;
            continue;
        }

        glob_component_t *component = &context->components[depth];
        const char *part = context->pattern + component->start;
        if (!component->has_pattern) {
            if (frame->literal_done) {
                frame->literal_done = false;
                if (depth == 0U) break;
                depth--;
                continue;
            }
            char *candidate = NULL;
            outcome = append_path_component(&candidate, frame->prefix, part,
                                            component->length);
            if (result_is_error(outcome)) break;
            if (depth + 1U == context->component_count) {
                outcome = append_result(context, candidate, false, false);
                free(candidate);
                if (result_is_error(outcome) || depth == 0U) break;
                depth--;
                continue;
            }
            frame->literal_done = true;
            free(frames[depth + 1U].prefix);
            frames[depth + 1U].prefix = candidate;
            frames[depth + 1U].directory = 0;
            frames[depth + 1U].opened = false;
            frames[depth + 1U].literal_done = false;
            frames[depth + 1U].follows_match = frame->follows_match;
            depth++;
            continue;
        }

        if (!frame->opened) {
            const char *directory_path = frame->prefix[0]
                ? frame->prefix : ".";
            mg_result_t opened = directory_open(directory_path);
            if (opened == MG_ERR_NOT_FOUND ||
                opened == MG_ERR_NOT_DIRECTORY) {
                if (opened == MG_ERR_NOT_FOUND && frame->follows_match) {
                    outcome = MG_ERR_BUSY;
                    break;
                }
                if (depth == 0U) break;
                depth--;
                continue;
            }
            if (result_is_error(opened)) {
                outcome = opened;
                break;
            }
            frame->directory = (mg_handle_t)opened;
            frame->opened = true;
        }

        mg_directory_entry_ex_t entry;
        mg_result_t read_result = directory_read_ex(frame->directory, &entry);
        if (read_result == MG_ERR_END_OF_FILE) {
            mg_result_t close_result = handle_close(frame->directory);
            frame->opened = false;
            if (result_is_error(close_result)) {
                outcome = close_result;
                break;
            }
            if (depth == 0U) break;
            depth--;
            continue;
        }
        if (result_is_error(read_result)) {
            outcome = read_result;
            break;
        }
        if (!entry.name[0] || strchr(entry.name, '/')) {
            outcome = MG_ERR_PROTOCOL;
            break;
        }
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, ".."))
            continue;
        if (!component_matches(part, context->flags + component->start,
                               component->length, entry.name)) continue;

        char *candidate = NULL;
        outcome = append_path_component(&candidate, frame->prefix, entry.name,
                                        strlen(entry.name));
        if (result_is_error(outcome)) break;
        if (depth + 1U == context->component_count) {
            if (context->trailing_slash && entry.type != MG_PATH_TYPE_DIRECTORY) {
                free(candidate);
                continue;
            }
            outcome = append_result(context, candidate, false, true);
            free(candidate);
            if (result_is_error(outcome)) break;
            continue;
        }
        if (entry.type != MG_PATH_TYPE_DIRECTORY) { free(candidate); continue; }
        free(frames[depth + 1U].prefix);
        frames[depth + 1U].prefix = candidate;
        frames[depth + 1U].directory = 0;
        frames[depth + 1U].opened = false;
        frames[depth + 1U].literal_done = false;
        frames[depth + 1U].follows_match = true;
        depth++;
    }

    for (usize index = 0; index <= context->component_count; index++) {
        if (!frames[index].opened) continue;
        mg_result_t close_result = handle_close(frames[index].directory);
        if (!result_is_error(outcome) && result_is_error(close_result))
            outcome = close_result;
    }
    for (usize index = 0; index <= context->component_count; index++)
        free(frames[index].prefix);
    free(frames);
    return outcome;
}

static int compare_paths(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static bool contains_pattern(const char *word, const u8 *flags)
{
    for (usize index = 0; word[index]; index++)
        if (flag_at(flags, index) &&
            (word[index] == '*' || word[index] == '?' ||
             word[index] == '[')) return true;
    return false;
}

static mg_result_t expand_one(const char *word, const u8 *flags,
                              shell_expanded_arguments_t *output)
{
    glob_context_t context;
    usize initial_count = output->count;
    const char *prefix;
    mg_result_t result;
    usize component_capacity = 0;

    if (!contains_pattern(word, flags)) return append_literal(output, word);
    memset(&context, 0, sizeof(context));
    context.pattern = word;
    context.flags = flags;
    context.pattern_length = strlen(word);
    context.trailing_slash = context.pattern_length &&
        word[context.pattern_length - 1U] == '/';
    context.output = output;
    if (context.pattern_length > MG_PATH_MAX_BYTES) return MG_ERR_NAME_TOO_LONG;
    if (!flags) return MG_ERR_BAD_ARGUMENT;
    bool in_component = false;
    for (usize index = 0; index < context.pattern_length; index++) {
        if (word[index] == '/') in_component = false;
        else if (!in_component) { component_capacity++; in_component = true; }
    }
    if (!component_capacity ||
        component_capacity > (usize)-1 / sizeof(*context.components))
        return MG_ERR_NO_MEMORY;
    context.components = (glob_component_t *)calloc(
        component_capacity, sizeof(*context.components));
    if (!context.components) return MG_ERR_NO_MEMORY;
    if (word[0] == '/') prefix = "/";
    else prefix = "";

    for (usize cursor = 0; cursor < context.pattern_length;) {
        usize start;
        glob_component_t *component;

        while (cursor < context.pattern_length && word[cursor] == '/') cursor++;
        if (cursor == context.pattern_length) break;
        start = cursor;
        while (cursor < context.pattern_length && word[cursor] != '/') cursor++;
        if (context.component_count == component_capacity) {
            free(context.components);
            return MG_ERR_BAD_ARGUMENT;
        }
        component = &context.components[context.component_count++];
        component->start = start;
        component->length = cursor - start;
        for (usize index = start; index < cursor; index++)
            if (flag_at(flags, index) &&
                (word[index] == '*' || word[index] == '?' ||
                 word[index] == '[')) component->has_pattern = true;
        if (!validate_component(word + start, flags + start,
                                component->length)) {
            free(context.components);
            return MG_ERR_BAD_ARGUMENT;
        }
    }
    result = walk_components(&context, prefix);
    free(context.components);
    if (result_is_error(result)) return result;
    if (output->count == initial_count) {
        result = append_literal(output, word);
        if (result_is_error(result)) return result;
    } else {
        qsort(output->arguments + initial_count,
              output->count - initial_count, sizeof(output->arguments[0]),
              compare_paths);
    }
    return MG_OK;
}

void shell_expanded_arguments_destroy(shell_expanded_arguments_t *expanded)
{
    if (!expanded) return;
    for (usize index = 0; index < expanded->count; index++)
        if (expanded->owned[index])
            free((void *)expanded->arguments[index]);
    free(expanded->arguments);
    memset(expanded, 0, sizeof(*expanded));
}

mg_result_t shell_expand_arguments(const shell_command_t *command,
                                  shell_expanded_arguments_t *out,
                                  const char **out_failed_pattern)
{
    if (out_failed_pattern) *out_failed_pattern = NULL;
    if (!command || !out) return MG_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->bytes = strlen(command->name) + 1U +
        (strchr(command->name, '/') ? 0U : 5U);
    for (usize index = 0; index < command->argument_count; index++) {
        mg_result_t result = expand_one(command->arguments[index],
            command->argument_glob_flags[index], out);
        if (result_is_error(result)) {
            if (out_failed_pattern)
                *out_failed_pattern = command->arguments[index];
            shell_expanded_arguments_destroy(out);
            return result;
        }
    }
    return MG_OK;
}
