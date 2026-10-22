/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <object.h>
#include <heap.h>
#include <terminal.h>
#include <console.h>
#include <process.h>
#include <mangrove_errors.h>
#include <string.h>

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
    mutex_t state_lock;
    u64 index;
    i64 pending_error;
    void *directory_state;
    bool uses_sequential_readdir;
} directory_object_t;

static i64 console_write(kernel_object_t *object, const void *buffer,
                         u64 length)
{
    process_t *process = process_current();
    if (!object || object->type != OBJECT_TYPE_CONSOLE ||
        (length && !buffer) || !process) return -1;
    return terminal_write_for_process(process->pid, (const char *)buffer,
                                      length);
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
    return console_read_bytes_checked(buffer, length);
}

static i64 file_read(kernel_object_t *object, void *buffer, u64 length)
{
    file_object_t *file = (file_object_t *)object;
    u64 transferred = 0;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle ||
        (length && !buffer)) return MG_ERR_BAD_ARGUMENT;
    int status = vfs_file_read_status(file->handle, length, buffer, &transferred);
    /* The native count ABI represents confirmed partial progress. A subsequent
     * read starts at the first unconfirmed byte. Zero success alone is EOF. */
    if (transferred) return (i64)transferred;
    if (status == VFS_ERR_DEVICE_GONE) return MG_ERR_DEVICE_GONE;
    if (status == VFS_ERR_UNCERTAIN) return MG_ERR_UNCERTAIN;
    if (status == VFS_ERR_ACCESS_DENIED) return MG_ERR_ACCESS_DENIED;
    if (status == VFS_ERR_INVALID_PARAM) return MG_ERR_BAD_ARGUMENT;
    if (status == VFS_ERR_NO_MEM) return MG_ERR_NO_MEMORY;
    if (status == VFS_ERR_CANCELLED) return MG_ERR_CANCELLED;
    if (status == VFS_ERR_UNSUPPORTED) return MG_ERR_UNSUPPORTED;
    if (status == VFS_ERR_NOT_FOUND) return MG_ERR_NOT_FOUND;
    return status == VFS_OK ? 0 : MG_ERR_IO;
}

static i64 file_write(kernel_object_t *object, const void *buffer,
                      u64 length)
{
    file_object_t *file = (file_object_t *)object;
    u64 transferred = 0;
    int status;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle ||
        (length && !buffer)) return -1;
    if (!vfs_super_is_live(file->handle->super)) return MG_ERR_DEVICE_GONE;
    status = vfs_file_write_status(file->handle, length, buffer, &transferred);
    if (status == VFS_ERR_UNCERTAIN) return MG_ERR_UNCERTAIN;
    if (status == VFS_ERR_DEVICE_GONE ||
        (status != VFS_OK && !vfs_super_is_live(file->handle->super)))
        return MG_ERR_DEVICE_GONE;
    if (status == VFS_ERR_TIME_UNAVAILABLE) return MG_ERR_TIME_UNAVAILABLE;
    if (status == VFS_ERR_NO_SPACE) return MG_ERR_NO_SPACE;
    if (status == VFS_ERR_ACCESS_DENIED) return MG_ERR_ACCESS_DENIED;
    if (status != VFS_OK) return MG_ERR_IO;
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
        if (directory->super && vfs_super_operation_lock(directory->super)) {
            directory->node->ops->readdir_close(directory->directory_state);
            vfs_super_operation_unlock(directory->super);
        } else {
            directory->node->ops->readdir_close(directory->directory_state);
        }
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
    u32 count;

    if (!object) return false;
    count = __atomic_load_n(&object->ref_count, __ATOMIC_ACQUIRE);
    while (count && count != ~(u32)0) {
        if (__atomic_compare_exchange_n(&object->ref_count, &count,
                                        count + 1U, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

void object_release(kernel_object_t *object)
{
    u32 count;

    if (!object) return;
    count = __atomic_load_n(&object->ref_count, __ATOMIC_ACQUIRE);
    while (count) {
        if (__atomic_compare_exchange_n(&object->ref_count, &count,
                                        count - 1U, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;
    }
    if (count == 1U) {
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

kernel_object_t *object_file_prepare(void)
{
    file_object_t *file = (file_object_t *)kmalloc(sizeof(*file));
    if (!file) return NULL;
    object_init(&file->base, OBJECT_TYPE_FILE, file_destroy);
    file->base.read = file_read;
    file->base.write = file_write;
    file->handle = NULL;
    return &file->base;
}

void object_file_bind_prepared(kernel_object_t *object,
                               vfs_file_handle_t *handle)
{
    file_object_t *file = (file_object_t *)object;
    file->handle = handle;
}

static kernel_object_t *object_file_create_node_with_open(
    vfs_node_t *node, u32 flags, vfs_authorization_scope_t authorization_scope)
{
    file_object_t *file;
    vfs_file_handle_t *handle = NULL;
    int result;

    if (authorization_scope == VFS_AUTH_CONFIGURATION_WRITE) {
        result = vfs_open_node_authorized(node, flags, &handle);
    } else if (authorization_scope == VFS_AUTH_REGULAR_USER_DATA) {
        result = vfs_open_node_user_authorized(node, flags, &handle);
    } else {
        result = vfs_open_node(node, flags, &handle);
    }
    if (!node || node->type != VFS_TYPE_FILE || result != VFS_OK || !handle) {
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
    return object_file_create_node_with_open(node, flags, VFS_AUTH_NONE);
}

kernel_object_t *object_file_create_node_authorized(vfs_node_t *node,
                                                     u32 flags)
{
    return object_file_create_node_with_open(
        node, flags, VFS_AUTH_CONFIGURATION_WRITE);
}

kernel_object_t *object_file_create_node_user_authorized(vfs_node_t *node,
                                                          u32 flags)
{
    return object_file_create_node_with_open(
        node, flags, VFS_AUTH_REGULAR_USER_DATA);
}

kernel_object_t *object_directory_create_node(vfs_node_t *node)
{
    directory_object_t *directory;
    vfs_super_t *super;
    bool operation_locked;

    if (!node || node->type != VFS_TYPE_DIRECTORY) {
        return NULL;
    }
    if (!vfs_node_retain_super(node, &super)) return NULL;
    directory = (directory_object_t *)kmalloc(sizeof(*directory));
    if (!directory) {
        vfs_super_release(super);
        return NULL;
    }
    object_init(&directory->base, OBJECT_TYPE_DIRECTORY, directory_destroy);
    directory->node = node;
    directory->super = super;
    mutex_init(&directory->state_lock);
    directory->index = 0;
    directory->pending_error = 0;
    directory->directory_state = NULL;
    directory->uses_sequential_readdir = false;
    /* Filesystem-specific streaming enumeration cannot see VFS-managed
     * runtime child mounts.  Use VFS enumeration for such directories so
     * /boot/efi and /vol/<name> are ordinary visible namespace entries. */
    if (!vfs_directory_has_mount_children(node) && node->ops &&
        node->ops->readdir_open &&
        (node->ops->readdir_next || node->ops->readdir_next_checked)) {
        operation_locked = vfs_super_operation_lock(super);
        if (!operation_locked ||
            !node->ops->readdir_open(node, &directory->directory_state)) {
            if (operation_locked)
                vfs_super_operation_unlock(super);
            vfs_super_release(super);
            kfree(directory);
            return NULL;
        }
        vfs_super_operation_unlock(super);
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

static i64 directory_native_error(int result)
{
    switch (result) {
        case VFS_ERR_NAME_TOO_LONG: return MG_ERR_NAME_TOO_LONG;
        case VFS_ERR_NO_MEM: return MG_ERR_NO_MEMORY;
        case VFS_ERR_DEVICE_GONE: return MG_ERR_DEVICE_GONE;
        case VFS_ERR_UNSUPPORTED: return MG_ERR_UNSUPPORTED;
        case VFS_ERR_ACCESS_DENIED: return MG_ERR_ACCESS_DENIED;
        case VFS_ERR_CANCELLED: return MG_ERR_CANCELLED;
        case VFS_ERR_BUSY: return MG_ERR_BUSY;
        case VFS_ERR_UNCERTAIN: return MG_ERR_UNCERTAIN;
        case VFS_ERR_NOT_FOUND: return MG_ERR_NOT_FOUND;
        case VFS_ERR_BUFFER_TOO_SMALL: return MG_ERR_BUFFER_TOO_SMALL;
        case VFS_ERR_INVALID_PARAM: return MG_ERR_BAD_ARGUMENT;
        case VFS_ERR_NOT_DIRECTORY: return MG_ERR_NOT_DIRECTORY;
        /* The native ABI has no separate corruption result. */
        default: return MG_ERR_IO;
    }
}

/* Caller owns state_lock. Deferred errors precede the next filesystem read. */
static i64 object_directory_read_locked(directory_object_t *directory,
                                        vfs_dirent_t *out_entry)
{
    int result;
    vfs_dirent_t candidate = {0};
    if (directory->pending_error) {
        i64 error = directory->pending_error;
        directory->pending_error = 0;
        return error;
    }
    if (!directory->node || !out_entry || !vfs_node_is_live(directory->node)) {
        if (directory && directory->super &&
            !vfs_super_is_live(directory->super)) return MG_ERR_DEVICE_GONE;
        return MG_ERR_BAD_ARGUMENT;
    }
    if (directory->uses_sequential_readdir) {
        if (!vfs_super_operation_lock(directory->super)) {
            if (vfs_super_operation_error(directory->super) == VFS_ERR_UNCERTAIN)
                return MG_ERR_UNCERTAIN;
            return vfs_super_is_live(directory->super)
                ? MG_ERR_CANCELLED : MG_ERR_DEVICE_GONE;
        }
        result = directory->node->ops->readdir_next_checked
            ? directory->node->ops->readdir_next_checked(directory->directory_state, &candidate)
            : directory->node->ops->readdir_next(directory->directory_state, &candidate) ? 1 : 0;
        vfs_super_operation_unlock(directory->super);
    } else {
        if (directory->index > ~(u32)0) return MG_ERR_BUFFER_TOO_SMALL;
        result = vfs_readdir_checked(directory->node, (u32)directory->index, &candidate);
    }
    if (result == 1) {
        usize length = 0U;
        while (length < sizeof(candidate.name) && candidate.name[length]) length++;
        if (length == sizeof(candidate.name)) return MG_ERR_IO;
        *out_entry = candidate;
        directory->index++;
    } else if (result == VFS_ERR_NAME_TOO_LONG) {
        directory->index++;
    }
    return result < 0 ? directory_native_error(result) : result > 1 ? MG_ERR_IO : result;
}

i64 object_directory_read(kernel_object_t *object, vfs_dirent_t *out_entry)
{
    directory_object_t *directory = (directory_object_t *)object;
    i64 result;
    if (!object || object->type != OBJECT_TYPE_DIRECTORY || !out_entry)
        return MG_ERR_BAD_ARGUMENT;
    if (!mutex_lock(&directory->state_lock)) return MG_ERR_CANCELLED;
    result = object_directory_read_locked(directory, out_entry);
    (void)mutex_unlock(&directory->state_lock);
    return result;
}

i64 object_directory_read_batch(kernel_object_t *object,
                                vfs_dirent_t *out_entries, u32 capacity)
{
    u32 count = 0;
    directory_object_t *directory = (directory_object_t *)object;
    i64 result = 0;

    if (!object || object->type != OBJECT_TYPE_DIRECTORY || !out_entries ||
        capacity == 0 || capacity > VFS_DIRECTORY_BATCH_MAX) {
        return MG_ERR_BAD_ARGUMENT;
    }
    if (!mutex_lock(&directory->state_lock)) return MG_ERR_CANCELLED;
    while (count < capacity) {
        result = object_directory_read_locked(directory, &out_entries[count]);
        if (result <= 0) break;
        count++;
    }
    if (count && result < 0) directory->pending_error = result;
    (void)mutex_unlock(&directory->state_lock);
    return count ? (i64)count : result;
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

int object_file_append_lines(kernel_object_t *object, const char *data,
                             usize length, u32 max_lines, u32 flags, mg_file_lines_state_t *state)
{
    file_object_t *file = (file_object_t *)object;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle)
        return VFS_ERR_INVALID_PARAM;
    return vfs_file_append_lines(file->handle, data, length, max_lines, flags, state);
}

int object_file_seek(kernel_object_t *object, i64 offset, u32 whence)
{
    file_object_t *file = (file_object_t *)object;

    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle)
        return VFS_ERR_INVALID_PARAM;
    return vfs_seek(file->handle, offset, (int)whence, NULL);
}

int object_file_position(kernel_object_t *object, u64 *out_position)
{
    file_object_t *file = (file_object_t *)object;

    if (!out_position || !file || object->type != OBJECT_TYPE_FILE ||
        !file->handle) return VFS_ERR_INVALID_PARAM;
    return vfs_seek(file->handle, 0, MG_SEEK_CUR, out_position);
}

int object_file_snapshot(kernel_object_t *object, char *data, usize capacity,
                          mg_file_lines_state_t *state)
{
    file_object_t *file = (file_object_t *)object;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle)
        return VFS_ERR_INVALID_PARAM;
    return vfs_file_snapshot(file->handle, data, capacity, state);
}

int object_file_snapshot_stream(kernel_object_t *object, usize capacity,
                                vfs_snapshot_sink_t sink,
                                vfs_snapshot_finish_t finish, void *context)
{
    file_object_t *file = (file_object_t *)object;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle)
        return VFS_ERR_INVALID_PARAM;
    return vfs_file_snapshot_stream(file->handle, capacity, sink, finish,
                                    context);
}

int object_file_replace(kernel_object_t *object, u64 length,
                        vfs_replace_source_t source, void *context)
{
    file_object_t *file = (file_object_t *)object;
    if (!file || object->type != OBJECT_TYPE_FILE || !file->handle)
        return VFS_ERR_INVALID_PARAM;
    return vfs_file_replace(file->handle, true, length, source, context);
}
