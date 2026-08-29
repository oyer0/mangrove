#pragma once

#include <mg/error.h>

typedef enum mg_path_type {
    MG_PATH_TYPE_UNKNOWN = 0,
    MG_PATH_TYPE_FILE = 1,
    MG_PATH_TYPE_DIRECTORY = 2,
} mg_path_type_t;

/* Stable metadata for a namespace object; no kernel pointers are exposed. */
typedef struct mg_path_info {
    u32 type;
    u32 permissions;
    u64 size;
    u64 identifier;
    u32 owner_uid;
    u32 reserved;
} mg_path_info_t;

/* One entry returned by directory_read(). */
typedef struct mg_directory_entry {
    char name[256];
    u32 type;
    u32 reserved;
    u64 identifier;
} mg_directory_entry_t;

#define MG_DIRECTORY_BATCH_MAX 32U

/* Writes the normalized current directory. out_size receives the required
 * byte count including the terminating NUL, even when the buffer is small. */
mg_result_t process_getcwd(char *buffer, usize capacity, usize *out_size);

mg_result_t path_info(const char *path, mg_path_info_t *out_info);

/* Returned directory handles are owned by the caller and must be closed. */
mg_result_t directory_open(const char *path);
/* Returns MG_ERR_END_OF_FILE after the final entry. */
mg_result_t directory_read(mg_handle_t handle, mg_directory_entry_t *out_entry);
/* Returns a bounded batch and maintains the directory handle cursor. */
mg_result_t directory_read_batch(mg_handle_t handle,
                                 mg_directory_entry_t *out_entries,
                                 usize capacity, usize *out_count);

/* Create one empty object. Existing paths are never replaced. */
mg_result_t file_create(const char *path);
mg_result_t directory_create(const char *path);

/* Atomically rename or move one object within its mounted filesystem. */
mg_result_t path_move(const char *source, const char *destination);
/* Removes a file or an empty directory. Recursive policy remains in userspace. */
mg_result_t path_remove(const char *path);

/* Explicitly truncate a writable file to zero bytes. */
mg_result_t file_truncate(mg_handle_t handle);
