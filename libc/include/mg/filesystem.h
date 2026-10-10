/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>

typedef enum mg_path_type {
    MG_PATH_TYPE_UNKNOWN = 0,
    MG_PATH_TYPE_FILE = 1,
    MG_PATH_TYPE_DIRECTORY = 2,
} mg_path_type_t;

/* Stored owner/other permissions exposed by native path_info(). */
#define MG_PERMISSION_OWNER_READ   (1U << 0)
#define MG_PERMISSION_OWNER_WRITE  (1U << 1)
#define MG_PERMISSION_OTHER_READ   (1U << 2)
#define MG_PERMISSION_OTHER_WRITE  (1U << 3)
#define MG_PERMISSION_KNOWN        (MG_PERMISSION_OWNER_READ | \
                                    MG_PERMISSION_OWNER_WRITE | \
                                    MG_PERMISSION_OTHER_READ | \
                                    MG_PERMISSION_OTHER_WRITE)

typedef enum mg_seek_whence {
    MG_SEEK_SET = 0,
    MG_SEEK_CUR = 1,
    MG_SEEK_END = 2,
} mg_seek_whence_t;

/* Explicit, short-lived administrator filesystem operations.  These are
 * policy requests, not a process-wide administrator mode. */
typedef enum mg_filesystem_admin_operation {
    MG_FILESYSTEM_ADMIN_OPEN = 1,
    MG_FILESYSTEM_ADMIN_CREATE_FILE,
    MG_FILESYSTEM_ADMIN_CREATE_DIRECTORY,
    MG_FILESYSTEM_ADMIN_MOVE,
    MG_FILESYSTEM_ADMIN_REMOVE,
} mg_filesystem_admin_operation_t;

typedef struct mg_filesystem_admin_request {
    u32 operation;
    u32 flags;
    const char *source;
    const char *destination;
} mg_filesystem_admin_request_t;

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

mg_result_t file_create_administrative(const char *path);
mg_result_t directory_create_administrative(const char *path);

/* Atomically rename or move one object within its mounted filesystem. */
mg_result_t path_move(const char *source, const char *destination);
/* Removes a file or an empty directory. Recursive policy remains in userspace. */
mg_result_t path_remove(const char *path);
mg_result_t path_move_administrative(const char *source,
                                     const char *destination);
mg_result_t path_remove_administrative(const char *path);

/* Explicitly truncate a writable file to zero bytes. */
mg_result_t file_truncate(mg_handle_t handle);
mg_result_t file_seek(mg_handle_t handle, i64 offset,
                      mg_seek_whence_t whence);

#define MG_FILE_LINES_MAX 4096U
#define MG_FILE_LINE_CAPACITY 512U
#define MG_FILE_LINES_RESET (1U << 0)

typedef struct mg_file_lines_state {
    u64 identifier;
    u64 revision;
    usize length;
    usize append_offset;
} mg_file_lines_state_t;

typedef struct mg_file_snapshot_request {
    char *data;
    usize capacity;
    mg_file_lines_state_t *state;
} mg_file_snapshot_request_t;

typedef struct mg_file_append_lines_request {
    const char *data;
    usize length;
    u32 max_lines;
    u32 flags;
    mg_file_lines_state_t *state;
} mg_file_append_lines_request_t;

/* Merge newline-terminated records and retain the newest max_lines under one
 * filesystem lock. Requires a readable and writable file handle. RESET
 * replaces the previous records. A mutating failure may return
 * MG_ERR_UNCERTAIN; callers must not blindly retry the same batch. */
mg_result_t file_append_lines(mg_handle_t handle, const char *data, usize length,
                              u32 max_lines, u32 flags);

/* Snapshot contents and identity/revision under one filesystem operation lock.
 * Capacity is bounded by MG_FILE_LINES_MAX * MG_FILE_LINE_CAPACITY. */
mg_result_t file_snapshot(mg_handle_t handle, char *data, usize capacity,
                           mg_file_lines_state_t *state);
/* Replace the opened MGFS 1.2 file only if its captured generation remains
 * current. The entire input is copied before filesystem mutation. */
#define MG_FILE_REPLACE_MAX_BYTES (512U * 1024U)
mg_result_t file_replace(mg_handle_t handle, const void *data, usize length);
mg_result_t file_append_lines_state(mg_handle_t handle, const char *data,
                                    usize length, u32 max_lines, u32 flags,
                                    mg_file_lines_state_t *state);
