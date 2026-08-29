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
kernel_object_t *object_file_create(const char *path, u32 flags);
kernel_object_t *object_file_create_node(vfs_node_t *node, u32 flags);
kernel_object_t *object_directory_create(const char *path);
kernel_object_t *object_directory_create_node(vfs_node_t *node);
i64 object_read(kernel_object_t *object, void *buffer, u64 length);
i64 object_write(kernel_object_t *object, const void *buffer, u64 length);
i64 object_directory_read(kernel_object_t *object, vfs_dirent_t *out_entry);
i64 object_directory_read_batch(kernel_object_t *object,
                                vfs_dirent_t *out_entries, u32 capacity);
int object_file_truncate(kernel_object_t *object);
