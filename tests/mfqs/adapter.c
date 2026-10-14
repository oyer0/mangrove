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
#define NODE_MAX 512U
#define OP_MAX 256U
static vfs_file_handle_t *handle;
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
    if (!decode(text, (u8 *)path, 256, &size) || size < 2 || path[0] != '/' ||
        memchr(path, 0, size) || memchr(path + 1, '/', size - 1)) return false;
    path[size] = 0;
    return true; /* Initial backend profile: one root-level component. */
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
    if (handle) return VFS_ERR_BUSY;
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

static int execute(const char *operation, const char *a, const char *b,
                   u8 data[DATA_MAX], unsigned *length, u64 *count,
                   u64 *file_size, u64 *offset)
{
    char path[257];
    u64 value;
    int status = VFS_OK;
    *length = 0; *count = 0; *file_size = 0; *offset = 0;
    if (strcmp(operation, "HELLO") == 0) {
        const char *capabilities = FS_NAME ";create-exclusive-empty,create-ordinary,open,write,seek,read,stat,list,close,remount;trusted-single-thread;MFQS1;" FS_CAPS;
        *length = (unsigned)strlen(capabilities);
        memcpy(data, capabilities, *length);
    } else if (strcmp(operation, "CREATE") == 0) {
        if (handle || !path_decode(a, path)) return VFS_ERR_INVALID_PARAM;
        status = vfs_file_create_exclusive("/", path + 1, 0, NULL, NULL, &handle);
    } else if (strcmp(operation, "CREATE_ORDINARY") == 0) {
        vfs_node_t *node = NULL;
        if (handle || !path_decode(a, path)) return VFS_ERR_INVALID_PARAM;
        status = vfs_create(vfs_get_root_node(), path + 1, &node);
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
        if (!size) putchar('-');
        else for (unsigned i = 0; i < size; i++) printf("%02x", data[i]);
        putchar('\n');
    }
    if (ferror(stdin) || handle || pins()) return 2;
    assert(unmount() == VFS_OK);
    mfqs_media_close();
    mfqs_metrics_t metrics;
    mfqs_metrics(&metrics);
    mfqs_heap_release_nodes(retired, retired_count);
    fprintf(stderr, "MFQS: production teardown complete; %u inert cached nodes reaped at process exit\n", retired_count);
    fprintf(stderr, "MFQS_METRICS {\"reads\":%llu,\"writes\":%llu,\"flushes\":%llu,\"heap_requests\":%llu,"
            "\"heap_peak\":%llu,\"cached_node_bytes\":%llu,\"retired_nodes\":%u}\n",
            metrics.reads, metrics.writes, metrics.flushes, metrics.heap_requests, metrics.heap_peak,
            metrics.heap_live, retired_count);
    return 0;
}
