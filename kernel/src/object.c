#include <object.h>
#include <heap.h>
#include <terminal.h>
#include <console.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef struct {
    kernel_object_t base;
    vfs_file_handle_t *handle;
} file_object_t;

typedef struct {
    kernel_object_t base;
    vfs_node_t *node;
    u32 index;
    void *directory_state;
    bool uses_sequential_readdir;
} directory_object_t;

static i64 console_write(kernel_object_t *object, const void *buffer,
                         u64 length)
{
    u64 i;
    if (!object || object->type != OBJECT_TYPE_CONSOLE ||
        (length && !buffer)) return -1;
    terminal_begin_batch();
    for (i = 0; i < length; i++) terminal_putc(((const char *)buffer)[i]);
    terminal_end_batch();
    return (i64)length;
}

static i64 console_read(kernel_object_t *object, void *buffer, u64 length)
{
    if (!object || object->type != OBJECT_TYPE_CONSOLE ||
        (length && !buffer)) return -1;
    return (i64)console_read_bytes(buffer, length);
}

static i64 file_read(kernel_object_t *object, void *buffer, u64 length)
{
    file_object_t *file = (file_object_t *)object;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle ||
        (length && !buffer)) return -1;
    return (i64)vfs_file_read(file->handle, length, buffer);
}

static i64 file_write(kernel_object_t *object, const void *buffer,
                      u64 length)
{
    file_object_t *file = (file_object_t *)object;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle ||
        (length && !buffer)) return -1;
    return (i64)vfs_file_write(file->handle, length, buffer);
}

static void file_destroy(kernel_object_t *object)
{
    file_object_t *file = (file_object_t *)object;
    if (!file) return;
    if (file->handle) vfs_close(file->handle);
    kfree(file);
}

static void directory_destroy(kernel_object_t *object)
{
    directory_object_t *directory = (directory_object_t *)object;
    if (directory && directory->uses_sequential_readdir && directory->node &&
        directory->node->ops && directory->node->ops->readdir_close) {
        directory->node->ops->readdir_close(directory->directory_state);
    }
    kfree(object);
}

void object_init(kernel_object_t *object, kernel_object_type_t type,
                 void (*destroy)(kernel_object_t *object))
{
    if (!object) return;
    object->type = type;
    object->ref_count = 1;
    object->destroy = destroy;
    object->read = NULL;
    object->write = NULL;
}

bool object_reference(kernel_object_t *object)
{
    if (!object || !object->ref_count || object->ref_count == ~(u32)0) {
        return false;
    }
    object->ref_count++;
    return true;
}

void object_release(kernel_object_t *object)
{
    if (!object || !object->ref_count) return;
    if (--object->ref_count == 0) {
        if (object->destroy) object->destroy(object);
        else kfree(object);
    }
}

kernel_object_t *object_console_create(void)
{
    kernel_object_t *object = (kernel_object_t *)kmalloc(sizeof(*object));
    if (!object) return NULL;
    object_init(object, OBJECT_TYPE_CONSOLE, NULL);
    object->read = console_read;
    object->write = console_write;
    return object;
}

i64 object_read(kernel_object_t *object, void *buffer, u64 length)
{
    if (!object || !object->read) return -1;
    return object->read(object, buffer, length);
}

i64 object_write(kernel_object_t *object, const void *buffer, u64 length)
{
    if (!object || !object->write) return -1;
    return object->write(object, buffer, length);
}

kernel_object_t *object_file_create(const char *path, u32 flags)
{
    file_object_t *file;
    vfs_file_handle_t *handle = NULL;

    if (!path || vfs_open(path, flags, &handle) != VFS_OK || !handle) {
        return NULL;
    }
    if (!handle->node || handle->node->type != VFS_TYPE_FILE) {
        vfs_close(handle);
        return NULL;
    }
    file = (file_object_t *)kmalloc(sizeof(*file));
    if (!file) {
        vfs_close(handle);
        return NULL;
    }
    object_init(&file->base, OBJECT_TYPE_FILE, file_destroy);
    file->base.read = file_read;
    file->base.write = file_write;
    file->handle = handle;
    return &file->base;
}

kernel_object_t *object_file_create_node(vfs_node_t *node, u32 flags)
{
    file_object_t *file;
    vfs_file_handle_t *handle = NULL;

    if (!node || node->type != VFS_TYPE_FILE ||
        vfs_open_node(node, flags, &handle) != VFS_OK || !handle) {
        return NULL;
    }
    if (!handle->node || handle->node->type != VFS_TYPE_FILE) {
        vfs_close(handle);
        return NULL;
    }
    file = (file_object_t *)kmalloc(sizeof(*file));
    if (!file) {
        vfs_close(handle);
        return NULL;
    }
    object_init(&file->base, OBJECT_TYPE_FILE, file_destroy);
    file->base.read = file_read;
    file->base.write = file_write;
    file->handle = handle;
    return &file->base;
}

kernel_object_t *object_directory_create_node(vfs_node_t *node)
{
    directory_object_t *directory;

    if (!node || node->type != VFS_TYPE_DIRECTORY) {
        return NULL;
    }
    directory = (directory_object_t *)kmalloc(sizeof(*directory));
    if (!directory) return NULL;
    object_init(&directory->base, OBJECT_TYPE_DIRECTORY, directory_destroy);
    directory->node = node;
    directory->index = 0;
    directory->directory_state = NULL;
    directory->uses_sequential_readdir = false;
    if (node->ops && node->ops->readdir_open && node->ops->readdir_next) {
        if (!node->ops->readdir_open(node, &directory->directory_state)) {
            kfree(directory);
            return NULL;
        }
        directory->uses_sequential_readdir = true;
    }
    return &directory->base;
}

kernel_object_t *object_directory_create(const char *path)
{
    vfs_node_t *node = NULL;

    if (!path || vfs_lookup(path, &node) != VFS_OK || !node) return NULL;
    return object_directory_create_node(node);
}

i64 object_directory_read(kernel_object_t *object, vfs_dirent_t *out_entry)
{
    directory_object_t *directory = (directory_object_t *)object;

    if (!directory || object->type != OBJECT_TYPE_DIRECTORY ||
        !directory->node || !out_entry) {
        return -1;
    }
    if (directory->uses_sequential_readdir) {
        if (!directory->node->ops->readdir_next(directory->directory_state,
                                                out_entry)) {
            return 0;
        }
    } else if (!vfs_readdir(directory->node, directory->index, out_entry)) {
        return 0;
    }
    directory->index++;
    return 1;
}

i64 object_directory_read_batch(kernel_object_t *object,
                                vfs_dirent_t *out_entries, u32 capacity)
{
    u32 count = 0;

    if (!out_entries || capacity == 0 || capacity > VFS_DIRECTORY_BATCH_MAX) {
        return -1;
    }
    while (count < capacity) {
        i64 result = object_directory_read(object, &out_entries[count]);
        if (result < 0) return -1;
        if (result == 0) break;
        count++;
    }
    return count;
}

int object_file_truncate(kernel_object_t *object)
{
    file_object_t *file = (file_object_t *)object;

    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle ||
        !(file->handle->flags & VFS_OPEN_WRITE)) {
        return VFS_ERR_INVALID_PARAM;
    }
    return vfs_truncate(file->handle->node);
}
