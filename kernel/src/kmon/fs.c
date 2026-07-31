#include <kmon/fs.h>
#include <vfs.h>
#include <kprint.h>
#include <heap.h>
#include <string.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

static char g_cwd[256] = "/";

const char *kmon_get_cwd(void) {
    return g_cwd;
}

void kmon_set_cwd(const char *path) {
    if (path) {
        strncpy(g_cwd, path, sizeof(g_cwd) - 1);
        g_cwd[sizeof(g_cwd) - 1] = '\0';
    }
}

static void kmon_split_path(const char *norm_path, char *parent_out, usize p_size, char *file_out, usize f_size) {
    const char *last_slash = NULL;
    for (const char *p = norm_path; *p; p++) {
        if (*p == '/') {
            last_slash = p;
        }
    }

    if (!last_slash || last_slash == norm_path) {
        strncpy(parent_out, "/", p_size - 1);
        parent_out[p_size - 1] = '\0';

        const char *name_start = last_slash ? (last_slash + 1) : norm_path;
        strncpy(file_out, name_start, f_size - 1);
        file_out[f_size - 1] = '\0';
    } else {
        usize p_len = (usize)(last_slash - norm_path);
        if (p_len >= p_size) p_len = p_size - 1;
        strncpy(parent_out, norm_path, p_len);
        parent_out[p_len] = '\0';

        strncpy(file_out, last_slash + 1, f_size - 1);
        file_out[f_size - 1] = '\0';
    }
}

void kmon_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    kprint("%s\n", g_cwd);
}

void kmon_cd(int argc, char **argv) {
    const char *target_input = (argc > 1) ? argv[1] : "/";
    char norm[256];
    vfs_resolve_path(g_cwd, target_input, norm, sizeof(norm));

    vfs_node_t *node = NULL;
    int res = vfs_lookup(norm, &node);
    if (res != VFS_OK || !node) {
        kprint("cd: no such directory: %s\n", target_input);
        return;
    }

    if (node->type != VFS_TYPE_DIRECTORY) {
        kprint("cd: not a directory: %s\n", target_input);
        return;
    }

    kmon_set_cwd(norm);
}

void kmon_ls(int argc, char **argv) {
    const char *target_input = (argc > 1) ? argv[1] : g_cwd;
    char norm[256];
    vfs_resolve_path(g_cwd, target_input, norm, sizeof(norm));

    vfs_node_t *dir = NULL;
    int res = vfs_lookup(norm, &dir);
    if (res != VFS_OK || !dir) {
        kprint("ls: cannot access '%s': No such file or directory\n", target_input);
        return;
    }

    if (dir->type != VFS_TYPE_DIRECTORY) {
        kprint("ls: '%s' is not a directory\n", target_input);
        return;
    }

    vfs_dirent_t entry;
    u32 index = 0;
    while (vfs_readdir(dir, index, &entry)) {
        kprint("  [%s] %s (inode: %u)\n",
               (entry.type == VFS_TYPE_DIRECTORY) ? "DIR" : "FILE",
               entry.name, (u32)entry.inode);
        index++;
    }
}

void kmon_cat(int argc, char **argv) {
    if (argc < 2) {
        kprint("Usage: cat <filename>\n");
        return;
    }

    char norm[256];
    vfs_resolve_path(g_cwd, argv[1], norm, sizeof(norm));

    vfs_file_handle_t *handle = NULL;
    int res = vfs_open(norm, VFS_OPEN_READ, &handle);
    if (res != VFS_OK || !handle) {
        kprint("cat: '%s': No such file\n", argv[1]);
        return;
    }

    if (handle->node->type == VFS_TYPE_DIRECTORY) {
        kprint("cat: '%s': Is a directory\n", argv[1]);
        vfs_close(handle);
        return;
    }

    u64 size = handle->node->size;
    if (size == 0) {
        vfs_close(handle);
        return;
    }

    char *buf = (char *)kmalloc(size + 1);
    if (!buf) {
        kprint("cat: out of memory\n");
        vfs_close(handle);
        return;
    }

    u64 bytes = vfs_file_read(handle, size, buf);
    buf[bytes] = '\0';
    kprint("%s\n", buf);
    kfree(buf);
    vfs_close(handle);
}

void kmon_touch(int argc, char **argv) {
    if (argc < 2) {
        kprint("Usage: touch <filename>\n");
        return;
    }

    char norm[256];
    vfs_resolve_path(g_cwd, argv[1], norm, sizeof(norm));

    char parent_path[256];
    char filename[256];
    kmon_split_path(norm, parent_path, sizeof(parent_path), filename, sizeof(filename));

    vfs_node_t *parent = NULL;
    if (vfs_lookup(parent_path, &parent) != VFS_OK || !parent) {
        kprint("touch: cannot touch '%s': Parent directory not found\n", argv[1]);
        return;
    }

    vfs_node_t *out_node = NULL;
    int res = vfs_create(parent, filename, &out_node);
    if (res != VFS_OK) {
        kprint("touch: failed to create '%s' (error: %d)\n", argv[1], res);
    }
}

void kmon_mkdir(int argc, char **argv) {
    if (argc < 2) {
        kprint("Usage: mkdir <dirname>\n");
        return;
    }

    char norm[256];
    vfs_resolve_path(g_cwd, argv[1], norm, sizeof(norm));

    char parent_path[256];
    char dirname[256];
    kmon_split_path(norm, parent_path, sizeof(parent_path), dirname, sizeof(dirname));

    vfs_node_t *parent = NULL;
    if (vfs_lookup(parent_path, &parent) != VFS_OK || !parent) {
        kprint("mkdir: cannot create directory '%s': Parent directory not found\n", argv[1]);
        return;
    }

    vfs_node_t *out_node = NULL;
    int res = vfs_mkdir(parent, dirname, &out_node);
    if (res != VFS_OK) {
        kprint("mkdir: cannot create directory '%s' (error: %d)\n", argv[1], res);
    }
}

void kmon_write(int argc, char **argv) {
    char norm[256];
    usize length;
    if (argc < 3) {
        kprint("Usage: write <path> <text>\n");
        return;
    }
    vfs_resolve_path(g_cwd, argv[1], norm, sizeof(norm));
    vfs_file_handle_t *handle = NULL;
    if (vfs_open(norm, VFS_OPEN_RDWR, &handle) != VFS_OK || !handle) {
        kprint("write: file not found: %s\n", argv[1]);
        return;
    }
    if (handle->node->type != VFS_TYPE_FILE) {
        kprint("write: not a regular file: %s\n", argv[1]);
        vfs_close(handle);
        return;
    }
    length = strlen(argv[2]);
    if (vfs_file_write(handle, length, argv[2]) != length) {
        kprint("write: failed for '%s'\n", argv[1]);
    }
    vfs_close(handle);
}

void kmon_rm(int argc, char **argv) {
    if (argc < 2) {
        kprint("Usage: rm <filename>\n");
        return;
    }

    char norm[256];
    vfs_resolve_path(g_cwd, argv[1], norm, sizeof(norm));

    char parent_path[256];
    char filename[256];
    kmon_split_path(norm, parent_path, sizeof(parent_path), filename, sizeof(filename));

    vfs_node_t *parent = NULL;
    if (vfs_lookup(parent_path, &parent) != VFS_OK || !parent) {
        kprint("rm: cannot remove '%s': No such file\n", argv[1]);
        return;
    }

    int res = vfs_unlink(parent, filename);
    if (res != VFS_OK) {
        kprint("rm: cannot remove '%s' (error: %d)\n", argv[1], res);
    }
}

void kmon_rmdir(int argc, char **argv) {
    if (argc < 2) {
        kprint("Usage: rmdir <dirname>\n");
        return;
    }

    char norm[256];
    vfs_resolve_path(g_cwd, argv[1], norm, sizeof(norm));

    char parent_path[256];
    char dirname[256];
    kmon_split_path(norm, parent_path, sizeof(parent_path), dirname, sizeof(dirname));

    vfs_node_t *parent = NULL;
    if (vfs_lookup(parent_path, &parent) != VFS_OK || !parent) {
        kprint("rmdir: failed to remove '%s': No such directory\n", argv[1]);
        return;
    }

    int res = vfs_rmdir(parent, dirname);
    if (res != VFS_OK) {
        kprint("rmdir: failed to remove '%s' (error: %d)\n", argv[1], res);
    }
}

void kmon_mv(int argc, char **argv) {
    char source[256], destination[256];
    char source_parent_path[256], source_name[256];
    char destination_parent_path[256], destination_name[256];
    vfs_node_t *source_parent = NULL, *destination_parent = NULL;
    int res;

    if (argc != 3) {
        kprint("Usage: mv <source> <destination>\n");
        return;
    }
    vfs_resolve_path(g_cwd, argv[1], source, sizeof(source));
    vfs_resolve_path(g_cwd, argv[2], destination, sizeof(destination));
    if (strcmp(source, destination) == 0) return;
    if (strcmp(source, "/") == 0) {
        kprint("mv: cannot move the root directory\n");
        return;
    }

    kmon_split_path(source, source_parent_path, sizeof(source_parent_path),
                    source_name, sizeof(source_name));
    kmon_split_path(destination, destination_parent_path,
                    sizeof(destination_parent_path), destination_name,
                    sizeof(destination_name));
    if (source_name[0] == '\0' || destination_name[0] == '\0') {
        kprint("mv: invalid source or destination\n");
        return;
    }
    if (vfs_lookup(source_parent_path, &source_parent) != VFS_OK || !source_parent ||
        source_parent->type != VFS_TYPE_DIRECTORY) {
        kprint("mv: source parent is not a directory\n");
        return;
    }
    if (vfs_lookup(destination_parent_path, &destination_parent) != VFS_OK ||
        !destination_parent || destination_parent->type != VFS_TYPE_DIRECTORY) {
        kprint("mv: destination parent is not a directory\n");
        return;
    }
    res = vfs_rename(source_parent, source_name, destination_parent, destination_name);
    if (res != VFS_OK) {
        kprint("mv: failed to move '%s' to '%s' (error: %d)\n",
               argv[1], argv[2], res);
    }
}
