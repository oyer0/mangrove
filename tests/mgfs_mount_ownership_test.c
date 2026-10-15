/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete VFS/MGFS units; only heap, locks, device registry and regular-file
 * block transport are controlled host dependencies. No filesystem mocks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mfqs/host.h"
#include "../kernel/src/storage/mgfs.c"

static u8 bytes[8192];
static u32 pins(void)
{
    u32 n = 0;
    int status = vfs_recovery_pins_snapshot(NULL, 0, &n);
    assert(status == VFS_OK || status == VFS_ERR_BUFFER_TOO_SMALL);
    return n;
}
static void reserved(void)
{ assert(vfs_format_preflight(mfqs_device()) == VFS_ERR_BUSY); }
static u32 source(void *context, u64 offset, u8 *out, u32 count)
{
    if (context) return 0;
    assert(offset + count <= sizeof(bytes));
    memcpy(out, bytes + offset, count);
    return count;
}
static vfs_node_t *virtual_find(vfs_node_t *dir, const char *name)
{ (void)dir; (void)name; return NULL; }
static const vfs_ops_t virtual_ops = {.finddir=virtual_find};
static int virtual_mount(vfs_fs_type_t *type, block_device_t *dev,
                          vfs_super_t **out, bool read_only)
{
    assert(!dev);
    *out = kmalloc(sizeof(**out));
    memset(*out, 0, sizeof(**out));
    (*out)->root_node = kmalloc(sizeof(vfs_node_t));
    memset((*out)->root_node, 0, sizeof(vfs_node_t));
    (*out)->root_node->type = VFS_TYPE_DIRECTORY;
    (*out)->root_node->ops = &virtual_ops;
    (*out)->root_node->super = *out;
    (*out)->root_node->inode = 1;
    (*out)->root_node->permissions = VFS_PERMISSION_KNOWN;
    (*out)->root_node->ref_count = 1;
    (*out)->fs_type = type;
    (*out)->read_only = read_only;
    return VFS_OK;
}
static vfs_fs_type_t virtual_type = {.name="fixture-root", .mount=virtual_mount};

static void teardown(vfs_super_t *sb)
{
    vfs_node_t *nodes[256];
    unsigned n = 0;
    for (vfs_node_t *node = sb->nodes; node; node = node->next_in_super) {
        assert(n < 256);
        nodes[n++] = node;
    }
    assert(vfs_unmount_device(mfqs_device()) == VFS_OK);
    mfqs_heap_release_nodes(nodes, n);
}

int main(int argc, char **argv)
{
    assert(argc == 6 && mfqs_media_open(argv[1]));
    const char *operation = argv[2];
    char fault = argv[3][0];
    u64 ordinal = strtoull(argv[4], NULL, 10);
    bool remove = atoi(argv[5]) != 0;
    memset(bytes, 'X', sizeof(bytes));
    vfs_init();
    assert(mgfs_init() == VFS_OK);
    mfqs_read_hook(reserved);
    if (!strcmp(operation, "reject") || !strcmp(operation, "mount-fault")) {
        mfqs_fail_after(fault, ordinal, true, remove);
        mfqs_metrics_t before, after;
        mfqs_metrics(&before);
        int status = vfs_mount_root("mgfs", mfqs_device());
        mfqs_metrics(&after);
        if (status == VFS_OK && !strcmp(operation,"mount-fault")) {
            teardown(vfs_get_root_node()->super);
            puts("NOFAULT: ordinal exceeds mount requests");
            return 0;
        }
        if (!strcmp(operation,"reject")) assert(status == VFS_ERR_BAD_FORMAT);
        assert(status != VFS_OK && !vfs_get_root_node());
        assert(before.writes == after.writes && before.heap_live == after.heap_live);
        if (!remove) assert(vfs_format_preflight(mfqs_device()) == VFS_OK);
        printf("REJECT status=%d reads=%llu heap_peak=%llu\n", status,
               (unsigned long long)after.reads, (unsigned long long)after.heap_peak);
        return 0;
    }
    if (!strcmp(operation, "recovery")) {
        assert(vfs_register_fs(&virtual_type) == VFS_OK);
        assert(vfs_mount_root("fixture-root", NULL) == VFS_OK);
        assert(vfs_mount_path(vfs_get_root_node(), "recovery", "/recovery", "mgfs",
                              mfqs_device(), VFS_MOUNT_ROLE_VOLUME, true) == VFS_OK);
        mfqs_read_hook(NULL);
        vfs_node_t *a, *b;
        assert(vfs_lookup("/recovery/a", &a) == VFS_OK);
        assert(vfs_lookup("/recovery/b", &b) == VFS_OK);
        u8 x, y;
        assert(vfs_read(a, 0, 1, &x) == 1 && vfs_read(b, 0, 1, &y) == 1 && x == y);
        mfqs_metrics_t before, after;
        mfqs_metrics(&before);
        assert(vfs_write(a, 0, 1, "X") == 0);
        assert(vfs_truncate_trusted(a) == VFS_ERR_ACCESS_DENIED);
        assert(vfs_unlink_trusted(a->super->root_node, "b") == VFS_ERR_ACCESS_DENIED);
        assert(a->super->root_node->ops->truncate == NULL);
        u64 pin = 0;
        assert(mgfs_truncate_pinned(a, &pin) == VFS_ERR_ACCESS_DENIED && !pin);
        mfqs_metrics(&after);
        assert(before.writes == after.writes && !pins());
        printf("READONLY recovery: shared content readable; trusted/direct mutations rejected\n");
        return 0;
    }
    clock_t mount_started = clock();
    assert(vfs_mount_root("mgfs", mfqs_device()) == VFS_OK);
    double mount_cpu_ms = 1000.0 * (double)(clock()-mount_started)/CLOCKS_PER_SEC;
    mfqs_read_hook(NULL);
    vfs_node_t *root = vfs_get_root_node(), *node = NULL;
    vfs_super_t *sb = root->super;
    mgfs_fs_t *fs = sb->private_data;
    assert(fs->ownership_confidence == MGFS_CONFIDENCE_PROVEN);
    u64 proof_peak = fs->ownership_proof_peak, proof_ranges = fs->ownership_proof_ranges;
    if (!strcmp(operation,"identity")) {
        assert(vfs_lookup("/a",&node)==VFS_OK);
        mfqs_metrics_t before,after;mfqs_metrics(&before);
        u64 original_id=mfqs_device()->id;
        mfqs_device()->id=original_id+1;
        u8 value;u64 count=0,pin=0;
        assert(!mgfs_read(node,0,1,&value));
        assert(mgfs_write_pinned(node,0,1,"Y",&count,&pin)==VFS_ERR_UNCERTAIN && !count && !pin);
        mfqs_metrics(&after);assert(after.reads==before.reads && after.writes==before.writes);
        mfqs_device()->id=original_id;
        teardown(sb);
        puts("IDENTITY saved registration refused replacement descriptor without I/O");return 0;
    }
    if (!strcmp(operation,"admit")) {
        mfqs_metrics_t metrics; mfqs_metrics(&metrics);
        printf("ADMIT proof=%llu ranges=%llu reads=%llu requests=%llu cpu_ms=%.3f\n",
            (unsigned long long)proof_peak,(unsigned long long)proof_ranges,
            (unsigned long long)metrics.reads,(unsigned long long)metrics.heap_requests,mount_cpu_ms);
        teardown(sb);return 0;
    }
    if (!strcmp(operation,"deep-move")) {
        vfs_node_t *target;
        assert(vfs_mkdir(root,"target",&target)==VFS_OK);
        assert(vfs_rename(root,"n",target,"moved")==VFS_OK);
        assert(vfs_lookup("/target/moved",&node)==VFS_OK);
        assert(vfs_rename(root,"target",node,"cycle")==VFS_ERR_INVALID_PARAM);
        assert(mgfs_complete_ownership_proof(fs,false)==MGFS_OWNERSHIP_VALID && !pins());
        printf("DEEP MOVE iterative ancestry retained 256-directory namespace; cycle refused\n");
        teardown(sb);return 0;
    }
    if (!strcmp(operation,"large")) {
        assert(vfs_lookup("/large",&node)==VFS_OK);
        u8 byte = 99;
        assert(vfs_read(node,0,1,&byte)==1 && !byte);
        assert(vfs_read(node,node->size-1,1,&byte)==1 && !byte);
        mfqs_metrics_t metrics; mfqs_metrics(&metrics);
        printf("LARGE bytes=%llu proof=%llu ranges=%llu reads=%llu requests=%llu heap=%llu cpu_ms=%.3f\n",
            (unsigned long long)node->size,(unsigned long long)proof_peak,
            (unsigned long long)proof_ranges,(unsigned long long)metrics.reads,
            (unsigned long long)metrics.heap_requests,(unsigned long long)metrics.heap_peak,mount_cpu_ms);
        teardown(sb);
        return 0;
    }
    if (!strcmp(operation, "budget")) {
        mgfs_proof_t *proof = kmalloc(sizeof(*proof));
        memset(proof, 0, sizeof(*proof));
        proof->live = proof->peak = sizeof(*proof);
        while (mgfs_proof_push(proof, &proof->ranges, (mgfs_owned_range_t){2,3})) {}
        assert(proof->result == MGFS_OWNERSHIP_LIMIT && proof->peak <= MGFS_PROOF_BUDGET);
        printf("BUDGET ranges=%llu peak=%llu\n", (unsigned long long)proof->ranges.count,
               (unsigned long long)proof->peak);
        mgfs_proof_clear(proof, &proof->ranges);
        assert(proof->live == sizeof(*proof));
        kfree(proof);
        teardown(sb);
        return 0;
    }
    if (!strcmp(operation, "cursor")) {
        void *cursor = NULL;
        vfs_dirent_t entry;
        assert(vfs_super_operation_lock(sb));
        assert(mgfs_readdir_open(root, &cursor));
        assert(mgfs_readdir_next_checked(cursor, &entry) == 1 && !strcmp(entry.name,"a"));
        vfs_super_operation_unlock(sb);
        vfs_file_handle_t *created = NULL;
        assert(vfs_file_create_exclusive("/", "new", 8192, source, NULL, &created) == VFS_OK);
        assert(vfs_close(created) == VFS_OK);
        assert(vfs_create(root,"reuse",&node)==VFS_OK);
        assert(vfs_open_node(node,VFS_OPEN_RDWR,&created)==VFS_OK);
        u64 written;
        assert(vfs_file_write_status(created,sizeof(bytes),bytes,&written)==VFS_OK && written==sizeof(bytes));
        assert(vfs_close(created)==VFS_OK);
        assert(vfs_super_operation_lock(sb));
        assert(mgfs_readdir_next_checked(cursor, &entry) == 1 && !strcmp(entry.name,"b"));
        mgfs_readdir_close(cursor);
        vfs_super_operation_unlock(sb);
        assert(!pins());
        assert(vfs_lookup("/empty",&node)==VFS_OK);
        assert(vfs_super_operation_lock(sb));
        assert(mgfs_readdir_open(node,&cursor));
        vfs_super_operation_unlock(sb);
        assert(vfs_rmdir(root,"empty")==VFS_OK);
        assert(vfs_mkdir(root,"again",&node)==VFS_OK);
        assert(vfs_super_operation_lock(sb));
        assert(mgfs_readdir_next_checked(cursor,&entry)==VFS_ERR_NOT_FOUND);
        mgfs_readdir_close(cursor);
        vfs_super_operation_unlock(sb);
        printf("CURSOR current Record mapping retained position after parent COW\n");
        teardown(sb);
        return 0;
    }
    vfs_file_handle_t *handle = NULL;
    if (!strcmp(operation,"write") || !strcmp(operation,"grow") ||
        !strcmp(operation,"truncate") || !strcmp(operation,"replace") ||
        !strcmp(operation,"history") || !strcmp(operation,"cancel"))
        assert(vfs_open(!strcmp(operation,"history") ? "/h" : "/a", VFS_OPEN_RDWR, &handle) == VFS_OK);
    vfs_node_t *destination = NULL;
    if (!strcmp(operation,"move")) assert(vfs_lookup("/empty",&destination)==VFS_OK);
    mfqs_metrics_t before, after;
    mfqs_metrics(&before);
    if (!strcmp(operation,"exhaust")) {
        u64 ids[VFS_MAX_RECOVERY_PINS];
        for (unsigned i=0; i<VFS_MAX_RECOVERY_PINS; i++)
            assert(vfs_recovery_pin_begin(sb, &ids[i]) == VFS_OK);
        assert(vfs_create(root, "new", &node) == VFS_ERR_NO_SPACE);
        mfqs_metrics(&after);
        assert(before.writes == after.writes && fs->ownership_confidence == MGFS_CONFIDENCE_PROVEN);
        for (unsigned i=0; i<VFS_MAX_RECOVERY_PINS; i++) assert(vfs_recovery_pin_finish(ids[i]) == VFS_OK);
        teardown(sb);
        puts("EXHAUST no mutation before registry admission");
        return 0;
    }
    mfqs_fail_after(fault, ordinal, true, remove);
    int result = VFS_ERR_UNSUPPORTED;
    u64 written = 0;
    if (!strcmp(operation,"write") || !strcmp(operation,"grow")) {
        if (!strcmp(operation,"grow")) assert(vfs_seek(handle,8192,VFS_SEEK_SET,NULL)==VFS_OK);
        result = vfs_file_write_status(handle, sizeof(bytes), bytes, &written);
    } else if (!strcmp(operation,"truncate")) result = vfs_truncate_handle(handle);
    else if (!strcmp(operation,"history")) {
        mg_file_lines_state_t state;
        result = vfs_file_append_lines(handle,"fresh\n",6,10,0,&state);
    } else if (!strcmp(operation,"replace") || !strcmp(operation,"cancel"))
        result = vfs_file_replace(handle,true,sizeof(bytes),source,
                                  !strcmp(operation,"cancel") ? (void *)1 : NULL);
    else if (!strcmp(operation,"unlink")) result = vfs_unlink(root,"a");
    else if (!strcmp(operation,"rmdir")) result = vfs_rmdir(root,"empty");
    else if (!strcmp(operation,"create")) result = vfs_create(root,"new",&node);
    else if (!strcmp(operation,"mkdir")) result = vfs_mkdir(root,"new",&node);
    else if (!strcmp(operation,"owned")) result = vfs_create_owned(root,"new",0,3,&node);
    else if (!strcmp(operation,"owned-dir")) result = vfs_mkdir_owned(root,"new",0,3,&node);
    else if (!strcmp(operation,"rename")) result = vfs_rename(root,"a",root,"new");
    else if (!strcmp(operation,"move")) {
        result = vfs_rename(root,"a",destination,"new");
    } else if (!strcmp(operation,"exclusive"))
        result = vfs_file_create_exclusive("/","new",sizeof(bytes),source,NULL,&handle);
    else assert(0);
    mfqs_metrics(&after);
    mfqs_fail_after('-',0,false,false);
    if (handle) assert(vfs_close(handle) == VFS_OK);
    if (result == VFS_ERR_UNCERTAIN) {
        assert(pins() == 1 && fs->ownership_confidence == MGFS_CONFIDENCE_INVALID);
        vfs_recovery_pin_info_t evidence;
        u32 count;
        assert(vfs_recovery_pins_snapshot(&evidence,1,&count)==VFS_OK);
        assert(evidence.state==VFS_RECOVERY_PIN_UNRESOLVED);
        assert(vfs_unmount_device(mfqs_device()) == VFS_ERR_BUSY);
        assert(vfs_format_preflight(mfqs_device()) == (remove ? VFS_ERR_DEVICE_GONE : VFS_ERR_BUSY));
        assert(vfs_mount_root("mgfs",mfqs_device()) != VFS_OK);
        u64 attempts = after.writes;
        int blocked = vfs_create(root,"blocked",&node);
        assert(blocked == VFS_ERR_UNCERTAIN || (remove && blocked == VFS_ERR_INVALID_PARAM));
        mfqs_metrics(&after); assert(after.writes == attempts);
        vfs_device_removed(mfqs_device()); /* Forced namespace detach preserves owner. */
        assert(pins()==1 && mfqs_heap_contains(fs));
        if (!remove) {
            assert(vfs_format_preflight(mfqs_device())==VFS_ERR_BUSY);
            mfqs_metrics_t fenced;mfqs_metrics(&fenced);
            assert(vfs_mount_root("mgfs",mfqs_device())!=VFS_OK);
            mfqs_metrics_t refused;mfqs_metrics(&refused);
            assert(refused.reads==fenced.reads && refused.writes==fenced.writes);
        }
        /* Unresolved state intentionally survives process exit, like reboot. */
    } else {
        assert(!pins() && fs->ownership_confidence == MGFS_CONFIDENCE_PROVEN);
        if (result != VFS_OK && strcmp(operation,"cancel") && strcmp(operation,"exclusive") && strcmp(operation,"replace")) {
            assert(after.writes == before.writes || after.writes == before.writes + 2);
        }
        assert(mgfs_complete_ownership_proof(fs,false)==MGFS_OWNERSHIP_VALID);
        if (result != VFS_OK) {
            vfs_node_t *unchanged; u8 byte;
            assert(vfs_lookup("/a",&unchanged)==VFS_OK && vfs_read(unchanged,0,1,&byte)==1 && byte=='A');
            assert(vfs_lookup("/new",&unchanged)==VFS_ERR_NOT_FOUND);
        }
        teardown(sb);
    }
    printf("RESULT op=%s status=%d reads=%llu writes=%llu flushes=%llu requests=%llu peak=%llu proof=%llu ranges=%llu tx=%zu\n",
        operation,result,(unsigned long long)(after.reads-before.reads),
        (unsigned long long)(after.writes-before.writes),(unsigned long long)(after.flushes-before.flushes),
        (unsigned long long)(after.heap_requests-before.heap_requests),(unsigned long long)after.heap_peak,
        (unsigned long long)proof_peak,(unsigned long long)proof_ranges,sizeof(mgfs_ordinary_transaction_t));
    return 0;
}
