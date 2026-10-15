/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Ordinary mutations are in-place operations, not atomic replacements. Keep
 * registry ownership of the whole physical range and bounded diagnostic
 * evidence until VFS completes the post-unlock handoff. Never retry/rollback
 * uncertain mutations here, or discard them through another mount proof. */
static bool mgfs_ordinary_finish_handoff(vfs_super_t *sb, void *context)
{
    mgfs_fs_t *fs = sb ? sb->private_data : NULL;
    mgfs_ordinary_transaction_t *tx = context;
    if (!fs || fs->ordinary_transaction != tx) return false;
    if (!tx->handoff_pending || fs->mutation_admitted ||
        fs->record_lifecycle_uncertain || fs->publication_uncertain) {
        fs->ownership_confidence = MGFS_CONFIDENCE_INVALID;
        fs->record_lifecycle_uncertain = true;
        return false;
    }
    fs->ordinary_transaction = NULL;
    kfree(tx);
    mgfs_mutation_complete(fs);
    return true;
}

static int mgfs_ordinary_begin(vfs_super_t *sb, vfs_node_t *first,
                               vfs_node_t *second, vfs_node_t *prepared)
{
    mgfs_fs_t *fs = sb ? sb->private_data : NULL;
    if (!fs || !first || first->super != sb) return VFS_ERR_INVALID_PARAM;
    if (sb->read_only || fs->read_only) return VFS_ERR_ACCESS_DENIED;
    if (!mgfs_mutation_begin(fs)) return VFS_ERR_UNCERTAIN;
    mgfs_ordinary_transaction_t *tx = kmalloc(sizeof(*tx));
    if (!tx) { mgfs_mutation_complete(fs); return VFS_ERR_NO_MEM; }
    memset(tx, 0, sizeof(*tx));
    tx->prepared_node = prepared;
    if (!mgfs_read_record(fs, first->inode, tx->records[tx->record_count++]) ||
        (second && second->inode != first->inode &&
         !mgfs_read_record(fs, second->inode, tx->records[tx->record_count++]))) {
        kfree(tx);
        mgfs_mutation_complete(fs);
        return VFS_ERR_IO;
    }
    int result = vfs_recovery_pin_begin_handoff(sb, mgfs_ordinary_finish_handoff,
                                                tx, &tx->pin_id);
    if (result != VFS_OK) {
        kfree(tx);
        mgfs_mutation_complete(fs);
        return result;
    }
    fs->ordinary_transaction = tx;
    return VFS_OK;
}

static int mgfs_ordinary_complete(vfs_super_t *sb, int result, u64 *out_pin)
{
    mgfs_fs_t *fs = sb->private_data;
    mgfs_ordinary_transaction_t *tx = fs->ordinary_transaction;
    tx->prepared_node = NULL; /* VFS owns this node, including error disposal. */
    /* Legacy block writers conservatively quarantine any false result. A
     * failed before-image read is distinguishable from an attempted write:
     * with no preceding non-marker mutation, cleanup is still provably safe. */
    if (tx->preparation_failed && !tx->write_failed && !fs->publication_uncertain &&
        (!tx->attempted || (tx->attempts == 1 && tx->block_number == 0))) {
        fs->record_lifecycle_uncertain = false;
        fs->ownership_confidence = MGFS_CONFIDENCE_IN_PROGRESS;
    }
    bool canceled = false;
    /* A sole confirmed superblock marker/identity-consumption write changed
     * no allocation or reachable mapping. Restore only its state flag, keep
     * consumed IDs monotonic, and confirm cleanup before resolving the pin. */
    if (result != VFS_OK && tx->attempts == 1 && tx->block_number == 0 &&
        !fs->record_lifecycle_uncertain && !fs->publication_uncertain) {
        u64 old_state = mgfs_get_le64(tx->original + 64);
        canceled = mgfs_set_state(fs, old_state) &&
                   block_flush_for_id(fs->dev, fs->device_id);
        if (!canceled) fs->record_lifecycle_uncertain = true;
    }
    if (tx->attempted && !canceled && (result != VFS_OK ||
                          !block_flush_for_id(fs->dev, fs->device_id)))
        fs->record_lifecycle_uncertain = true;
    if (result == VFS_ERR_UNCERTAIN && !fs->record_lifecycle_uncertain &&
        !fs->publication_uncertain && (!tx->attempted || canceled))
        result = VFS_ERR_IO; /* Merge callback could not distinguish preparation failure. */
    if (fs->record_lifecycle_uncertain || fs->publication_uncertain) {
        (void)vfs_recovery_pin_mark_unresolved(tx->pin_id);
        result = VFS_ERR_UNCERTAIN;
    } else {
        tx->handoff_pending = true;
        *out_pin = tx->pin_id;
    }
    mgfs_mutation_complete(fs);
    return result;
}

static int mgfs_write_pinned(vfs_node_t *node, u64 offset, u64 size,
                             const void *buffer, u64 *out_bytes, u64 *out_pin)
{
    *out_bytes = *out_pin = 0;
    if (size && !buffer) return VFS_ERR_INVALID_PARAM;
    if (!size) return VFS_OK;
    vfs_super_t *sb = node->super;
    int result = mgfs_ordinary_begin(sb, node, NULL, NULL);
    if (result != VFS_OK) return result;
    u64 written = mgfs_write_internal_impl(node, offset, size, buffer, false);
    result = mgfs_ordinary_complete(sb, written == size ? VFS_OK : VFS_ERR_IO, out_pin);
    if (result != VFS_ERR_UNCERTAIN) *out_bytes = written;
    return result;
}

static int mgfs_truncate_pinned(vfs_node_t *node, u64 *out_pin)
{
    *out_pin = 0;
    vfs_super_t *sb = node->super;
    int result = mgfs_ordinary_begin(sb, node, NULL, NULL);
    if (result != VFS_OK) return result;
    return mgfs_ordinary_complete(sb, mgfs_truncate_impl(node), out_pin);
}

static int mgfs_retire_pinned(vfs_node_t *dir, const char *name,
                              bool directory, u64 *out_pin)
{
    *out_pin = 0;
    vfs_super_t *sb = dir->super;
    int result = mgfs_ordinary_begin(sb, dir, NULL, NULL);
    if (result != VFS_OK) return result;
    return mgfs_ordinary_complete(sb, mgfs_delete_entry_impl(dir, name, directory), out_pin);
}
static int mgfs_unlink_pinned(vfs_node_t *dir, const char *name, u64 *out_pin)
{ return mgfs_retire_pinned(dir, name, false, out_pin); }
static int mgfs_rmdir_pinned(vfs_node_t *dir, const char *name, u64 *out_pin)
{ return mgfs_retire_pinned(dir, name, true, out_pin); }

static int mgfs_rename_pinned(vfs_super_t *sb, vfs_node_t *src, const char *name,
                              vfs_node_t *dst, const char *new_name, u64 *out_pin)
{
    *out_pin = 0;
    if (!dst || dst->super != sb) return VFS_ERR_INVALID_PARAM;
    int result = mgfs_ordinary_begin(sb, src, dst, NULL);
    if (result != VFS_OK) return result;
    return mgfs_ordinary_complete(sb, mgfs_rename_entry_impl(src, name, dst, new_name), out_pin);
}

static int mgfs_new_node_pinned(vfs_node_t *dir, vfs_node_t *prepared,
                                 const char *name, bool directory, bool owned,
                                 u32 uid, u32 permissions, u64 *out_pin)
{
    *out_pin = 0;
    vfs_super_t *sb = dir->super;
    int result = mgfs_ordinary_begin(sb, dir, NULL, prepared);
    if (result != VFS_OK) return result;
    vfs_node_t *node = NULL;
    result = mgfs_create_node_impl(dir, name, directory ? MGFS_RECORD_DIRECTORY : MGFS_RECORD_FILE,
                                    owned, uid, permissions, &node);
    return mgfs_ordinary_complete(sb, result, out_pin);
}
static int mgfs_create_pinned(vfs_node_t *dir, vfs_node_t *prepared,
                              const char *name, u64 *out_pin)
{ return mgfs_new_node_pinned(dir, prepared, name, false, false, 0, 0, out_pin); }
static int mgfs_mkdir_pinned(vfs_node_t *dir, vfs_node_t *prepared,
                             const char *name, u64 *out_pin)
{ return mgfs_new_node_pinned(dir, prepared, name, true, false, 0, 0, out_pin); }
static int mgfs_create_owned_pinned(vfs_node_t *dir, vfs_node_t *prepared,
                                    const char *name, u32 uid, u32 permissions, u64 *out_pin)
{ return mgfs_new_node_pinned(dir, prepared, name, false, true, uid, permissions, out_pin); }
static int mgfs_mkdir_owned_pinned(vfs_node_t *dir, vfs_node_t *prepared,
                                   const char *name, u32 uid, u32 permissions, u64 *out_pin)
{ return mgfs_new_node_pinned(dir, prepared, name, true, true, uid, permissions, out_pin); }
static void mgfs_discard_created_node(vfs_node_t *node, vfs_super_t *sb)
{ (void)sb; node->fs_data = NULL; }

static int mgfs_append_lines_pinned(vfs_node_t *node, const char *data, usize length,
                                    u32 max_lines, u32 flags,
                                    mg_file_lines_state_t *state, u64 *out_pin)
{
    *out_pin = 0;
    vfs_super_t *sb = node->super;
    int result = mgfs_ordinary_begin(sb, node, NULL, NULL);
    if (result != VFS_OK) return result;
    return mgfs_ordinary_complete(sb, mgfs_append_lines(node, data, length,
                                                       max_lines, flags, state), out_pin);
}
