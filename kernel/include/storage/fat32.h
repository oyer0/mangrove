#pragma once

#include <vfs.h>
#include <block.h>

#define FAT32_EOC_MIN     0x0FFFFFF8
#define FAT32_CLUSTER_ERR 0x0FFFFFFF

/* Registers the "fat32" filesystem driver plugin with the VFS */
int fat32_init(void);

/* Allocation Primitives (Exposed for Testing & Driver Components) */
u32 fat32_alloc_cluster(vfs_super_t *sb);
u32 fat32_extend_chain(vfs_super_t *sb, u32 last_cluster);
bool fat32_free_chain(vfs_super_t *sb, u32 start_cluster);
u32 fat32_get_cluster_link(vfs_super_t *sb, u32 cluster);
