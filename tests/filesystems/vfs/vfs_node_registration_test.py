#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production VFS node registration across forced detachment."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[3]
source = (root / "kernel/src/vfs.c").read_text()


def function(name):
    match = re.search(r"^(?:static )?[^\n;{}]*\b" + name +
                      r"\s*\([^;{}]*\)\s*\{", source, re.M)
    if not match:
        raise AssertionError(name)
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
typedef uint64_t u64;
typedef struct vfs_super vfs_super_t;
typedef struct vfs_node vfs_node_t;
typedef struct {u64 id;} block_device_t;
struct vfs_node {vfs_super_t *super; vfs_node_t *next_in_super;
                 u64 inode,size;int type;};
struct vfs_super {block_device_t *dev;u64 device_id;int state;vfs_node_t *nodes;};
#define VFS_SUPER_ACTIVE 1
static vfs_super_t dead_super;
static pthread_mutex_t vfs_metadata_lock=PTHREAD_MUTEX_INITIALIZER;
static bool device_live=true;
static bool block_device_id_is_live(u64 id){return device_live&&id==42;}
static u64 spin_lock_irqsave(pthread_mutex_t *lock)
{assert(!pthread_mutex_lock(lock));return 0;}
static void spin_unlock_irqrestore(pthread_mutex_t *lock,u64 flags)
{(void)flags;assert(!pthread_mutex_unlock(lock));}
'''
fixture += "\n".join(function(name) for name in (
    "vfs_retarget_nodes_locked", "vfs_super_is_live_locked",
    "vfs_node_register", "vfs_nodes_update_size"))
fixture += r'''
int main(void)
{
    block_device_t device={.id=42};
    vfs_super_t super={.dev=&device,.device_id=42,
                       .state=VFS_SUPER_ACTIVE};
    vfs_node_t first={.super=&super,.inode=1,.type=2},
               second={.super=&super};
    vfs_node_register(&first);
    assert(super.nodes==&first&&first.super==&super);
    vfs_nodes_update_size(&super,1,1024);
    assert(first.size==1024); /* directory size uses the same locked helper */
    assert(!pthread_mutex_lock(&vfs_metadata_lock));
    super.state=0;
    vfs_retarget_nodes_locked(&super);
    assert(!pthread_mutex_unlock(&vfs_metadata_lock));
    assert(first.super==&dead_super&&super.nodes==0);
    vfs_node_register(&second);
    assert(second.super==&dead_super&&super.nodes==0);
    super.state=VFS_SUPER_ACTIVE;
    device_live=false;
    vfs_node_t third={.super=&super};
    vfs_node_register(&third);
    assert(third.super==&dead_super&&super.nodes==0);
    puts("VFS node registration after detach and device removal passed");
    return 0;
}
'''

compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
with tempfile.TemporaryDirectory(prefix="mangrove-vfs-nodes-") as directory:
    path = Path(directory) / "fixture.c"
    binary = Path(directory) / "fixture"
    path.write_text(fixture)
    subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-pthread", "-fsanitize=address,undefined", str(path),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
