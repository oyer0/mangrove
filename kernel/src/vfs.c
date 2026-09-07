/* SPDX-License-Identifier: GPL-3.0-or-later */
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
static vfs_super_t dead_super;

static vfs_mount_t *vfs_find_mount_child(vfs_node_t *parent,
                                         const char *name);

static void vfs_retarget_nodes(vfs_super_t *sb)
{
    vfs_node_t *node;

    if (!sb) return;
    node = sb->nodes;
    while (node) {
        vfs_node_t *next = node->next_in_super;
        node->super = &dead_super;
        node->next_in_super = NULL;
        node = next;
    }
    sb->nodes = NULL;
}

static void vfs_super_dispose(vfs_super_t *sb)
{
    if (!sb || sb == &dead_super) return;
    sb->state = VFS_SUPER_DEAD;
    /* Nodes returned by lookup are intentionally long-lived in the current
     * VFS.  Retarget them before releasing driver-private state so a stale
     * node can only observe the inert dead-super sentinel. */
    vfs_retarget_nodes(sb);
    if (sb->ops && sb->ops->unmount)
        (void)sb->ops->unmount(sb);
    kfree(sb);
}

bool vfs_super_is_live(const vfs_super_t *sb)
{
    if (!sb || sb->state != VFS_SUPER_ACTIVE) return false;
    if (sb->dev && !block_device_id_is_live(sb->device_id)) return false;
    return true;
}

bool vfs_node_is_live(const vfs_node_t *node)
{
    return node && vfs_super_is_live(node->super);
}

bool vfs_super_retain(vfs_super_t *sb)
{
    if (!vfs_super_is_live(sb) || sb->reference_count == ~(u32)0)
        return false;
    sb->reference_count++;
    return true;
}

void vfs_super_release(vfs_super_t *sb)
{
    if (!sb || sb == &dead_super || !sb->reference_count) return;
    sb->reference_count--;
    if (!sb->reference_count && sb->state != VFS_SUPER_ACTIVE)
        vfs_super_dispose(sb);
}

void vfs_node_register(vfs_node_t *node)
{
    vfs_super_t *sb;

    if (!node || !node->super) return;
    sb = node->super;
    node->next_in_super = sb->nodes;
    sb->nodes = node;
}

static bool vfs_node_on_read_only_mount(const vfs_node_t *node)
{
    if (!node || !node->super) return false;
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].sb == node->super &&
            mount_table[i].read_only) return true;
    }
    return false;
}

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
    memset(&dead_super, 0, sizeof(dead_super));
    dead_super.state = VFS_SUPER_DEAD;
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        memset(&mount_table[i], 0, sizeof(mount_table[i]));
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

const char *vfs_probe_filesystem(block_device_t *dev)
{
    static const char *supported[] = { "mgfs", "fat32", "exfat" };

    if (!dev || !block_device_is_live(dev)) return NULL;
    for (usize index = 0; index < sizeof(supported) / sizeof(supported[0]);
         index++) {
        vfs_fs_type_t *type = vfs_find_fs(supported[index]);
        if (type && type->probe && type->probe(dev) && type->name)
            return type->name;
    }
    return NULL;
}

bool vfs_filesystem_label(block_device_t *dev, const char *fs_name,
                          char *out, usize capacity)
{
    vfs_fs_type_t *type;

    if (!out || capacity < 2U) return false;
    out[0] = '\0';
    if (!dev || !fs_name || !block_device_is_live(dev)) return false;
    type = vfs_find_fs(fs_name);
    if (!type || !type->label) return false;
    return type->label(dev, out, capacity) && out[0] != '\0';
}

int vfs_mount_root(const char *fs_name, block_device_t *dev) {
    if (!fs_name) {
        return VFS_ERR_INVALID_PARAM;
    }

    if (mount_table[0].active) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (dev && !block_device_is_live(dev)) return VFS_ERR_DEVICE_GONE;

    vfs_fs_type_t *fs_type = vfs_find_fs(fs_name);
    if (!fs_type || !fs_type->mount) {
        return VFS_ERR_NOT_FOUND;
    }

    vfs_super_t *sb = NULL;
    int res = fs_type->mount(fs_type, dev, &sb, false);
    if (res != VFS_OK || !sb || !sb->root_node) {
        return (res != VFS_OK) ? res : VFS_ERR_BAD_FORMAT;
    }

    sb->device_id = dev ? dev->id : 0ULL;
    sb->reference_count = 0;
    sb->state = VFS_SUPER_ACTIVE;
    sb->nodes = NULL;
    vfs_node_register(sb->root_node);

    mount_table[0].sb = sb;
    mount_table[0].covered_node = NULL;
    mount_table[0].covered_super = NULL;
    mount_table[0].covered_inode = 0;
    mount_table[0].parent_super = NULL;
    mount_table[0].parent_inode = 0;
    mount_table[0].dev = dev;
    mount_table[0].dev_id = dev ? dev->id : 0ULL;
    mount_table[0].role = VFS_MOUNT_ROLE_ROOT;
    mount_table[0].read_only = false;
    strcpy(mount_table[0].mount_point, "/");
    mount_table[0].name[0] = '\0';
    mount_table[0].active = true;

    return VFS_OK;
}

static int vfs_mount_slot(void) {
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
    return slot;
}

static int vfs_mount_instance(vfs_node_t *parent, const char *name,
                              vfs_node_t *target_node,
                              const char *mount_point, const char *fs_name,
                              block_device_t *dev, vfs_mount_role_t role,
                              bool read_only) {
    int slot;
    vfs_fs_type_t *fs_type;
    vfs_super_t *sb = NULL;
    int res;

    if (!fs_name || !mount_point || mount_point[0] != '/' ||
        strlen(mount_point) >= VFS_MOUNT_PATH_MAX ||
        (parent && (!name || !*name || strlen(name) >= VFS_MOUNT_PATH_MAX)) ||
        (!parent && !target_node)) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (dev && !block_device_is_live(dev)) return VFS_ERR_DEVICE_GONE;
    if (target_node && target_node->type != VFS_TYPE_DIRECTORY) {
        return VFS_ERR_NOT_DIRECTORY;
    }

    slot = vfs_mount_slot();
    if (slot < 0) return slot;

    fs_type = vfs_find_fs(fs_name);
    if (!fs_type || !fs_type->mount) return VFS_ERR_NOT_FOUND;

    res = fs_type->mount(fs_type, dev, &sb, read_only);
    if (res != VFS_OK || !sb || !sb->root_node) {
        if (res != VFS_OK) return res;
        return VFS_ERR_BAD_FORMAT;
    }

    sb->device_id = dev ? dev->id : 0ULL;
    sb->reference_count = 0;
    sb->state = VFS_SUPER_ACTIVE;
    sb->nodes = NULL;
    vfs_node_register(sb->root_node);

    memset(&mount_table[slot], 0, sizeof(mount_table[slot]));
    mount_table[slot].sb = sb;
    mount_table[slot].covered_node = target_node;
    if (target_node) {
        mount_table[slot].covered_super = target_node->super;
        mount_table[slot].covered_inode = target_node->inode;
    }
    if (parent) {
        mount_table[slot].parent_super = parent->super;
        mount_table[slot].parent_inode = parent->inode;
        strncpy(mount_table[slot].name, name,
                sizeof(mount_table[slot].name) - 1U);
    }
    mount_table[slot].dev = dev;
    mount_table[slot].dev_id = dev ? dev->id : 0ULL;
    mount_table[slot].role = role;
    mount_table[slot].read_only = read_only;
    strncpy(mount_table[slot].mount_point, mount_point,
            sizeof(mount_table[slot].mount_point) - 1U);
    mount_table[slot].active = true;
    KERNEL_BOOT_DEBUG_LOG(
        "[VFS] mount slot=%u role=%u path=%s fs=%s dev=%llu parent=%llu "
        "covered=%llu virtual=%u\n",
        (u32)slot, (u32)role, mount_table[slot].mount_point, fs_name,
        dev ? dev->id : 0ULL,
        parent ? parent->inode : 0ULL,
        target_node ? target_node->inode : 0ULL,
        target_node ? 0U : 1U);
    return VFS_OK;
}

int vfs_mount_node(vfs_node_t *target_node, const char *fs_name, block_device_t *dev) {
    if (!vfs_node_is_live(target_node) ||
        target_node->type != VFS_TYPE_DIRECTORY || !fs_name) {
        return VFS_ERR_INVALID_PARAM;
    }

    if (vfs_find_mount_for_node(target_node) != NULL) {
        return VFS_ERR_INVALID_PARAM;
    }

    return vfs_mount_instance(NULL, NULL, target_node, "/", fs_name,
                              dev, VFS_MOUNT_ROLE_VOLUME, false);
}

int vfs_mount_path(vfs_node_t *parent, const char *name,
                   const char *mount_point, const char *fs_name,
                   block_device_t *dev, vfs_mount_role_t role,
                   bool read_only) {
    vfs_node_t *target_node;

    if (!vfs_node_is_live(parent) ||
        parent->type != VFS_TYPE_DIRECTORY || !name || !*name ||
        !mount_point || mount_point[0] != '/' || !fs_name) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_find_mount_child(parent, name) != NULL) {
        return VFS_ERR_INVALID_PARAM;
    }

    target_node = parent->ops && parent->ops->finddir
        ? parent->ops->finddir(parent, name) : NULL;
    if (target_node && target_node->type != VFS_TYPE_DIRECTORY) {
        return VFS_ERR_NOT_DIRECTORY;
    }

    return vfs_mount_instance(parent, name, target_node, mount_point,
                              fs_name, dev, role, read_only);
}

static void vfs_mount_detach(vfs_mount_t *mount)
{
    if (!mount) return;
    mount->sb = NULL;
    mount->covered_node = NULL;
    mount->covered_super = NULL;
    mount->parent_super = NULL;
    mount->parent_inode = 0;
    mount->dev = NULL;
    mount->dev_id = 0;
    mount->mount_point[0] = '\0';
    mount->name[0] = '\0';
    mount->active = false;
}

static int vfs_unmount_preflight_super(vfs_super_t *sb)
{
    if (!sb) return VFS_ERR_INVALID_PARAM;
    if (sb->state != VFS_SUPER_ACTIVE) return VFS_ERR_DEVICE_GONE;
    if (sb->reference_count) return VFS_ERR_BUSY;
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].sb == sb &&
            process_any_cwd_under_path(mount_table[i].mount_point))
            return VFS_ERR_BUSY;
    }
    return VFS_OK;
}

static vfs_super_t *vfs_mounted_super_for_device(block_device_t *device)
{
    if (!device) return NULL;
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].dev_id == device->id)
            return mount_table[i].sb;
    }
    return NULL;
}

int vfs_unmount_device_preflight(block_device_t *device)
{
    vfs_super_t *sb;

    if (!device) return VFS_ERR_INVALID_PARAM;
    sb = vfs_mounted_super_for_device(device);
    if (!sb) return VFS_ERR_NOT_FOUND;
    return vfs_unmount_preflight_super(sb);
}

int vfs_format_preflight(block_device_t *device)
{
    if (!device || !block_device_is_live(device)) return VFS_ERR_DEVICE_GONE;
    /* A mounted superblock is the authoritative indication that filesystem
     * objects may still issue I/O.  Formatting deliberately does not
     * unmount or invalidate it on the caller's behalf. */
    if (vfs_mounted_super_for_device(device)) return VFS_ERR_BUSY;
    return VFS_OK;
}

int vfs_unmount_devices_preflight(block_device_t *devices[], u32 count)
{
    vfs_super_t *supers[VFS_MAX_MOUNTS];
    u32 super_count = 0;

    if (!devices || count == 0 || count > VFS_MAX_MOUNTS)
        return VFS_ERR_INVALID_PARAM;
    for (u32 index = 0; index < count; index++) {
        vfs_super_t *sb = vfs_mounted_super_for_device(devices[index]);
        bool duplicate = false;
        if (!sb) continue;
        for (u32 prior = 0; prior < super_count; prior++)
            if (supers[prior] == sb) duplicate = true;
        if (duplicate) continue;
        if (super_count >= VFS_MAX_MOUNTS) return VFS_ERR_NO_MEM;
        supers[super_count++] = sb;
    }
    if (!super_count) return VFS_ERR_NOT_FOUND;
    for (u32 index = 0; index < super_count; index++) {
        int result = vfs_unmount_preflight_super(supers[index]);
        if (result != VFS_OK) return result;
    }
    return VFS_OK;
}

int vfs_unmount_devices(block_device_t *devices[], u32 count)
{
    vfs_super_t *supers[VFS_MAX_MOUNTS];
    u32 super_count = 0;
    int result;

    if (!devices || count == 0 || count > VFS_MAX_MOUNTS)
        return VFS_ERR_INVALID_PARAM;
    for (u32 index = 0; index < count; index++) {
        vfs_super_t *sb = vfs_mounted_super_for_device(devices[index]);
        bool duplicate = false;
        if (!sb) continue;
        for (u32 prior = 0; prior < super_count; prior++)
            if (supers[prior] == sb) duplicate = true;
        if (duplicate) continue;
        if (super_count >= VFS_MAX_MOUNTS) return VFS_ERR_NO_MEM;
        supers[super_count++] = sb;
    }
    if (!super_count) return VFS_ERR_NOT_FOUND;

    /* Preflight every child before changing any mount state.  This makes a
     * busy child fail an eject without partially detaching an earlier child. */
    for (u32 index = 0; index < super_count; index++) {
        result = vfs_unmount_preflight_super(supers[index]);
        if (result != VFS_OK) return result;
    }
    for (u32 index = 0; index < super_count; index++)
        supers[index]->state = VFS_SUPER_DETACHING;
    for (u32 index = 0; index < super_count; index++) {
        if (!supers[index]->ops || !supers[index]->ops->sync) continue;
        result = supers[index]->ops->sync(supers[index]);
        if (result != VFS_OK) {
            for (u32 restore = 0; restore < super_count; restore++)
                if (supers[restore]->state == VFS_SUPER_DETACHING)
                    supers[restore]->state = VFS_SUPER_ACTIVE;
            return result;
        }
    }
    for (u32 index = 0; index < VFS_MAX_MOUNTS; index++) {
        vfs_mount_t *mount = &mount_table[index];
        bool selected = false;
        if (!mount->active) continue;
        for (u32 super = 0; super < super_count; super++)
            if (mount->sb == supers[super]) selected = true;
        if (selected) vfs_mount_detach(mount);
    }
    for (u32 index = 0; index < super_count; index++)
        vfs_super_dispose(supers[index]);
    return VFS_OK;
}

int vfs_unmount_sb(vfs_super_t *sb)
{
    block_device_t *device = NULL;

    if (!sb) return VFS_ERR_INVALID_PARAM;
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mount_table[i].active && mount_table[i].sb == sb)
            device = mount_table[i].dev;
    if (!device) return VFS_ERR_NOT_FOUND;
    return vfs_unmount_device(device);
}

int vfs_unmount_device(block_device_t *device)
{
    block_device_t *devices[1];

    if (!device) return VFS_ERR_INVALID_PARAM;
    devices[0] = device;
    return vfs_unmount_devices(devices, 1);
}

void vfs_device_removed(block_device_t *device)
{
    if (!device) return;
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        vfs_mount_t *mount = &mount_table[i];
        vfs_super_t *sb;
        if (!mount->active || mount->dev_id != device->id) continue;
        sb = mount->sb;
        if (!sb) {
            vfs_mount_detach(mount);
            continue;
        }
        /* Physical loss is not a clean unmount: no flush is attempted and
         * dirty data is considered lost.  Detach the namespace immediately,
         * retaining the superblock while open objects unwind. */
        sb->state = VFS_SUPER_DEAD;
        vfs_mount_detach(mount);
        if (!sb->reference_count) vfs_super_dispose(sb);
    }
}

vfs_node_t *vfs_get_root_node(void) {
    if (mount_table[0].active && mount_table[0].sb &&
        vfs_super_is_live(mount_table[0].sb)) {
        return mount_table[0].sb->root_node;
    }
    return NULL;
}

vfs_mount_t *vfs_find_mount_for_node(vfs_node_t *node) {
    if (!node) {
        return NULL;
    }

    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].covered_node &&
            ((mount_table[i].covered_node == node) ||
             (mount_table[i].covered_super == node->super &&
              mount_table[i].covered_inode == node->inode))) {
            return &mount_table[i];
        }
    }

    return NULL;
}

bool vfs_mount_point_for_device(block_device_t *device, char *out_path,
                                usize capacity, vfs_mount_role_t *out_role)
{
    if (!device || !out_path || capacity < 2U) return false;
    for (u32 index = 0; index < VFS_MAX_MOUNTS; index++) {
        if (!mount_table[index].active || mount_table[index].dev_id != device->id)
            continue;
        strncpy(out_path, mount_table[index].mount_point, capacity - 1U);
        out_path[capacity - 1U] = '\0';
        if (out_role) *out_role = mount_table[index].role;
        return true;
    }
    return false;
}

static bool vfs_mount_parent_matches(const vfs_mount_t *mount,
                                     const vfs_node_t *parent)
{
    return mount && mount->active && parent && mount->parent_super == parent->super &&
           mount->parent_inode == parent->inode;
}

bool vfs_directory_has_mount_children(vfs_node_t *dir)
{
    if (!dir || dir->type != VFS_TYPE_DIRECTORY) return false;
    for (u32 i = 1; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mount_parent_matches(&mount_table[i], dir)) return true;
    }
    return false;
}

static vfs_mount_t *vfs_find_mount_child(vfs_node_t *parent, const char *name)
{
    if (!parent || !name) return NULL;
    for (u32 i = 1; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mount_parent_matches(&mount_table[i], parent) &&
            strcmp(mount_table[i].name, name) == 0) {
            return &mount_table[i];
        }
    }
    return NULL;
}

static vfs_node_t *vfs_resolve_mount(vfs_node_t *node) {
    if (!vfs_node_is_live(node)) return NULL;
    vfs_mount_t *m = vfs_find_mount_for_node(node);
    if (m && m->active && m->sb && m->sb->root_node) {
        return m->sb->root_node;
    }
    return node;
}

static vfs_node_t *vfs_finddir_internal(vfs_node_t *dir, const char *name,
                                        bool enforce) {
    vfs_mount_t *mount;

    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->finddir || !vfs_node_is_live(dir) || (enforce &&
        !vfs_check_access(dir, VFS_ACCESS_READ))) {
        return NULL;
    }
    mount = vfs_find_mount_child(dir, name);
    if (mount && mount->sb && mount->sb->root_node) {
        return mount->sb->root_node;
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
        if (!curr) return VFS_ERR_DEVICE_GONE;

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
        if (!curr) return VFS_ERR_DEVICE_GONE;
    }

    /* Looking up a protected object is itself a read operation.  This keeps
       private files and directories from being discoverable by pathname. */
    if (!vfs_node_is_live(curr)) return VFS_ERR_DEVICE_GONE;
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

static int vfs_open_node_internal(vfs_node_t *node, u32 flags,
                                  vfs_file_handle_t **out_handle,
                                  bool authorized_write) {
    vfs_file_handle_t *handle;

    if (!node || !out_handle || !vfs_node_is_live(node) ||
        (flags != VFS_OPEN_READ && flags != VFS_OPEN_WRITE && flags != VFS_OPEN_RDWR)) {
        return VFS_ERR_INVALID_PARAM;
    }

    *out_handle = NULL;
    if ((flags & VFS_OPEN_WRITE) && vfs_node_on_read_only_mount(node)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    if (((flags & VFS_OPEN_READ) &&
         !vfs_check_access(node, VFS_ACCESS_READ)) ||
        ((flags & VFS_OPEN_WRITE) && !authorized_write &&
         !vfs_check_access(node, VFS_ACCESS_WRITE))) {
        return VFS_ERR_ACCESS_DENIED;
    }

    handle = (vfs_file_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) {
        return VFS_ERR_NO_MEM;
    }

    handle->node = node;
    handle->super = node->super;
    handle->offset = 0;
    handle->flags = flags;
    handle->valid = VFS_FILE_HANDLE_VALID;
    handle->authorized_write = authorized_write &&
                               (flags & VFS_OPEN_WRITE) != 0U;
    if (!vfs_super_retain(handle->super)) {
        kfree(handle);
        return VFS_ERR_DEVICE_GONE;
    }
    *out_handle = handle;
    return VFS_OK;
}

int vfs_open_node(vfs_node_t *node, u32 flags,
                  vfs_file_handle_t **out_handle) {
    return vfs_open_node_internal(node, flags, out_handle, false);
}

int vfs_open_node_authorized(vfs_node_t *node, u32 flags,
                             vfs_file_handle_t **out_handle) {
    return vfs_open_node_internal(node, flags, out_handle, true);
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
    handle->super = node->super;
    handle->offset = 0;
    handle->flags = flags;
    handle->valid = VFS_FILE_HANDLE_VALID;
    handle->authorized_write = false;
    if (!vfs_super_retain(handle->super)) {
        kfree(handle);
        return VFS_ERR_DEVICE_GONE;
    }
    *out_handle = handle;
    return VFS_OK;
}

int vfs_close(vfs_file_handle_t *handle) {
    if (!handle || handle->valid != VFS_FILE_HANDLE_VALID || !handle->node) {
        return VFS_ERR_INVALID_PARAM;
    }

    vfs_super_t *super = handle->super;
    handle->valid = 0;
    handle->node = NULL;
    handle->super = NULL;
    vfs_super_release(super);
    kfree(handle);
    return VFS_OK;
}

static bool vfs_handle_valid(const vfs_file_handle_t *handle) {
    return handle && handle->valid == VFS_FILE_HANDLE_VALID && handle->node &&
           vfs_super_is_live(handle->super);
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
    if ((!handle->authorized_write &&
         !vfs_check_access(handle->node, VFS_ACCESS_WRITE)) ||
        vfs_node_on_read_only_mount(handle->node)) return 0;
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

    if (!handle || handle->valid != VFS_FILE_HANDLE_VALID || !handle->node ||
        !handle->super) return VFS_ERR_INVALID_PARAM;
    if (!vfs_super_is_live(handle->super)) return VFS_ERR_DEVICE_GONE;
    if ((whence != VFS_SEEK_SET && whence != VFS_SEEK_CUR &&
         whence != VFS_SEEK_END)) {
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
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->ops ||
        !dir->ops->create) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir) ||
        !vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return dir->ops->create(dir, name, out_node);
}

int vfs_create_owned(vfs_node_t *dir, const char *name, u32 owner_uid,
                     u32 permissions, vfs_node_t **out_node) {
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !out_node ||
        !dir->ops || !dir->ops->create_owned ||
        (permissions & ~VFS_PERMISSION_KNOWN) != 0U || !permissions) {
        return VFS_ERR_INVALID_PARAM;
    }
    return dir->ops->create_owned(dir, name, owner_uid, permissions, out_node);
}

int vfs_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->ops ||
        !dir->ops->mkdir) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir) ||
        !vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return dir->ops->mkdir(dir, name, out_node);
}

int vfs_mkdir_owned(vfs_node_t *dir, const char *name, u32 owner_uid,
                    u32 permissions, vfs_node_t **out_node) {
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !out_node ||
        !dir->ops || !dir->ops->mkdir_owned ||
        (permissions & ~VFS_PERMISSION_KNOWN) != 0U || !permissions) {
        return VFS_ERR_INVALID_PARAM;
    }
    return dir->ops->mkdir_owned(dir, name, owner_uid, permissions, out_node);
}

int vfs_unlink(vfs_node_t *dir, const char *name) {
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->unlink) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir) ||
        !vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return dir->ops->unlink(dir, name);
}

int vfs_unlink_trusted(vfs_node_t *dir, const char *name) {
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->unlink) return VFS_ERR_INVALID_PARAM;
    return dir->ops->unlink(dir, name);
}

int vfs_rmdir(vfs_node_t *dir, const char *name) {
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->rmdir) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir) ||
        !vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return dir->ops->rmdir(dir, name);
}

int vfs_rmdir_trusted(vfs_node_t *dir, const char *name) {
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->rmdir) return VFS_ERR_INVALID_PARAM;
    return dir->ops->rmdir(dir, name);
}

int vfs_rename(vfs_node_t *src_dir, const char *src_name,
               vfs_node_t *dst_dir, const char *dst_name) {
    if (!vfs_node_is_live(src_dir) || !src_name ||
        !vfs_node_is_live(dst_dir) || !dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY ||
        !src_dir->ops || !src_dir->ops->rename) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(src_dir) ||
        vfs_node_on_read_only_mount(dst_dir) ||
        !vfs_check_access(src_dir, VFS_ACCESS_WRITE) ||
        !vfs_check_access(dst_dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return src_dir->ops->rename(src_dir, src_name, dst_dir, dst_name);
}

int vfs_rename_trusted(vfs_node_t *src_dir, const char *src_name,
                       vfs_node_t *dst_dir, const char *dst_name) {
    if (!vfs_node_is_live(src_dir) || !src_name ||
        !vfs_node_is_live(dst_dir) || !dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY || !src_dir->ops ||
        !src_dir->ops->rename) return VFS_ERR_INVALID_PARAM;
    return src_dir->ops->rename(src_dir, src_name, dst_dir, dst_name);
}

int vfs_truncate_handle(vfs_file_handle_t *handle) {
    vfs_node_t *node;

    if (!handle || handle->valid != VFS_FILE_HANDLE_VALID || !handle->node ||
        !handle->super) return VFS_ERR_INVALID_PARAM;
    if (!vfs_super_is_live(handle->super)) return VFS_ERR_DEVICE_GONE;
    if (!(handle->flags & VFS_OPEN_WRITE)) return VFS_ERR_INVALID_PARAM;
    node = handle->node;
    if (node->type != VFS_TYPE_FILE) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!node->ops || !node->ops->truncate) return VFS_ERR_UNSUPPORTED;
    if (vfs_node_on_read_only_mount(node) ||
        (!handle->authorized_write &&
         !vfs_check_access(node, VFS_ACCESS_WRITE))) {
        return VFS_ERR_ACCESS_DENIED;
    }
    return node->ops->truncate(node);
}

int vfs_truncate(vfs_node_t *node) {
    vfs_file_handle_t handle;

    if (!vfs_node_is_live(node)) return VFS_ERR_DEVICE_GONE;
    memset(&handle, 0, sizeof(handle));
    handle.node = node;
    handle.super = node->super;
    handle.flags = VFS_OPEN_WRITE;
    handle.valid = VFS_FILE_HANDLE_VALID;
    return vfs_truncate_handle(&handle);
}

int vfs_truncate_trusted(vfs_node_t *node) {
    if (!vfs_node_is_live(node) || node->type != VFS_TYPE_FILE || !node->ops ||
        !node->ops->truncate) return VFS_ERR_INVALID_PARAM;
    return node->ops->truncate(node);
}

u64 vfs_read(vfs_node_t *node, u64 offset, u64 size, void *buffer) {
    if (!vfs_node_is_live(node) || !buffer || !node->ops || !node->ops->read) {
        return 0;
    }
    if (!vfs_check_access(node, VFS_ACCESS_READ)) return 0;
    return node->ops->read(node, offset, size, buffer);
}

u64 vfs_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer) {
    if (!vfs_node_is_live(node) || !buffer || !node->ops || !node->ops->write) {
        return 0;
    }
    if (vfs_node_on_read_only_mount(node) ||
        !vfs_check_access(node, VFS_ACCESS_WRITE)) return 0;
    return node->ops->write(node, offset, size, buffer);
}

vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name) {
    return vfs_finddir_internal(dir, name, true);
}

vfs_node_t *vfs_finddir_trusted(vfs_node_t *dir, const char *name) {
    return vfs_finddir_internal(dir, name, false);
}

bool vfs_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    u32 base_count;
    u32 mount_index;

    if (!vfs_node_is_live(dir) || !out_entry ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->readdir) {
        return false;
    }
    if (!vfs_check_access(dir, VFS_ACCESS_READ)) return false;

    if (dir->ops->readdir(dir, index, out_entry)) return true;
    base_count = 0;
    while (dir->ops->readdir(dir, base_count, out_entry)) base_count++;
    if (index < base_count) return false;

    mount_index = 0;
    for (u32 i = 1; i < VFS_MAX_MOUNTS; i++) {
        vfs_mount_t *mount = &mount_table[i];
        bool already_present = false;
        if (!vfs_mount_parent_matches(mount, dir)) continue;
        for (u32 base_index = 0; base_index < base_count; base_index++) {
            vfs_dirent_t base_entry;
            if (!dir->ops->readdir(dir, base_index, &base_entry)) break;
            if (strcmp(base_entry.name, mount->name) == 0) {
                already_present = true;
                break;
            }
        }
        if (already_present) continue;
        if (mount_index == index - base_count) {
            strncpy(out_entry->name, mount->name, sizeof(out_entry->name) - 1U);
            out_entry->name[sizeof(out_entry->name) - 1U] = '\0';
            out_entry->inode = mount->sb && mount->sb->root_node
                ? mount->sb->root_node->inode : 0;
            out_entry->type = VFS_TYPE_DIRECTORY;
            return true;
        }
        mount_index++;
    }
    return false;
}

bool vfs_readdir_trusted(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    u32 base_count;
    u32 mount_index;

    if (!vfs_node_is_live(dir) || !out_entry ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->readdir) return false;
    if (dir->ops->readdir(dir, index, out_entry)) return true;
    base_count = 0;
    while (dir->ops->readdir(dir, base_count, out_entry)) base_count++;
    if (index < base_count) return false;

    mount_index = 0;
    for (u32 i = 1; i < VFS_MAX_MOUNTS; i++) {
        vfs_mount_t *mount = &mount_table[i];
        bool already_present = false;
        if (!vfs_mount_parent_matches(mount, dir)) continue;
        for (u32 base_index = 0; base_index < base_count; base_index++) {
            vfs_dirent_t base_entry;
            if (!dir->ops->readdir(dir, base_index, &base_entry)) break;
            if (strcmp(base_entry.name, mount->name) == 0) {
                already_present = true;
                break;
            }
        }
        if (already_present) continue;
        if (mount_index == index - base_count) {
            strncpy(out_entry->name, mount->name, sizeof(out_entry->name) - 1U);
            out_entry->name[sizeof(out_entry->name) - 1U] = '\0';
            out_entry->inode = mount->sb && mount->sb->root_node
                ? mount->sb->root_node->inode : 0;
            out_entry->type = VFS_TYPE_DIRECTORY;
            return true;
        }
        mount_index++;
    }
    return false;
}
