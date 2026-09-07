/* SPDX-License-Identifier: GPL-3.0-only */
#include <object.h>
#include <heap.h>
#include <terminal.h>
#include <console.h>
#include <process.h>
#include <mangrove_errors.h>

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
    vfs_super_t *super;
    u32 index;
    void *directory_state;
    bool uses_sequential_readdir;
} directory_object_t;

static i64 console_write(kernel_object_t *object, const void *buffer,
                         u64 length)
{
    process_t *process = process_current();
    u64 i;
    if (!object || object->type != OBJECT_TYPE_CONSOLE ||
        (length && !buffer) || !process ||
        !terminal_process_output_allowed(process->pid)) return -1;
    terminal_begin_batch();
    for (i = 0; i < length; i++) terminal_putc(((const char *)buffer)[i]);
    terminal_end_batch();
    return (i64)length;
}

static i64 console_read(kernel_object_t *object, void *buffer, u64 length)
{
    process_t *process = process_current();
    if (!object || object->type != OBJECT_TYPE_CONSOLE ||
        (length && !buffer) || !process ||
        !terminal_process_input_allowed(process->pid)) return -1;

    /* Console transactions are presentation-only.  A parent shell may keep
     * one open while it waits for an external child, but an interactive child
     * must never block behind an invisible prompt.  Flush the pending output
     * before handing this process ownership of console input. */
    terminal_force_end_batch();
    return (i64)console_read_bytes(buffer, length);
}

static i64 file_read(kernel_object_t *object, void *buffer, u64 length)
{
    file_object_t *file = (file_object_t *)object;
    u64 transferred;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle ||
        (length && !buffer)) return -1;
    if (!vfs_super_is_live(file->handle->super)) return MG_ERR_DEVICE_GONE;
    transferred = vfs_file_read(file->handle, length, buffer);
    /* Filesystem drivers expose byte counts, so an in-flight transfer that
     * loses its backing device can otherwise look exactly like EOF.  Preserve
     * successful/partial reads, but turn a zero-byte completion after the
     * superblock became dead into the explicit lifecycle error. */
    if (transferred == 0 && !vfs_super_is_live(file->handle->super))
        return MG_ERR_DEVICE_GONE;
    return (i64)transferred;
}

static i64 file_write(kernel_object_t *object, const void *buffer,
                      u64 length)
{
    file_object_t *file = (file_object_t *)object;
    u64 transferred;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle ||
        (length && !buffer)) return -1;
    if (!vfs_super_is_live(file->handle->super)) return MG_ERR_DEVICE_GONE;
    transferred = vfs_file_write(file->handle, length, buffer);
    if (transferred == 0 && !vfs_super_is_live(file->handle->super))
        return MG_ERR_DEVICE_GONE;
    return (i64)transferred;
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
    if (directory) vfs_super_release(directory->super);
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

static kernel_object_t *object_file_create_node_with_open(
    vfs_node_t *node, u32 flags, bool authorized)
{
    file_object_t *file;
    vfs_file_handle_t *handle = NULL;

    if (!node || node->type != VFS_TYPE_FILE ||
        (authorized ? vfs_open_node_authorized(node, flags, &handle) :
                      vfs_open_node(node, flags, &handle)) != VFS_OK || !handle) {
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
    return object_file_create_node_with_open(node, flags, false);
}

kernel_object_t *object_file_create_node_authorized(vfs_node_t *node,
                                                     u32 flags)
{
    return object_file_create_node_with_open(node, flags, true);
}

kernel_object_t *object_directory_create_node(vfs_node_t *node)
{
    directory_object_t *directory;

    if (!node || node->type != VFS_TYPE_DIRECTORY) {
        return NULL;
    }
    if (!vfs_super_retain(node->super)) return NULL;
    directory = (directory_object_t *)kmalloc(sizeof(*directory));
    if (!directory) {
        vfs_super_release(node->super);
        return NULL;
    }
    object_init(&directory->base, OBJECT_TYPE_DIRECTORY, directory_destroy);
    directory->node = node;
    directory->super = node->super;
    directory->index = 0;
    directory->directory_state = NULL;
    directory->uses_sequential_readdir = false;
    /* Filesystem-specific streaming enumeration cannot see VFS-managed
     * runtime child mounts.  Use VFS enumeration for such directories so
     * /boot/efi and /vol/<name> are ordinary visible namespace entries. */
    if (!vfs_directory_has_mount_children(node) && node->ops &&
        node->ops->readdir_open && node->ops->readdir_next) {
    if (!node->ops->readdir_open(node, &directory->directory_state)) {
            vfs_super_release(node->super);
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
        !directory->node || !out_entry || !vfs_node_is_live(directory->node)) {
        if (directory && directory->super &&
            !vfs_super_is_live(directory->super)) return MG_ERR_DEVICE_GONE;
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
        if (result < 0) return result;
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
    return vfs_truncate_handle(file->handle);
}

int object_file_seek(kernel_object_t *object, i64 offset, u32 whence)
{
    file_object_t *file = (file_object_t *)object;

    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle)
        return VFS_ERR_INVALID_PARAM;
    return vfs_seek(file->handle, offset, (int)whence, NULL);
}
