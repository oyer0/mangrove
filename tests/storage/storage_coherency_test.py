#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production block cache/registry, GPT and management range admission.
Pthread parking/IRQs, VFS policy, device media and failures are controlled.
--baseline reproduces the failed initialization and stale child read at the
pinned original commit; neither mode accesses a real storage device.
"""
from pathlib import Path
import ast
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
baseline = '--baseline' in sys.argv
compiler = next((s for s in sys.argv[1:] if not s.startswith('--')), 'cc')
ref = '3c16cf3cac8ddca5a32caba691be704f8cc8b0b8'
def read(path):
    return subprocess.check_output(['git', 'show', ref+':'+path], cwd=root).decode() if baseline else (root/path).read_text()
def strip(source):
    return re.sub(r'^(?:#include .*|#pragma once)\n', '', source, flags=re.M)
def function(source, name):
    for match in re.finditer(r'^(?:static )?[^\n;{}]*\b'+name+r'\s*\(', source, re.M):
        brace = source.index('{', match.end())
        if ';' in source[match.end():brace]: continue
        end, depth = brace+1, 1
        while depth:
            depth += (source[end]=='{')-(source[end]=='}'); end += 1
        return source[match.start():end]+'\n'
    raise AssertionError(name)
module = ast.parse((root/'tests/kernel/mutex_test.py').read_text())
pre = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id=='pre' for t in n.targets))
mutex = strip(read('kernel/src/mutex.c'))
mutex = re.sub(r'static u64 mutex_irq_save\(void\).*?\n}', 'static u64 mutex_irq_save(void){return 0;}', mutex, count=1, flags=re.S)
mutex = re.sub(r'static void mutex_irq_restore\(u64 flags\).*?\n}', 'static void mutex_irq_restore(u64 flags){(void)flags;}', mutex, count=1, flags=re.S)
block = strip(read('kernel/src/block.c')).replace('static mutex_t block_io_gate;', 'static mutex_t block_io_gate={.state_lock=PTHREAD_MUTEX_INITIALIZER};').replace('static spinlock_t block_registry_lock;', 'static spinlock_t block_registry_lock=PTHREAD_MUTEX_INITIALIZER;')
gpt = strip(read('kernel/src/storage/gpt.c')).replace('static spinlock_t partition_state_lock;', 'static spinlock_t partition_state_lock=PTHREAD_MUTEX_INITIALIZER;')
headers = strip(read('kernel/include/block.h')) + strip(read('kernel/include/storage/gpt.h')) + strip(read('kernel/include/storage/format.h'))
model = r'''
#include <string.h>
typedef unsigned u32;typedef unsigned char u8;typedef size_t usize;
#define MG_EVENT_CLASS_DEVICE 1
#define MG_EVENT_CLASS_BLOCK 2
#define MG_EVENT_BLOCK_ADDED 3
#define MG_EVENT_BLOCK_REMOVED 4
#define VFS_OK 0
#define VFS_ERR_INVALID_PARAM -1
#define VFS_ERR_NOT_FOUND -2
#define VFS_ERR_IO -3
#define VFS_ERR_UNSUPPORTED -6
#define VFS_ERR_BAD_FORMAT -5
#define VFS_ERR_ACCESS_DENIED -8
#define VFS_ERR_NO_SPACE -12
#define VFS_ERR_BUSY -10
#define VFS_ERR_DEVICE_GONE -11
#define KERNEL_BOOT_DEBUG_LOG(...) ((void)0)
static void ipc_publish_event(unsigned c,unsigned e,u64 id,const char*n){(void)c;(void)e;(void)id;(void)n;}
static void vfs_device_removed(block_device_t*d){(void)d;}
/* VFS policy is controlled here; complete owner retention is tested separately. */
static bool vfs_registration_retained(u64 id){(void)id;return false;}
static int vfs_format_preflight(block_device_t*d){(void)d;return VFS_OK;}
static void kprint(const char*format,...){(void)format;}
static bool entropy_random_u64(u64*out){*out=123456789;return true;}
static u64 timekeeping_boot_id(void){return 1;}
static u64 timekeeping_monotonic_ms(void){return 1;}
static bool storage_operation_in_progress;
'''
types, model = model.split('#define MG_EVENT_CLASS_DEVICE', 1)
model = '#define MG_EVENT_CLASS_DEVICE' + model
fmt = read('kernel/src/storage/format.c')
management = ''.join(function(fmt,n) for n in ('storage_mutation_begin','storage_mutation_end','storage_mutation_busy'))
if not baseline: management += function(fmt,'storage_management_range_preflight')
source = pre + types + headers + model + function(read('kernel/src/scheduler.c'),'scheduler_mutex_wake') + mutex + block + gpt + management
source += (root/'tests/storage/storage_coherency_test.c').read_text()
with tempfile.TemporaryDirectory(prefix='mangrove-coherency-') as d:
    c = Path(d)/'test.c'; c.write_text(source); exe = Path(d)/'test'
    subprocess.run([compiler, '-O1','-std=c11','-Wall','-Wextra','-Werror','-pthread',
        '-fsanitize=address,undefined', *(['-DCOHERENCY_BASELINE'] if baseline else []), str(c), '-o', str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)
