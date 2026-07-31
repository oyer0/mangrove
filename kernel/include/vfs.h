#pragma once

#include <types.h>
#include <block.h>

#define VFS_OK                    0
#define VFS_ERR_INVALID_PARAM   (-1)
#define VFS_ERR_NOT_FOUND       (-2)
#define VFS_ERR_IO              (-3)
#define VFS_ERR_NO_MEM          (-4)
#define VFS_ERR_BAD_FORMAT      (-5)
#define VFS_ERR_UNSUPPORTED     (-6)

#define VFS_OPEN_READ            0x0001U
#define VFS_OPEN_WRITE           0x0002U
#define VFS_OPEN_RDWR            (VFS_OPEN_READ | VFS_OPEN_WRITE)

#define VFS_SEEK_SET             0
#define VFS_SEEK_CUR             1
#define VFS_SEEK_END             2

#define VFS_MAX_MOUNTS 32

typedef enum {
    VFS_TYPE_UNKNOWN = 0,
    VFS_TYPE_FILE,
    VFS_TYPE_DIRECTORY,
} vfs_node_type_t;

typedef struct {
    char name[256];
    u64 inode;
    vfs_node_type_t type;
} vfs_dirent_t;

typedef struct vfs_node vfs_node_t;
typedef struct vfs_super vfs_super_t;
typedef struct vfs_fs_type vfs_fs_type_t;
typedef struct vfs_file_handle vfs_file_handle_t;

/* Node Operations Table */
typedef struct {
    u64 (*read)(vfs_node_t *node, u64 offset, u64 size, void *buffer);
    u64 (*write)(vfs_node_t *node, u64 offset, u64 size, const void *buffer);
    vfs_node_t *(*finddir)(vfs_node_t *dir, const char *name);
    bool (*readdir)(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry);
    int (*create)(vfs_node_t *dir, const char *name, vfs_node_t **out_node);
    int (*mkdir)(vfs_node_t *dir, const char *name, vfs_node_t **out_node);
    int (*unlink)(vfs_node_t *dir, const char *name);
    int (*rmdir)(vfs_node_t *dir, const char *name);
    int (*rename)(vfs_node_t *src_dir, const char *src_name,
                  vfs_node_t *dst_dir, const char *dst_name);
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
};

/* Filesystem Driver Registration Plugin (Static Kernel Lifetime) */
struct vfs_fs_type {
    const char *name;             // Driver plugin name, e.g. "fat32"
    bool (*probe)(block_device_t *dev);
    int (*mount)(vfs_fs_type_t *fs_type, block_device_t *dev, vfs_super_t **out_sb);
    vfs_fs_type_t *next;
};

/* Inode / Metadata Node */
struct vfs_node {
    u64 inode;
    vfs_node_type_t type;
    u64 size;
    u32 ref_count;
    vfs_super_t *super;           // Owning superblock instance
    void *fs_data;                // Driver-private node state
    const vfs_ops_t *ops;
};

/* Kernel-side open instance; this is not a process file descriptor. */
struct vfs_file_handle {
    vfs_node_t *node;
    u64 offset;
    u32 flags;
    u32 valid;
};

/* Node-based Mount Entry */
typedef struct {
    vfs_super_t *sb;             // Mounted filesystem instance
    vfs_node_t *covered_node;    // Directory node covered by this mount (NULL for root "/")
    bool active;                 // Slot active flag
} vfs_mount_t;

/* VFS Initialization & Mount Management API */
void vfs_init(void);
int vfs_register_fs(vfs_fs_type_t *fs_type);
vfs_fs_type_t *vfs_find_fs(const char *name);

int vfs_mount_root(const char *fs_name, block_device_t *dev);
int vfs_mount_node(vfs_node_t *target_node, const char *fs_name, block_device_t *dev);
int vfs_unmount_sb(vfs_super_t *sb);

vfs_node_t *vfs_get_root_node(void);
vfs_mount_t *vfs_find_mount_for_node(vfs_node_t *node);

/* Mount-Aware Path Resolution API */
int vfs_lookup(const char *path, vfs_node_t **out_node);
int vfs_resolve_path(const char *cwd, const char *input_path, char *out_buf, usize out_size);
int vfs_open(const char *path, u32 flags, vfs_file_handle_t **out_handle);
int vfs_close(vfs_file_handle_t *handle);
u64 vfs_file_read(vfs_file_handle_t *handle, u64 size, void *buffer);
u64 vfs_file_write(vfs_file_handle_t *handle, u64 size, const void *buffer);
int vfs_seek(vfs_file_handle_t *handle, i64 offset, int whence, u64 *out_offset);

/* Core Node-level VFS Operations */
int vfs_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node);
int vfs_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node);
int vfs_unlink(vfs_node_t *dir, const char *name);
int vfs_rmdir(vfs_node_t *dir, const char *name);
int vfs_rename(vfs_node_t *src_dir, const char *src_name,
               vfs_node_t *dst_dir, const char *dst_name);
u64 vfs_read(vfs_node_t *node, u64 offset, u64 size, void *buffer);
u64 vfs_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer);
vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name);
bool vfs_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry);
