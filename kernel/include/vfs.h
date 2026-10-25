/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <block.h>
#include <mutex.h>
#include <identity.h>
#include <mg/filesystem.h>

#define VFS_OK                    0
#define VFS_ERR_INVALID_PARAM   (-1)
#define VFS_ERR_NOT_FOUND       (-2)
#define VFS_ERR_IO              (-3)
#define VFS_ERR_NO_MEM          (-4)
#define VFS_ERR_BAD_FORMAT      (-5)
#define VFS_ERR_UNSUPPORTED     (-6)
#define VFS_ERR_NOT_EMPTY       (-7)
#define VFS_ERR_ACCESS_DENIED   (-8)
#define VFS_ERR_NOT_DIRECTORY   (-9)
#define VFS_ERR_BUSY           (-10)
#define VFS_ERR_DEVICE_GONE   (-11)
#define VFS_ERR_NO_SPACE       (-12)

#define VFS_ERR_ALREADY_EXISTS (-13)
#define VFS_ERR_UNCERTAIN      (-14)
#define VFS_ERR_BUFFER_TOO_SMALL (-15)
#define VFS_ERR_STALE_GENERATION (-16)
#define VFS_ERR_NAME_TOO_LONG    (-17)
#define VFS_ERR_CANCELLED        (-18)
#define VFS_ERR_TIME_UNAVAILABLE (-19)

#define VFS_UID_SYSTEM          0U

#define VFS_PERMISSION_OWNER_READ   (1U << 0)
#define VFS_PERMISSION_OWNER_WRITE  (1U << 1)
#define VFS_PERMISSION_OTHER_READ   (1U << 2)
#define VFS_PERMISSION_OTHER_WRITE  (1U << 3)
#define VFS_PERMISSION_KNOWN        (VFS_PERMISSION_OWNER_READ | \
                                     VFS_PERMISSION_OWNER_WRITE | \
                                     VFS_PERMISSION_OTHER_READ | \
                                     VFS_PERMISSION_OTHER_WRITE)

/* Access requests use operation bits; stored permissions use owner/other
 * bits above. */
#define VFS_ACCESS_READ          (1U << 0)
#define VFS_ACCESS_WRITE         (1U << 1)
#define VFS_ACCESS_READ_WRITE    (VFS_ACCESS_READ | VFS_ACCESS_WRITE)

#define VFS_DEFAULT_SYSTEM_PERMISSIONS \
    (VFS_PERMISSION_OWNER_READ | VFS_PERMISSION_OWNER_WRITE | \
     VFS_PERMISSION_OTHER_READ)
#define VFS_DEFAULT_USER_PERMISSIONS \
    (VFS_PERMISSION_OWNER_READ | VFS_PERMISSION_OWNER_WRITE)

/* Creation defaults are independent of the permissions on the containing
 * directory.  The legacy names above remain for explicit system/user paths. */
#define VFS_DEFAULT_FILE_PERMISSIONS VFS_DEFAULT_SYSTEM_PERMISSIONS
#define VFS_DEFAULT_DIRECTORY_PERMISSIONS VFS_DEFAULT_SYSTEM_PERMISSIONS

typedef enum {
    VFS_CHILD_MUTATION_OPEN = 0,
    VFS_CHILD_MUTATION_OWNER_RESTRICTED,
} vfs_child_mutation_policy_t;

#define VFS_OPEN_READ            0x0001U
#define VFS_OPEN_WRITE           0x0002U
#define VFS_OPEN_RDWR            (VFS_OPEN_READ | VFS_OPEN_WRITE)
#define VFS_OPEN_APPEND          0x0004U

#define VFS_SEEK_SET             0
#define VFS_SEEK_CUR             1
#define VFS_SEEK_END             2

#define VFS_MAX_MOUNTS 32
#define VFS_MAX_RECOVERY_PINS 32U
#define VFS_DIRECTORY_BATCH_MAX 32U
#define VFS_MOUNT_PATH_MAX 256U
/* Complete paths are admitted to the same bound at every public and internal
 * lookup boundary. Absolute rendering reports its own required NUL size. */
#define VFS_PATH_MAX_BYTES MG_PATH_MAX_BYTES
#define VFS_COMPONENT_MAX_BYTES MG_DIRECTORY_NAME_MAX_BYTES

typedef enum {
    VFS_TYPE_UNKNOWN = 0,
    VFS_TYPE_FILE,
    VFS_TYPE_DIRECTORY,
} vfs_node_type_t;

typedef struct {
    char name[MG_DIRECTORY_NAME_CAPACITY];
    u64 inode;
    vfs_node_type_t type;
} vfs_dirent_t;

typedef struct vfs_node vfs_node_t;
typedef struct vfs_super vfs_super_t;
typedef struct vfs_fs_type vfs_fs_type_t;
typedef struct vfs_file_handle vfs_file_handle_t;
typedef struct vfs_location vfs_location_t;

typedef enum {
    VFS_MOUNT_ROLE_ROOT = 0,
    VFS_MOUNT_ROLE_BOOT,
    VFS_MOUNT_ROLE_VOLUME,
} vfs_mount_role_t;

typedef enum {
    VFS_SUPER_ACTIVE = 0,
    VFS_SUPER_DETACHING,
    VFS_SUPER_DEAD,
} vfs_super_state_t;

typedef enum {
    VFS_RECOVERY_PIN_ACTIVE = 0,
    VFS_RECOVERY_PIN_UNRESOLVED,
} vfs_recovery_pin_state_t;

typedef struct {
    u64 id;
    u64 device_id;
    vfs_recovery_pin_state_t state;
    u64 physical_id;
    u32 flags;
} vfs_recovery_pin_info_t;
#define VFS_RECOVERY_DEVICE_ONLINE   (1U << 0)
#define VFS_RECOVERY_PHYSICAL_ONLINE (1U << 1)
#define VFS_RECOVERY_PRIVATE         (1U << 2)
#define VFS_RECOVERY_HAS_DEVICE      (1U << 3)

/* Authorization carried by one kernel-side file handle.  These scopes are
 * deliberately not process credentials or persistent object metadata. */
typedef enum {
    VFS_AUTH_NONE = 0,
    VFS_AUTH_CONFIGURATION_WRITE,
    VFS_AUTH_REGULAR_USER_DATA,
} vfs_authorization_scope_t;

/* Current, validated regular-file metadata. Filesystem hooks are called with
 * the owning superblock's operation lock held. */
typedef struct {
    u64 record_id;
    u64 generation;
    u64 size;
    u32 owner_uid;
    u32 permissions;
} vfs_file_facts_t;

/* Scalar node metadata copied while the owning superblock operation lock is
 * held. Callers receive no node or superblock pointer ownership. */
typedef struct {
    vfs_node_type_t type;
    u32 permissions;
    u64 size;
    u64 inode;
    u32 owner_uid;
} vfs_node_metadata_t;

/* Kernel-owned source; it must provide exactly the requested bytes while the
 * caller retains the file handle and the mount operation lock is held. */
typedef u32 (*vfs_replace_source_t)(void *context, u64 offset, u8 *buffer,
                                    u32 requested);
/* A snapshot sink may write partial output before returning an error. The
 * finalizer publishes result metadata; expectation binding follows it. */
typedef int (*vfs_snapshot_sink_t)(void *context, u64 offset,
                                   const u8 *data, usize length);
typedef int (*vfs_snapshot_finish_t)(void *context,
                                     const mg_file_lines_state_t *state);

/* Node Operations Table */
typedef struct {
    u64 (*read)(vfs_node_t *node, u64 offset, u64 size, void *buffer);
    /* Under operation_lock: confirmed prefix bytes are independent of status.
     * VFS advances only by that prefix. Zero/OK is EOF (or a zero request),
     * never a failed media operation. No recovery pin is needed for reads. */
    int (*read_checked)(vfs_node_t *node, u64 offset, u64 size,
                          void *buffer, u64 *out_bytes);
    u64 (*write)(vfs_node_t *node, u64 offset, u64 size, const void *buffer);
    /* Optional recovery-aware ordinary write. Called under operation_lock.
     * A positive confirmed byte count may accompany a definite error; VFS
     * reports those bytes as a partial write. On uncertainty, report no
     * confirmed bytes, retain an UNRESOLVED pin in the filesystem, and
     * return VFS_ERR_UNCERTAIN. A resolved ACTIVE pin is returned in out_pin
     * for VFS to finish after unlocking. The driver acquires it before its
     * first potentially persistent mutation and gates further mutations
     * until the handoff callback completes. */
    int (*write_pinned)(vfs_node_t *node, u64 offset, u64 size,
                        const void *buffer, u64 *out_bytes, u64 *out_pin);
    int (*file_facts)(vfs_node_t *node, vfs_file_facts_t *out);
    /* Called with operation_lock held. A resolved pin is returned for finish
     * after unlocking; uncertain outcomes retain their pin in the filesystem. */
    int (*replace)(vfs_super_t *sb, u64 record_id,
                   u64 expected_generation, u64 length,
                   vfs_replace_source_t source, void *context, u64 *out_pin);
    u64 replacement_max_bytes;
    /* Optional exclusive creation hook. Caller holds operation_lock and owns
     * all VFS return-object allocations before invoking it. The private
     * prepared_node may receive driver state on success; if so, the driver
     * supplies discard_created_node for failed VFS return paths. A failing
     * hook must leave prepared_node free of driver state. */
    int (*create_contents)(vfs_node_t *dir, vfs_node_t *prepared_node,
                           const char *name, u64 length,
                           vfs_replace_source_t source, void *context,
                           vfs_file_facts_t *out_facts, u64 *out_pin);
    /* Optional cleanup for driver-private state attached to a created node
     * when VFS cannot return that node after publication. */
    void (*discard_created_node)(vfs_node_t *node, vfs_super_t *sb);
    u64 create_contents_max_bytes;
    /* Legacy pointer-only inspection; error-capable drivers use checked lookup. */
    vfs_node_t *(*finddir)(vfs_node_t *dir, const char *name);
    /* Operation-locked lookup: OK and a live node, NOT_FOUND only after a
     * completed search, or a distinct error. Output is NULL on failure. */
    int (*finddir_checked)(vfs_node_t *dir, const char *name, vfs_node_t **out);
    bool (*readdir)(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry);
    /* Optional per-open sequential enumeration. */
    bool (*readdir_open)(vfs_node_t *dir, void **out_state);
    bool (*readdir_next)(void *state, vfs_dirent_t *out_entry);
    void (*readdir_close)(void *state);
    /* Optional checked enumeration: 1 entry, 0 EOF, negative VFS error.
     * Output is unchanged unless an entry is returned. NAME_TOO_LONG counts
     * as one indexed position and consumes one sequential entry; other
     * errors must not advance past an unreported entry. Operation lock held. */
    int (*readdir_checked)(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry);
    int (*readdir_next_checked)(void *state, vfs_dirent_t *out_entry);
    int (*create)(vfs_node_t *dir, const char *name, vfs_node_t **out_node);
    int (*mkdir)(vfs_node_t *dir, const char *name, vfs_node_t **out_node);
    /* Optional ordinary creation with recovery ownership. Called under the
     * mount operation lock with a private, zeroed, preallocated return node.
     * Success initializes that node completely; failure leaves it without
     * driver state. Resolved pins are finished after unlocking; uncertain
     * transactions retain their UNRESOLVED pins. As with create_contents,
     * lookups must not expose the node before completed handoff, and
     * discard_created_node releases private state under operation_lock on
     * failed return paths. Completed handoff transfers node lifetime to the
     * mounted cache, including when a subsequent detach prevents its return. */
    int (*create_pinned)(vfs_node_t *dir, vfs_node_t *prepared_node,
                         const char *name, u64 *out_pin);
    int (*mkdir_pinned)(vfs_node_t *dir, vfs_node_t *prepared_node,
                        const char *name, u64 *out_pin);
    /* Trusted owned creation uses the same preallocated-node/pin contract.
     * VFS enforces read-only mounts; the authorized caller supplies security
     * metadata instead of the ordinary current-credential write check. */
    int (*create_owned_pinned)(vfs_node_t *dir, vfs_node_t *prepared_node,
                               const char *name, u32 owner_uid,
                               u32 permissions, u64 *out_pin);
    int (*mkdir_owned_pinned)(vfs_node_t *dir, vfs_node_t *prepared_node,
                              const char *name, u32 owner_uid,
                              u32 permissions, u64 *out_pin);
    /* Trusted kernel creation path with explicit security metadata. */
    int (*create_owned)(vfs_node_t *dir, const char *name, u32 owner_uid,
                        u32 permissions, vfs_node_t **out_node);
    int (*mkdir_owned)(vfs_node_t *dir, const char *name, u32 owner_uid,
                       u32 permissions, vfs_node_t **out_node);
    int (*unlink)(vfs_node_t *dir, const char *name);
    /* Optional: leave a resolved recovery pin attached until VFS has
     * released operation_lock and completed its filesystem handoff. */
    int (*unlink_pinned)(vfs_node_t *dir, const char *name, u64 *out_pin);
    int (*rmdir)(vfs_node_t *dir, const char *name);
    /* Optional directory removal with the same post-unlock pin handoff. */
    int (*rmdir_pinned)(vfs_node_t *dir, const char *name, u64 *out_pin);
    int (*rename)(vfs_node_t *src_dir, const char *src_name,
                  vfs_node_t *dst_dir, const char *dst_name);
    /* Same post-operation-lock recovery ownership as unlink_pinned. Use the
     * explicitly retained sb: forced detach can retarget node->super. */
    int (*rename_pinned)(vfs_super_t *sb, vfs_node_t *src_dir, const char *src_name,
                         vfs_node_t *dst_dir, const char *dst_name,
                         u64 *out_pin);
    int (*truncate)(vfs_node_t *node);
    /* Same pin ownership and post-unlock handoff as write_pinned. */
    int (*truncate_pinned)(vfs_node_t *node, u64 *out_pin);
    /* Optional internal resize with the same recovery ownership. Zero is
     * truncate-to-empty; growth must expose zero-filled bytes. */
    int (*resize_pinned)(vfs_node_t *node, u64 length, u64 *out_pin);
    int (*append_lines)(vfs_node_t *node, const char *data, usize length,
                        u32 max_lines, u32 flags, mg_file_lines_state_t *state);
    /* Internal history mutation; same post-unlock recovery ownership as
     * write_pinned. The native ABI and merge/line semantics are unchanged. */
    int (*append_lines_pinned)(vfs_node_t *node, const char *data, usize length,
                               u32 max_lines, u32 flags,
                               mg_file_lines_state_t *state, u64 *out_pin);
    int (*snapshot)(vfs_node_t *node, char *data, usize capacity,
                    mg_file_lines_state_t *state);
    int (*snapshot_stream)(vfs_node_t *node, usize capacity,
                           vfs_snapshot_sink_t sink, void *context,
                           mg_file_lines_state_t *state);
} vfs_ops_t;

/* Superblock Operations Table */
typedef struct {
    int (*unmount)(vfs_super_t *sb);
    int (*sync)(vfs_super_t *sb);
} vfs_super_ops_t;

/* Filesystem Instance (Superblock) */
struct vfs_super {
    vfs_fs_type_t *fs_type;       // Pointer to driver plugin
    block_device_t *dev;          // Associated block device (or NULL for RAM)
    vfs_node_t *root_node;        // Root directory node of this instance
    void *private_data;           // Driver-private superblock state
    const vfs_super_ops_t *ops;   // Instance operations
    u64 device_id;                // Immutable backing block-device identity
    u32 reference_count;          // Handles, location leases, operations and pins
    vfs_super_state_t state;
    vfs_node_t *nodes;            // Nodes retained for safe dead-state retargeting
    vfs_location_t *locations;    // Weak namespace cache; external leases retain sb
    /* Serializes complete filesystem operations that may sleep in block I/O. */
    mutex_t operation_lock;
    /* Logical operation lifetime, including post-unlock pin handoff. Drivers
     * whose confidence remains gated until completion must opt in. Admission
     * precedes operation_lock; only its owning caller releases it. */
    bool serialize_handoff;
    mutex_t operation_guard;
    void *operation_guard_owner;
    int operation_error; /* Sticky uncertainty; never cleared by admission. */
    bool dispose_pending;
    /* Immutable mount policy, retained through detach and pin handoff.
     * Trusted operations bypass credentials, never this policy. */
    bool read_only;
    /* Private recovery owners are never namespace mounts. */
    bool recovery_private;
};

/* Filesystem Driver Registration Plugin (Static Kernel Lifetime) */
struct vfs_fs_type {
    const char *name;             // Driver plugin name, e.g. "fat32"
    bool (*probe)(block_device_t *dev);
    /* Optional bounded metadata read used by inspection/policy services. */
    bool (*label)(block_device_t *dev, char *out, usize capacity);
    int (*mount)(vfs_fs_type_t *fs_type, block_device_t *dev,
                 vfs_super_t **out_sb, bool read_only);
    vfs_fs_type_t *next;
};

/* Inode / Metadata Node */
struct vfs_node {
    u64 inode;
    vfs_node_type_t type;
    u64 size;
    u32 owner_uid;
    u32 permissions;
    /* Meaningful only for directories. */
    vfs_child_mutation_policy_t child_mutation_policy;
    u32 ref_count;
    vfs_super_t *super;           // Owning superblock instance
    void *fs_data;                // Driver-private node state
    const vfs_ops_t *ops;
    vfs_node_t *next_in_super;    // Kernel lifetime list for teardown retargeting
};

/* Kernel-side open instance; this is not a process file descriptor. */
struct vfs_file_handle {
    vfs_node_t *node;
    vfs_super_t *super;
    u64 offset;
    u32 flags;
    u32 valid;
    /* A single open instance may be shared by duplicated kernel objects. */
    mutex_t offset_lock;
    /* Narrow, kernel-created authorization bound to this exact node. */
    u32 authorized_access;
    vfs_authorization_scope_t authorization_scope;
    u32 authorization_owner_uid;
    /* Captured once at open for eligible files; ordinary writes and conflicts
     * never silently refresh this kernel-held save expectation. */
    u64 save_record_id;
    u64 save_generation;
    bool save_expectation_valid;
    int save_expectation_error;
};

/* Node-based Mount Entry */
typedef struct {
    vfs_super_t *sb;             // Mounted filesystem instance
    vfs_node_t *covered_node;    // Directory node covered by this mount (NULL for root "/")
    vfs_super_t *covered_super;  // Stable identity of covered_node's filesystem
    u64 covered_inode;           // Stable identity of covered_node within that FS
    vfs_super_t *parent_super;   // Parent directory identity for virtual mounts
    u64 parent_inode;
    block_device_t *dev;
    u64 dev_id;                   // Immutable backing device identity
    vfs_mount_role_t role;
    bool read_only;
    char mount_point[VFS_MOUNT_PATH_MAX];
    char name[VFS_MOUNT_PATH_MAX];
    bool active;                 // Slot active flag
} vfs_mount_t;

/* VFS Initialization & Mount Management API */
void vfs_init(void);
int vfs_register_fs(vfs_fs_type_t *fs_type);
vfs_fs_type_t *vfs_find_fs(const char *name);
/* Probe only the currently supported on-disk filesystem drivers.  This is a
 * read-only kernel helper; it never mounts or changes a device. */
const char *vfs_probe_filesystem(block_device_t *dev);
bool vfs_filesystem_label(block_device_t *dev, const char *fs_name,
                          char *out, usize capacity);

int vfs_mount_root(const char *fs_name, block_device_t *dev);
int vfs_mount_node(vfs_node_t *target_node, const char *fs_name, block_device_t *dev);
/* Mount at an existing directory or at a runtime-only child mount point.
 * The latter is used for /vol/<name> without persisting fake MGFS entries. */
int vfs_mount_path(vfs_node_t *parent, const char *name,
                   const char *mount_point, const char *fs_name,
                   block_device_t *dev, vfs_mount_role_t role,
                   bool read_only);
int vfs_unmount_sb(vfs_super_t *sb);
int vfs_unmount_device(block_device_t *device);
int vfs_unmount_device_preflight(block_device_t *device);
int vfs_unmount_devices(block_device_t *devices[], u32 count);
int vfs_unmount_devices_preflight(block_device_t *devices[], u32 count);
/* Quiesce mounted superblocks, run each filesystem sync hook or a backing
 * device flush, then restore the mounts. Returns only after media sync is
 * confirmed or a concrete failure is reported. */
int vfs_sync_all(void);
/* Formatting is permitted only when no live mount or VFS reference can
 * still issue filesystem I/O against the exact device instance. */
int vfs_format_preflight(block_device_t *device);
/* Probe under caller-owned quiescence; stop if any probe loses this identity. */
const char *vfs_probe_filesystem_for_id(block_device_t *device, u64 expected_id);
/* Mandatory device-loss notification; unlike unmount, it cannot fail or
 * wait for user references because the hardware is already gone. */
void vfs_device_removed(block_device_t *device);
/* Directory-location leases bind resolution to an actual mount incarnation.
 * NULL start selects root. Absolute paths ignore start, including a dead CWD.
 * Access callbacks are kernel-only and must check each traversed object. */
typedef bool (*vfs_location_access_t)(const vfs_node_t *, void *);
/* Find the terminator of a kernel-owned C pathname within the bounded VFS
 * byte limit. Returns NAME_TOO_LONG when no terminator occurs within it. */
int vfs_path_cstr_length(const char *path, usize *out_length);
int vfs_location_lookup(vfs_location_t *start, const char *path,
                         vfs_location_access_t access, void *context,
                         vfs_location_t **out);
/* Length-aware forms consume exactly path_length bytes. Embedded NUL is
 * invalid; lengths and component limits are measured in bytes. */
int vfs_location_lookup_n(vfs_location_t *start, const char *path,
                           usize path_length, vfs_location_access_t access,
                           void *context, vfs_location_t **out);
int vfs_location_parent(vfs_location_t *start, const char *path,
                         vfs_location_access_t access, void *context,
                         vfs_location_t **out, char *name, usize capacity);
/* On success, reports whether the original spelling ended in one or more
 * separators. Callers creating or mutating the final component must enforce
 * the directory requirement associated with that spelling. */
int vfs_location_parent_n(vfs_location_t *start, const char *path,
                           usize path_length, vfs_location_access_t access,
                           void *context, vfs_location_t **out, char *name,
                           usize capacity, bool *out_trailing_separator);
vfs_node_t *vfs_location_node(vfs_location_t *location);
int vfs_location_check_access(vfs_location_t *location,
                               vfs_location_access_t access, void *context);
int vfs_location_path(vfs_location_t *location, char *out, usize capacity,
                       usize *required);
bool vfs_location_retain(vfs_location_t *location);
void vfs_location_release(vfs_location_t *location);
static inline void vfs_location_cleanup(vfs_location_t **location)
{
    vfs_location_release(*location);
}
#define VFS_LOCATION_AUTO __attribute__((cleanup(vfs_location_cleanup)))

bool vfs_location_cwd_retain(vfs_location_t *location);
/* Inheritance retains an already-held dead CWD; it never validates new access. */
bool vfs_location_cwd_inherit(vfs_location_t *location);
void vfs_location_cwd_release(vfs_location_t *location);
bool vfs_super_is_live(const vfs_super_t *sb);
bool vfs_node_is_live(const vfs_node_t *node);
bool vfs_super_retain(vfs_super_t *sb);
void vfs_super_release(vfs_super_t *sb);
/* Internal filesystem lifetime API. Begin under operation_lock before the
 * first possible persistent mutation. Mark uncertain outcomes before leaving
 * that lock. Finish only a resolved ACTIVE pin, after releasing operation_lock;
 * it returns BUSY while that mutex is held. IDs are never reused this boot. */
int vfs_recovery_pin_begin(vfs_super_t *sb, u64 *out_id);
int vfs_recovery_pin_mark_unresolved(u64 id);
int vfs_recovery_pin_finish(u64 id);
/* Bind a resolved-state handoff at admission. The callback runs under
 * operation_lock without the VFS metadata lock and returns false without
 * changing its transaction on failure. Only the handoff finish may release
 * this pin; callback failure retains it as UNRESOLVED. */
typedef bool (*vfs_recovery_handoff_t)(vfs_super_t *sb, void *context);
int vfs_recovery_pin_begin_handoff(vfs_super_t *sb,
                                   vfs_recovery_handoff_t handoff,
                                   void *context, u64 *out_id);
int vfs_recovery_pin_finish_handoff(u64 id);
/* Caller holds storage-management quiescence through admission and driver I/O.
 * Exclusive unmounted owner, admitted before driver I/O. Caller and registry
 * own separate references. Ops must dispose private driver state on final
 * release. No conflicting mount, format or private owner may overlap its
 * saved physical range; unresolved pins retain that exclusion after detach. */
int vfs_recovery_private_begin(block_device_t *dev, u64 expected_id,
                                const vfs_super_ops_t *ops,
                                vfs_recovery_handoff_t handoff, void *context,
                                vfs_super_t **out_sb, u64 *out_id);
void vfs_recovery_private_release(vfs_super_t *sb);
/* Block teardown calls after marking id offline, with I/O quiesced and no
 * registry lock held. Pins and pending mount admission retain both selected
 * and physical-parent registrations. Offline liveness prevents new pins
 * after this metadata-locked check; a false teardown result retains backing. */
bool vfs_registration_retained(u64 id);

/* Copies scalar identity/status only. On insufficient capacity, out_count is
 * the required count and no partial snapshot is returned. */
int vfs_recovery_pins_snapshot(vfs_recovery_pin_info_t *out, u32 capacity,
                               u32 *out_count);
void vfs_node_register(vfs_node_t *node);
/* Caller holds operation_lock; node membership is protected by VFS metadata
 * locking. This changes cached sizes only, never open-handle offsets. */
void vfs_nodes_update_size(vfs_super_t *sb, u64 inode, u64 size);
int vfs_node_metadata_snapshot(vfs_node_t *node, vfs_node_metadata_t *out);
/* Pin a node's live superblock before calling a filesystem operation. */
bool vfs_node_retain_super(vfs_node_t *node, vfs_super_t **out_super);
/* The caller must hold a superblock reference while using these helpers. */
bool vfs_super_operation_lock(vfs_super_t *sb);
void vfs_super_operation_unlock(vfs_super_t *sb);
/* For opted-in mounts, lock/unlock are non-recursive and thread-owned.
 * Pin finish_handoff must run on the guard owner, after raw unlock, before
 * guard release. Internal finish/cleanup uses only the raw mutex. Callers
 * must not wait for admission while holding locks needed by completion.
 * Raw unlock retains the logical guard until explicit release. Used only by
 * mutation completion and private cleanup; never re-enter normal admission. */
void vfs_super_operation_pause(vfs_super_t *sb);
bool vfs_super_operation_guard_release(vfs_super_t *sb);
int vfs_super_operation_complete(vfs_super_t *sb, int result, u64 pin);
int vfs_super_operation_error(const vfs_super_t *sb);

vfs_node_t *vfs_get_root_node(void);
vfs_mount_t *vfs_find_mount_for_node(vfs_node_t *node);
bool vfs_mount_point_for_device(block_device_t *device, char *out_path,
                                usize capacity, vfs_mount_role_t *out_role);
/* A directory with a runtime child mount must use VFS enumeration so the
 * synthetic child is visible alongside its persistent entries. */
bool vfs_directory_has_mount_children(vfs_node_t *dir);

/* Mount-Aware Path Resolution API */
int vfs_lookup(const char *path, vfs_node_t **out_node);
int vfs_open(const char *path, u32 flags, vfs_file_handle_t **out_handle);
int vfs_open_node(vfs_node_t *node, u32 flags, vfs_file_handle_t **out_handle);
int vfs_open_node_authorized(vfs_node_t *node, u32 flags,
                             vfs_file_handle_t **out_handle);
int vfs_open_node_user_authorized(vfs_node_t *node, u32 flags,
                                  vfs_file_handle_t **out_handle);
int vfs_truncate_handle(vfs_file_handle_t *handle);
/* Internal only; preserves the handle offset and native truncate ABI. */
int vfs_resize_handle(vfs_file_handle_t *handle, u64 length);
/* Internal replacement admission foundation. The caller must have retained
 * the process file object with write rights and pass that fact explicitly.
 * This checks current record facts under operation_lock without mutating it. */
int vfs_file_save_check(vfs_file_handle_t *handle, bool process_write_right,
                        vfs_file_facts_t *out_facts);
/* Internal only. Caller owns a retained file object and asserts process write
 * rights; source and context remain valid until this call returns. */
int vfs_file_replace(vfs_file_handle_t *handle, bool process_write_right,
                     u64 length, vfs_replace_source_t source, void *context);
/* Internal only. The at form retains the supplied directory identity; the
 * wrapper resolves an absolute parent_path. source stays valid through return.
 * Success returns a new read/write handle to the created identity. */
int vfs_file_create_exclusive_at(vfs_node_t *dir, const char *name,
                                 u64 length, vfs_replace_source_t source,
                                 void *context, vfs_file_handle_t **out_handle);
int vfs_file_create_exclusive_at_flags(vfs_node_t *dir, const char *name,
                                       u64 length, vfs_replace_source_t source,
                                       void *context, u32 flags,
                                       vfs_file_handle_t **out_handle);
int vfs_file_create_exclusive(const char *parent_path, const char *name,
                              u64 length, vfs_replace_source_t source,
                              void *context, vfs_file_handle_t **out_handle);
int vfs_file_append_lines(vfs_file_handle_t *handle, const char *data,
                          usize length, u32 max_lines, u32 flags, mg_file_lines_state_t *state);
int vfs_close(vfs_file_handle_t *handle);
int vfs_file_read_status(vfs_file_handle_t *handle, u64 size,
                           void *buffer, u64 *out_bytes);
int vfs_file_read_trusted_status(vfs_file_handle_t *handle, u64 size,
                                   void *buffer, u64 *out_bytes);
u64 vfs_file_read(vfs_file_handle_t *handle, u64 size, void *buffer);
u64 vfs_file_write(vfs_file_handle_t *handle, u64 size, const void *buffer);
/* Internal signed result for ordinary file objects. On VFS_OK, out_written
 * is a confirmed count; uncertainty never reports a confirmed count. */
int vfs_file_write_status(vfs_file_handle_t *handle, u64 size,
                          const void *buffer, u64 *out_written);
int vfs_seek(vfs_file_handle_t *handle, i64 offset, int whence, u64 *out_offset);

/* Authorization is centralized here so all filesystem implementations and
 * userspace entry points apply the same owner/other policy. */
bool vfs_check_access(const vfs_node_t *node, u32 permission);
bool vfs_check_access_credentials(const vfs_node_t *node, u32 permission,
                                   const process_credentials_t *credentials);
bool vfs_current_uid(u32 *out_uid);
void vfs_node_set_security(vfs_node_t *node, u32 owner_uid, u32 permissions);
void vfs_node_set_child_mutation_policy(
    vfs_node_t *node, vfs_child_mutation_policy_t child_mutation_policy);
bool vfs_directory_child_mutation_allowed(const vfs_node_t *dir,
                                          const vfs_node_t *child);
/* True only for a human administrator acting on a live regular-user-owned
 * object with persistent MGFS security metadata. */
bool vfs_administrator_override_allowed(
    const vfs_node_t *node, const process_credentials_t *credentials);

/* Core Node-level VFS Operations */
int vfs_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node);
int vfs_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node);
int vfs_unlink(vfs_node_t *dir, const char *name);
int vfs_rmdir(vfs_node_t *dir, const char *name);
int vfs_rename(vfs_node_t *src_dir, const char *src_name,
               vfs_node_t *dst_dir, const char *dst_name);
int vfs_truncate(vfs_node_t *node);
int vfs_read_status(vfs_node_t *node, u64 offset, u64 size,
                      void *buffer, u64 *out_bytes);
u64 vfs_read(vfs_node_t *node, u64 offset, u64 size, void *buffer);
u64 vfs_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer);
vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name);
int vfs_finddir_checked(vfs_node_t *dir, const char *name, vfs_node_t **out);
bool vfs_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry);
int vfs_readdir_checked(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry);

/* Kernel-internal operations for managed system state.  These deliberately
 * bypass the current process credentials and are not exposed through libc. */
int vfs_lookup_trusted(const char *path, vfs_node_t **out_node);
int vfs_open_trusted(const char *path, u32 flags,
                     vfs_file_handle_t **out_handle);
u64 vfs_file_read_trusted(vfs_file_handle_t *handle, u64 size, void *buffer);
u64 vfs_file_write_trusted(vfs_file_handle_t *handle, u64 size,
                           const void *buffer);
int vfs_close_trusted(vfs_file_handle_t *handle);
int vfs_truncate_trusted(vfs_node_t *node);
int vfs_create_owned(vfs_node_t *dir, const char *name, u32 owner_uid,
                     u32 permissions, vfs_node_t **out_node);
int vfs_mkdir_owned(vfs_node_t *dir, const char *name, u32 owner_uid,
                    u32 permissions, vfs_node_t **out_node);
int vfs_unlink_trusted(vfs_node_t *dir, const char *name);
int vfs_rmdir_trusted(vfs_node_t *dir, const char *name);
int vfs_rename_trusted(vfs_node_t *src_dir, const char *src_name,
                       vfs_node_t *dst_dir, const char *dst_name);
/* Name-based mutation with an already-resolved stable object identity. */
int vfs_unlink_expected(vfs_node_t *dir, const char *name,
                        vfs_super_t *expected_super, u64 expected_inode);
/* Used only after a separate, object-bound administrator authorization has
 * been validated by the syscall layer. */
int vfs_unlink_expected_authorized(vfs_node_t *dir, const char *name,
                                   vfs_super_t *expected_super,
                                   u64 expected_inode);
int vfs_rmdir_expected(vfs_node_t *dir, const char *name,
                       vfs_super_t *expected_super, u64 expected_inode);
int vfs_rmdir_expected_authorized(vfs_node_t *dir, const char *name,
                                  vfs_super_t *expected_super,
                                  u64 expected_inode);
int vfs_rename_expected(vfs_node_t *src_dir, const char *src_name,
                        vfs_super_t *expected_super, u64 expected_inode,
                        vfs_node_t *dst_dir, const char *dst_name);
int vfs_rename_expected_authorized(
    vfs_node_t *src_dir, const char *src_name,
    vfs_super_t *expected_super, u64 expected_inode,
    vfs_node_t *dst_dir, const char *dst_name);
vfs_node_t *vfs_finddir_trusted(vfs_node_t *dir, const char *name);
int vfs_finddir_trusted_checked(vfs_node_t *dir, const char *name, vfs_node_t **out);
bool vfs_readdir_trusted(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry);
int vfs_readdir_trusted_checked(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry);

int vfs_file_snapshot(vfs_file_handle_t *handle, char *data, usize capacity,
                       mg_file_lines_state_t *state);
int vfs_file_snapshot_stream(vfs_file_handle_t *handle, usize capacity,
                             vfs_snapshot_sink_t sink,
                             vfs_snapshot_finish_t finish, void *context);
