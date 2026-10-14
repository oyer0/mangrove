/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <vfs.h>
#include <block.h>

#define FAT32_EOC_MIN     0x0FFFFFF8
#define FAT32_CLUSTER_ERR 0x0FFFFFFF

/* Registers the "fat32" filesystem driver plugin with the VFS */
int fat32_init(void);

/* Read the bounded FAT32 volume label without mounting the filesystem. */
bool fat32_read_label(block_device_t *dev, char *out, usize capacity);

/* FAT32 carries a volume label in both its extended boot record and, when a
 * label exists, a root-directory volume-label entry.  Keep those standard
 * representations synchronized for interoperable relabeling. */
bool fat32_label_valid(const char *label);
/* Exclusive unmounted transaction; returns a VFS result, including UNCERTAIN. */
int fat32_set_label(block_device_t *dev, const char *label);
int fat32_set_label_for_id(block_device_t *dev, u64 expected_id, const char *label);

/* Read-only active-FAT inspection, not an ownership or mirror-consistency
 * proof. Caller retains the superblock and holds its operation lock. */
u32 fat32_get_cluster_link(vfs_super_t *sb, u32 cluster);

/* Read-only namespace ownership proof used by recovery-owned mutations.
 * Caller holds the operation lock and consumes the result before unlocking;
 * incomplete validation never establishes ownership. */
int fat32_ownership_preflight_locked(vfs_node_t *target);
