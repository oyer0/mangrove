#include <vfs.h>
#include <heap.h>
#include <string.h>
#include <kprint.h>
#include <process.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

static vfs_fs_type_t *fs_type_list = NULL;
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];

#define VFS_FILE_HANDLE_VALID 0x56465348U

static bool vfs_context_uid(u32 *out_uid, bool *out_kernel)
{
    process_t *process;
    process_credentials_t credentials;

    if (!out_uid || !out_kernel) return false;
    process = process_current();
    if (!process) {
        *out_uid = VFS_UID_SYSTEM;
        *out_kernel = true;
        return true;
    }
    if (!process_get_credentials(process, &credentials) ||
        !identity_credentials_valid(&credentials)) {
        return false;
    }
    *out_uid = credentials.uid;
    *out_kernel = false;
    return true;
}

bool vfs_current_uid(u32 *out_uid)
{
    bool kernel_context;
    return vfs_context_uid(out_uid, &kernel_context);
}

bool vfs_check_access(const vfs_node_t *node, u32 permission)
{
    u32 uid;
    bool kernel_context;
    u32 available;

    if (!node || (permission & ~VFS_ACCESS_READ_WRITE) != 0U ||
        permission == 0U || !vfs_context_uid(&uid, &kernel_context)) {
        return false;
    }
    if (kernel_context) return true;
    available = 0U;
    if (uid == node->owner_uid) {
        if ((node->permissions & VFS_PERMISSION_OWNER_READ) != 0U) {
            available |= VFS_ACCESS_READ;
        }
        if ((node->permissions & VFS_PERMISSION_OWNER_WRITE) != 0U) {
            available |= VFS_ACCESS_WRITE;
        }
    } else {
        if ((node->permissions & VFS_PERMISSION_OTHER_READ) != 0U) {
            available |= VFS_ACCESS_READ;
        }
        if ((node->permissions & VFS_PERMISSION_OTHER_WRITE) != 0U) {
            available |= VFS_ACCESS_WRITE;
        }
    }
    return (available & permission) == permission;
}

void vfs_node_set_security(vfs_node_t *node, u32 owner_uid, u32 permissions)
{
    if (!node) return;
    node->owner_uid = owner_uid;
    node->permissions = permissions & VFS_PERMISSION_KNOWN;
}

void vfs_init(void) {
    fs_type_list = NULL;
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_table[i].sb = NULL;
        mount_table[i].covered_node = NULL;
        mount_table[i].active = false;
    }
}

int vfs_register_fs(vfs_fs_type_t *fs_type) {
    if (!fs_type || !fs_type->name) {
        return VFS_ERR_INVALID_PARAM;
    }

    if (vfs_find_fs(fs_type->name) != NULL) {
        return VFS_ERR_INVALID_PARAM;
    }

    fs_type->next = fs_type_list;
    fs_type_list = fs_type;
    return VFS_OK;
}

vfs_fs_type_t *vfs_find_fs(const char *name) {
    if (!name) {
        return NULL;
    }

    vfs_fs_type_t *curr = fs_type_list;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

int vfs_mount_root(const char *fs_name, block_device_t *dev) {
    if (!fs_name) {
        return VFS_ERR_INVALID_PARAM;
    }

    if (mount_table[0].active) {
        return VFS_ERR_INVALID_PARAM;
    }

    vfs_fs_type_t *fs_type = vfs_find_fs(fs_name);
    if (!fs_type || !fs_type->mount) {
        return VFS_ERR_NOT_FOUND;
    }

    vfs_super_t *sb = NULL;
    int res = fs_type->mount(fs_type, dev, &sb);
    if (res != VFS_OK || !sb || !sb->root_node) {
        return (res != VFS_OK) ? res : VFS_ERR_BAD_FORMAT;
    }

    mount_table[0].sb = sb;
    mount_table[0].covered_node = NULL;
    mount_table[0].active = true;

    return VFS_OK;
}

int vfs_mount_node(vfs_node_t *target_node, const char *fs_name, block_device_t *dev) {
    if (!target_node || target_node->type != VFS_TYPE_DIRECTORY || !fs_name) {
        return VFS_ERR_INVALID_PARAM;
    }

    int slot = -1;
    for (u32 i = 1; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        return VFS_ERR_NO_MEM;
    }

    if (vfs_find_mount_for_node(target_node) != NULL) {
        return VFS_ERR_INVALID_PARAM;
    }

    vfs_fs_type_t *fs_type = vfs_find_fs(fs_name);
    if (!fs_type || !fs_type->mount) {
        return VFS_ERR_NOT_FOUND;
    }

    vfs_super_t *sb = NULL;
    int res = fs_type->mount(fs_type, dev, &sb);
    if (res != VFS_OK || !sb || !sb->root_node) {
        return (res != VFS_OK) ? res : VFS_ERR_BAD_FORMAT;
    }

    mount_table[slot].sb = sb;
    mount_table[slot].covered_node = target_node;
    mount_table[slot].active = true;

    return VFS_OK;
}

int vfs_unmount_sb(vfs_super_t *sb) {
    if (!sb) {
        return VFS_ERR_INVALID_PARAM;
    }

    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].sb == sb) {
            if (sb->ops && sb->ops->unmount) {
                sb->ops->unmount(sb);
            }
            mount_table[i].sb = NULL;
            mount_table[i].covered_node = NULL;
            mount_table[i].active = false;
            return VFS_OK;
        }
    }

    return VFS_ERR_NOT_FOUND;
}

vfs_node_t *vfs_get_root_node(void) {
    if (mount_table[0].active && mount_table[0].sb) {
        return mount_table[0].sb->root_node;
    }
    return NULL;
}

vfs_mount_t *vfs_find_mount_for_node(vfs_node_t *node) {
    if (!node) {
        return NULL;
    }

    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].covered_node == node) {
            return &mount_table[i];
        }
    }

    return NULL;
}

static vfs_node_t *vfs_resolve_mount(vfs_node_t *node) {
    if (!node) return NULL;
    vfs_mount_t *m = vfs_find_mount_for_node(node);
    if (m && m->active && m->sb && m->sb->root_node) {
        return m->sb->root_node;
    }
    return node;
}

static vfs_node_t *vfs_finddir_internal(vfs_node_t *dir, const char *name,
                                        bool enforce) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->finddir || (enforce &&
        !vfs_check_access(dir, VFS_ACCESS_READ))) {
        return NULL;
    }
    return dir->ops->finddir(dir, name);
}

static int vfs_lookup_internal(const char *path, vfs_node_t **out_node,
                               bool enforce) {
    if (!path || path[0] != '/' || !out_node) {
        return VFS_ERR_INVALID_PARAM;
    }

    vfs_node_t *curr = vfs_get_root_node();
    if (!curr) {
        return VFS_ERR_NOT_FOUND;
    }

    const char *ptr = path;

    while (*ptr) {
        while (*ptr == '/') {
            ptr++;
        }

        if (*ptr == '\0') {
            break;
        }

        char component[256];
        u32 len = 0;
        while (*ptr && *ptr != '/') {
            if (len >= sizeof(component) - 1) {
                return VFS_ERR_INVALID_PARAM;
            }
            component[len++] = *ptr;
            ptr++;
        }
        component[len] = '\0';

        curr = vfs_resolve_mount(curr);

        if (curr->type != VFS_TYPE_DIRECTORY) {
            return VFS_ERR_NOT_FOUND;
        }
        if (enforce && !vfs_check_access(curr, VFS_ACCESS_READ)) {
            return VFS_ERR_ACCESS_DENIED;
        }

        vfs_node_t *next = vfs_finddir_internal(curr, component, enforce);
        if (!next) {
            return VFS_ERR_NOT_FOUND;
        }

        curr = vfs_resolve_mount(next);
    }

    /* Looking up a protected object is itself a read operation.  This keeps
       private files and directories from being discoverable by pathname. */
    if (enforce && !vfs_check_access(curr, VFS_ACCESS_READ)) {
        return VFS_ERR_ACCESS_DENIED;
    }

    *out_node = curr;
    return VFS_OK;
}

int vfs_lookup(const char *path, vfs_node_t **out_node) {
    return vfs_lookup_internal(path, out_node, true);
}

int vfs_lookup_trusted(const char *path, vfs_node_t **out_node) {
    return vfs_lookup_internal(path, out_node, false);
}

int vfs_open_node(vfs_node_t *node, u32 flags,
                  vfs_file_handle_t **out_handle) {
    vfs_file_handle_t *handle;

    if (!node || !out_handle ||
        (flags != VFS_OPEN_READ && flags != VFS_OPEN_WRITE && flags != VFS_OPEN_RDWR)) {
        return VFS_ERR_INVALID_PARAM;
    }

    *out_handle = NULL;
    if (((flags & VFS_OPEN_READ) &&
         !vfs_check_access(node, VFS_ACCESS_READ)) ||
        ((flags & VFS_OPEN_WRITE) &&
         !vfs_check_access(node, VFS_ACCESS_WRITE))) {
        return VFS_ERR_ACCESS_DENIED;
    }

    handle = (vfs_file_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) {
        return VFS_ERR_NO_MEM;
    }

    handle->node = node;
    handle->offset = 0;
    handle->flags = flags;
    handle->valid = VFS_FILE_HANDLE_VALID;
    *out_handle = handle;
    return VFS_OK;
}

int vfs_open(const char *path, u32 flags, vfs_file_handle_t **out_handle) {
    vfs_node_t *node = NULL;
    int result;

    if (!path || path[0] == '\0' || path[0] != '/' || !out_handle) {
        return VFS_ERR_INVALID_PARAM;
    }
    result = vfs_lookup(path, &node);
    if (result != VFS_OK) return result;
    return vfs_open_node(node, flags, out_handle);
}

int vfs_open_trusted(const char *path, u32 flags,
                     vfs_file_handle_t **out_handle) {
    vfs_node_t *node = NULL;
    vfs_file_handle_t *handle;

    if (!path || path[0] != '/' || !out_handle ||
        (flags != VFS_OPEN_READ && flags != VFS_OPEN_WRITE &&
         flags != VFS_OPEN_RDWR)) {
        return VFS_ERR_INVALID_PARAM;
    }
    *out_handle = NULL;
    if (vfs_lookup_trusted(path, &node) != VFS_OK || !node) {
        return VFS_ERR_NOT_FOUND;
    }
    handle = (vfs_file_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return VFS_ERR_NO_MEM;
    handle->node = node;
    handle->offset = 0;
    handle->flags = flags;
    handle->valid = VFS_FILE_HANDLE_VALID;
    *out_handle = handle;
    return VFS_OK;
}

int vfs_close(vfs_file_handle_t *handle) {
    if (!handle || handle->valid != VFS_FILE_HANDLE_VALID || !handle->node) {
        return VFS_ERR_INVALID_PARAM;
    }

    handle->valid = 0;
    handle->node = NULL;
    kfree(handle);
    return VFS_OK;
}

static bool vfs_handle_valid(const vfs_file_handle_t *handle) {
    return handle && handle->valid == VFS_FILE_HANDLE_VALID && handle->node;
}

u64 vfs_file_read(vfs_file_handle_t *handle, u64 size, void *buffer) {
    u64 transferred;

    if (!vfs_handle_valid(handle) || !(handle->flags & VFS_OPEN_READ) ||
        (size != 0 && !buffer) || !handle->node->ops || !handle->node->ops->read) {
        return 0;
    }
    if (!vfs_check_access(handle->node, VFS_ACCESS_READ)) return 0;
    if (size == 0) {
        return 0;
    }

    transferred = handle->node->ops->read(handle->node, handle->offset, size, buffer);
    if (transferred <= (u64)-1 - handle->offset) {
        handle->offset += transferred;
    }
    return transferred;
}

u64 vfs_file_read_trusted(vfs_file_handle_t *handle, u64 size, void *buffer) {
    u64 transferred;

    if (!vfs_handle_valid(handle) || !(handle->flags & VFS_OPEN_READ) ||
        (size != 0 && !buffer) || !handle->node->ops ||
        !handle->node->ops->read || size == 0) return 0;
    transferred = handle->node->ops->read(handle->node, handle->offset, size,
                                          buffer);
    if (transferred <= (u64)-1 - handle->offset) handle->offset += transferred;
    return transferred;
}

u64 vfs_file_write(vfs_file_handle_t *handle, u64 size, const void *buffer) {
    u64 transferred;

    if (!vfs_handle_valid(handle) || !(handle->flags & VFS_OPEN_WRITE) ||
        (size != 0 && !buffer) || !handle->node->ops || !handle->node->ops->write) {
        return 0;
    }
    if (!vfs_check_access(handle->node, VFS_ACCESS_WRITE)) return 0;
    if (size == 0) {
        return 0;
    }

    transferred = handle->node->ops->write(handle->node, handle->offset, size, buffer);
    if (transferred <= (u64)-1 - handle->offset) {
        handle->offset += transferred;
    }
    return transferred;
}

u64 vfs_file_write_trusted(vfs_file_handle_t *handle, u64 size,
                           const void *buffer) {
    u64 transferred;

    if (!vfs_handle_valid(handle) || !(handle->flags & VFS_OPEN_WRITE) ||
        (size != 0 && !buffer) || !handle->node->ops ||
        !handle->node->ops->write || size == 0) return 0;
    transferred = handle->node->ops->write(handle->node, handle->offset, size,
                                           buffer);
    if (transferred <= (u64)-1 - handle->offset) handle->offset += transferred;
    return transferred;
}

int vfs_close_trusted(vfs_file_handle_t *handle)
{
    return vfs_close(handle);
}

int vfs_seek(vfs_file_handle_t *handle, i64 offset, int whence, u64 *out_offset) {
    u64 base, target, magnitude;

    if (!vfs_handle_valid(handle) ||
        (whence != VFS_SEEK_SET && whence != VFS_SEEK_CUR && whence != VFS_SEEK_END)) {
        return VFS_ERR_INVALID_PARAM;
    }

    if (whence == VFS_SEEK_SET) {
        if (offset < 0) return VFS_ERR_INVALID_PARAM;
        target = (u64)offset;
    } else {
        base = (whence == VFS_SEEK_CUR) ? handle->offset : handle->node->size;
        if (offset >= 0) {
            if (base > (u64)-1 - (u64)offset) return VFS_ERR_INVALID_PARAM;
            target = base + (u64)offset;
        } else {
            magnitude = (u64)(-(offset + 1)) + 1;
            if (magnitude > base) return VFS_ERR_INVALID_PARAM;
            target = base - magnitude;
        }
    }

    handle->offset = target;
    if (out_offset) *out_offset = target;
    return VFS_OK;
}

int vfs_resolve_path(const char *cwd, const char *input_path, char *out_buf, usize out_size) {
    const char *base;
    usize input_len;
    usize base_len;
    usize raw_len;
    usize required;
    usize written;

    if (!input_path || !out_buf || out_size == 0) {
        return VFS_ERR_INVALID_PARAM;
    }

    char raw[512];
    if (input_path[0] == '/') {
        input_len = strlen(input_path);
        if (input_len >= sizeof(raw)) return VFS_ERR_INVALID_PARAM;
        memcpy(raw, input_path, input_len + 1);
    } else {
        base = (cwd && cwd[0]) ? cwd : "/";
        base_len = strlen(base);
        input_len = strlen(input_path);
        raw_len = base_len + input_len;
        if (base_len && base[base_len - 1] != '/') raw_len++;
        if (raw_len >= sizeof(raw)) return VFS_ERR_INVALID_PARAM;
        memcpy(raw, base, base_len);
        written = base_len;
        if (written && raw[written - 1] != '/') raw[written++] = '/';
        memcpy(raw + written, input_path, input_len + 1);
    }

    char *stack[32];
    int top = 0;

    char *ptr = raw;
    while (*ptr) {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        char *comp = ptr;
        while (*ptr && *ptr != '/') ptr++;
        if (*ptr) {
            *ptr++ = '\0';
        }

        if (strcmp(comp, ".") == 0) {
            continue;
        }
        if (strcmp(comp, "..") == 0) {
            if (top > 0) {
                top--;
            }
            continue;
        }

        if (strlen(comp) >= 256 || top == 32) return VFS_ERR_INVALID_PARAM;
        stack[top++] = comp;
    }

    required = 1;
    for (int i = 0; i < top; i++) {
        usize component_length = strlen(stack[i]);
        if (component_length > ~(usize)0 - required - (i ? 1 : 0)) {
            return VFS_ERR_INVALID_PARAM;
        }
        required += component_length + (i ? 1 : 0);
    }
    if (required >= out_size) return VFS_ERR_INVALID_PARAM;

    out_buf[0] = '/';
    written = 1;
    for (int i = 0; i < top; i++) {
        usize component_length = strlen(stack[i]);
        if (i) out_buf[written++] = '/';
        memcpy(out_buf + written, stack[i], component_length);
        written += component_length;
    }
    out_buf[written] = '\0';

    return VFS_OK;
}

int vfs_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->ops || !dir->ops->create) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return dir->ops->create(dir, name, out_node);
}

int vfs_create_owned(vfs_node_t *dir, const char *name, u32 owner_uid,
                     u32 permissions, vfs_node_t **out_node) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node ||
        !dir->ops || !dir->ops->create_owned ||
        (permissions & ~VFS_PERMISSION_KNOWN) != 0U || !permissions) {
        return VFS_ERR_INVALID_PARAM;
    }
    return dir->ops->create_owned(dir, name, owner_uid, permissions, out_node);
}

int vfs_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->ops || !dir->ops->mkdir) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return dir->ops->mkdir(dir, name, out_node);
}

int vfs_mkdir_owned(vfs_node_t *dir, const char *name, u32 owner_uid,
                    u32 permissions, vfs_node_t **out_node) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node ||
        !dir->ops || !dir->ops->mkdir_owned ||
        (permissions & ~VFS_PERMISSION_KNOWN) != 0U || !permissions) {
        return VFS_ERR_INVALID_PARAM;
    }
    return dir->ops->mkdir_owned(dir, name, owner_uid, permissions, out_node);
}

int vfs_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops || !dir->ops->unlink) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return dir->ops->unlink(dir, name);
}

int vfs_unlink_trusted(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->unlink) return VFS_ERR_INVALID_PARAM;
    return dir->ops->unlink(dir, name);
}

int vfs_rmdir(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops || !dir->ops->rmdir) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return dir->ops->rmdir(dir, name);
}

int vfs_rmdir_trusted(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->rmdir) return VFS_ERR_INVALID_PARAM;
    return dir->ops->rmdir(dir, name);
}

int vfs_rename(vfs_node_t *src_dir, const char *src_name,
               vfs_node_t *dst_dir, const char *dst_name) {
    if (!src_dir || !src_name || !dst_dir || !dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY ||
        !src_dir->ops || !src_dir->ops->rename) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_check_access(src_dir, VFS_ACCESS_WRITE) ||
        !vfs_check_access(dst_dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return src_dir->ops->rename(src_dir, src_name, dst_dir, dst_name);
}

int vfs_rename_trusted(vfs_node_t *src_dir, const char *src_name,
                       vfs_node_t *dst_dir, const char *dst_name) {
    if (!src_dir || !src_name || !dst_dir || !dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY || !src_dir->ops ||
        !src_dir->ops->rename) return VFS_ERR_INVALID_PARAM;
    return src_dir->ops->rename(src_dir, src_name, dst_dir, dst_name);
}

int vfs_truncate(vfs_node_t *node) {
    if (!node || node->type != VFS_TYPE_FILE) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!node->ops || !node->ops->truncate) return VFS_ERR_UNSUPPORTED;
    if (!vfs_check_access(node, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return node->ops->truncate(node);
}

int vfs_truncate_trusted(vfs_node_t *node) {
    if (!node || node->type != VFS_TYPE_FILE || !node->ops ||
        !node->ops->truncate) return VFS_ERR_INVALID_PARAM;
    return node->ops->truncate(node);
}

u64 vfs_read(vfs_node_t *node, u64 offset, u64 size, void *buffer) {
    if (!node || !buffer || !node->ops || !node->ops->read) {
        return 0;
    }
    if (!vfs_check_access(node, VFS_ACCESS_READ)) return 0;
    return node->ops->read(node, offset, size, buffer);
}

u64 vfs_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer) {
    if (!node || !buffer || !node->ops || !node->ops->write) {
        return 0;
    }
    if (!vfs_check_access(node, VFS_ACCESS_WRITE)) return 0;
    return node->ops->write(node, offset, size, buffer);
}

vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name) {
    return vfs_finddir_internal(dir, name, true);
}

vfs_node_t *vfs_finddir_trusted(vfs_node_t *dir, const char *name) {
    return vfs_finddir_internal(dir, name, false);
}

bool vfs_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    if (!dir || !out_entry || dir->type != VFS_TYPE_DIRECTORY || !dir->ops || !dir->ops->readdir) {
        return false;
    }
    if (!vfs_check_access(dir, VFS_ACCESS_READ)) return false;
    return dir->ops->readdir(dir, index, out_entry);
}

bool vfs_readdir_trusted(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    if (!dir || !out_entry || dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->readdir) return false;
    return dir->ops->readdir(dir, index, out_entry);
}
