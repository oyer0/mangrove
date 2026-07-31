#include <vfs.h>
#include <heap.h>
#include <string.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

static vfs_fs_type_t *fs_type_list = NULL;
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];

#define VFS_FILE_HANDLE_VALID 0x56465348U

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

int vfs_lookup(const char *path, vfs_node_t **out_node) {
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

        vfs_node_t *next = vfs_finddir(curr, component);
        if (!next) {
            return VFS_ERR_NOT_FOUND;
        }

        curr = vfs_resolve_mount(next);
    }

    *out_node = curr;
    return VFS_OK;
}

int vfs_open(const char *path, u32 flags, vfs_file_handle_t **out_handle) {
    vfs_node_t *node = NULL;
    vfs_file_handle_t *handle;

    if (!path || path[0] == '\0' || path[0] != '/' || !out_handle ||
        (flags != VFS_OPEN_READ && flags != VFS_OPEN_WRITE && flags != VFS_OPEN_RDWR)) {
        return VFS_ERR_INVALID_PARAM;
    }

    *out_handle = NULL;
    int result = vfs_lookup(path, &node);
    if (result != VFS_OK) {
        return result;
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

int vfs_close(vfs_file_handle_t *handle) {
    if (!handle || handle->valid != VFS_FILE_HANDLE_VALID || !handle->node) {
        return VFS_ERR_INVALID_PARAM;
    }

    handle->valid = 0;
    handle->node = NULL;
    kfree(handle);
    return VFS_OK;
}

int vfs_resolve_path(const char *cwd, const char *input_path, char *out_buf, usize out_size) {
    if (!input_path || !out_buf || out_size == 0) {
        return VFS_ERR_INVALID_PARAM;
    }

    char raw[512];
    if (input_path[0] == '/') {
        strncpy(raw, input_path, sizeof(raw) - 1);
    } else {
        const char *base = (cwd && cwd[0]) ? cwd : "/";
        usize base_len = strlen(base);
        strncpy(raw, base, sizeof(raw) - 1);
        if (base_len > 0 && base[base_len - 1] != '/') {
            strncat(raw, "/", sizeof(raw) - strlen(raw) - 1);
        }
        strncat(raw, input_path, sizeof(raw) - strlen(raw) - 1);
    }
    raw[sizeof(raw) - 1] = '\0';

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

        if (top < 32) {
            stack[top++] = comp;
        }
    }

    out_buf[0] = '/';
    out_buf[1] = '\0';
    usize curr_len = 1;

    for (int i = 0; i < top; i++) {
        if (curr_len > 1) {
            if (curr_len < out_size - 1) {
                out_buf[curr_len++] = '/';
                out_buf[curr_len] = '\0';
            }
        }
        usize c_len = strlen(stack[i]);
        if (curr_len + c_len < out_size) {
            strcpy(out_buf + curr_len, stack[i]);
            curr_len += c_len;
        }
    }

    return VFS_OK;
}

int vfs_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->ops || !dir->ops->create) {
        return VFS_ERR_INVALID_PARAM;
    }
    return dir->ops->create(dir, name, out_node);
}

int vfs_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->ops || !dir->ops->mkdir) {
        return VFS_ERR_INVALID_PARAM;
    }
    return dir->ops->mkdir(dir, name, out_node);
}

int vfs_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops || !dir->ops->unlink) {
        return VFS_ERR_INVALID_PARAM;
    }
    return dir->ops->unlink(dir, name);
}

int vfs_rmdir(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops || !dir->ops->rmdir) {
        return VFS_ERR_INVALID_PARAM;
    }
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
    return src_dir->ops->rename(src_dir, src_name, dst_dir, dst_name);
}

u64 vfs_read(vfs_node_t *node, u64 offset, u64 size, void *buffer) {
    if (!node || !buffer || !node->ops || !node->ops->read) {
        return 0;
    }
    return node->ops->read(node, offset, size, buffer);
}

u64 vfs_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer) {
    if (!node || !buffer || !node->ops || !node->ops->write) {
        return 0;
    }
    return node->ops->write(node, offset, size, buffer);
}

vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops || !dir->ops->finddir) {
        return NULL;
    }
    return dir->ops->finddir(dir, name);
}

bool vfs_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    if (!dir || !out_entry || dir->type != VFS_TYPE_DIRECTORY || !dir->ops || !dir->ops->readdir) {
        return false;
    }
    return dir->ops->readdir(dir, index, out_entry);
}
