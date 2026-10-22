/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MFQS1 transport only. All namespace, allocation, mutation, checksum,
 * recovery handoff and mount/handle teardown work is in production units. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(MFQS_FAT32)
#include <storage/fat32.h>
#define FS_NAME "fat32"
#define FS_INIT fat32_init
#define FS_CAPS "ascii-ci;gap-zero;resize-arbitrary"
#elif defined(MFQS_EXFAT)
#include <storage/exfat.h>
#define FS_NAME "exfat"
#define FS_INIT exfat_init
#define FS_CAPS "ascii-ci;gap-zero;resize-empty-only"
#else
#include <storage/mgfs.h>
#define FS_NAME "mgfs"
#define FS_INIT mgfs_init
#define FS_CAPS "case-sensitive;no-sparse;resize-empty-only"
#endif
#include "host.h"

#define DATA_MAX 8192U
#define COMMAND_MAX (DATA_MAX * 2U + 1024U)
#define NODE_MAX 4096U
#define OP_MAX 8192U
static vfs_file_handle_t *handle;
static u8 snapshot_data[65536];
static vfs_node_t *retired[NODE_MAX];
static unsigned retired_count;

static bool number(const char *text, u64 *out)
{
    if (!*text) return false;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    *out = strtoull(text, &end, 10);
    return !errno && !*end;
}
static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static bool decode(const char *text, u8 *out, unsigned capacity, unsigned *size)
{
    *size = 0;
    if (strcmp(text, "-") == 0) return true;
    size_t length = strlen(text);
    if (length % 2 || length / 2 > capacity) return false;
    for (size_t i = 0; i < length; i += 2) {
        int high = nibble(text[i]), low = nibble(text[i + 1]);
        if (high < 0 || low < 0) return false;
        out[(*size)++] = (u8)((high << 4) | low);
    }
    return true;
}
static bool path_decode(const char *text, char path[257])
{
    unsigned size;
    if (!decode(text, (u8 *)path, 256, &size) || size < 1 || path[0] != '/' ||
        memchr(path, 0, size)) return false;
    path[size] = 0;
    return true; /* Replay validation bounds components and nesting. */
}
static int parent_lookup(char *path, vfs_node_t **parent, char **name)
{
    *name = strrchr(path, '/') + 1;
    if (!**name) return VFS_ERR_INVALID_PARAM;
    if (*name == path + 1) *parent = vfs_get_root_node();
    else { (*name)[-1] = 0; return vfs_lookup(path, parent); }
    return *parent ? VFS_OK : VFS_ERR_NOT_FOUND;
}
static u32 pins(void)
{
    u32 count = 0;
    int status = vfs_recovery_pins_snapshot(NULL, 0, &count);
    assert(status == VFS_OK || status == VFS_ERR_BUFFER_TOO_SMALL);
    return count;
}
static int unmount(void)
{
    vfs_node_t *root = vfs_get_root_node();
    if (!root) return VFS_ERR_NOT_FOUND;
    vfs_super_t *sb = root->super;
    void *driver_state = sb->private_data;
    unsigned first = retired_count;
    for (vfs_node_t *node = sb->nodes; node; node = node->next_in_super) {
        assert(retired_count < NODE_MAX);
        retired[retired_count++] = node;
    }
    int status = vfs_unmount_device(mfqs_device());
    if (status != VFS_OK) { retired_count = first; return status; }
    /* Observe genuine production disposal, without replacing that disposal. */
    assert(!vfs_get_root_node() && !pins());
    assert(!mfqs_heap_contains(sb) && !mfqs_heap_contains(driver_state));
    for (unsigned i = first; i < retired_count; i++)
        assert(!vfs_node_is_live(retired[i]));
    return VFS_OK;
}

typedef struct { const u8 *bytes; unsigned length; } source_t;
static u32 source_copy(void *context, u64 offset, u8 *out, u32 requested)
{
    source_t *source = context;
    if (offset > source->length || requested > source->length - offset) return 0;
    memcpy(out, source->bytes + offset, requested);
    return requested;
}
typedef struct { usize copied; mg_file_lines_state_t state; } snapshot_t;
static int snapshot_sink(void *context, u64 offset, const u8 *input, usize length)
{
    snapshot_t *snapshot = context;
    if (offset != snapshot->copied || offset > sizeof(snapshot_data) ||
        length > sizeof(snapshot_data) - offset) return VFS_ERR_INVALID_PARAM;
    memcpy(snapshot_data + offset, input, length);
    snapshot->copied += length;
    return VFS_OK;
}
static int snapshot_finish(void *context, const mg_file_lines_state_t *state)
{
    snapshot_t *snapshot = context;
    if (state->length != snapshot->copied) return VFS_ERR_BAD_FORMAT;
    snapshot->state = *state;
    return VFS_OK;
}

static int execute(const char *operation, const char *a, const char *b,
                   u8 data[DATA_MAX], unsigned *length, u64 *count,
                   u64 *file_size, u64 *offset)
{
    char path[257], destination[257];
    vfs_node_t *parent = NULL, *other_parent = NULL;
    char *name = NULL, *other_name = NULL;
    u64 value;
    int status = VFS_OK;
    *length = 0; *count = 0; *file_size = 0; *offset = 0;
    if (strcmp(operation, "HELLO") == 0) {
        const char *capabilities = FS_NAME ";create-exclusive-empty,create-ordinary,open,write,seek,read,stat,list,close,remount,truncate,resize,rename,unlink,mkdir,rmdir,snapshot,check,replace,create-contents;trusted-single-thread;MFQS1;" FS_CAPS;
        *length = (unsigned)strlen(capabilities);
        memcpy(data, capabilities, *length);
    } else if (strcmp(operation, "CREATE") == 0 || strcmp(operation, "CREATE_CONTENTS") == 0) {
        unsigned size = 0;
        if (strcmp(operation, "CREATE_CONTENTS") == 0 && !decode(b, data, DATA_MAX, &size))
            return VFS_ERR_INVALID_PARAM;
        source_t source = {data, size};
        if (handle || !path_decode(a, path)) return VFS_ERR_INVALID_PARAM;
        status = parent_lookup(path, &parent, &name);
        if (status == VFS_OK) {
            const char *directory = name == path + 1 ? "/" : path;
            status = vfs_file_create_exclusive(directory, name, size, source_copy, &source, &handle);
        }
    } else if (strcmp(operation, "CREATE_ORDINARY") == 0) {
        vfs_node_t *node = NULL;
        if (handle || !path_decode(a, path)) return VFS_ERR_INVALID_PARAM;
        status = parent_lookup(path, &parent, &name);
        if (status == VFS_OK) status = vfs_create(parent, name, &node);
        if (status == VFS_OK) status = vfs_open_node(node, VFS_OPEN_RDWR, &handle);
    } else if (strcmp(operation, "OPEN") == 0) {
        if (handle || !path_decode(a, path)) return VFS_ERR_INVALID_PARAM;
        status = vfs_open(path, VFS_OPEN_RDWR, &handle);
    } else if (strcmp(operation, "CLOSE") == 0) {
        if (!handle) return VFS_ERR_INVALID_PARAM;
        status = vfs_close(handle);
        if (status == VFS_OK) handle = NULL;
    } else if (strcmp(operation, "WRITE") == 0) {
        unsigned size;
        if (!handle || !decode(b, data, DATA_MAX, &size)) return VFS_ERR_INVALID_PARAM;
        if (handle->offset > 65536 || size > 65536 - handle->offset)
            return VFS_ERR_INVALID_PARAM;
        status = vfs_file_write_status(handle, size, data, count);
    } else if (strcmp(operation, "READ") == 0) {
        if (!handle || !number(a, &value) || value > DATA_MAX)
            return VFS_ERR_INVALID_PARAM;
        *count = vfs_file_read(handle, value, data);
        *length = (unsigned)*count;
    } else if (strcmp(operation, "SEEK") == 0) {
        if (!handle || !number(a, &value) || value > 65536)
            return VFS_ERR_INVALID_PARAM;
        status = vfs_seek(handle, (i64)value, 0, offset);
    } else if (strcmp(operation, "SNAPSHOT") == 0) {
        if (!handle) return VFS_ERR_INVALID_PARAM;
        snapshot_t snapshot = {0};
        status = vfs_file_snapshot_stream(handle, sizeof(snapshot_data), snapshot_sink,
                                          snapshot_finish, &snapshot);
        if (status == VFS_OK) {
            assert(handle->save_expectation_valid && snapshot.state.identifier == handle->save_record_id &&
                   snapshot.state.revision == handle->save_generation && snapshot.state.revision);
            *length = (unsigned)snapshot.copied;
            *count = snapshot.copied;
        }
    } else if (strcmp(operation, "CHECK") == 0) {
        if (!handle) return VFS_ERR_INVALID_PARAM;
        status = vfs_file_save_check(handle, true, NULL);
    } else if (strcmp(operation, "REPLACE") == 0) {
        unsigned size;
        if (!handle || !decode(b, data, DATA_MAX, &size)) return VFS_ERR_INVALID_PARAM;
        source_t source = {data, size};
        status = vfs_file_replace(handle, true, size, source_copy, &source);
    } else if (strcmp(operation, "TRUNCATE") == 0 || strcmp(operation, "RESIZE") == 0) {
        if (!handle || !number(a, &value) || value > 65536) return VFS_ERR_INVALID_PARAM;
        status = strcmp(operation, "TRUNCATE") == 0 ? vfs_truncate_handle(handle) :
                 vfs_resize_handle(handle, value);
    } else if (strcmp(operation, "MKDIR") == 0 || strcmp(operation, "RMDIR") == 0 ||
               strcmp(operation, "UNLINK") == 0 || strcmp(operation, "RENAME") == 0) {
        if (handle || !path_decode(a, path)) return VFS_ERR_INVALID_PARAM;
        status = parent_lookup(path, &parent, &name);
        if (status == VFS_OK && strcmp(operation, "MKDIR") == 0) {
            vfs_node_t *node = NULL;
            status = vfs_mkdir(parent, name, &node);
        } else if (status == VFS_OK && strcmp(operation, "RMDIR") == 0)
            status = vfs_rmdir(parent, name);
        else if (status == VFS_OK && strcmp(operation, "UNLINK") == 0)
            status = vfs_unlink(parent, name);
        else if (status == VFS_OK) {
            if (!path_decode(b, destination)) return VFS_ERR_INVALID_PARAM;
            status = parent_lookup(destination, &other_parent, &other_name);
            if (status == VFS_OK) status = vfs_rename(parent, name, other_parent, other_name);
        }
    } else if (strcmp(operation, "STAT") == 0) {
        vfs_node_t *node = NULL;
        vfs_node_metadata_t metadata;
        if (!path_decode(a, path)) return VFS_ERR_INVALID_PARAM;
        status = vfs_lookup(path, &node);
        if (status == VFS_OK) status = vfs_node_metadata_snapshot(node, &metadata);
        if (status == VFS_OK) {
            *file_size = metadata.size;
            *count = metadata.type;
        }
        return status;
    } else if (strcmp(operation, "LIST") == 0) {
        vfs_node_t *root = vfs_get_root_node();
        if (strcmp(a, "-")) {
            if (!path_decode(a, path)) return VFS_ERR_INVALID_PARAM;
            status = vfs_lookup(path, &root);
            if (status != VFS_OK) return status;
        }
        if (!root) return VFS_ERR_NOT_FOUND;
        for (u32 index = 0; index < 32; index++) {
            vfs_dirent_t entry;
            status = vfs_readdir_checked(root, index, &entry);
            if (status == 0) return VFS_OK;
            if (status != 1) return status;
            unsigned size = (unsigned)strlen(entry.name) + 1;
            if (size > DATA_MAX - *length) return VFS_ERR_BUFFER_TOO_SMALL;
            memcpy(data + *length, entry.name, size);
            *length += size;
            (*count)++;
        }
        return VFS_ERR_BUFFER_TOO_SMALL; /* Never call an incomplete list complete. */
    } else if (strcmp(operation, "REMOUNT") == 0) {
        status = unmount();
        if (status == VFS_OK && !mfqs_media_reopen()) return VFS_ERR_IO;
        if (status == VFS_OK) status = vfs_mount_root(FS_NAME, mfqs_device());
    } else return VFS_ERR_UNSUPPORTED;
    if (handle) {
        vfs_node_metadata_t metadata;
        int facts = vfs_node_metadata_snapshot(handle->node, &metadata);
        if (facts != VFS_OK) return facts;
        *file_size = metadata.size;
        *offset = handle->offset;
    }
    return status;
}

int main(int argc, char **argv)
{
    if (argc != 2 || !mfqs_media_open(argv[1])) {
        fputs("MFQS adapter requires a bounded disposable regular image\n", stderr);
        return 2;
    }
    vfs_init();
    assert(FS_INIT() == VFS_OK);
    int status = vfs_mount_root(FS_NAME, mfqs_device());
    if (status != VFS_OK) { fprintf(stderr, "production mount failed: %d\n", status); return 2; }
    char line[COMMAND_MAX];
    u8 data[DATA_MAX];
    unsigned sequence = 0;
    while (fgets(line, sizeof(line), stdin)) {
        if (++sequence > OP_MAX || !strchr(line, '\n')) return 2;
        line[strcspn(line, "\n")] = 0;
        char *field[5], *cursor = line;
        for (unsigned i = 0; i < 5; i++) {
            field[i] = strsep(&cursor, "\t");
            if (!field[i] || !*field[i]) return 2;
        }
        u64 requested;
        if (cursor || strcmp(field[0], "MFQS1") || !number(field[1], &requested) ||
            requested != sequence) return 2;
        unsigned size;
        u64 count, file_size, offset;
        status = execute(field[2], field[3], field[4], data, &size, &count,
                         &file_size, &offset);
        printf("MFQS1\t%u\t%d\t%llu\t%llu\t%llu\t%u\t", sequence,
               status, file_size, offset, count, pins());
        const u8 *output = strcmp(field[2], "SNAPSHOT") == 0 ? snapshot_data : data;
        if (!size) putchar('-');
        else for (unsigned i = 0; i < size; i++) printf("%02x", output[i]);
        putchar('\n');
        if (fflush(stdout)) return 2;
    }
    if (ferror(stdin) || handle || pins()) return 2;
    assert(unmount() == VFS_OK);
    mfqs_media_close();
    mfqs_metrics_t metrics;
    mfqs_metrics(&metrics);
    mfqs_heap_release_nodes(retired, retired_count);
    fprintf(stderr, "MFQS: production teardown complete; %u inert cached nodes reaped at process exit\n", retired_count);
    fprintf(stderr, "MFQS_METRICS {\"reads\":%llu,\"writes\":%llu,\"flushes\":%llu,\"heap_requests\":%llu,"
            "\"heap_peak\":%llu,\"cached_node_bytes\":%llu,\"retired_nodes\":%u,\"read_sectors\":%llu,\"write_sectors\":%llu}\n",
            metrics.reads, metrics.writes, metrics.flushes, metrics.heap_requests, metrics.heap_peak,
            metrics.heap_live, retired_count, metrics.read_sectors, metrics.write_sectors);
    return 0;
}
