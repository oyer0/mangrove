# SPDX-License-Identifier: GPL-3.0-or-later
"""Native host-only linker selection; target builds never import this module."""
import platform
import sys


def dead_strip_flags():
    return ["-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]


def native_uefi_flags():
    # Preserve the x86-64 Microsoft ABI fixture on Linux/Intel hosts. ARM
    # Darwin can test parsing with native callbacks, not that target ABI.
    return ["-DMANGROVE_HOST_TEST"] if (sys.platform == "darwin" and
        platform.machine().lower() in ("arm64", "aarch64")) else []


def empty_namespace_dependencies(source):
    """Controlled admission for focused dispatch fixtures with no locations.

    These fixtures test hooks/authorization rather than namespace ancestry.
    Real location leases and FIFO admission execute in vfs_location_test and
    mgfs_handoff_test, and complete VFS is linked by MFQS and guest tests.
    Assert the domain instead of allowing a fake location to satisfy a test.
    """
    start = source.index('struct vfs_location {')
    structure = source[start:source.index('\n};', start) + 4]
    return structure + r'''
#define NAMESPACE_SCOPE(failure) ((void)0)
static __attribute__((unused)) vfs_location_t *location_cached_locked(vfs_super_t *sb,u64 inode)
{ (void)inode; assert(!sb->locations); return NULL; }
static __attribute__((unused)) vfs_location_t *location_for_node(vfs_node_t *node)
{ assert(!node || !node->super->locations); return NULL; }
static __attribute__((unused)) void location_note_child(vfs_node_t *parent,const char *name,vfs_node_t *child)
{ (void)name; (void)child; assert(!parent->super->locations); }
static __attribute__((unused)) void location_detach(vfs_node_t *node)
{ assert(!node || !node->super->locations); }
typedef struct {unsigned unused;} location_move_plan_t;
static __attribute__((unused)) int location_move_prepare(vfs_location_t *source,vfs_location_t *parent,const char *name,location_move_plan_t **out)
{ (void)parent;(void)name;(void)out;assert(!source);return VFS_OK; }
static __attribute__((unused)) void location_move_publish(location_move_plan_t *plan){assert(!plan);}
static __attribute__((unused)) void location_move_plan_release(location_move_plan_t **plan){assert(!*plan);}
#define NAMESPACE_MOVE_AUTO __attribute__((cleanup(location_move_plan_release)))
bool vfs_directory_has_mount_children(vfs_node_t *node)
{ assert(!node || !node->super->locations);return false; }
void vfs_location_release(vfs_location_t *location) { assert(!location); }
'''
