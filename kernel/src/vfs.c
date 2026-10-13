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
static spinlock_t vfs_metadata_lock;
static mutex_t vfs_mount_lock;

typedef struct vfs_recovery_pin {
    struct vfs_recovery_pin *next;
    vfs_super_t *sb;
    u64 id;
    u64 device_id;
    vfs_recovery_pin_state_t state;
    bool finishing;
    vfs_recovery_handoff_t handoff;
    void *handoff_context;
} vfs_recovery_pin_t;

/* Protected by vfs_metadata_lock. A listed pin owns one superblock reference;
 * detaching the namespace does not remove it. */
static vfs_recovery_pin_t *recovery_pins;
static u32 recovery_pin_count;
static u64 next_recovery_pin_id;

static vfs_mount_t *vfs_find_mount_child(vfs_node_t *parent,
                                         const char *name);
static vfs_mount_t *vfs_find_mount_child_locked(vfs_node_t *parent,
                                                const char *name);
static vfs_mount_t *vfs_find_mount_for_node_locked(vfs_node_t *node);

/* Mount admission is serialized by vfs_mount_lock.  This metadata-locked
 * lookup uses the immutable registration ID, which is never reused for a
 * different block-device instance. */
static bool vfs_device_already_mounted(block_device_t *dev)
{
    bool mounted = false;
    u64 flags;

    if (!dev) return false;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].dev_id == dev->id) {
            mounted = true;
            break;
        }
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return mounted;
}

static void vfs_retarget_nodes_locked(vfs_super_t *sb)
{
    vfs_node_t *node;

    if (!sb) return;
    node = sb->nodes;
    while (node) {
        vfs_node_t *next = node->next_in_super;
        __atomic_store_n(&node->super, &dead_super, __ATOMIC_RELEASE);
        node->next_in_super = NULL;
        node = next;
    }
    sb->nodes = NULL;
}

static bool vfs_super_is_live_locked(const vfs_super_t *sb)
{
    return sb && sb != &dead_super &&
           sb->state == VFS_SUPER_ACTIVE &&
           (!sb->dev || block_device_id_is_live(sb->device_id));
}

static bool vfs_super_mark_dispose_locked(vfs_super_t *sb)
{
    if (!sb || sb == &dead_super || sb->reference_count ||
        sb->state == VFS_SUPER_ACTIVE || sb->dispose_pending) return false;
    sb->dispose_pending = true;
    sb->state = VFS_SUPER_DEAD;
    /* Nodes returned by lookup are intentionally long-lived in the current
     * VFS.  Retarget them before releasing driver-private state so a stale
     * node can only observe the inert dead-super sentinel. */
    vfs_retarget_nodes_locked(sb);
    return true;
}

static void vfs_super_destroy(vfs_super_t *sb)
{
    if (!sb || sb == &dead_super) return;
    if (sb->ops && sb->ops->unmount)
        (void)sb->ops->unmount(sb);
    kfree(sb);
}

static void vfs_dispose_unpublished(vfs_super_t *sb)
{
    u64 flags;

    if (!sb || sb == &dead_super) return;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    sb->state = VFS_SUPER_DEAD;
    sb->dispose_pending = true;
    vfs_retarget_nodes_locked(sb);
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    vfs_super_destroy(sb);
}

static void vfs_super_dispose(vfs_super_t *sb)
{
    bool dispose;
    u64 flags;

    if (!sb || sb == &dead_super) return;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    dispose = vfs_super_mark_dispose_locked(sb);
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    if (dispose) vfs_super_destroy(sb);
}

bool vfs_super_is_live(const vfs_super_t *sb)
{
    vfs_super_state_t state;

    if (!sb || sb == &dead_super) return false;
    state = __atomic_load_n(&sb->state, __ATOMIC_ACQUIRE);
    if (state != VFS_SUPER_ACTIVE) return false;
    if (sb->dev && !block_device_id_is_live(sb->device_id)) return false;
    return true;
}

bool vfs_node_is_live(const vfs_node_t *node)
{
    vfs_super_t *sb;

    if (!node) return false;
    sb = __atomic_load_n(&node->super, __ATOMIC_ACQUIRE);
    return vfs_super_is_live(sb);
}

bool vfs_super_retain(vfs_super_t *sb)
{
    u64 flags;
    bool retained = false;

    if (!sb || sb == &dead_super) return false;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    if (vfs_super_is_live_locked(sb) &&
        sb->reference_count != ~(u32)0) {
        sb->reference_count++;
        retained = true;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return retained;
}

void vfs_super_release(vfs_super_t *sb)
{
    bool dispose = false;
    u64 flags;

    if (!sb || sb == &dead_super) return;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    if (sb->reference_count) {
        sb->reference_count--;
        if (!sb->reference_count)
            dispose = vfs_super_mark_dispose_locked(sb);
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    if (dispose) vfs_super_destroy(sb);
}

static int vfs_recovery_pin_begin_internal(vfs_super_t *sb,
                                           vfs_recovery_handoff_t handoff,
                                           void *context, u64 *out_id)
{
    vfs_recovery_pin_t *pin;
    u64 flags;
    int result = VFS_ERR_DEVICE_GONE;

    if (!sb || !out_id || sb == &dead_super) return VFS_ERR_INVALID_PARAM;
    *out_id = 0;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    bool full = recovery_pin_count == VFS_MAX_RECOVERY_PINS;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    if (full) return VFS_ERR_NO_SPACE;
    pin = (vfs_recovery_pin_t *)kmalloc(sizeof(*pin));
    if (!pin) return VFS_ERR_NO_MEM;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    if (vfs_super_is_live_locked(sb)) {
        bool mounted = false;
        for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
            if (mount_table[i].active && mount_table[i].sb == sb) {
                mounted = true;
                break;
            }
        }
        if (mounted && recovery_pin_count < VFS_MAX_RECOVERY_PINS &&
            sb->reference_count != ~(u32)0 &&
            next_recovery_pin_id != 0) {
            pin->sb = sb;
            pin->id = next_recovery_pin_id++;
            pin->device_id = sb->device_id;
            pin->state = VFS_RECOVERY_PIN_ACTIVE;
            pin->finishing = false;
            pin->handoff = handoff;
            pin->handoff_context = context;
            pin->next = recovery_pins;
            recovery_pins = pin;
            recovery_pin_count++;
            sb->reference_count++;
            *out_id = pin->id;
            result = VFS_OK;
        } else if (mounted) {
            result = VFS_ERR_NO_SPACE;
        }
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    if (result != VFS_OK) kfree(pin);
    return result;
}

int vfs_recovery_pin_begin(vfs_super_t *sb, u64 *out_id)
{
    return vfs_recovery_pin_begin_internal(sb, NULL, NULL, out_id);
}

int vfs_recovery_pin_begin_handoff(vfs_super_t *sb,
                                   vfs_recovery_handoff_t handoff,
                                   void *context, u64 *out_id)
{
    if (!handoff) return VFS_ERR_INVALID_PARAM;
    return vfs_recovery_pin_begin_internal(sb, handoff, context, out_id);
}

int vfs_recovery_pin_mark_unresolved(u64 id)
{
    u64 flags;
    int result = VFS_ERR_NOT_FOUND;

    if (!id) return VFS_ERR_INVALID_PARAM;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (vfs_recovery_pin_t *pin = recovery_pins; pin; pin = pin->next) {
        if (pin->id != id) continue;
        result = pin->state == VFS_RECOVERY_PIN_ACTIVE && !pin->finishing ?
            VFS_OK : VFS_ERR_BUSY;
        if (result == VFS_OK) pin->state = VFS_RECOVERY_PIN_UNRESOLVED;
        break;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return result;
}

int vfs_recovery_pin_finish(u64 id)
{
    vfs_recovery_pin_t **link;
    vfs_recovery_pin_t *pin;
    u64 flags;

    if (!id) return VFS_ERR_INVALID_PARAM;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    link = &recovery_pins;
    while (*link && (*link)->id != id) link = &(*link)->next;
    pin = *link;
    if (!pin || pin->state != VFS_RECOVERY_PIN_ACTIVE || pin->finishing ||
        pin->handoff) {
        int result = !pin ? VFS_ERR_NOT_FOUND :
            pin->state == VFS_RECOVERY_PIN_UNRESOLVED ?
            VFS_ERR_UNCERTAIN : VFS_ERR_BUSY;
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        return result;
    }
    /* Reserve this pin while checking the operation mutex outside the spinlock.
     * This rejects an accidental finish by its current operation-lock owner. */
    pin->finishing = true;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    if (!mutex_try_lock(&pin->sb->operation_lock)) {
        flags = spin_lock_irqsave(&vfs_metadata_lock);
        pin->finishing = false;
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        return VFS_ERR_BUSY;
    }
    (void)mutex_unlock(&pin->sb->operation_lock);
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    link = &recovery_pins;
    while (*link != pin) link = &(*link)->next;
    *link = pin->next;
    recovery_pin_count--;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    /* The mutex is unlocked before this possibly final reference release. */
    vfs_super_release(pin->sb);
    kfree(pin);
    return VFS_OK;
}

int vfs_recovery_pin_finish_handoff(u64 id)
{
    vfs_recovery_pin_t **link;
    vfs_recovery_pin_t *pin;
    u64 flags;

    if (!id) return VFS_ERR_INVALID_PARAM;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    link = &recovery_pins;
    while (*link && (*link)->id != id) link = &(*link)->next;
    pin = *link;
    if (!pin || pin->state != VFS_RECOVERY_PIN_ACTIVE || pin->finishing ||
        !pin->handoff) {
        int result = !pin ? VFS_ERR_NOT_FOUND :
            pin->state == VFS_RECOVERY_PIN_UNRESOLVED ?
            VFS_ERR_UNCERTAIN : VFS_ERR_BUSY;
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        return result;
    }
    pin->finishing = true;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    if (!mutex_try_lock(&pin->sb->operation_lock)) {
        flags = spin_lock_irqsave(&vfs_metadata_lock);
        pin->finishing = false;
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        return VFS_ERR_BUSY;
    }
    /* The callback owns no VFS reference and must leave its transaction
     * attached on failure. No registry lock spans filesystem code. */
    bool completed = pin->handoff(pin->sb, pin->handoff_context);
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    if (completed) {
        link = &recovery_pins;
        while (*link != pin) link = &(*link)->next;
        *link = pin->next;
        recovery_pin_count--;
    } else {
        pin->state = VFS_RECOVERY_PIN_UNRESOLVED;
        pin->finishing = false;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    (void)mutex_unlock(&pin->sb->operation_lock);
    if (!completed) return VFS_ERR_UNCERTAIN;
    vfs_super_release(pin->sb);
    kfree(pin);
    return VFS_OK;
}

int vfs_recovery_pins_snapshot(vfs_recovery_pin_info_t *out, u32 capacity,
                               u32 *out_count)
{
    u32 count = 0;
    u64 flags;

    if (!out_count || (capacity && !out)) return VFS_ERR_INVALID_PARAM;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    *out_count = recovery_pin_count;
    if (recovery_pin_count > capacity) {
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        return VFS_ERR_BUFFER_TOO_SMALL;
    }
    count = 0;
    for (vfs_recovery_pin_t *pin = recovery_pins; pin; pin = pin->next) {
        out[count].id = pin->id;
        out[count].device_id = pin->device_id;
        out[count].state = pin->state;
        count++;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return VFS_OK;
}

void vfs_node_register(vfs_node_t *node)
{
    vfs_super_t *sb;

    if (!node) return;
    sb = __atomic_load_n(&node->super, __ATOMIC_ACQUIRE);
    if (!sb) return;
    u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
    if (vfs_super_is_live_locked(sb)) {
        node->next_in_super = sb->nodes;
        sb->nodes = node;
    } else {
        /* Forced detach may retarget the list while an already admitted
         * lookup runs. Never leave its newly returned node pointing at a
         * superblock that the caller's last reference can now destroy. */
        __atomic_store_n(&node->super, &dead_super, __ATOMIC_RELEASE);
        node->next_in_super = NULL;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
}

/* A create return node is private until its recovery-pin handoff succeeds.
 * Link it while the mount operation lock is held, atomically with teardown's
 * retargeting under the metadata lock. */
static bool vfs_node_register_created(vfs_node_t *node, vfs_super_t *sb)
{
    bool registered = false;
    u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
    if (node && node->super == sb && vfs_super_is_live_locked(sb)) {
        node->next_in_super = sb->nodes;
        sb->nodes = node;
        registered = true;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return registered;
}

/* No caller has received this node yet. Forced detach may already have
 * emptied the list and retargeted it, so search under the same lock. */
static void vfs_node_unregister_created(vfs_node_t *node, vfs_super_t *sb)
{
    u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
    vfs_node_t **link = &sb->nodes;
    while (*link && *link != node) link = &(*link)->next_in_super;
    if (*link == node) *link = node->next_in_super;
    node->next_in_super = NULL;
    node->super = NULL;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
}

void vfs_nodes_update_size(vfs_super_t *sb, u64 inode, u64 size)
{
    if (!sb || !inode) return;
    u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (vfs_node_t *node = sb->nodes; node; node = node->next_in_super) {
        if (node->inode == inode)
            node->size = size;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
}

int vfs_node_metadata_snapshot(vfs_node_t *node, vfs_node_metadata_t *out)
{
    vfs_super_t *sb;
    u64 flags;

    if (!node || !out) return VFS_ERR_INVALID_PARAM;
    if (!vfs_node_retain_super(node, &sb)) return VFS_ERR_DEVICE_GONE;
    if (!vfs_super_operation_lock(sb)) {
        vfs_super_release(sb);
        return VFS_ERR_DEVICE_GONE;
    }
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    *out = (vfs_node_metadata_t){
        .type = node->type,
        .permissions = node->permissions,
        .size = node->size,
        .inode = node->inode,
        .owner_uid = node->owner_uid,
    };
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    int result = vfs_super_is_live(sb) ? VFS_OK : VFS_ERR_DEVICE_GONE;
    vfs_super_operation_unlock(sb);
    vfs_super_release(sb);
    return result;
}

bool vfs_node_retain_super(vfs_node_t *node, vfs_super_t **out_super)
{
    vfs_super_t *sb;
    u64 flags;
    bool retained = false;

    if (!node || !out_super) return false;
    *out_super = NULL;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    sb = __atomic_load_n(&node->super, __ATOMIC_ACQUIRE);
    if (vfs_super_is_live_locked(sb) &&
        sb->reference_count != ~(u32)0) {
        sb->reference_count++;
        *out_super = sb;
        retained = true;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return retained;
}

bool vfs_super_operation_lock(vfs_super_t *sb)
{
    if (!sb || !vfs_super_is_live(sb) ||
        !mutex_lock(&sb->operation_lock)) return false;
    /* A queued caller may wake after forced device removal. */
    if (!vfs_super_is_live(sb)) {
        (void)mutex_unlock(&sb->operation_lock);
        return false;
    }
    return true;
}

void vfs_super_operation_unlock(vfs_super_t *sb)
{
    if (sb) (void)mutex_unlock(&sb->operation_lock);
}

static bool vfs_node_operation_begin(vfs_node_t *node, vfs_super_t **out_sb)
{
    vfs_super_t *sb;

    if (!vfs_node_retain_super(node, &sb)) return false;
    if (!vfs_super_operation_lock(sb)) {
        vfs_super_release(sb);
        return false;
    }
    if (out_sb) *out_sb = sb;
    return true;
}

static void vfs_node_operation_end(vfs_super_t *sb)
{
    if (!sb) return;
    (void)mutex_unlock(&sb->operation_lock);
    vfs_super_release(sb);
}

static bool vfs_node_on_read_only_mount(const vfs_node_t *node)
{
    u64 flags;
    bool read_only = false;

    if (!node) return false;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    vfs_super_t *node_super = __atomic_load_n(&node->super,
                                               __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].sb == node_super &&
            mount_table[i].read_only) {
            read_only = true;
            break;
        }
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return read_only;
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

static bool vfs_node_has_persistent_security(const vfs_node_t *node)
{
    return node && node->super && node->super->fs_type &&
           node->super->fs_type->name &&
           strcmp(node->super->fs_type->name, "mgfs") == 0;
}

bool vfs_administrator_override_allowed(
    const vfs_node_t *node, const process_credentials_t *credentials)
{
    user_identity_t owner;

    if (!node || !credentials || !vfs_node_is_live(node) ||
        !vfs_node_has_persistent_security(node) ||
        !identity_credentials_is_admin(credentials) ||
        node->owner_uid == VFS_UID_SYSTEM ||
        node->owner_uid == credentials->uid ||
        !identity_registry_lookup_uid(node->owner_uid, &owner)) return false;
    return owner.role == MG_IDENTITY_ROLE_REGULAR;
}

void vfs_node_set_security(vfs_node_t *node, u32 owner_uid, u32 permissions)
{
    if (!node) return;
    node->owner_uid = owner_uid;
    node->permissions = permissions & VFS_PERMISSION_KNOWN;
}

void vfs_node_set_child_mutation_policy(
    vfs_node_t *node, vfs_child_mutation_policy_t child_mutation_policy)
{
    if (!node) return;
    if (node->type != VFS_TYPE_DIRECTORY ||
        child_mutation_policy > VFS_CHILD_MUTATION_OWNER_RESTRICTED) {
        node->child_mutation_policy = VFS_CHILD_MUTATION_OPEN;
        return;
    }
    node->child_mutation_policy = child_mutation_policy;
}

bool vfs_directory_child_mutation_allowed(const vfs_node_t *dir,
                                          const vfs_node_t *child)
{
    u32 uid;

    if (!dir || dir->type != VFS_TYPE_DIRECTORY) return false;
    if (dir->child_mutation_policy == VFS_CHILD_MUTATION_OPEN) return true;
    if (dir->child_mutation_policy != VFS_CHILD_MUTATION_OWNER_RESTRICTED ||
        !child || !vfs_current_uid(&uid)) return false;
    return uid == dir->owner_uid || uid == child->owner_uid;
}

void vfs_init(void) {
    fs_type_list = NULL;
    recovery_pins = NULL;
    recovery_pin_count = 0;
    next_recovery_pin_id = 1;
    spinlock_init(&vfs_metadata_lock);
    mutex_init(&vfs_mount_lock);
    memset(&dead_super, 0, sizeof(dead_super));
    dead_super.state = VFS_SUPER_DEAD;
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++) {
        memset(&mount_table[i], 0, sizeof(mount_table[i]));
    }
}

int vfs_register_fs(vfs_fs_type_t *fs_type) {
    u64 flags;

    if (!fs_type || !fs_type->name) {
        return VFS_ERR_INVALID_PARAM;
    }

    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (vfs_fs_type_t *curr = fs_type_list; curr; curr = curr->next) {
        if (strcmp(curr->name, fs_type->name) == 0) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            return VFS_ERR_INVALID_PARAM;
        }
    }
    fs_type->next = fs_type_list;
    fs_type_list = fs_type;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return VFS_OK;
}

vfs_fs_type_t *vfs_find_fs(const char *name) {
    u64 flags;
    vfs_fs_type_t *result = NULL;

    if (!name) {
        return NULL;
    }

    flags = spin_lock_irqsave(&vfs_metadata_lock);
    vfs_fs_type_t *curr = fs_type_list;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            result = curr;
            break;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return result;
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

/* Keep the identity admitted before a driver mount from changing when its
 * registry slot is retired or reused before namespace publication. */
static bool vfs_mount_device_identity_live(block_device_t *dev, u64 id)
{
    return dev ? dev->id == id && block_device_id_is_live(id) : id == 0;
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
    u64 flags;
    u64 device_id;
    int result;
    bool mount_busy;

    if (!fs_name) return VFS_ERR_INVALID_PARAM;
    if (!mutex_lock(&vfs_mount_lock)) return VFS_ERR_IO;

    flags = spin_lock_irqsave(&vfs_metadata_lock);
    if (mount_table[0].active) {
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_INVALID_PARAM;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    device_id = dev ? dev->id : 0ULL;
    if (dev && !block_device_is_live(dev)) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_DEVICE_GONE;
    }
    if (vfs_device_already_mounted(dev)) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_INVALID_PARAM;
    }

    vfs_fs_type_t *fs_type = vfs_find_fs(fs_name);
    if (!fs_type || !fs_type->mount) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_NOT_FOUND;
    }

    vfs_super_t *sb = NULL;
    int res = fs_type->mount(fs_type, dev, &sb, false);
    if (res != VFS_OK || !sb || !sb->root_node) {
        result = (res != VFS_OK) ? res : VFS_ERR_BAD_FORMAT;
        (void)mutex_unlock(&vfs_mount_lock);
        return result;
    }

    sb->device_id = device_id;
    sb->reference_count = 0;
    sb->state = VFS_SUPER_ACTIVE;
    sb->nodes = NULL;
    mutex_init(&sb->operation_lock);
    sb->dispose_pending = false;
    vfs_node_register(sb->root_node);

    flags = spin_lock_irqsave(&vfs_metadata_lock);
    mount_busy = mount_table[0].active;
    if (mount_busy || !vfs_mount_device_identity_live(dev, device_id)) {
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        vfs_dispose_unpublished(sb);
        (void)mutex_unlock(&vfs_mount_lock);
        return mount_busy ? VFS_ERR_INVALID_PARAM : VFS_ERR_DEVICE_GONE;
    }
    mount_table[0].sb = sb;
    mount_table[0].covered_node = NULL;
    mount_table[0].covered_super = NULL;
    mount_table[0].covered_inode = 0;
    mount_table[0].parent_super = NULL;
    mount_table[0].parent_inode = 0;
    mount_table[0].dev = dev;
    mount_table[0].dev_id = device_id;
    mount_table[0].role = VFS_MOUNT_ROLE_ROOT;
    mount_table[0].read_only = false;
    strcpy(mount_table[0].mount_point, "/");
    mount_table[0].name[0] = '\0';
    mount_table[0].active = true;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);

    (void)mutex_unlock(&vfs_mount_lock);
    return VFS_OK;
}

static int vfs_mount_slot_locked(void) {
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
    u64 device_id = dev ? dev->id : 0ULL;
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

    fs_type = vfs_find_fs(fs_name);
    if (!fs_type || !fs_type->mount) return VFS_ERR_NOT_FOUND;

    res = fs_type->mount(fs_type, dev, &sb, read_only);
    if (res != VFS_OK || !sb || !sb->root_node) {
        if (res != VFS_OK) return res;
        return VFS_ERR_BAD_FORMAT;
    }

    sb->device_id = device_id;
    sb->reference_count = 0;
    sb->state = VFS_SUPER_ACTIVE;
    sb->nodes = NULL;
    mutex_init(&sb->operation_lock);
    sb->dispose_pending = false;
    vfs_node_register(sb->root_node);

    {
        u64 flags = spin_lock_irqsave(&vfs_metadata_lock);

        if (!vfs_mount_device_identity_live(dev, device_id) ||
            (target_node && !vfs_super_is_live_locked(target_node->super)) ||
            (parent && !vfs_super_is_live_locked(parent->super))) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            vfs_dispose_unpublished(sb);
            return VFS_ERR_DEVICE_GONE;
        }
        slot = vfs_mount_slot_locked();
        if (slot < 0) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            vfs_dispose_unpublished(sb);
            return slot;
        }
        if ((parent && vfs_find_mount_child_locked(parent, name)) ||
            (target_node && vfs_find_mount_for_node_locked(target_node))) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            vfs_dispose_unpublished(sb);
            return VFS_ERR_INVALID_PARAM;
        }

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
    mount_table[slot].dev_id = device_id;
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
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    }
    return VFS_OK;
}

int vfs_mount_node(vfs_node_t *target_node, const char *fs_name, block_device_t *dev) {
    int result;

    if (!mutex_lock(&vfs_mount_lock)) return VFS_ERR_IO;
    if (vfs_device_already_mounted(dev)) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_node_is_live(target_node) ||
        target_node->type != VFS_TYPE_DIRECTORY || !fs_name) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_INVALID_PARAM;
    }

    if (vfs_find_mount_for_node(target_node) != NULL) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_INVALID_PARAM;
    }

    result = vfs_mount_instance(NULL, NULL, target_node, "/", fs_name,
                                dev, VFS_MOUNT_ROLE_VOLUME, false);
    (void)mutex_unlock(&vfs_mount_lock);
    return result;
}

int vfs_mount_path(vfs_node_t *parent, const char *name,
                   const char *mount_point, const char *fs_name,
                   block_device_t *dev, vfs_mount_role_t role,
                   bool read_only) {
    vfs_node_t *target_node;
    int result;

    if (!mutex_lock(&vfs_mount_lock)) return VFS_ERR_IO;
    if (vfs_device_already_mounted(dev)) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_node_is_live(parent) ||
        parent->type != VFS_TYPE_DIRECTORY || !name || !*name ||
        !mount_point || mount_point[0] != '/' || !fs_name) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_find_mount_child(parent, name) != NULL) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_INVALID_PARAM;
    }

    target_node = vfs_finddir_trusted(parent, name);
    if (target_node && target_node->type != VFS_TYPE_DIRECTORY) {
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_NOT_DIRECTORY;
    }

    result = vfs_mount_instance(parent, name, target_node, mount_point,
                                fs_name, dev, role, read_only);
    (void)mutex_unlock(&vfs_mount_lock);
    return result;
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

static int vfs_unmount_preflight_super_locked(vfs_super_t *sb)
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

static vfs_super_t *vfs_mounted_super_for_device_locked(block_device_t *device)
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
    u64 flags;

    if (!device) return VFS_ERR_INVALID_PARAM;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    sb = vfs_mounted_super_for_device_locked(device);
    int result = sb ? vfs_unmount_preflight_super_locked(sb) :
        VFS_ERR_NOT_FOUND;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return result;
}

int vfs_format_preflight(block_device_t *device)
{
    u64 flags;
    bool mounted;

    if (!device || !block_device_is_live(device)) return VFS_ERR_DEVICE_GONE;
    /* A mounted superblock is the authoritative indication that filesystem
     * objects may still issue I/O.  Formatting deliberately does not
     * unmount or invalidate it on the caller's behalf. */
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    mounted = vfs_mounted_super_for_device_locked(device) != NULL;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    if (mounted) return VFS_ERR_BUSY;
    return VFS_OK;
}

int vfs_unmount_devices_preflight(block_device_t *devices[], u32 count)
{
    vfs_super_t *supers[VFS_MAX_MOUNTS];
    u32 super_count = 0;
    u64 flags;

    if (!devices || count == 0 || count > VFS_MAX_MOUNTS)
        return VFS_ERR_INVALID_PARAM;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (u32 index = 0; index < count; index++) {
        vfs_super_t *sb = vfs_mounted_super_for_device_locked(devices[index]);
        bool duplicate = false;
        if (!sb) continue;
        for (u32 prior = 0; prior < super_count; prior++)
            if (supers[prior] == sb) duplicate = true;
        if (duplicate) continue;
        if (super_count >= VFS_MAX_MOUNTS) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            return VFS_ERR_NO_MEM;
        }
        supers[super_count++] = sb;
    }
    if (!super_count) {
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        return VFS_ERR_NOT_FOUND;
    }
    for (u32 index = 0; index < super_count; index++) {
        int result = vfs_unmount_preflight_super_locked(supers[index]);
        if (result != VFS_OK) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            return result;
        }
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return VFS_OK;
}

int vfs_unmount_devices(block_device_t *devices[], u32 count)
{
    vfs_super_t *supers[VFS_MAX_MOUNTS];
    u32 super_count = 0;
    int result;
    u64 flags;

    if (!devices || count == 0 || count > VFS_MAX_MOUNTS)
        return VFS_ERR_INVALID_PARAM;
    if (!mutex_lock(&vfs_mount_lock)) return VFS_ERR_IO;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (u32 index = 0; index < count; index++) {
        vfs_super_t *sb = vfs_mounted_super_for_device_locked(devices[index]);
        bool duplicate = false;
        if (!sb) continue;
        for (u32 prior = 0; prior < super_count; prior++)
            if (supers[prior] == sb) duplicate = true;
        if (duplicate) continue;
        if (super_count >= VFS_MAX_MOUNTS) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            (void)mutex_unlock(&vfs_mount_lock);
            return VFS_ERR_NO_MEM;
        }
        supers[super_count++] = sb;
    }
    if (!super_count) {
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        (void)mutex_unlock(&vfs_mount_lock);
        return VFS_ERR_NOT_FOUND;
    }

    /* Preflight every child before changing any mount state.  This makes a
     * busy child fail an eject without partially detaching an earlier child. */
    for (u32 index = 0; index < super_count; index++) {
        result = vfs_unmount_preflight_super_locked(supers[index]);
        if (result != VFS_OK) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            (void)mutex_unlock(&vfs_mount_lock);
            return result;
        }
    }
    for (u32 index = 0; index < super_count; index++) {
        if (supers[index]->reference_count == ~(u32)0) {
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            (void)mutex_unlock(&vfs_mount_lock);
            return VFS_ERR_NO_MEM;
        }
        /* The mount itself is an implicit lifetime reference while sync and
         * teardown run outside the metadata spinlock. */
        supers[index]->reference_count++;
        supers[index]->state = VFS_SUPER_DETACHING;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    for (u32 index = 0; index < super_count; index++) {
        if (!supers[index]->ops || !supers[index]->ops->sync) continue;
        if (!mutex_lock(&supers[index]->operation_lock)) {
            result = VFS_ERR_IO;
        } else {
            result = supers[index]->ops->sync(supers[index]);
            (void)mutex_unlock(&supers[index]->operation_lock);
        }
        if (result != VFS_OK) {
            flags = spin_lock_irqsave(&vfs_metadata_lock);
            for (u32 restore = 0; restore < super_count; restore++)
                if (supers[restore]->state == VFS_SUPER_DETACHING)
                    supers[restore]->state = VFS_SUPER_ACTIVE;
            spin_unlock_irqrestore(&vfs_metadata_lock, flags);
            for (u32 release = 0; release < super_count; release++)
                vfs_super_release(supers[release]);
            (void)mutex_unlock(&vfs_mount_lock);
            return result;
        }
    }
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (u32 index = 0; index < VFS_MAX_MOUNTS; index++) {
        vfs_mount_t *mount = &mount_table[index];
        bool selected = false;
        if (!mount->active) continue;
        for (u32 super = 0; super < super_count; super++)
            if (mount->sb == supers[super]) selected = true;
        if (selected) vfs_mount_detach(mount);
    }
    for (u32 index = 0; index < super_count; index++) {
        supers[index]->state = VFS_SUPER_DEAD;
        vfs_retarget_nodes_locked(supers[index]);
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    for (u32 index = 0; index < super_count; index++)
        vfs_super_release(supers[index]);
    (void)mutex_unlock(&vfs_mount_lock);
    return VFS_OK;
}

int vfs_unmount_sb(vfs_super_t *sb)
{
    block_device_t *device = NULL;
    u64 flags;

    if (!sb) return VFS_ERR_INVALID_PARAM;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (u32 i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mount_table[i].active && mount_table[i].sb == sb)
            device = mount_table[i].dev;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
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
    vfs_super_t *dead_supers[VFS_MAX_MOUNTS];
    u32 dead_count = 0;
    u64 flags;

    if (!device) return;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
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
        if (dead_count < VFS_MAX_MOUNTS)
            dead_supers[dead_count++] = sb;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    for (u32 i = 0; i < dead_count; i++)
        vfs_super_dispose(dead_supers[i]);
}

vfs_node_t *vfs_get_root_node(void) {
    vfs_node_t *root = NULL;
    u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
    if (mount_table[0].active && mount_table[0].sb &&
        vfs_super_is_live_locked(mount_table[0].sb))
        root = mount_table[0].sb->root_node;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return root;
}

static vfs_mount_t *vfs_find_mount_for_node_locked(vfs_node_t *node) {
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

vfs_mount_t *vfs_find_mount_for_node(vfs_node_t *node) {
    vfs_mount_t *mount;
    u64 flags;

    flags = spin_lock_irqsave(&vfs_metadata_lock);
    mount = vfs_find_mount_for_node_locked(node);
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return mount;
}

bool vfs_mount_point_for_device(block_device_t *device, char *out_path,
                                usize capacity, vfs_mount_role_t *out_role)
{
    u64 flags;
    bool found = false;

    if (!device || !out_path || capacity < 2U) return false;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (u32 index = 0; index < VFS_MAX_MOUNTS; index++) {
        if (!mount_table[index].active || mount_table[index].dev_id != device->id)
            continue;
        strncpy(out_path, mount_table[index].mount_point, capacity - 1U);
        out_path[capacity - 1U] = '\0';
        if (out_role) *out_role = mount_table[index].role;
        found = true;
        break;
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return found;
}

static bool vfs_mount_parent_matches(const vfs_mount_t *mount,
                                     const vfs_node_t *parent)
{
    return mount && mount->active && parent && mount->parent_super == parent->super &&
           mount->parent_inode == parent->inode;
}

bool vfs_directory_has_mount_children(vfs_node_t *dir)
{
    bool found = false;
    u64 flags;

    if (!dir || dir->type != VFS_TYPE_DIRECTORY) return false;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    for (u32 i = 1; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mount_parent_matches(&mount_table[i], dir)) {
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return found;
}

static vfs_mount_t *vfs_find_mount_child_locked(vfs_node_t *parent,
                                                const char *name)
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

static vfs_mount_t *vfs_find_mount_child(vfs_node_t *parent, const char *name)
{
    vfs_mount_t *mount;
    u64 flags;

    flags = spin_lock_irqsave(&vfs_metadata_lock);
    mount = vfs_find_mount_child_locked(parent, name);
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return mount;
}

static vfs_node_t *vfs_resolve_mount(vfs_node_t *node) {
    vfs_node_t *resolved = node;
    u64 flags;

    if (!vfs_node_is_live(node)) return NULL;
    flags = spin_lock_irqsave(&vfs_metadata_lock);
    vfs_mount_t *m = vfs_find_mount_for_node_locked(node);
    if (m && m->active && m->sb && m->sb->root_node)
        resolved = m->sb->root_node;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    return resolved;
}

static vfs_node_t *vfs_finddir_internal(vfs_node_t *dir, const char *name,
                                        bool enforce) {
    vfs_node_t *mounted_root = NULL;
    vfs_super_t *sb;

    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->finddir || !vfs_node_is_live(dir) || (enforce &&
        !vfs_check_access(dir, VFS_ACCESS_READ))) {
        return NULL;
    }
    if (!vfs_node_operation_begin(dir, &sb)) return NULL;
    {
        u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
        vfs_mount_t *mount = vfs_find_mount_child_locked(dir, name);
        if (mount && mount->sb && mount->sb->root_node)
            mounted_root = mount->sb->root_node;
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    }
    if (!mounted_root)
        mounted_root = dir->ops->finddir(dir, name);
    vfs_node_operation_end(sb);
    return mounted_root;
}

/* Find a child while the caller owns the containing superblock operation
 * mutex.  This is used to bind name-based mutations to the object that was
 * authorized before a possible path replacement. */
static vfs_node_t *vfs_finddir_locked(vfs_node_t *dir, const char *name)
{
    vfs_node_t *mounted_root = NULL;

    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->finddir) return NULL;
    {
        u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
        vfs_mount_t *mount = vfs_find_mount_child_locked(dir, name);
        if (mount && mount->sb && mount->sb->root_node)
            mounted_root = mount->sb->root_node;
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    }
    if (!mounted_root)
        mounted_root = dir->ops->finddir(dir, name);
    return mounted_root;
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
                                  u32 authorized_access,
                                  vfs_authorization_scope_t authorization_scope,
                                  u32 authorization_owner_uid,
                                  bool enforce_access) {
    vfs_file_handle_t *handle;
    vfs_super_t *super = NULL;
    vfs_file_facts_t facts = {0};
    bool save_expectation_valid = false;
    int facts_result = VFS_ERR_UNSUPPORTED;

    if (!node || !out_handle ||
        (flags != VFS_OPEN_READ && flags != VFS_OPEN_WRITE && flags != VFS_OPEN_RDWR)) {
        return VFS_ERR_INVALID_PARAM;
    }

    *out_handle = NULL;
    if (!vfs_node_retain_super(node, &super)) return VFS_ERR_DEVICE_GONE;
    if ((flags & VFS_OPEN_WRITE) && vfs_node_on_read_only_mount(node)) {
        vfs_super_release(super);
        return VFS_ERR_ACCESS_DENIED;
    }
    if (enforce_access && (((flags & VFS_OPEN_READ) &&
         !(authorized_access & VFS_ACCESS_READ) &&
         !vfs_check_access(node, VFS_ACCESS_READ)) ||
        ((flags & VFS_OPEN_WRITE) &&
         !(authorized_access & VFS_ACCESS_WRITE) &&
         !vfs_check_access(node, VFS_ACCESS_WRITE)))) {
        vfs_super_release(super);
        return VFS_ERR_ACCESS_DENIED;
    }

    if (node->type == VFS_TYPE_FILE && node->ops && node->ops->file_facts) {
        if (!vfs_super_operation_lock(super)) {
            vfs_super_release(super);
            return VFS_ERR_DEVICE_GONE;
        }
        facts_result = node->ops->file_facts(node, &facts);
        vfs_super_operation_unlock(super);
        if (!vfs_super_is_live(super)) {
            vfs_super_release(super);
            return VFS_ERR_DEVICE_GONE;
        }
        /* Ordinary open remains available after a retryable facts read error
         * or an unrelated mutation quarantine; no save expectation is made. */
        save_expectation_valid = facts_result == VFS_OK &&
            facts.record_id == node->inode && facts.generation != 0;
        if (facts_result == VFS_OK && !save_expectation_valid)
            facts_result = VFS_ERR_BAD_FORMAT;
    }

    handle = (vfs_file_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) {
        vfs_super_release(super);
        return VFS_ERR_NO_MEM;
    }

    handle->node = node;
    handle->super = super;
    handle->offset = 0;
    handle->flags = flags;
    handle->valid = VFS_FILE_HANDLE_VALID;
    mutex_init(&handle->offset_lock);
    handle->authorized_access = authorized_access & VFS_ACCESS_READ_WRITE;
    handle->authorization_scope = authorization_scope;
    handle->authorization_owner_uid =
        authorization_scope == VFS_AUTH_CONFIGURATION_WRITE &&
        save_expectation_valid ? facts.owner_uid : authorization_owner_uid;
    handle->save_record_id = save_expectation_valid ? facts.record_id : 0;
    handle->save_generation = save_expectation_valid ? facts.generation : 0;
    handle->save_expectation_valid = save_expectation_valid;
    handle->save_expectation_error = save_expectation_valid ? VFS_OK : facts_result;
    *out_handle = handle;
    return VFS_OK;
}

int vfs_open_node(vfs_node_t *node, u32 flags,
                  vfs_file_handle_t **out_handle) {
    return vfs_open_node_internal(node, flags, out_handle, 0U,
                                  VFS_AUTH_NONE, 0U, true);
}

int vfs_open_node_authorized(vfs_node_t *node, u32 flags,
                             vfs_file_handle_t **out_handle) {
    return vfs_open_node_internal(node, flags, out_handle, VFS_ACCESS_WRITE,
                                  VFS_AUTH_CONFIGURATION_WRITE, 0U, true);
}

int vfs_open_node_user_authorized(vfs_node_t *node, u32 flags,
                                  vfs_file_handle_t **out_handle) {
    process_credentials_t credentials;

    if (!process_get_credentials(process_current(), &credentials) ||
        !vfs_administrator_override_allowed(node, &credentials))
        return VFS_ERR_ACCESS_DENIED;
    return vfs_open_node_internal(node, flags, out_handle,
                                  VFS_ACCESS_READ_WRITE,
                                  VFS_AUTH_REGULAR_USER_DATA,
                                  node->owner_uid, true);
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

    if (!path || path[0] != '/' || !out_handle ||
        (flags != VFS_OPEN_READ && flags != VFS_OPEN_WRITE &&
         flags != VFS_OPEN_RDWR)) {
        return VFS_ERR_INVALID_PARAM;
    }
    *out_handle = NULL;
    if (vfs_lookup_trusted(path, &node) != VFS_OK || !node) {
        return VFS_ERR_NOT_FOUND;
    }
    return vfs_open_node_internal(node, flags, out_handle, 0U,
                                  VFS_AUTH_NONE, 0U, false);
}

int vfs_close(vfs_file_handle_t *handle) {
    vfs_super_t *super;

    if (!handle || !mutex_lock(&handle->offset_lock)) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (handle->valid != VFS_FILE_HANDLE_VALID || !handle->node) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_INVALID_PARAM;
    }

    super = handle->super;
    handle->valid = 0;
    handle->node = NULL;
    handle->super = NULL;
    (void)mutex_unlock(&handle->offset_lock);
    vfs_super_release(super);
    kfree(handle);
    return VFS_OK;
}

static bool vfs_handle_valid(const vfs_file_handle_t *handle) {
    return handle && handle->valid == VFS_FILE_HANDLE_VALID && handle->node &&
           vfs_super_is_live(handle->super);
}

static bool vfs_handle_authorized(const vfs_file_handle_t *handle,
                                  u32 permission)
{
    process_credentials_t credentials;

    if (!handle || !(handle->authorized_access & permission)) return false;
    if (handle->authorization_scope == VFS_AUTH_CONFIGURATION_WRITE)
        return permission == VFS_ACCESS_WRITE;
    if (handle->authorization_scope != VFS_AUTH_REGULAR_USER_DATA ||
        !process_get_credentials(process_current(), &credentials) ||
        handle->node->owner_uid != handle->authorization_owner_uid) {
        return false;
    }
    return vfs_administrator_override_allowed(handle->node, &credentials);
}

/* The caller holds operation_lock. Permissions come from the freshly read
 * record, not a node cached by a different open or before a metadata change. */
static bool vfs_save_facts_write_allowed(const vfs_file_handle_t *handle,
                                         const vfs_file_facts_t *facts)
{
    process_t *process = process_current();
    process_credentials_t credentials;
    user_identity_t owner;

    if (!process) return true; /* trusted kernel caller */
    if (!process_get_credentials(process, &credentials)) return false;
    if ((credentials.uid == facts->owner_uid &&
         (facts->permissions & VFS_PERMISSION_OWNER_WRITE)) ||
        (credentials.uid != facts->owner_uid &&
         (facts->permissions & VFS_PERMISSION_OTHER_WRITE))) return true;
    if (handle->authorization_scope == VFS_AUTH_CONFIGURATION_WRITE)
        return (handle->authorized_access & VFS_ACCESS_WRITE) &&
               facts->owner_uid == handle->authorization_owner_uid &&
               identity_credentials_has_privilege(
                   &credentials, IDENTITY_PRIVILEGE_MANAGE_CONFIGURATION);
    if (handle->authorization_scope != VFS_AUTH_REGULAR_USER_DATA ||
        !(handle->authorized_access & VFS_ACCESS_WRITE) ||
        facts->owner_uid != handle->authorization_owner_uid ||
        facts->owner_uid == VFS_UID_SYSTEM ||
        facts->owner_uid == credentials.uid ||
        !identity_credentials_has_privilege(
            &credentials, IDENTITY_PRIVILEGE_MANAGE_USER_DATA) ||
        !identity_credentials_is_admin(&credentials) ||
        !identity_registry_lookup_uid(facts->owner_uid, &owner)) return false;
    return owner.role == MG_IDENTITY_ROLE_REGULAR;
}

/* This locked core can be used by the later replacement operation without a
 * check/use gap. A conflict never changes the handle's captured generation. */
static int vfs_file_save_check_locked(vfs_file_handle_t *handle,
                                      bool process_write_right,
                                      vfs_file_facts_t *out_facts)
{
    vfs_file_facts_t facts;
    int result;

    if (!process_write_right || !(handle->flags & VFS_OPEN_WRITE) ||
        vfs_node_on_read_only_mount(handle->node))
        return VFS_ERR_ACCESS_DENIED;
    if (!handle->save_expectation_valid)
        return handle->save_expectation_error;
    if (!handle->node->ops || !handle->node->ops->file_facts)
        return VFS_ERR_UNSUPPORTED;
    result = handle->node->ops->file_facts(handle->node, &facts);
    if (result != VFS_OK) return result;
    if (facts.record_id != handle->save_record_id ||
        facts.record_id != handle->node->inode || !facts.generation ||
        (facts.permissions & ~VFS_PERMISSION_KNOWN) || !facts.permissions)
        return VFS_ERR_BAD_FORMAT;
    if (!vfs_save_facts_write_allowed(handle, &facts))
        return VFS_ERR_ACCESS_DENIED;
    if (facts.generation != handle->save_generation)
        return VFS_ERR_STALE_GENERATION;
    if (out_facts) *out_facts = facts;
    return VFS_OK;
}

int vfs_file_save_check(vfs_file_handle_t *handle, bool process_write_right,
                        vfs_file_facts_t *out_facts)
{
    int result;

    if (!handle || !mutex_lock(&handle->offset_lock))
        return VFS_ERR_INVALID_PARAM;
    if (handle->valid != VFS_FILE_HANDLE_VALID || !handle->node ||
        !handle->super) result = VFS_ERR_INVALID_PARAM;
    else if (!vfs_super_is_live(handle->super)) result = VFS_ERR_DEVICE_GONE;
    else if (handle->node->type != VFS_TYPE_FILE) result = VFS_ERR_UNSUPPORTED;
    else if (!vfs_super_operation_lock(handle->super)) result = VFS_ERR_DEVICE_GONE;
    else {
        result = vfs_file_save_check_locked(handle, process_write_right,
                                            out_facts);
        vfs_super_operation_unlock(handle->super);
    }
    (void)mutex_unlock(&handle->offset_lock);
    return result;
}

int vfs_file_replace(vfs_file_handle_t *handle, bool process_write_right,
                     u64 length, vfs_replace_source_t source, void *context)
{
    int result, finish;
    u64 pin = 0;

    if (!handle || (length && !source) || !mutex_lock(&handle->offset_lock))
        return VFS_ERR_INVALID_PARAM;
    if (handle->valid != VFS_FILE_HANDLE_VALID || !handle->node ||
        !handle->super) result = VFS_ERR_INVALID_PARAM;
    else if (!vfs_super_is_live(handle->super)) result = VFS_ERR_DEVICE_GONE;
    else if (handle->node->type != VFS_TYPE_FILE || !handle->node->ops ||
             !handle->node->ops->replace) result = VFS_ERR_UNSUPPORTED;
    else if (!vfs_super_operation_lock(handle->super))
        result = VFS_ERR_DEVICE_GONE;
    else {
        result = vfs_file_save_check_locked(handle, process_write_right, NULL);
        if (result == VFS_OK && !vfs_super_is_live(handle->super))
            result = VFS_ERR_DEVICE_GONE;
        if (result == VFS_OK &&
            length > handle->node->ops->replacement_max_bytes)
            result = VFS_ERR_NO_SPACE;
        if (result == VFS_OK)
            result = handle->node->ops->replace(
                handle->super, handle->save_record_id,
                handle->save_generation, length, source, context, &pin);
        if ((result == VFS_ERR_IO || result == VFS_ERR_INVALID_PARAM) &&
            !vfs_super_is_live(handle->super))
            result = VFS_ERR_DEVICE_GONE;
        if (result == VFS_OK && !pin) result = VFS_ERR_UNCERTAIN;
        vfs_super_operation_unlock(handle->super);
        if (pin) {
            do {
                finish = vfs_recovery_pin_finish_handoff(pin);
                if (finish == VFS_ERR_BUSY) {
                    if (!mutex_lock(&handle->super->operation_lock)) break;
                    (void)mutex_unlock(&handle->super->operation_lock);
                }
            } while (finish == VFS_ERR_BUSY);
            if (finish != VFS_OK) {
                if (finish == VFS_ERR_BUSY)
                    (void)vfs_recovery_pin_mark_unresolved(pin);
                result = VFS_ERR_UNCERTAIN;
            } else if (result == VFS_OK) {
                handle->save_generation++;
            }
        }
    }
    (void)mutex_unlock(&handle->offset_lock);
    return result;
}

int vfs_file_create_exclusive(const char *parent_path, const char *name,
                              u64 length, vfs_replace_source_t source,
                              void *context, vfs_file_handle_t **out_handle)
{
    vfs_node_t *dir = NULL, *node = NULL;
    vfs_file_handle_t *handle = NULL;
    vfs_super_t *sb = NULL;
    vfs_file_facts_t facts = {0};
    u64 pin = 0;
    usize name_length;
    bool registered = false;
    bool private_bound = false;
    bool public_node = false;
    int result, finish;

    if (!parent_path || parent_path[0] != '/' || !name || !out_handle ||
        (length && !source)) return VFS_ERR_INVALID_PARAM;
    *out_handle = NULL;
    name_length = strlen(name);
    if (!name_length || name_length > 255 ||
        (name_length == 1 && name[0] == '.') ||
        (name_length == 2 && name[0] == '.' && name[1] == '.'))
        return VFS_ERR_INVALID_PARAM;
    result = vfs_lookup(parent_path, &dir);
    if (result != VFS_OK) return result;
    if (!dir || dir->type != VFS_TYPE_DIRECTORY) return VFS_ERR_NOT_DIRECTORY;
    if (!vfs_node_retain_super(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    if (!dir->ops || !dir->ops->create_contents) {
        result = VFS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    if (length > dir->ops->create_contents_max_bytes) {
        result = VFS_ERR_NO_SPACE;
        goto cleanup;
    }
    node = (vfs_node_t *)kmalloc(sizeof(*node));
    if (node) memset(node, 0, sizeof(*node));
    handle = (vfs_file_handle_t *)kmalloc(sizeof(*handle));
    if (!node || !handle) { result = VFS_ERR_NO_MEM; goto cleanup; }
    memset(handle, 0, sizeof(*handle));
    if (!vfs_super_operation_lock(sb)) {
        result = VFS_ERR_DEVICE_GONE;
        goto cleanup;
    }
    if (dir->super != sb || dir->type != VFS_TYPE_DIRECTORY ||
        !vfs_super_is_live(sb)) result = VFS_ERR_DEVICE_GONE;
    else if (vfs_node_on_read_only_mount(dir)) result = VFS_ERR_ACCESS_DENIED;
    else {
        u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
        bool mount_child = vfs_find_mount_child_locked(dir, name) != NULL;
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
        result = mount_child ? VFS_ERR_ALREADY_EXISTS :
            dir->ops->create_contents(dir, node, name, length, source,
                                      context, &facts, &pin);
    }

    if ((result == VFS_ERR_IO || result == VFS_ERR_INVALID_PARAM) &&
        !vfs_super_is_live(sb)) result = VFS_ERR_DEVICE_GONE;
    if (result == VFS_OK) {
        /* The hook supplies facts from its committed image, without a read
         * that could fail after successful publication. */
        node->inode = facts.record_id;
        node->type = VFS_TYPE_FILE;
        node->size = facts.size;
        node->owner_uid = facts.owner_uid;
        node->permissions = facts.permissions;
        node->child_mutation_policy = VFS_CHILD_MUTATION_OPEN;
        node->ref_count = 1;
        node->super = sb;
        private_bound = node->fs_data != NULL;
        if (!private_bound)
            node->fs_data = (void *)(uintptr_t)facts.record_id;
        node->ops = dir->ops;
        handle->node = node;
        handle->super = sb;
        handle->flags = VFS_OPEN_RDWR;
        handle->valid = VFS_FILE_HANDLE_VALID;
        mutex_init(&handle->offset_lock);
        handle->authorization_scope = VFS_AUTH_NONE;
        handle->save_record_id = facts.record_id;
        handle->save_generation = facts.generation;
        handle->save_expectation_valid = true;
        handle->save_expectation_error = VFS_OK;
        if (!facts.record_id || !facts.generation || facts.size != length ||
            !facts.permissions ||
            (facts.permissions & ~VFS_PERMISSION_KNOWN) || !pin) {
            result = VFS_ERR_UNCERTAIN;
            if (pin) {
                (void)vfs_recovery_pin_mark_unresolved(pin);
                pin = 0;
            }
        } else if (!vfs_node_register_created(node, sb))
            result = VFS_ERR_DEVICE_GONE;
        else registered = true;
    }
    vfs_super_operation_unlock(sb);

    if (pin) {
        do {
            finish = vfs_recovery_pin_finish_handoff(pin);
            if (finish == VFS_ERR_BUSY) {
                if (!mutex_lock(&sb->operation_lock)) break;
                (void)mutex_unlock(&sb->operation_lock);
            }
        } while (finish == VFS_ERR_BUSY);
        if (finish != VFS_OK) {
            if (finish == VFS_ERR_BUSY)
                (void)vfs_recovery_pin_mark_unresolved(pin);
            result = VFS_ERR_UNCERTAIN;
        }
    }
    /* Handoff has opened lookup admission. Registered nodes are now public,
     * even if a late detach prevents returning the prepared handle. */
    public_node = result == VFS_OK && registered;
    if (result == VFS_OK && !vfs_super_is_live(sb))
        result = VFS_ERR_DEVICE_GONE;
    if (result == VFS_OK) {
        *out_handle = handle;
        return VFS_OK; /* The retained superblock reference moves to handle. */
    }
cleanup:
    if (!public_node && node) {
        if (registered || private_bound) {
            if (mutex_lock(&sb->operation_lock)) {
                if (registered) vfs_node_unregister_created(node, sb);
                if (private_bound && node->fs_data && node->ops &&
                    node->ops->discard_created_node)
                    node->ops->discard_created_node(node, sb);
                (void)mutex_unlock(&sb->operation_lock);
            } else { node = NULL; result = VFS_ERR_UNCERTAIN; }
        }
        kfree(node);
    }
    kfree(handle);
    if (sb) vfs_super_release(sb);
    return result;
}

u64 vfs_file_read(vfs_file_handle_t *handle, u64 size, void *buffer) {
    u64 transferred;

    if (!handle || !mutex_lock(&handle->offset_lock) ||
        !vfs_handle_valid(handle) || !(handle->flags & VFS_OPEN_READ) ||
        (size != 0 && !buffer) || !handle->node->ops || !handle->node->ops->read) {
        if (handle) (void)mutex_unlock(&handle->offset_lock);
        return 0;
    }
    if (!vfs_handle_authorized(handle, VFS_ACCESS_READ) &&
        !vfs_check_access(handle->node, VFS_ACCESS_READ)) {
        (void)mutex_unlock(&handle->offset_lock);
        return 0;
    }
    if (size == 0) {
        (void)mutex_unlock(&handle->offset_lock);
        return 0;
    }

    if (!vfs_super_operation_lock(handle->super)) {
        (void)mutex_unlock(&handle->offset_lock);
        return 0;
    }
    transferred = handle->node->ops->read(handle->node, handle->offset, size,
                                           buffer);
    vfs_super_operation_unlock(handle->super);
    if (transferred <= (u64)-1 - handle->offset) {
        handle->offset += transferred;
    }
    (void)mutex_unlock(&handle->offset_lock);
    return transferred;
}

u64 vfs_file_read_trusted(vfs_file_handle_t *handle, u64 size, void *buffer) {
    u64 transferred;

    if (!handle || !mutex_lock(&handle->offset_lock) ||
        !vfs_handle_valid(handle) || !(handle->flags & VFS_OPEN_READ) ||
        (size != 0 && !buffer) || !handle->node->ops ||
        !handle->node->ops->read || size == 0) {
        if (handle) (void)mutex_unlock(&handle->offset_lock);
        return 0;
    }
    if (!vfs_super_operation_lock(handle->super)) {
        (void)mutex_unlock(&handle->offset_lock);
        return 0;
    }
    transferred = handle->node->ops->read(handle->node, handle->offset, size,
                                          buffer);
    vfs_super_operation_unlock(handle->super);
    if (transferred <= (u64)-1 - handle->offset) handle->offset += transferred;
    (void)mutex_unlock(&handle->offset_lock);
    return transferred;
}

static int vfs_operation_finish_pin(vfs_super_t *sb, int result, u64 pin);

/* Caller holds operation_lock. The legacy callback keeps its byte-count
 * contract; a recovery-aware callback supplies a separate error and pin. */
static int vfs_write_dispatch(vfs_node_t *node, u64 offset, u64 size,
                              const void *buffer, u64 *out_bytes, u64 *out_pin)
{
    int result;
    *out_bytes = *out_pin = 0;
    if (node->ops->write_pinned) {
        result = node->ops->write_pinned(node, offset, size, buffer,
                                         out_bytes, out_pin);
        if (result > VFS_OK || *out_bytes > size ||
            (result == VFS_ERR_UNCERTAIN && *out_bytes))
            result = VFS_ERR_UNCERTAIN;
        if (result == VFS_ERR_UNCERTAIN) {
            *out_bytes = 0;
            if (*out_pin) {
                (void)vfs_recovery_pin_mark_unresolved(*out_pin);
                *out_pin = 0;
            }
        }
        return result;
    }
    *out_bytes = node->ops->write(node, offset, size, buffer);
    return VFS_OK;
}

int vfs_file_write_status(vfs_file_handle_t *handle, u64 size,
                          const void *buffer, u64 *out_written)
{
    u64 transferred = 0, pin = 0;
    int result;

    if (!out_written) return VFS_ERR_INVALID_PARAM;
    *out_written = 0;
    if (!handle || !mutex_lock(&handle->offset_lock))
        return VFS_ERR_INVALID_PARAM;
    if (handle->valid != VFS_FILE_HANDLE_VALID || !handle->node ||
        !handle->super || !(handle->flags & VFS_OPEN_WRITE) ||
        (size && !buffer) || !handle->node->ops ||
        (!handle->node->ops->write && !handle->node->ops->write_pinned))
        result = VFS_ERR_INVALID_PARAM;
    else if (!vfs_super_is_live(handle->super))
        result = VFS_ERR_DEVICE_GONE;
    else if ((!vfs_handle_authorized(handle, VFS_ACCESS_WRITE) &&
              !vfs_check_access(handle->node, VFS_ACCESS_WRITE)) ||
             vfs_node_on_read_only_mount(handle->node))
        result = VFS_ERR_ACCESS_DENIED;
    else if (!size)
        result = VFS_OK;
    else if (!vfs_super_operation_lock(handle->super))
        result = VFS_ERR_DEVICE_GONE;
    else {
        result = vfs_write_dispatch(handle->node, handle->offset, size,
                                    buffer, &transferred, &pin);
        vfs_super_operation_unlock(handle->super);
        result = vfs_operation_finish_pin(handle->super, result, pin);
        if (result == VFS_ERR_UNCERTAIN) transferred = 0;
        else if (transferred) result = VFS_OK; /* Confirmed partial write. */
        if (result == VFS_OK && transferred <= (u64)-1 - handle->offset)
            handle->offset += transferred;
    }
    *out_written = transferred;
    (void)mutex_unlock(&handle->offset_lock);
    return result;
}

u64 vfs_file_write(vfs_file_handle_t *handle, u64 size, const void *buffer)
{
    u64 written = 0;
    (void)vfs_file_write_status(handle, size, buffer, &written);
    return written;
}

u64 vfs_file_write_trusted(vfs_file_handle_t *handle, u64 size,
                           const void *buffer) {
    u64 transferred = 0, pin = 0;
    int result;

    if (!handle || !mutex_lock(&handle->offset_lock) ||
        !vfs_handle_valid(handle) || !(handle->flags & VFS_OPEN_WRITE) ||
        (size != 0 && !buffer) || !handle->node->ops ||
        (!handle->node->ops->write && !handle->node->ops->write_pinned) ||
        size == 0) {
        if (handle) (void)mutex_unlock(&handle->offset_lock);
        return 0;
    }
    if (!vfs_super_operation_lock(handle->super)) {
        (void)mutex_unlock(&handle->offset_lock);
        return 0;
    }
    result = vfs_write_dispatch(handle->node, handle->offset, size, buffer,
                                &transferred, &pin);
    vfs_super_operation_unlock(handle->super);
    result = vfs_operation_finish_pin(handle->super, result, pin);
    if (result == VFS_ERR_UNCERTAIN) transferred = 0;
    if (transferred <= (u64)-1 - handle->offset) handle->offset += transferred;
    (void)mutex_unlock(&handle->offset_lock);
    return transferred;
}

int vfs_close_trusted(vfs_file_handle_t *handle)
{
    return vfs_close(handle);
}

int vfs_seek(vfs_file_handle_t *handle, i64 offset, int whence, u64 *out_offset) {
    u64 base, target, magnitude;

    if (!handle || !mutex_lock(&handle->offset_lock))
        return VFS_ERR_INVALID_PARAM;
    if (handle->valid != VFS_FILE_HANDLE_VALID || !handle->node ||
        !handle->super) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_super_is_live(handle->super)) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_DEVICE_GONE;
    }
    if ((whence != VFS_SEEK_SET && whence != VFS_SEEK_CUR &&
         whence != VFS_SEEK_END)) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_INVALID_PARAM;
    }

    if (whence == VFS_SEEK_SET) {
        if (offset < 0) {
            (void)mutex_unlock(&handle->offset_lock);
            return VFS_ERR_INVALID_PARAM;
        }
        target = (u64)offset;
    } else {
        if (whence == VFS_SEEK_END &&
            !vfs_super_operation_lock(handle->super)) {
            (void)mutex_unlock(&handle->offset_lock);
            return VFS_ERR_DEVICE_GONE;
        }
        base = (whence == VFS_SEEK_CUR) ? handle->offset : handle->node->size;
        if (whence == VFS_SEEK_END)
            vfs_super_operation_unlock(handle->super);
        if (offset >= 0) {
            if (base > (u64)-1 - (u64)offset) {
                (void)mutex_unlock(&handle->offset_lock);
                return VFS_ERR_INVALID_PARAM;
            }
            target = base + (u64)offset;
        } else {
            magnitude = (u64)(-(offset + 1)) + 1;
            if (magnitude > base) {
                (void)mutex_unlock(&handle->offset_lock);
                return VFS_ERR_INVALID_PARAM;
            }
            target = base - magnitude;
        }
    }

    handle->offset = target;
    if (out_offset) *out_offset = target;
    (void)mutex_unlock(&handle->offset_lock);
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

/* Optional create/mkdir transactions return a private node and resolved pin.
 * Keep the operation reference through handoff and any private-node cleanup. */
static int vfs_create_node(vfs_node_t *dir, const char *name,
                            vfs_node_t **out_node, bool directory)
{
    vfs_super_t *sb;
    vfs_node_t *node = NULL;
    bool registered = false;
    u64 pin = 0U;
    int result;
    int (*pinned)(vfs_node_t *, vfs_node_t *, const char *, u64 *);
    int (*legacy)(vfs_node_t *, const char *, vfs_node_t **);

    if (!out_node) return VFS_ERR_INVALID_PARAM;
    *out_node = NULL;
    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops)
        return VFS_ERR_INVALID_PARAM;
    pinned = directory ? dir->ops->mkdir_pinned : dir->ops->create_pinned;
    legacy = directory ? dir->ops->mkdir : dir->ops->create;
    if (!pinned && !legacy) return VFS_ERR_INVALID_PARAM;
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    /* Authorization is checked under the same lock as collision checks and
     * publication. The driver validates its current parent record as well. */
    if (vfs_node_on_read_only_mount(dir) ||
        !vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        result = VFS_ERR_ACCESS_DENIED;
        goto unlock;
    }
    if (!pinned) {
        result = legacy(dir, name, out_node);
        goto unlock;
    }
    if (!dir->ops->discard_created_node) {
        result = VFS_ERR_UNSUPPORTED;
        goto unlock;
    }
    node = (vfs_node_t *)kmalloc(sizeof(*node));
    if (!node) { result = VFS_ERR_NO_MEM; goto unlock; }
    memset(node, 0, sizeof(*node));
    u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
    bool mount_child = vfs_find_mount_child_locked(dir, name) != NULL;
    spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    result = mount_child ? VFS_ERR_ALREADY_EXISTS : pinned(dir, node, name, &pin);
    if (result == VFS_OK) {
        if (!pin || node->super != sb || !node->inode || !node->ops ||
            node->type != (directory ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE))
            result = VFS_ERR_UNCERTAIN;
        else if (!vfs_node_register_created(node, sb))
            result = VFS_ERR_DEVICE_GONE;
        else registered = true;
    }
    if (result == VFS_ERR_UNCERTAIN && pin) {
        (void)vfs_recovery_pin_mark_unresolved(pin);
        pin = 0U;
    }
unlock:
    vfs_super_operation_unlock(sb);
    result = vfs_operation_finish_pin(sb, result, pin);
    if (result == VFS_OK && node) {
        /* Completed handoff makes this a public cached node: a concurrent
         * lookup may already hold it. Late removal must not free that node. */
        if (vfs_super_is_live(sb)) *out_node = node;
        else result = VFS_ERR_DEVICE_GONE;
        node = NULL; /* Its lifetime now follows the mounted node cache. */
    }
    if (node) {
        /* A retained operation reference keeps driver state alive even
         * after detach. Cleanup must serialize with driver node lookup. */
        if (mutex_lock(&sb->operation_lock)) {
            if (registered) vfs_node_unregister_created(node, sb);
            if (node->fs_data)
                dir->ops->discard_created_node(node, sb);
            (void)mutex_unlock(&sb->operation_lock);
            kfree(node);
        } else result = VFS_ERR_UNCERTAIN;
    }
    vfs_super_release(sb);
    return result;
}

int vfs_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node)
{
    return vfs_create_node(dir, name, out_node, false);
}

int vfs_create_owned(vfs_node_t *dir, const char *name, u32 owner_uid,
                     u32 permissions, vfs_node_t **out_node) {
    vfs_super_t *sb;
    int result;

    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !out_node ||
        !dir->ops || !dir->ops->create_owned ||
        (permissions & ~VFS_PERMISSION_KNOWN) != 0U || !permissions) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir)) return VFS_ERR_ACCESS_DENIED;
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    result = dir->ops->create_owned(dir, name, owner_uid, permissions,
                                    out_node);
    vfs_node_operation_end(sb);
    return result;
}

int vfs_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node)
{
    return vfs_create_node(dir, name, out_node, true);
}

int vfs_mkdir_owned(vfs_node_t *dir, const char *name, u32 owner_uid,
                    u32 permissions, vfs_node_t **out_node) {
    vfs_super_t *sb;
    int result;

    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !out_node ||
        !dir->ops || !dir->ops->mkdir_owned ||
        (permissions & ~VFS_PERMISSION_KNOWN) != 0U || !permissions) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir)) return VFS_ERR_ACCESS_DENIED;
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    result = dir->ops->mkdir_owned(dir, name, owner_uid, permissions,
                                    out_node);
    vfs_node_operation_end(sb);
    return result;
}

static int vfs_unlink_locked(vfs_node_t *dir, const char *name, u64 *pin)
{
    *pin = 0U;
    return dir->ops->unlink_pinned
        ? dir->ops->unlink_pinned(dir, name, pin)
        : dir->ops->unlink(dir, name);
}

static int vfs_operation_finish_pin(vfs_super_t *sb, int result, u64 pin)
{
    int finish;
    if (!pin) return result;
    do {
        finish = vfs_recovery_pin_finish_handoff(pin);
        if (finish == VFS_ERR_BUSY) {
            if (!mutex_lock(&sb->operation_lock)) break;
            (void)mutex_unlock(&sb->operation_lock);
        }
    } while (finish == VFS_ERR_BUSY);
    if (finish != VFS_OK) {
        if (finish == VFS_ERR_BUSY)
            (void)vfs_recovery_pin_mark_unresolved(pin);
        return VFS_ERR_UNCERTAIN;
    }
    return result;
}

int vfs_unlink(vfs_node_t *dir, const char *name) {
    vfs_super_t *sb;
    vfs_node_t *child;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->unlink) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir) ||
        !vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    child = vfs_finddir_locked(dir, name);
    result = child && !vfs_directory_child_mutation_allowed(dir, child)
        ? VFS_ERR_ACCESS_DENIED : vfs_unlink_locked(dir, name, &pin);
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

int vfs_unlink_trusted(vfs_node_t *dir, const char *name) {
    vfs_super_t *sb;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->unlink) return VFS_ERR_INVALID_PARAM;
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    result = vfs_unlink_locked(dir, name, &pin);
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

static int vfs_rmdir_locked(vfs_node_t *dir, const char *name, u64 *pin)
{
    *pin = 0U;
    return dir->ops->rmdir_pinned
        ? dir->ops->rmdir_pinned(dir, name, pin)
        : dir->ops->rmdir(dir, name);
}

int vfs_rmdir(vfs_node_t *dir, const char *name) {
    vfs_super_t *sb;
    vfs_node_t *child;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->rmdir) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir) ||
        !vfs_check_access(dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    child = vfs_finddir_locked(dir, name);
    result = child && !vfs_directory_child_mutation_allowed(dir, child)
        ? VFS_ERR_ACCESS_DENIED : vfs_rmdir_locked(dir, name, &pin);
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

int vfs_rmdir_trusted(vfs_node_t *dir, const char *name) {
    vfs_super_t *sb;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(dir) || !name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->rmdir) return VFS_ERR_INVALID_PARAM;
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    result = vfs_rmdir_locked(dir, name, &pin);
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

static int vfs_rename_locked(vfs_super_t *sb, vfs_node_t *src_dir,
                             const char *src_name,
                             vfs_node_t *dst_dir, const char *dst_name,
                             u64 *pin)
{
    *pin = 0U;
    return src_dir->ops->rename_pinned
        ? src_dir->ops->rename_pinned(sb, src_dir, src_name, dst_dir, dst_name, pin)
        : src_dir->ops->rename(src_dir, src_name, dst_dir, dst_name);
}

int vfs_rename(vfs_node_t *src_dir, const char *src_name,
               vfs_node_t *dst_dir, const char *dst_name) {
    vfs_super_t *sb;
    vfs_node_t *source;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(src_dir) || !src_name ||
        !vfs_node_is_live(dst_dir) || !dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY ||
        !src_dir->ops || (!src_dir->ops->rename && !src_dir->ops->rename_pinned)) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(src_dir) ||
        vfs_node_on_read_only_mount(dst_dir) ||
        !vfs_check_access(src_dir, VFS_ACCESS_WRITE) ||
        !vfs_check_access(dst_dir, VFS_ACCESS_WRITE)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    if (src_dir->super != dst_dir->super) return VFS_ERR_UNSUPPORTED;
    if (!vfs_node_operation_begin(src_dir, &sb)) return VFS_ERR_DEVICE_GONE;
    source = vfs_finddir_locked(src_dir, src_name);
    result = source && !vfs_directory_child_mutation_allowed(src_dir, source)
        ? VFS_ERR_ACCESS_DENIED
        : vfs_rename_locked(sb, src_dir, src_name, dst_dir, dst_name, &pin);
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

static bool vfs_node_identity_matches(const vfs_node_t *node,
                                      vfs_super_t *expected_super,
                                      u64 expected_inode)
{
    return node && expected_super && node->super == expected_super &&
           node->inode == expected_inode;
}

static int vfs_unlink_expected_internal(
    vfs_node_t *dir, const char *name, vfs_super_t *expected_super,
    u64 expected_inode, bool authorized)
{
    vfs_super_t *sb;
    vfs_node_t *actual;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(dir) || !name || !*name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->unlink || expected_super != dir->super) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir)) return VFS_ERR_ACCESS_DENIED;
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    actual = vfs_finddir_locked(dir, name);
    if (!vfs_node_identity_matches(actual, expected_super, expected_inode) ||
        actual->type != VFS_TYPE_FILE ||
        (!authorized && !vfs_directory_child_mutation_allowed(dir, actual))) {
        result = VFS_ERR_ACCESS_DENIED;
    } else {
        result = vfs_unlink_locked(dir, name, &pin);
    }
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

int vfs_unlink_expected(vfs_node_t *dir, const char *name,
                        vfs_super_t *expected_super, u64 expected_inode)
{
    return vfs_unlink_expected_internal(dir, name, expected_super,
                                        expected_inode, false);
}

int vfs_unlink_expected_authorized(vfs_node_t *dir, const char *name,
                                   vfs_super_t *expected_super,
                                   u64 expected_inode)
{
    return vfs_unlink_expected_internal(dir, name, expected_super,
                                        expected_inode, true);
}

static int vfs_rmdir_expected_internal(
    vfs_node_t *dir, const char *name, vfs_super_t *expected_super,
    u64 expected_inode, bool authorized)
{
    vfs_super_t *sb;
    vfs_node_t *actual;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(dir) || !name || !*name ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->rmdir || expected_super != dir->super) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(dir)) return VFS_ERR_ACCESS_DENIED;
    if (!vfs_node_operation_begin(dir, &sb)) return VFS_ERR_DEVICE_GONE;
    actual = vfs_finddir_locked(dir, name);
    if (!vfs_node_identity_matches(actual, expected_super, expected_inode) ||
        actual->type != VFS_TYPE_DIRECTORY ||
        (!authorized && !vfs_directory_child_mutation_allowed(dir, actual))) {
        result = VFS_ERR_ACCESS_DENIED;
    } else {
        result = vfs_rmdir_locked(dir, name, &pin);
    }
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

int vfs_rmdir_expected(vfs_node_t *dir, const char *name,
                       vfs_super_t *expected_super, u64 expected_inode)
{
    return vfs_rmdir_expected_internal(dir, name, expected_super,
                                       expected_inode, false);
}

int vfs_rmdir_expected_authorized(vfs_node_t *dir, const char *name,
                                  vfs_super_t *expected_super,
                                  u64 expected_inode)
{
    return vfs_rmdir_expected_internal(dir, name, expected_super,
                                       expected_inode, true);
}

static int vfs_rename_expected_internal(
    vfs_node_t *src_dir, const char *src_name,
    vfs_super_t *expected_super, u64 expected_inode,
    vfs_node_t *dst_dir, const char *dst_name, bool authorized)
{
    vfs_super_t *sb;
    vfs_node_t *actual_source;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(src_dir) || !src_name || !*src_name ||
        !vfs_node_is_live(dst_dir) || !dst_name || !*dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY || !src_dir->ops ||
        (!src_dir->ops->rename && !src_dir->ops->rename_pinned) ||
        expected_super != src_dir->super ||
        src_dir->super != dst_dir->super) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (vfs_node_on_read_only_mount(src_dir) ||
        vfs_node_on_read_only_mount(dst_dir)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    if (!vfs_node_operation_begin(src_dir, &sb)) return VFS_ERR_DEVICE_GONE;
    actual_source = vfs_finddir_locked(src_dir, src_name);
    if (!vfs_node_identity_matches(actual_source, expected_super,
                                   expected_inode) ||
        (!authorized && (!vfs_check_access(src_dir, VFS_ACCESS_WRITE) ||
                         !vfs_check_access(dst_dir, VFS_ACCESS_WRITE) ||
                         !vfs_directory_child_mutation_allowed(src_dir,
                                                               actual_source)))) {
        result = VFS_ERR_ACCESS_DENIED;
    } else {
        vfs_node_t *destination = vfs_finddir_locked(dst_dir, dst_name);
        result = destination && !vfs_node_identity_matches(destination,
            expected_super, expected_inode) ? VFS_ERR_ALREADY_EXISTS :
            vfs_rename_locked(sb, src_dir, src_name, dst_dir, dst_name, &pin);
    }
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

int vfs_rename_expected(vfs_node_t *src_dir, const char *src_name,
                        vfs_super_t *expected_super, u64 expected_inode,
                        vfs_node_t *dst_dir, const char *dst_name)
{
    return vfs_rename_expected_internal(src_dir, src_name, expected_super,
                                        expected_inode, dst_dir, dst_name,
                                        false);
}

int vfs_rename_expected_authorized(
    vfs_node_t *src_dir, const char *src_name,
    vfs_super_t *expected_super, u64 expected_inode,
    vfs_node_t *dst_dir, const char *dst_name)
{
    return vfs_rename_expected_internal(src_dir, src_name, expected_super,
                                        expected_inode, dst_dir, dst_name,
                                        true);
}

int vfs_rename_trusted(vfs_node_t *src_dir, const char *src_name,
                       vfs_node_t *dst_dir, const char *dst_name) {
    vfs_super_t *sb;
    int result;
    u64 pin = 0U;

    if (!vfs_node_is_live(src_dir) || !src_name ||
        !vfs_node_is_live(dst_dir) || !dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY || !src_dir->ops ||
        (!src_dir->ops->rename && !src_dir->ops->rename_pinned)) return VFS_ERR_INVALID_PARAM;
    if (src_dir->super != dst_dir->super) return VFS_ERR_UNSUPPORTED;
    if (vfs_node_on_read_only_mount(src_dir) ||
        vfs_node_on_read_only_mount(dst_dir)) return VFS_ERR_ACCESS_DENIED;
    if (!vfs_node_operation_begin(src_dir, &sb)) return VFS_ERR_DEVICE_GONE;
    result = vfs_rename_locked(sb, src_dir, src_name, dst_dir, dst_name, &pin);
    vfs_node_operation_end(sb);
    return vfs_operation_finish_pin(sb, result, pin);
}

int vfs_file_append_lines(vfs_file_handle_t *handle, const char *data,
                          usize length, u32 max_lines, u32 flags, mg_file_lines_state_t *state)
{
    int result;

    if (!handle || !mutex_lock(&handle->offset_lock))
        return VFS_ERR_INVALID_PARAM;
    if (!vfs_handle_valid(handle)) {
        result = VFS_ERR_DEVICE_GONE;
    } else if ((handle->flags & VFS_OPEN_RDWR) != VFS_OPEN_RDWR ||
               !vfs_check_access(handle->node, VFS_ACCESS_READ_WRITE) ||
               vfs_node_on_read_only_mount(handle->node)) {
        result = VFS_ERR_ACCESS_DENIED;
    } else if (!handle->node->ops || !handle->node->ops->append_lines) {
        result = VFS_ERR_UNSUPPORTED;
    } else if (!vfs_super_operation_lock(handle->super)) {
        result = VFS_ERR_DEVICE_GONE;
    } else {
        result = handle->node->ops->append_lines(handle->node, data, length,
                                                max_lines, flags, state);
        vfs_super_operation_unlock(handle->super);
    }
    (void)mutex_unlock(&handle->offset_lock);
    return result;
}

static int vfs_truncate_dispatch(vfs_node_t *node, u64 length, u64 *out_pin)
{
    int result;
    *out_pin = 0;
    if (node->ops->resize_pinned)
        result = node->ops->resize_pinned(node, length, out_pin);
    else {
        if (length) return VFS_ERR_UNSUPPORTED;
        if (!node->ops->truncate_pinned) return node->ops->truncate(node);
        result = node->ops->truncate_pinned(node, out_pin);
    }
    if (result > VFS_OK) result = VFS_ERR_UNCERTAIN;
    if (result == VFS_ERR_UNCERTAIN && *out_pin) {
        (void)vfs_recovery_pin_mark_unresolved(*out_pin);
        *out_pin = 0;
    }
    return result;
}

int vfs_resize_handle(vfs_file_handle_t *handle, u64 length) {
    vfs_node_t *node;
    int result;
    u64 pin = 0;

    if (!handle || !mutex_lock(&handle->offset_lock))
        return VFS_ERR_INVALID_PARAM;
    if (handle->valid != VFS_FILE_HANDLE_VALID || !handle->node ||
        !handle->super) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_INVALID_PARAM;
    }
    if (!vfs_super_is_live(handle->super)) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_DEVICE_GONE;
    }
    if (!(handle->flags & VFS_OPEN_WRITE)) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_INVALID_PARAM;
    }
    node = handle->node;
    if (node->type != VFS_TYPE_FILE) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_INVALID_PARAM;
    }
    if (!node->ops || (!node->ops->truncate && !node->ops->truncate_pinned &&
                      !node->ops->resize_pinned)) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_UNSUPPORTED;
    }
    if (vfs_node_on_read_only_mount(node) ||
        (!vfs_handle_authorized(handle, VFS_ACCESS_WRITE) &&
         !vfs_check_access(node, VFS_ACCESS_WRITE))) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_ACCESS_DENIED;
    }
    if (!vfs_super_operation_lock(handle->super)) {
        (void)mutex_unlock(&handle->offset_lock);
        return VFS_ERR_DEVICE_GONE;
    }
    result = vfs_truncate_dispatch(node, length, &pin);
    vfs_super_operation_unlock(handle->super);
    result = vfs_operation_finish_pin(handle->super, result, pin);
    (void)mutex_unlock(&handle->offset_lock);
    return result;
}

int vfs_truncate_handle(vfs_file_handle_t *handle) {
    return vfs_resize_handle(handle, 0U);
}

int vfs_truncate(vfs_node_t *node) {
    vfs_super_t *sb;
    int result;
    u64 pin = 0;

    if (!node || node->type != VFS_TYPE_FILE || !node->ops ||
        (!node->ops->truncate && !node->ops->truncate_pinned && !node->ops->resize_pinned))
        return VFS_ERR_INVALID_PARAM;
    if (vfs_node_on_read_only_mount(node) ||
        !vfs_check_access(node, VFS_ACCESS_WRITE))
        return VFS_ERR_ACCESS_DENIED;
    if (!vfs_node_operation_begin(node, &sb)) return VFS_ERR_DEVICE_GONE;
    result = vfs_truncate_dispatch(node, 0U, &pin);
    vfs_node_operation_end(sb);
    return pin ? vfs_operation_finish_pin(sb, result, pin) : result;
}

int vfs_truncate_trusted(vfs_node_t *node) {
    vfs_super_t *sb;
    int result;
    u64 pin = 0;

    if (!vfs_node_is_live(node) || node->type != VFS_TYPE_FILE || !node->ops ||
        (!node->ops->truncate && !node->ops->truncate_pinned && !node->ops->resize_pinned))
        return VFS_ERR_INVALID_PARAM;
    if (!vfs_node_operation_begin(node, &sb)) return VFS_ERR_DEVICE_GONE;
    result = vfs_truncate_dispatch(node, 0U, &pin);
    vfs_node_operation_end(sb);
    return pin ? vfs_operation_finish_pin(sb, result, pin) : result;
}

u64 vfs_read(vfs_node_t *node, u64 offset, u64 size, void *buffer) {
    vfs_super_t *sb;
    u64 result;

    if (!vfs_node_is_live(node) || !buffer || !node->ops || !node->ops->read) {
        return 0;
    }
    if (!vfs_check_access(node, VFS_ACCESS_READ)) return 0;
    if (!vfs_node_operation_begin(node, &sb)) return 0;
    result = node->ops->read(node, offset, size, buffer);
    vfs_node_operation_end(sb);
    return result;
}

u64 vfs_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer) {
    vfs_super_t *sb;
    u64 written = 0, pin = 0;
    int result;

    if (!vfs_node_is_live(node) || !buffer || !node->ops ||
        (!node->ops->write && !node->ops->write_pinned)) {
        return 0;
    }
    if (vfs_node_on_read_only_mount(node) ||
        !vfs_check_access(node, VFS_ACCESS_WRITE)) return 0;
    if (!size) return 0;
    if (!vfs_node_operation_begin(node, &sb)) return 0;
    result = vfs_write_dispatch(node, offset, size, buffer, &written, &pin);
    vfs_node_operation_end(sb);
    if (pin) result = vfs_operation_finish_pin(sb, result, pin);
    return result == VFS_ERR_UNCERTAIN ? 0 : written;
}

vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name) {
    return vfs_finddir_internal(dir, name, true);
}

vfs_node_t *vfs_finddir_trusted(vfs_node_t *dir, const char *name) {
    return vfs_finddir_internal(dir, name, false);
}

typedef struct {
    char name[VFS_MOUNT_PATH_MAX];
    u64 inode;
} vfs_mount_dirent_snapshot_t;

static bool vfs_readdir_internal(vfs_node_t *dir, u32 index,
                                 vfs_dirent_t *out_entry, bool enforce) {
    u32 base_count;
    u32 mount_index;
    u32 mount_count = 0;
    bool result = false;
    vfs_super_t *sb = NULL;
    vfs_mount_dirent_snapshot_t *mounts = NULL;

    if (!vfs_node_is_live(dir) || !out_entry ||
        dir->type != VFS_TYPE_DIRECTORY || !dir->ops ||
        !dir->ops->readdir) return false;
    if (enforce && !vfs_check_access(dir, VFS_ACCESS_READ)) return false;
    if (!vfs_node_operation_begin(dir, &sb)) return false;

    if (dir->ops->readdir(dir, index, out_entry)) {
        result = true;
        goto done;
    }
    base_count = 0;
    while (dir->ops->readdir(dir, base_count, out_entry)) base_count++;
    if (index < base_count) goto done;

    mounts = (vfs_mount_dirent_snapshot_t *)kmalloc(
        sizeof(*mounts) * VFS_MAX_MOUNTS);
    if (!mounts) goto done;

    /* Snapshot mount names while holding only VFS metadata state.  Filesystem
     * callbacks below may sleep and must never run under this spinlock. */
    {
        u64 flags = spin_lock_irqsave(&vfs_metadata_lock);
        for (u32 i = 1; i < VFS_MAX_MOUNTS; i++) {
            vfs_mount_t *mount = &mount_table[i];
            if (!vfs_mount_parent_matches(mount, dir)) continue;
            strncpy(mounts[mount_count].name, mount->name,
                    sizeof(mounts[mount_count].name) - 1U);
            mounts[mount_count].name[
                sizeof(mounts[mount_count].name) - 1U] = '\0';
            mounts[mount_count].inode = mount->sb && mount->sb->root_node
                ? mount->sb->root_node->inode : 0;
            mount_count++;
        }
        spin_unlock_irqrestore(&vfs_metadata_lock, flags);
    }

    mount_index = 0;
    for (u32 i = 0; i < mount_count; i++) {
        bool already_present = false;
        for (u32 base_index = 0; base_index < base_count; base_index++) {
            vfs_dirent_t base_entry;
            if (!dir->ops->readdir(dir, base_index, &base_entry)) break;
            if (strcmp(base_entry.name, mounts[i].name) == 0) {
                already_present = true;
                break;
            }
        }
        if (already_present) continue;
        if (mount_index == index - base_count) {
            strncpy(out_entry->name, mounts[i].name,
                    sizeof(out_entry->name) - 1U);
            out_entry->name[sizeof(out_entry->name) - 1U] = '\0';
            out_entry->inode = mounts[i].inode;
            out_entry->type = VFS_TYPE_DIRECTORY;
            result = true;
            break;
        }
        mount_index++;
    }

done:
    if (mounts) kfree(mounts);
    vfs_node_operation_end(sb);
    return result;
}

bool vfs_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    return vfs_readdir_internal(dir, index, out_entry, true);
}

bool vfs_readdir_trusted(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    return vfs_readdir_internal(dir, index, out_entry, false);
}

int vfs_file_snapshot(vfs_file_handle_t *handle, char *data, usize capacity,
                       mg_file_lines_state_t *state)
{
    int result;
    vfs_file_facts_t facts = {0};
    bool has_facts;
    if (!handle || !state || !mutex_lock(&handle->offset_lock))
        return VFS_ERR_INVALID_PARAM;
    if (!vfs_handle_valid(handle)) result = VFS_ERR_DEVICE_GONE;
    else if (!(handle->flags & VFS_OPEN_READ))
        result = VFS_ERR_ACCESS_DENIED;
    else if (handle->node->type != VFS_TYPE_FILE || !handle->node->ops ||
             !handle->node->ops->snapshot)
        result = VFS_ERR_UNSUPPORTED;
    else if (!vfs_super_operation_lock(handle->super)) result = VFS_ERR_DEVICE_GONE;
    else {
        has_facts = handle->node->ops->file_facts != NULL;
        result = has_facts
            ? handle->node->ops->file_facts(handle->node, &facts) : VFS_OK;
        if (result == VFS_OK && has_facts &&
            (facts.record_id != handle->node->inode ||
             !facts.generation ||
             (facts.permissions & ~VFS_PERMISSION_KNOWN) ||
             !facts.permissions))
            result = VFS_ERR_BAD_FORMAT;
        if (result == VFS_OK) {
            bool allowed;
            if (!has_facts) allowed = vfs_check_access(handle->node, VFS_ACCESS_READ);
            else {
                process_t *process = process_current();
                process_credentials_t credentials;
                allowed = !process ||
                    (process_get_credentials(process, &credentials) &&
                     ((credentials.uid == facts.owner_uid &&
                       (facts.permissions & VFS_PERMISSION_OWNER_READ)) ||
                      (credentials.uid != facts.owner_uid &&
                       (facts.permissions & VFS_PERMISSION_OTHER_READ))));
            }
            if (!allowed) result = VFS_ERR_ACCESS_DENIED;
        }
        if (result == VFS_OK)
            result = handle->node->ops->snapshot(handle->node, data, capacity, state);
        if ((result == VFS_OK || result == VFS_ERR_IO) &&
            !vfs_super_is_live(handle->super))
            result = VFS_ERR_DEVICE_GONE;
        if (result == VFS_OK && state->length > capacity)
            result = VFS_ERR_BAD_FORMAT;
        if (result == VFS_OK && has_facts) {
            if (state->identifier != facts.record_id ||
                state->revision != facts.generation ||
                state->length != facts.size)
                result = VFS_ERR_BAD_FORMAT;
            else {
                handle->save_record_id = facts.record_id;
                handle->save_generation = facts.generation;
                handle->save_expectation_valid = true;
                handle->save_expectation_error = VFS_OK;
            }
        }
        vfs_super_operation_unlock(handle->super);
    }
    (void)mutex_unlock(&handle->offset_lock);
    return result;
}

/* Data and result metadata are delivered before adopting the generation.
 * A sink can leave partial user output on failure, but cannot authorize a
 * later conditional save from those incomplete bytes. */
int vfs_file_snapshot_stream(vfs_file_handle_t *handle, usize capacity,
                             vfs_snapshot_sink_t sink,
                             vfs_snapshot_finish_t finish, void *context)
{
    int result;
    vfs_file_facts_t facts = {0};
    mg_file_lines_state_t state = {0};
    process_t *process;
    process_credentials_t credentials;

    if (!handle || !sink || !finish || !capacity ||
        capacity > MG_FILE_CONTENT_MAX_BYTES ||
        !mutex_lock(&handle->offset_lock)) return VFS_ERR_INVALID_PARAM;
    if (!vfs_handle_valid(handle)) result = VFS_ERR_DEVICE_GONE;
    else if (!(handle->flags & VFS_OPEN_READ)) result = VFS_ERR_ACCESS_DENIED;
    else if (handle->node->type != VFS_TYPE_FILE || !handle->node->ops ||
             !handle->node->ops->snapshot_stream ||
             !handle->node->ops->file_facts) result = VFS_ERR_UNSUPPORTED;
    else if (!vfs_super_operation_lock(handle->super))
        result = VFS_ERR_DEVICE_GONE;
    else {
        result = handle->node->ops->file_facts(handle->node, &facts);
        if (result == VFS_OK &&
            (facts.record_id != handle->node->inode || !facts.generation ||
             (facts.permissions & ~VFS_PERMISSION_KNOWN) ||
             !facts.permissions)) result = VFS_ERR_BAD_FORMAT;
        process = process_current();
        if (result == VFS_OK && process &&
            (!process_get_credentials(process, &credentials) ||
             !((credentials.uid == facts.owner_uid &&
                (facts.permissions & VFS_PERMISSION_OWNER_READ)) ||
               (credentials.uid != facts.owner_uid &&
                (facts.permissions & VFS_PERMISSION_OTHER_READ)))))
            result = VFS_ERR_ACCESS_DENIED;
        if (result == VFS_OK)
            result = handle->node->ops->snapshot_stream(
                handle->node, capacity, sink, context, &state);
        if ((result == VFS_OK || result == VFS_ERR_IO) &&
            !vfs_super_is_live(handle->super)) result = VFS_ERR_DEVICE_GONE;
        if (result == VFS_OK &&
            (state.identifier != facts.record_id ||
             state.revision != facts.generation ||
             state.length != facts.size || state.length > capacity))
            result = VFS_ERR_BAD_FORMAT;
        if (result == VFS_OK) result = finish(context, &state);
        if (result == VFS_OK) {
            handle->save_record_id = facts.record_id;
            handle->save_generation = facts.generation;
            handle->save_expectation_valid = true;
            handle->save_expectation_error = VFS_OK;
        }
        vfs_super_operation_unlock(handle->super);
    }
    (void)mutex_unlock(&handle->offset_lock);
    return result;
}
