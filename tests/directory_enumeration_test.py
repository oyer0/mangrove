#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production VFS/object enumeration with controlled FS and pthread locks."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
vfs = (root / "kernel/src/vfs.c").read_text()
obj = (root / "kernel/src/object.c").read_text()


def function(source, name):
    for match in re.finditer(r"^(?:static )?[^\n;{}]*\b" + name + r"\s*\(", source, re.M):
        brace = source.index("{", match.end())
        if ";" not in source[match.end():brace]:
            break
    else:
        raise AssertionError(name)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


directory_type = obj[obj.index("typedef struct {\n    kernel_object_t base;\n    vfs_node_t *node;"):obj.index("} directory_object_t;") + len("} directory_object_t;")]
mount_type = vfs[vfs.index("typedef struct {\n    char name[VFS_MOUNT_PATH_MAX];"):vfs.index("} vfs_mount_dirent_snapshot_t;") + len("} vfs_mount_dirent_snapshot_t;")]
prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <object.h>
#include <mangrove_errors.h>
static vfs_super_t super, child_super;
static vfs_node_t node, child_root;
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static spinlock_t vfs_metadata_lock;
static pthread_mutex_t operation = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cursor_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool live, allowed, cancel_cursor, cancel_operation, fail_alloc, mounts;
static unsigned position, calls, limit, closes;
static int injected;
static bool stream, long_at_one, cancel_after_first;
static void *kmalloc(usize size) { return fail_alloc ? NULL : malloc(size); }
static void kfree(void *p) { free(p); }
bool vfs_node_is_live(const vfs_node_t *n) { return n == &node && live; }
bool vfs_super_is_live(const vfs_super_t *s) { return s == &super && live; }
bool vfs_check_access(const vfs_node_t *n, u32 rights)
{ assert(n == &node && rights == VFS_ACCESS_READ); return allowed; }
bool mutex_lock(mutex_t *lock) {
    if (lock == &super.operation_lock) {
        if (cancel_operation) { cancel_operation = false; return false; }
        return pthread_mutex_lock(&operation) == 0;
    }
    if (cancel_cursor) { cancel_cursor = false; return false; }
    return pthread_mutex_lock(&cursor_mutex) == 0;
}
bool mutex_unlock(mutex_t *lock)
{ return pthread_mutex_unlock(lock == &super.operation_lock ? &operation : &cursor_mutex) == 0; }
void mutex_init(mutex_t *m) { memset(m, 0, sizeof(*m)); }
bool vfs_super_operation_lock(vfs_super_t *s)
{ return vfs_super_is_live(s) && mutex_lock(&s->operation_lock); }
bool vfs_node_retain_super(vfs_node_t *n, vfs_super_t **out)
{ if (!vfs_node_is_live(n)) return false; super.reference_count++; *out = &super; return true; }
void vfs_super_release(vfs_super_t *s)
{ assert(s == &super && s->reference_count); s->reference_count--; }
static bool vfs_node_operation_begin(vfs_node_t *n, vfs_super_t **out)
{ if (!vfs_node_retain_super(n, out)) return false;
  if (vfs_super_operation_lock(*out)) return true; vfs_super_release(*out); return false; }
static void vfs_node_operation_end(vfs_super_t *s)
{ mutex_unlock(&s->operation_lock); vfs_super_release(s); }
bool vfs_directory_has_mount_children(vfs_node_t *n) { assert(n == &node); return mounts; }
static bool vfs_mount_parent_matches(vfs_mount_t *m, vfs_node_t *n)
{ return n == &node && mounts && m->sb; }
u64 spin_lock_irqsave(spinlock_t *l) { assert(l == &vfs_metadata_lock); return 7U; }
void spin_unlock_irqrestore(spinlock_t *l, u64 f) { assert(l == &vfs_metadata_lock && f == 7U); }
static int checked(vfs_node_t *n, u32 index, vfs_dirent_t *out) {
    assert(n == &node); calls++;
    strcpy(out->name, "unreported"); /* Exercise output shielding on errors. */
    if (injected) return injected;
    if (index >= limit) return 0;
    if ((long_at_one && index == 1U) || (stream && index % 5U == 1U)) return VFS_ERR_NAME_TOO_LONG;
    snprintf(out->name, sizeof(out->name), "entry%u", index);
    out->inode = index + 1U; out->type = VFS_TYPE_FILE;
    if (cancel_after_first && !index) cancel_operation = true;
    return 1;
}
static int next(void *unused, vfs_dirent_t *out) {
    (void)unused; int result = checked(&node, position, out);
    if (result == 1 || result == VFS_ERR_NAME_TOO_LONG) position++;
    return result;
}
static bool legacy(vfs_node_t *n, u32 index, vfs_dirent_t *out)
{ return checked(n, index, out) == 1; }
static bool open_cursor(vfs_node_t *n, void **out) { assert(n == &node); *out = &position; return true; }
static void close_cursor(void *state) { assert(state == &position); closes++; }
'''
prefix += r"""
void vfs_super_operation_unlock(vfs_super_t *s) { (void)mutex_unlock(&s->operation_lock); }
int vfs_super_operation_error(const vfs_super_t *s) { return vfs_super_is_live(s) ? VFS_OK : VFS_ERR_DEVICE_GONE; }
"""
code = prefix + directory_type + mount_type
for name in ("vfs_readdir_driver_checked", "vfs_readdir_internal", "vfs_readdir_checked",
             "vfs_readdir_trusted_checked", "vfs_readdir", "vfs_readdir_trusted"):
    code += function(vfs, name)
for name in ("directory_native_error", "object_directory_read_locked", "object_directory_read",
             "object_directory_read_batch", "directory_destroy", "object_init",
             "object_directory_create_node"):
    code += function(obj, name)
code += r'''
static vfs_ops_t ops;
static kernel_object_t *reset(bool sequential) {
    assert(super.reference_count == 0U);
    memset(&ops, 0, sizeof(ops)); memset(&node, 0, sizeof(node));
    memset(&super, 0, sizeof(super)); memset(mount_table, 0, sizeof(mount_table));
    node.super = &super; node.type = VFS_TYPE_DIRECTORY; node.ops = &ops;
    live = allowed = true; cancel_cursor = cancel_operation = fail_alloc = mounts = false;
    injected = 0; position = calls = 0U; limit = 3U; stream = cancel_after_first = false; long_at_one = true;
    ops.readdir_checked = checked;
    if (sequential) { ops.readdir_open = open_cursor; ops.readdir_next_checked = next; ops.readdir_close = close_cursor; }
    return object_directory_create_node(&node);
}
static void destroy(kernel_object_t *o) { o->destroy(o); assert(!super.reference_count); }
static void output_unchanged(const vfs_dirent_t *before, const vfs_dirent_t *after)
{ assert(!memcmp(before, after, sizeof(*before))); }
static void checked_paths(void) {
    for (unsigned seq = 0U; seq < 2U; seq++) {
        kernel_object_t *o = reset(seq); assert(o);
        vfs_dirent_t out, before; memset(&out, 0x55, sizeof(out)); before = out;
        assert(object_directory_read(o, &out) == 1 && out.inode == 1U);
        before = out;
        assert(object_directory_read(o, &out) == MG_ERR_NAME_TOO_LONG); output_unchanged(&before, &out);
        assert(object_directory_read(o, &out) == 1 && out.inode == 3U); before = out;
        assert(!object_directory_read(o, &out)); output_unchanged(&before, &out); destroy(o);
        o = reset(seq); vfs_dirent_t batch[4], old[4]; memset(batch, 0x55, sizeof(batch)); memcpy(old, batch, sizeof(old));
        assert(object_directory_read_batch(o, batch, 4U) == 1 && batch[0].inode == 1U);
        assert(!memcmp(batch + 1, old + 1, 3U * sizeof(*batch)));
        unsigned prior = calls;
        assert(object_directory_read(o, &out) == MG_ERR_NAME_TOO_LONG && calls == prior);
        assert(object_directory_read_batch(o, batch, 4U) == 1 && batch[0].inode == 3U);
        assert(!object_directory_read_batch(o, batch, 4U)); destroy(o);
        const int errors[] = {VFS_ERR_IO, VFS_ERR_BAD_FORMAT, VFS_ERR_NO_MEM, VFS_ERR_UNSUPPORTED,
            VFS_ERR_UNCERTAIN, VFS_ERR_ACCESS_DENIED};
        for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
            o = reset(seq); injected = errors[i]; memset(&out, 0x55, sizeof(out)); before = out;
            assert(object_directory_read(o, &out) == directory_native_error(errors[i]));
            output_unchanged(&before, &out); assert(!((directory_object_t *)o)->index && !position);
            injected = 0; assert(object_directory_read(o, &out) == 1 && out.inode == 1U); destroy(o);
        }
        o = reset(seq); cancel_cursor = true;
        assert(object_directory_read(o, &out) == MG_ERR_CANCELLED && !calls);
        cancel_operation = true; assert(object_directory_read(o, &out) == MG_ERR_CANCELLED && !calls);
        live = false; assert(object_directory_read(o, &out) == MG_ERR_DEVICE_GONE); live = true;
        assert(object_directory_read(o, &out) == 1 && out.inode == 1U); destroy(o);
        o = reset(seq); assert(object_directory_read_batch(o, batch, 4U) == 1);
        cancel_cursor = true; assert(object_directory_read(o, &out) == MG_ERR_CANCELLED);
        live = false; assert(object_directory_read(o, &out) == MG_ERR_NAME_TOO_LONG);
        assert(object_directory_read(o, &out) == MG_ERR_DEVICE_GONE); live = true; destroy(o);
    }
    for (unsigned seq = 0; seq < 2; seq++) {
        kernel_object_t *o = reset(seq); cancel_after_first = true; long_at_one = false;
        vfs_dirent_t entries[4]; assert(object_directory_read_batch(o, entries, 4) == 1);
        unsigned prior = calls; assert(object_directory_read(o, entries) == MG_ERR_CANCELLED && calls == prior);
        assert(object_directory_read(o, entries) == 1 && entries[0].inode == 2); destroy(o);
    }
    kernel_object_t *overflow = reset(false); ((directory_object_t *)overflow)->index = (u64)(~(u32)0) + 1U;
    vfs_dirent_t untouched; assert(object_directory_read(overflow, &untouched) == MG_ERR_BUFFER_TOO_SMALL && !calls); destroy(overflow);
    kernel_object_t *o = reset(false); ops.readdir_checked = NULL; ops.readdir = legacy; long_at_one = false;
    vfs_dirent_t out; assert(object_directory_read(o, &out) == 1); destroy(o);
}
static void mounts_path(void) {
    kernel_object_t *o = reset(false); mounts = true;
    child_root.inode = 99U; child_super.root_node = &child_root;
    mount_table[1].sb = &child_super; strcpy(mount_table[1].name, "entry2");
    mount_table[2].sb = &child_super; strcpy(mount_table[2].name, "mounted");
    vfs_dirent_t out, old; memset(&out, 0x55, sizeof(out)); old = out;
    assert(vfs_readdir_checked(&node, 1U, &out) == VFS_ERR_NAME_TOO_LONG); output_unchanged(&old, &out);
    assert(vfs_readdir_checked(&node, 3U, &out) == 1 && !strcmp(out.name, "mounted")); old = out;
    assert(!vfs_readdir_checked(&node, 4U, &out)); output_unchanged(&old, &out);
    fail_alloc = true; assert(vfs_readdir_checked(&node, 3U, &out) == VFS_ERR_NO_MEM); fail_alloc = false;
    injected = VFS_ERR_IO; assert(vfs_readdir_checked(&node, 3U, &out) == VFS_ERR_IO);
    allowed = false; assert(vfs_readdir_checked(&node, 0U, &out) == VFS_ERR_ACCESS_DENIED);
    injected = 0; assert(vfs_readdir_trusted_checked(&node, 0U, &out) == 1); destroy(o);
}
static pthread_mutex_t results_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned seen[200], long_errors;
static void *read_thread(void *arg) {
    kernel_object_t *o = arg;
    for (;;) {
        vfs_dirent_t entries[7]; i64 result = object_directory_read_batch(o, entries, 7U);
        if (!result) break;
        pthread_mutex_lock(&results_lock);
        if (result == MG_ERR_NAME_TOO_LONG) long_errors++;
        else { assert(result > 0); for (i64 i = 0; i < result; i++) {
            assert(entries[i].inode && entries[i].inode <= 200U); seen[entries[i].inode - 1U]++;
            if (i) assert(entries[i].inode == entries[i - 1U].inode + 1U);
        } }
        pthread_mutex_unlock(&results_lock);
    }
    return NULL;
}
int main(void) {
    checked_paths(); mounts_path();
    kernel_object_t *o = reset(true); stream = true; limit = 200U;
    pthread_t a, b; assert(!pthread_create(&a, NULL, read_thread, o)); assert(!pthread_create(&b, NULL, read_thread, o));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    for (unsigned i = 0U; i < 200U; i++) assert(seen[i] == (i % 5U == 1U ? 0U : 1U));
    assert(long_errors == 40U); destroy(o);
    printf("Checked directory enumeration: object=%zu bytes, cursor serialization, deferred errors and child mounts passed\n",
        sizeof(directory_object_t));
}
'''
with tempfile.TemporaryDirectory(prefix="mangrove-enumeration-") as directory:
    source = Path(directory) / "test.c"
    binary = source.with_suffix("")
    source.write_text(code)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else "gcc", "-std=gnu11",
                    "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                    "-fsanitize=address,undefined", "-pthread", "-I" + str(root / "kernel/include"),
                    "-I" + str(root / "include"), "-idirafter", str(root / "libc/include"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Production MGFS checked traversal/name-state logic; byte/record reads are
# controlled dependencies. Initramfs executes its production in-memory walker.
mgfs = (root / "kernel/src/storage/mgfs.c").read_text()
ramfs = (root / "kernel/src/initramfs.c").read_text()


def structure(source, name):
    end = source.index("} " + name + ";") + len("} " + name + ";")
    return source[source.rfind("typedef struct", 0, end):end] + "\n"


drivers = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vfs.h>
#define MGFS_RECORD_BYTES 192U
#define MGFS_RECORD_DIRECTORY 2U
#define MGFS_SEEN_NAME_BUCKET_COUNT 64U
#define MGFS_DIRENT_TOMBSTONE 2U
typedef struct {u64 device_id;} mgfs_fs_t;
static bool live, duplicate, fail_alloc;
static u64 read_failure, record_failure;
static unsigned heap;
static void *kmalloc(usize size) { if (fail_alloc) return NULL; void *p = malloc(size); if (p) heap++; return p; }
static void kfree(void *p) { if (p) { assert(heap); heap--; free(p); } }
bool block_device_id_is_live(u64 id) { return id == 1U && live; }
static void mgfs_set_error(const char *message) { assert(message); }
static bool mgfs_bytes_equal(const u8 *a, const u8 *b, usize n) { return !memcmp(a, b, n); }
static bool mgfs_find_record_slot(mgfs_fs_t *fs,u64 id,u64 *slot)
{ (void)fs; *slot=id-1;return id>=1 && id<=3; }
static bool mgfs_read_record(mgfs_fs_t *fs, u64 id, u8 record[192]) {
    assert(fs->device_id == 1U); if (id == record_failure) return false;
    memset(record, 0, 192U); u64 type = id == 1U ? 2U : 1U, size = 64U;
    memcpy(record, &type, 8U); memcpy(record + 16U, &id, 8U); memcpy(record + 32U, &size, 8U); return true;
}
'''
drivers += structure(mgfs, "mgfs_directory_entry_t")
drivers += r'''
static bool mgfs_read_directory_entry(mgfs_fs_t *fs, const u8 *record, u64 off,
                                      mgfs_directory_entry_t *out) {
    assert(fs && record && off < 64U); if (off == read_failure) return false;
    out->target_record_id = off ? 3U : 2U; out->total_bytes = 32U; out->flags = 1U;
    strcpy(out->name, off && !duplicate ? "second" : "first"); out->name_length = strlen(out->name); return true;
}
'''
drivers += structure(mgfs, "mgfs_seen_name_t") + structure(mgfs, "mgfs_directory_cursor_t")
for name in ("mgfs_get_le64", "mgfs_name_hash", "mgfs_seen_bucket", "mgfs_seen_name_contains",
             "mgfs_seen_name_add", "mgfs_free_seen_names", "mgfs_scan_directory_checked",
             "mgfs_readdir_open", "mgfs_readdir_next_checked", "mgfs_readdir_close", "mgfs_readdir_checked"):
    drivers += function(mgfs, name)
drivers += structure(ramfs, "initramfs_entry_t") + function(ramfs, "initramfs_readdir_checked")
drivers += r'''
int main(void) {
    mgfs_fs_t fs = {.device_id = 1U}; vfs_super_t sb = {.private_data = &fs};
    vfs_node_t dir = {.super = &sb, .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    vfs_dirent_t out, before; void *state; live = true; read_failure = record_failure = ~(u64)0;
    assert(mgfs_readdir_checked(&dir, 0U, &out) == 1 && !strcmp(out.name, "first"));
    assert(mgfs_readdir_checked(&dir, 1U, &out) == 1 && !strcmp(out.name, "second")); before = out;
    assert(!mgfs_readdir_checked(&dir, 2U, &out) && !memcmp(&out, &before, sizeof(out)));
    assert(mgfs_readdir_open(&dir, &state));
    record_failure = 2U; assert(mgfs_readdir_next_checked(state, &out) == VFS_ERR_IO);
    assert(!((mgfs_directory_cursor_t *)state)->offset && !memcmp(&out, &before, sizeof(out)));
    record_failure = ~(u64)0; fail_alloc = true;
    assert(mgfs_readdir_next_checked(state, &out) == VFS_ERR_NO_MEM && !((mgfs_directory_cursor_t *)state)->offset);
    fail_alloc = false; assert(mgfs_readdir_next_checked(state, &out) == 1);
    live = false; assert(mgfs_readdir_next_checked(state, &out) == VFS_ERR_DEVICE_GONE); live = true;
    read_failure = 32U; assert(mgfs_readdir_next_checked(state, &out) == VFS_ERR_IO);
    read_failure = ~(u64)0; duplicate = true; before = out;
    assert(mgfs_readdir_next_checked(state, &out) == VFS_ERR_BAD_FORMAT && !memcmp(&out, &before, sizeof(out)));
    duplicate = false; assert(mgfs_readdir_next_checked(state, &out) == 1);
    assert(!mgfs_readdir_next_checked(state, &out)); mgfs_readdir_close(state); assert(!heap);
    fail_alloc = true; assert(mgfs_readdir_checked(&dir, 0U, &out) == VFS_ERR_NO_MEM); fail_alloc = false;
    duplicate = true; assert(mgfs_readdir_checked(&dir, 0U, &out) == VFS_ERR_BAD_FORMAT); duplicate = false;
    assert(!heap);
    initramfs_entry_t a = {.type = VFS_TYPE_FILE}, b = {.type = VFS_TYPE_DIRECTORY}, parent = {.children = &a};
    strcpy(a.name, "file"); memset(b.name, 'b', 255U); b.name[255] = 0; a.next = &b; dir.fs_data = &parent;
    assert(initramfs_readdir_checked(&dir, 0U, &out) == 1 && !strcmp(out.name, "file"));
    assert(initramfs_readdir_checked(&dir, 1U, &out) == 1 && strlen(out.name) == 255U); before = out;
    assert(!initramfs_readdir_checked(&dir, 2U, &out) && !memcmp(&out, &before, sizeof(out)));
    assert(initramfs_readdir_checked(NULL, 0U, &out) == VFS_ERR_INVALID_PARAM);
    puts("Production MGFS/initramfs checked enumeration, failure cleanup and retry passed");
}
'''
with tempfile.TemporaryDirectory(prefix="mangrove-enumeration-drivers-") as directory:
    source = Path(directory) / "test.c"
    binary = source.with_suffix("")
    source.write_text(drivers)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else "gcc", "-std=gnu11",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I" + str(root / "kernel/include"), "-I" + str(root / "include"),
                    "-idirafter", str(root / "libc/include"), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Actual syscall cases and libc wrappers, with controlled process/copy boundaries.
syscalls = (root / 'kernel/src/syscall.c').read_text()
start = syscalls.index('        case SYSCALL_DIRECTORY_READ:')
end = syscalls.index('        case SYSCALL_FILE_CREATE:', start)
abi = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <object.h>
#include <mg/filesystem.h>
typedef u32 process_handle_t;
#define SYSCALL_DIRECTORY_READ 15
#define SYSCALL_DIRECTORY_READ_BATCH 31
typedef struct {u64 rax, rdi, rsi, rdx;} frame_t;
static i64 answer; static bool valid = true, fail_alloc; static unsigned references, allocations;
static kernel_object_t object;
static void *process_current(void) { return &object; }
static kernel_object_t *process_handle_lookup_retain(void *p, process_handle_t h, kernel_object_type_t t, u32 r)
{ assert(p && h == 7 && t == OBJECT_TYPE_DIRECTORY && r == OBJECT_RIGHT_READ); references++; return &object; }
void object_release(kernel_object_t *o) { assert(o == &object && references); references--; }
static bool syscall_user_buffer_valid(void *p, usize n) { return p && n && valid; }
static void syscall_fail(frame_t *f, i64 error) { f->rax = (u64)error; }
static void *kmalloc(usize n) { if (fail_alloc) return NULL; allocations++; return malloc(n); }
static void kfree(void *p) { assert(p && allocations); allocations--; free(p); }
i64 object_directory_read(kernel_object_t *o, vfs_dirent_t *out)
{ assert(o == &object); if (answer == 1) { memset(out, 0, sizeof(*out)); strcpy(out->name, "entry"); out->inode = 42; } return answer; }
i64 object_directory_read_batch(kernel_object_t *o, vfs_dirent_t *out, u32 n)
{ assert(n); return object_directory_read(o, out); }
static void dispatch(frame_t *frame, int number) { switch (number) {
'''
abi += syscalls[start:end] + 'default: abort(); }}\n'
abi += r'''
static long mg_syscall(unsigned long number, unsigned long a, unsigned long b, unsigned long c)
{ frame_t f = {.rdi = a, .rsi = b, .rdx = c}; dispatch(&f, number); return (long)f.rax; }
'''
libc = (root / 'libc/src/mangrove_syscall.c').read_text()
abi += function(libc, 'directory_read') + function(libc, 'directory_read_batch')
abi += r'''
int main(void) {
    mg_directory_entry_t out, old; memset(&out, 0x55, sizeof(out)); old = out;
    const i64 errors[] = {MG_ERR_NAME_TOO_LONG, MG_ERR_IO, MG_ERR_UNSUPPORTED, MG_ERR_CANCELLED, MG_ERR_UNCERTAIN};
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
        answer = errors[i]; usize count = 123;
        assert(directory_read(7, &out) == answer && !memcmp(&out, &old, sizeof(out)));
        assert(directory_read_batch(7, &out, 1, &count) == answer && count == 123);
        assert(!memcmp(&out, &old, sizeof(out)) && !references && !allocations);
    }
    answer = 0; usize count = 123;
    assert(directory_read(7, &out) == MG_ERR_END_OF_FILE);
    assert(directory_read_batch(7, &out, 1, &count) == MG_ERR_END_OF_FILE && count == 123);
    fail_alloc = true; assert(directory_read_batch(7, &out, 1, &count) == MG_ERR_NO_MEMORY && !references);
    fail_alloc = false; valid = false; assert(directory_read(7, &out) == MG_ERR_BAD_ARGUMENT && !references); valid = true;
    answer = 1; assert(directory_read(7, &out) == MG_OK && out.identifier == 42);
    assert(directory_read_batch(7, &out, 1, &count) == MG_OK && count == 1);
    assert(!references && !allocations); puts("Production directory syscall/libc error translation and retained ownership passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-enumeration-abi-') as directory:
    source = Path(directory) / 'test.c'; source.write_text(abi); binary = source.with_suffix('')
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root/'kernel/include'), '-I'+str(root/'include'),
                    '-idirafter', str(root/'libc/include'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Listing and completion read loops are production functions; formatting,
# account metadata, candidate storage and directory syscalls are dependencies.
consumers = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <mg/filesystem.h>
#include <mg/account.h>
#define LS_PATH_CAPACITY 512
#define SHOOT_LINE_CAPACITY 512
#define COMPLETION_TEXT_CAPACITY 512
typedef struct {mg_directory_entry_t entry; usize display_width;} ls_entry_t;
typedef struct {char name[512];} completion_candidate_t;
typedef struct {const char *name;} shell_command_info_t;
static unsigned reads, closes, warnings; static mg_result_t error;
mg_result_t directory_open(const char *p) { assert(p); return 7; }
static mg_result_t handle_close(mg_handle_t h) { assert(h == 7); closes++; return MG_OK; }
mg_result_t directory_read_batch(mg_handle_t h, mg_directory_entry_t *out, usize n, usize *count) {
 assert(h == 7 && n); assert(reads < 4); unsigned i = reads++;
 if (i == 1) return error; if (i == 3) return MG_ERR_END_OF_FILE;
 memset(out, 0, sizeof(*out)); strcpy(out->name, i ? "later" : "first"); out->type = MG_PATH_TYPE_FILE; *count = 1; return MG_OK;
}
bool result_is_error(mg_result_t r) { return r < 0; }
static int warning_printf(const char *f, ...) { assert(strstr(f, "255 UTF-8 bytes")); warnings++; return 0; }
#define printf warning_printf
static bool join_path(const char *a, const char *b, char *o, usize n) { return snprintf(o, n, "%s/%s", a, b) > 0; }
static bool fill_metadata(ls_entry_t *e, const char *p, const mg_account_info_t *a, usize n) { (void)e;(void)p;(void)a;(void)n;return true; }
static usize entry_display_width(ls_entry_t *e, bool s) { (void)s; return strlen(e->entry.name); }
static bool append_entry(ls_entry_t **e, usize *n, usize *c, const ls_entry_t *a) { assert(*n < *c); (*e)[(*n)++] = *a; return true; }
static usize shell_builtin_count(void) { return 0; }
static const shell_command_info_t *shell_builtin_at(usize i) { (void)i; return NULL; }
static bool candidate_add(completion_candidate_t *c, usize *n, const char *s, bool d, const char *p, bool cs)
{ (void)d;(void)p;(void)cs; assert(*n < 4); strcpy(c[(*n)++].name, s); return true; }
static bool command_expand_home_path(const char *a, bool b, char *c, usize n) { (void)b; assert(strlen(a) < n); strcpy(c,a);return true; }
static bool command_resolve_path(const char *a, char *c, usize n) { assert(strlen(a) < n);strcpy(c,a);return true; }
'''
consumers += function((root/'userspace/ls/main.c').read_text(), 'collect_directory')
shoot = (root/'userspace/shoot/completion.c').read_text()
for name in ('collect_commands', 'append_text', 'collect_paths'): consumers += function(shoot, name)
consumers += r'''
#undef printf
int main(void) {
 for (unsigned i = 0; i < 2; i++) {
  error = i ? MG_ERR_IO : MG_ERR_NAME_TOO_LONG; reads = closes = warnings = 0;
  ls_entry_t storage[4], *entries = storage; usize count = 0, capacity = 4;
  assert(collect_directory("/", false, false, NULL, 0, &entries, &count, &capacity) == (i ? MG_ERR_IO : MG_OK));
  assert(count == (i ? 1U : 2U) && closes == 1 && warnings == (i ? 0U : 1U));
  completion_candidate_t candidates[4]; count = 0; reads = closes = 0;
  assert(collect_commands(candidates, &count, "", false)); assert(count == (i ? 1U : 2U) && closes == 1);
  count = 0; reads = closes = 0; assert(collect_paths("", false, candidates, &count, false));
  assert(count == (i ? 1U : 2U) && closes == 1);
  if (!i) assert(!strcmp(candidates[1].name, "later"));
 }
 puts("Production ls and Shoot loops: visible warning, continuation and no retry loops passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-enumeration-consumers-') as directory:
    source = Path(directory) / 'test.c'; source.write_text(consumers); binary = source.with_suffix('')
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-I'+str(root/'include'),
                    '-idirafter', str(root/'libc/include'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
