/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/help.h"
#include "../common/path.h"

typedef struct {
    char *path;
    u64 identifier;
    u32 type;
    usize depth;
} del_plan_entry_t;

typedef struct {
    del_plan_entry_t *entries;
    usize count;
    usize capacity;
} del_plan_t;

/* Extended records can hold a 765-byte Unicode name. Keep the per-frame
 * directory working set comfortably below Mangrove's fixed user stack. */
#define DEL_DIRECTORY_BATCH_CAPACITY 4U

static bool append_path(const char *directory, const char *name,
                        char **out_path)
{
    return command_path_join_alloc(directory, name, out_path);
}

static bool valid_entry_name(const char *name)
{
    usize length;

    if (!name) return false;
    length = 0U;
    while (length < MG_DIRECTORY_NAME_MAX_BYTES && name[length])
        length++;
    if (!length || length > MG_DIRECTORY_NAME_MAX_BYTES ||
        !strcmp(name, ".") || !strcmp(name, "..")) return false;
    return strchr(name, '/') == NULL;
}

static bool plan_append(del_plan_t *plan, const char *path,
                        const mg_path_info_t *info, usize depth)
{
    del_plan_entry_t *replacement;
    char *path_copy;
    usize capacity;

    if (!plan || !path || !info || plan->count == (usize)-1)
        return false;
    usize length = strlen(path);
    if (!length || length > MG_PATH_MAX_BYTES) return false;
    path_copy = (char *)malloc(length + 1U);
    if (!path_copy) return false;
    memcpy(path_copy, path, length + 1U);
    if (plan->count == plan->capacity) {
        capacity = plan->capacity ? plan->capacity * 2U : 32U;
        if (capacity < plan->capacity) { free(path_copy); return false; }
        if (capacity <= plan->count ||
            capacity > (usize)-1 / sizeof(*plan->entries)) {
            free(path_copy);
            return false;
        }
        replacement = realloc(plan->entries,
                              capacity * sizeof(*plan->entries));
        if (!replacement) { free(path_copy); return false; }
        plan->entries = replacement;
        plan->capacity = capacity;
    }
    del_plan_entry_t *entry = &plan->entries[plan->count];
    entry->path = path_copy;
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
            mg_directory_entry_ex_t batch[DEL_DIRECTORY_BATCH_CAPACITY];
            usize count = 0;

            result = directory_read_batch_ex(handle, batch,
                                             DEL_DIRECTORY_BATCH_CAPACITY,
                                             &count);
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
                char *child_path = NULL;
                mg_path_info_t info;

                if (!valid_entry_name(batch[entry_index].name) ||
                    !append_path(current.path, batch[entry_index].name,
                                 &child_path)) {
                    failed = true;
                    failure = MG_ERR_BUFFER_TOO_SMALL;
                    break;
                }
                result = path_info(child_path, &info);
                if (result_is_error(result)) {
                    free(child_path);
                    failed = true;
                    failure = result;
                    break;
                }
                if (info.type != MG_PATH_TYPE_FILE &&
                    info.type != MG_PATH_TYPE_DIRECTORY) {
                    free(child_path);
                    failed = true;
                    failure = MG_ERR_UNSUPPORTED;
                    break;
                }
                if (!plan_append(plan, child_path, &info, current.depth + 1U)) {
                    free(child_path);
                    failed = true;
                    failure = MG_ERR_NO_MEMORY;
                    break;
                }
                free(child_path);
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
        for (usize i = 0; i < plan.count; i++) free(plan.entries[i].path);
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
    for (usize i = 0; i < plan.count; i++) free(plan.entries[i].path);
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
        mg_path_info_t info;
        mg_result_t result;

        result = path_info(target, &info);
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
            result = remove_tree(target);
        else
            result = path_remove(target);
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
