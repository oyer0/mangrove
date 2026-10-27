/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <vfs.h>

typedef enum {
    OBJECT_TYPE_INVALID = 0,
    OBJECT_TYPE_PROCESS,
    OBJECT_TYPE_THREAD,
    OBJECT_TYPE_FILE,
    OBJECT_TYPE_DIRECTORY,
    OBJECT_TYPE_CONSOLE,
    OBJECT_TYPE_NETWORK_ICMP,
    OBJECT_TYPE_NETWORK_DATAGRAM,
    OBJECT_TYPE_NETWORK_STREAM,
    OBJECT_TYPE_IPC_ENDPOINT,
    OBJECT_TYPE_IPC_REQUEST,
    OBJECT_TYPE_TERMINAL_CONTROL_STREAM,
} kernel_object_type_t;

#define OBJECT_RIGHT_READ  (1U << 0)
#define OBJECT_RIGHT_WRITE (1U << 1)

typedef struct kernel_object kernel_object_t;

typedef i64 (*kernel_object_read_op_t)(kernel_object_t *object,
                                       void *buffer, u64 length);
typedef i64 (*kernel_object_write_op_t)(kernel_object_t *object,
                                        const void *buffer, u64 length);

struct kernel_object {
    kernel_object_type_t type;
    u32 ref_count;
    void (*destroy)(kernel_object_t *object);
    kernel_object_read_op_t read;
    kernel_object_write_op_t write;
};

void object_init(kernel_object_t *object, kernel_object_type_t type,
                 void (*destroy)(kernel_object_t *object));
bool object_reference(kernel_object_t *object);
void object_release(kernel_object_t *object);
kernel_object_t *object_console_create(void);
kernel_object_t *object_terminal_control_stream_create(void);
kernel_object_t *object_file_create(const char *path, u32 flags);
/* Allocate before a persistent create. Binding transfers the VFS handle to
 * this object and cannot allocate or fail. */
kernel_object_t *object_file_prepare(void);
void object_file_bind_prepared(kernel_object_t *object,
                               vfs_file_handle_t *handle);
kernel_object_t *object_file_create_node(vfs_node_t *node, u32 flags);
kernel_object_t *object_file_create_node_authorized(vfs_node_t *node,
                                                     u32 flags);
kernel_object_t *object_file_create_node_user_authorized(vfs_node_t *node,
                                                          u32 flags);
kernel_object_t *object_directory_create(const char *path);
kernel_object_t *object_directory_create_node(vfs_node_t *node);
i64 object_read(kernel_object_t *object, void *buffer, u64 length);
i64 object_write(kernel_object_t *object, const void *buffer, u64 length);
i64 object_directory_read(kernel_object_t *object, vfs_dirent_t *out_entry);
i64 object_directory_read_batch(kernel_object_t *object,
                                vfs_dirent_t *out_entries, u32 capacity,
                                usize name_capacity);
int object_file_truncate(kernel_object_t *object);
int object_file_append_lines(kernel_object_t *object, const char *data,
                             usize length, u32 max_lines, u32 flags, mg_file_lines_state_t *state);
int object_file_seek(kernel_object_t *object, i64 offset, u32 whence);
int object_file_position(kernel_object_t *object, u64 *out_position);

int object_file_snapshot(kernel_object_t *object, char *data, usize capacity,
                          mg_file_lines_state_t *state);
int object_file_snapshot_stream(kernel_object_t *object, usize capacity,
                                vfs_snapshot_sink_t sink,
                                vfs_snapshot_finish_t finish, void *context);
/* Caller has retained the process file object with OBJECT_RIGHT_WRITE. */
int object_file_replace(kernel_object_t *object, u64 length,
                        vfs_replace_source_t source, void *context);
