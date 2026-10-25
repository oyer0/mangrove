/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/help.h"
#include "../common/path.h"

#define DEL_MAX_ENTRIES 4096U
#define DEL_MAX_DEPTH 32U

typedef struct {
    char path[MG_PATH_MAX_BYTES + 1U];
    u64 identifier;
    u32 type;
    u32 depth;
} del_plan_entry_t;

typedef struct {
    del_plan_entry_t *entries;
    usize count;
    usize capacity;
} del_plan_t;

static bool append_path(char *output, usize capacity,
                        const char *directory, const char *name)
{
    usize directory_length;
    usize name_length;
    bool root;

    if (!output || !directory || !name || !capacity) return false;
    directory_length = strlen(directory);
    name_length = strlen(name);
    root = directory_length == 1U && directory[0] == '/';
    if (root) {
        if (name_length + 2U > capacity) return false;
        output[0] = '/';
        memcpy(output + 1U, name, name_length + 1U);
        return true;
    }
    if (!directory_length ||
        directory_length > (usize)-1 - name_length - 1U ||
        directory_length + name_length + 2U > capacity) return false;
    memcpy(output, directory, directory_length);
    output[directory_length] = '/';
    memcpy(output + directory_length + 1U, name, name_length + 1U);
    return true;
}

static bool valid_entry_name(const char *name)
{
    usize length;

    if (!name) return false;
    length = 0U;
    while (length < sizeof(((mg_directory_entry_t *)0)->name) && name[length])
        length++;
    if (!length || length >= sizeof(((mg_directory_entry_t *)0)->name) ||
        !strcmp(name, ".") || !strcmp(name, "..")) return false;
    return strchr(name, '/') == NULL;
}

static bool plan_append(del_plan_t *plan, const char *path,
                        const mg_path_info_t *info, u32 depth)
{
    del_plan_entry_t *replacement;
    usize capacity;

    if (!plan || !path || !info || plan->count >= DEL_MAX_ENTRIES)
        return false;
    if (plan->count == plan->capacity) {
        capacity = plan->capacity ? plan->capacity * 2U : 32U;
        if (capacity > DEL_MAX_ENTRIES) capacity = DEL_MAX_ENTRIES;
        if (capacity <= plan->count ||
            capacity > (usize)-1 / sizeof(*plan->entries)) return false;
        replacement = realloc(plan->entries,
                              capacity * sizeof(*plan->entries));
        if (!replacement) return false;
        plan->entries = replacement;
        plan->capacity = capacity;
    }
    del_plan_entry_t *entry = &plan->entries[plan->count];
    usize length = strlen(path);
    if (length > MG_PATH_MAX_BYTES) return false;
    memcpy(entry->path, path, length + 1U);
    entry->identifier = info->identifier;
    entry->type = info->type;
    entry->depth = depth;
    plan->count++;
    return true;
}

static mg_result_t collect_tree(del_plan_t *plan)
{
    for (usize index = 0; index < plan->count; index++) {
        del_plan_entry_t current = plan->entries[index];

        if (current.type != MG_PATH_TYPE_DIRECTORY) continue;
        if (current.depth >= DEL_MAX_DEPTH) return MG_ERR_BUFFER_TOO_SMALL;

        /* Probe removal before opening a directory. A successful removal means
         * it was empty; NOT_EMPTY identifies a traversable directory. Other
         * errors include mount, CWD, permission, and media-lifetime barriers. */
        mg_result_t result = path_remove(current.path);
        if (result == MG_OK) {
            plan->entries[index].type = MG_PATH_TYPE_UNKNOWN;
            continue;
        }
        if (result != MG_ERR_NOT_EMPTY) return result;

        result = directory_open(current.path);
        if (result_is_error(result)) return result;
        mg_handle_t handle = (mg_handle_t)result;
        bool failed = false;
        mg_result_t failure = MG_OK;

        for (;;) {
            mg_directory_entry_t batch[MG_DIRECTORY_BATCH_MAX];
            usize count = 0;

            result = directory_read_batch(handle, batch,
                                         MG_DIRECTORY_BATCH_MAX, &count);
            if (result == MG_ERR_END_OF_FILE) break;
            if (result_is_error(result)) {
                failed = true;
                failure = result;
                break;
            }
            if (!count) {
                failed = true;
                failure = MG_ERR_IO;
                break;
            }
            for (usize entry_index = 0; entry_index < count; entry_index++) {
                char child_path[MG_PATH_MAX_BYTES + 1U];
                mg_path_info_t info;

                if (!valid_entry_name(batch[entry_index].name) ||
                    !append_path(child_path, sizeof(child_path), current.path,
                                 batch[entry_index].name)) {
                    failed = true;
                    failure = MG_ERR_BUFFER_TOO_SMALL;
                    break;
                }
                result = path_info(child_path, &info);
                if (result_is_error(result)) {
                    failed = true;
                    failure = result;
                    break;
                }
                if (info.type != MG_PATH_TYPE_FILE &&
                    info.type != MG_PATH_TYPE_DIRECTORY) {
                    failed = true;
                    failure = MG_ERR_UNSUPPORTED;
                    break;
                }
                if (!plan_append(plan, child_path, &info, current.depth + 1U)) {
                    failed = true;
                    failure = MG_ERR_NO_MEMORY;
                    break;
                }
            }
            if (failed) break;
        }

        mg_result_t close_result = handle_close(handle);
        if (failed) return failure;
        if (result_is_error(close_result)) return close_result;
    }
    return MG_OK;
}

static mg_result_t remove_tree(const char *root_path)
{
    del_plan_t plan = {0};
    mg_path_info_t root_info;
    mg_result_t result = path_info(root_path, &root_info);

    if (result_is_error(result)) return result;
    if (root_info.type != MG_PATH_TYPE_DIRECTORY) return MG_ERR_BAD_ARGUMENT;
    if (!plan_append(&plan, root_path, &root_info, 0U)) return MG_ERR_NO_MEMORY;

    result = collect_tree(&plan);
    if (result_is_error(result)) {
        free(plan.entries);
        return result;
    }

    for (usize index = plan.count; index > 0U; index--) {
        del_plan_entry_t *entry = &plan.entries[index - 1U];
        mg_path_info_t current_info;

        if (entry->type == MG_PATH_TYPE_UNKNOWN) continue;
        result = path_info(entry->path, &current_info);
        if (result_is_error(result)) break;
        if (current_info.identifier != entry->identifier ||
            current_info.type != entry->type) {
            result = MG_ERR_BUSY;
            break;
        }
        result = path_remove(entry->path);
        if (result_is_error(result)) break;
    }
    free(plan.entries);
    return result;
}

int main(int argc, char **argv)
{
    usize target_count = 0U;
    bool recursive = false;
    bool failed = false;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    for (int index = 1; index < argc; index++) {
        if (!strcmp(argv[index], "-r") ||
            !strcmp(argv[index], "--recursive")) {
            if (recursive) {
                command_usage_error(argv[0],
                    "del [-r|--recursive] <file|directory>...",
                    argv[index]);
                return 1;
            }
            recursive = true;
            continue;
        }
        if (argv[index][0] == '-') {
            command_usage_error(argv[0],
                "del [-r|--recursive] <file|directory>...", argv[index]);
            return 1;
        }
        target_count++;
    }
    if (!target_count) {
        command_usage_error(argv[0],
            "del [-r|--recursive] <file|directory>...", NULL);
        return 1;
    }

    for (int index = 1; index < argc; index++) {
        const char *target = argv[index];
        if (!strcmp(target, "-r") || !strcmp(target, "--recursive")) continue;
        char path[MG_PATH_MAX_BYTES + 1U];
        mg_path_info_t info;
        mg_result_t result;

        if (!command_resolve_path(target, path, sizeof(path))) {
            printf("Could not delete \"%s\": invalid path.\n", target);
            failed = true;
            continue;
        }
        result = path_info(path, &info);
        if (result_is_error(result)) {
            printf("Could not delete \"%s\": %s.\n", target,
                   error_string(result));
            failed = true;
            continue;
        }
        if (info.type != MG_PATH_TYPE_FILE &&
            info.type != MG_PATH_TYPE_DIRECTORY) {
            printf("Could not delete \"%s\": unsupported object type.\n",
                   target);
            failed = true;
            continue;
        }

        if (info.type == MG_PATH_TYPE_DIRECTORY && recursive)
            result = remove_tree(path);
        else
            result = path_remove(path);
        if (!recursive && result == MG_ERR_NOT_EMPTY) {
            printf("Could not delete \"%s\": directory is not empty; use del -r.\n",
                   target);
            failed = true;
            continue;
        }
        if (result_is_error(result)) {
            if (recursive) {
                printf("Could not delete \"%s\": %s. Recursive removal may have removed earlier entries.\n",
                       target, error_string(result));
            } else {
                printf("Could not delete \"%s\": %s.\n", target,
                       error_string(result));
            }
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
