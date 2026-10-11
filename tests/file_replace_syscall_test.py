#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production file-replace syscall boundary with controlled VFS and user maps.

The fixture executes the actual process handle, object reference, chunk-copy,
source, error mapping, and libc wrapper functions. VFS replacement outcomes,
user mappings, allocation failures, and object destruction are controlled.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def function(path, name):
    source = (ROOT / path).read_text()
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\("
    for match in re.finditer(pattern, source, re.M):
        start = source.index("{", match.end())
        if ";" in source[match.end():start]:
            continue
        end, depth = start + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise AssertionError(f"missing production function {path}:{name}")


def run(source, name):
    with tempfile.TemporaryDirectory(prefix="mangrove-file-replace-") as tmp:
        path = Path(tmp) / f"{name}.c"
        binary = Path(tmp) / name
        path.write_text(source)
        subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-pthread",
                        str(path), "-o",
                        str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


vfs_defines = "\n".join(
    line for line in (ROOT / "kernel/include/vfs.h").read_text().splitlines()
    if line.startswith("#define VFS_OK ") or line.startswith("#define VFS_ERR_")
)
native_defines = "\n".join(
    line for line in (ROOT / "libc/include/mangrove_errors.h").read_text().splitlines()
    if line.startswith("#define MG_OK ") or line.startswith("#define MG_ERR_")
)
limit_define = next(
    line for line in (ROOT / "libc/include/mg/filesystem.h").read_text().splitlines()
    if line.startswith("#define MG_FILE_REPLACE_MAX_BYTES ")
)

prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef size_t usize; typedef int64_t i64;
typedef u32 mg_handle_t; typedef i64 mg_result_t;
typedef u32 process_handle_t;
typedef enum {PROCESS_STATE_ACTIVE, PROCESS_STATE_TERMINATED} process_state_t;
typedef enum {OBJECT_TYPE_INVALID, OBJECT_TYPE_FILE} kernel_object_type_t;
typedef struct kernel_object kernel_object_t;
typedef struct process process_t;
typedef struct {pthread_mutex_t native;} spinlock_t;
typedef struct {kernel_object_t *object; u32 rights; u16 generation;
                bool active, reserved;} process_handle_slot_t;
typedef struct {u64 offset, save_generation;} vfs_file_handle_t;
typedef u32 (*vfs_replace_source_t)(void *, u64, u8 *, u32);
struct kernel_object {
    kernel_object_type_t type; u32 ref_count;
    void (*destroy)(kernel_object_t *); void *read, *write;
};
typedef struct {kernel_object_t base; vfs_file_handle_t *handle;} file_object_t;
struct process {
    process_state_t state; void *handle_table; usize handle_capacity;
    spinlock_t handle_lock;
};
#define OBJECT_RIGHT_READ (1U << 0)
#define OBJECT_RIGHT_WRITE (1U << 1)
#define PROCESS_HANDLE_SLOTS 16U
#define HANDLE_INDEX_MASK 0xffffU
#define FILE_REPLACE_CHUNK_BYTES 4096U
#define FILE_REPLACE_CHUNKS (MG_FILE_REPLACE_MAX_BYTES / FILE_REPLACE_CHUNK_BYTES)
typedef struct {u8 *chunks[FILE_REPLACE_CHUNKS]; usize length;}
    syscall_replace_input_t;
static process_t process;
static u8 user_bytes[MG_FILE_REPLACE_MAX_BYTES];
static u8 expected_bytes[MG_FILE_REPLACE_MAX_BYTES];
static usize readable_bytes = MG_FILE_REPLACE_MAX_BYTES;
static int allocation_budget = -1, forced_vfs_error;
static unsigned live_chunks, total_chunks, destroyed, vfs_calls;
static bool change_user_before_source, close_and_reuse;
static process_handle_t closing_handle, reused_handle;
static u64 disk_generation = 1, last_length;
static file_object_t *active_file;
static u64 spin_lock_irqsave(spinlock_t *lock) {
    assert(!pthread_mutex_lock(&lock->native)); return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    (void)flags; assert(!pthread_mutex_unlock(&lock->native));
}
static bool vmm_user_range_valid(const void *address, usize length) {
    uintptr_t start=(uintptr_t)address, base=(uintptr_t)user_bytes;
    return start>=base && start-base<=readable_bytes &&
           length<=readable_bytes-(start-base);
}
static void *kmalloc(usize size) {
    if (allocation_budget==0) return NULL;
    if (allocation_budget>0) allocation_budget--;
    void *block=malloc(size);
    if (block) {live_chunks++; total_chunks++;}
    return block;
}
static void kfree(void *block) {
    if (!block) return;
    assert(live_chunks); live_chunks--; free(block);
}
static void file_destroy(kernel_object_t *object) {
    file_object_t *file=(file_object_t *)object;
    destroyed++; free(file->handle); free(file);
}
static int vfs_file_replace(vfs_file_handle_t *handle, bool write_right,
                            u64 length, vfs_replace_source_t source,
                            void *context);
bool object_reference(kernel_object_t *object);
void object_release(kernel_object_t *object);
'''

parts = "".join(function(path, name) for path, name in (
    ("kernel/src/process.c", "handle_slots"),
    ("kernel/src/process.c", "encode_handle"),
    ("kernel/src/process.c", "decode_handle"),
    ("kernel/src/process.c", "process_handle_install"),
    ("kernel/src/process.c", "process_handle_lookup_reference"),
    ("kernel/src/process.c", "process_handle_lookup_retain"),
    ("kernel/src/process.c", "process_handle_close"),
    ("kernel/src/object.c", "object_init"),
    ("kernel/src/object.c", "object_reference"),
    ("kernel/src/object.c", "object_release"),
    ("kernel/src/object.c", "object_file_replace"),
    ("kernel/src/syscall.c", "syscall_user_buffer_valid"),
    ("kernel/src/syscall.c", "syscall_vfs_error"),
    ("kernel/src/syscall.c", "syscall_replace_source"),
    ("kernel/src/syscall.c", "syscall_file_replace"),
))

suffix = r'''
static file_object_t *new_file(void) {
    file_object_t *file=calloc(1,sizeof(*file)); assert(file);
    file->handle=calloc(1,sizeof(*file->handle)); assert(file->handle);
    file->handle->offset=37; file->handle->save_generation=disk_generation;
    object_init(&file->base,OBJECT_TYPE_FILE,file_destroy);
    return file;
}
static process_handle_t install_file(file_object_t *file, u32 rights) {
    process_handle_t handle=0;
    assert(process_handle_install(&process,&file->base,rights,&handle));
    object_release(&file->base); return handle;
}
static void *close_worker(void *argument) {
    (void)argument;
    assert(process_handle_close(&process,closing_handle));
    reused_handle=install_file(new_file(),OBJECT_RIGHT_WRITE);
    return NULL;
}
static int vfs_file_replace(vfs_file_handle_t *handle, bool write_right,
                            u64 length, vfs_replace_source_t source,
                            void *context) {
    u8 block[3000];
    assert(write_right && handle && source && context);
    assert(handle->offset==37); vfs_calls++; last_length=length;
    assert(live_chunks==(length+FILE_REPLACE_CHUNK_BYTES-1U)/
                        FILE_REPLACE_CHUNK_BYTES);
    if (close_and_reuse) {
        pthread_t closer; close_and_reuse=false;
        assert(!pthread_create(&closer,NULL,close_worker,NULL));
        assert(!pthread_join(closer,NULL));
        assert(active_file->base.ref_count==1 &&
               closing_handle!=reused_handle && destroyed==0);
    }
    if (forced_vfs_error) return forced_vfs_error;
    if (disk_generation!=handle->save_generation)
        return VFS_ERR_STALE_GENERATION;
    if (change_user_before_source) {
        memset(user_bytes,0xa5,(usize)length);
        change_user_before_source=false;
    }
    for (u64 offset=0; offset<length;) {
        u32 requested=length-offset<sizeof(block) ?
            (u32)(length-offset) : (u32)sizeof(block);
        assert(source(context,offset,block,requested)==requested);
        assert(!memcmp(block,expected_bytes+offset,requested));
        offset+=requested;
    }
    assert(source(context,length+1,block,1)==0);
    handle->save_generation++; disk_generation++;
    return VFS_OK;
}
static void input(usize length) {
    for (usize i=0;i<length;i++) user_bytes[i]=(u8)(i*23U+7U);
    memcpy(expected_bytes,user_bytes,length);
}
int main(void) {
    process_handle_slot_t slots[PROCESS_HANDLE_SLOTS]={0};
    process.state=PROCESS_STATE_ACTIVE; process.handle_table=slots;
    process.handle_capacity=PROCESS_HANDLE_SLOTS;
    assert(!pthread_mutex_init(&process.handle_lock.native,NULL));
    process_handle_t readonly=install_file(new_file(),OBJECT_RIGHT_READ);
    input(73);
    assert(syscall_file_replace(&process,readonly,user_bytes,73)==
           MG_ERR_INVALID_HANDLE);
    assert(!vfs_calls && !live_chunks);
    assert(process_handle_close(&process,readonly) && destroyed==1);

    active_file=new_file();
    process_handle_t handle=install_file(active_file,OBJECT_RIGHT_WRITE);
    assert(syscall_file_replace(&process,handle,(void *)1,8)==MG_ERR_BAD_ARGUMENT);
    input(8192); readable_bytes=4096;
    assert(syscall_file_replace(&process,handle,user_bytes,8192)==MG_ERR_BAD_ARGUMENT);
    assert(!vfs_calls && !live_chunks);
    readable_bytes=MG_FILE_REPLACE_MAX_BYTES;
    assert(syscall_file_replace(&process,handle,user_bytes,
           MG_FILE_REPLACE_MAX_BYTES+1U)==MG_ERR_REPLACEMENT_TOO_LARGE);
    assert(!vfs_calls && !live_chunks);

    input(3U*FILE_REPLACE_CHUNK_BYTES);
    allocation_budget=2;
    assert(syscall_file_replace(&process,handle,user_bytes,
           3U*FILE_REPLACE_CHUNK_BYTES)==MG_ERR_NO_MEMORY);
    assert(total_chunks==2 && !live_chunks && !vfs_calls &&
           active_file->base.ref_count==1);
    allocation_budget=-1;
    assert(syscall_file_replace(&process,handle,NULL,0)==MG_OK);
    assert(last_length==0 && active_file->handle->save_generation==2 &&
           active_file->handle->offset==37 && !live_chunks);
    input(73); change_user_before_source=true;
    assert(syscall_file_replace(&process,handle,user_bytes,73)==MG_OK);
    assert(active_file->handle->save_generation==3 &&
           active_file->handle->offset==37 && !live_chunks);
    input(MG_FILE_REPLACE_MAX_BYTES); change_user_before_source=true;
    assert(syscall_file_replace(&process,handle,user_bytes,
           MG_FILE_REPLACE_MAX_BYTES)==MG_OK);
    assert(active_file->handle->save_generation==4 &&
           active_file->handle->offset==37 && !live_chunks);

    input(4097); disk_generation++;
    assert(syscall_file_replace(&process,handle,user_bytes,4097)==
           MG_ERR_STALE_GENERATION);
    assert(active_file->handle->save_generation==4 && !live_chunks);
    const struct {int vfs, native;} failures[] = {
        {VFS_ERR_ACCESS_DENIED,MG_ERR_ACCESS_DENIED},
        {VFS_ERR_UNSUPPORTED,MG_ERR_UNSUPPORTED},
        {VFS_ERR_NO_SPACE,MG_ERR_NO_SPACE},
        {VFS_ERR_DEVICE_GONE,MG_ERR_DEVICE_GONE},
        {VFS_ERR_UNCERTAIN,MG_ERR_UNCERTAIN},
    };
    for (usize i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
        forced_vfs_error=failures[i].vfs;
        assert(syscall_file_replace(&process,handle,user_bytes,4097)==
               failures[i].native);
        assert(active_file->handle->save_generation==4 &&
               active_file->handle->offset==37 && !live_chunks &&
               active_file->base.ref_count==1);
    }
    forced_vfs_error=0;
    /* A retained old object survives concurrent close and slot reuse. */
    disk_generation=active_file->handle->save_generation;
    closing_handle=handle; close_and_reuse=true; input(4101);
    destroyed=0;
    assert(syscall_file_replace(&process,handle,user_bytes,4101)==MG_OK);
    assert(destroyed==1 && !live_chunks &&
           !process_handle_lookup_retain(&process,handle,OBJECT_TYPE_FILE,0));
    assert((handle & 0xffffU)==(reused_handle & 0xffffU));
    assert(process_handle_close(&process,reused_handle) && destroyed==2);
    assert(!pthread_mutex_destroy(&process.handle_lock.native));
    puts("file replace syscall: production handles, chunk source, bridge and errors passed");
    return 0;
}
'''

run(native_defines + "\n" + limit_define + "\n" +
    vfs_defines + "\n" + prefix + parts + suffix, "kernel")

libc = native_defines + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint32_t mg_handle_t; typedef int64_t mg_result_t;
typedef size_t usize;
static unsigned long number, arg0, arg1, arg2;
long mg_syscall(unsigned long n, unsigned long a, unsigned long b,
                unsigned long c) {
    number=n; arg0=a; arg1=b; arg2=c; return MG_ERR_STALE_GENERATION;
}
''' + function("libc/src/mangrove_syscall.c", "file_replace") + function(
    "libc/src/mangrove_syscall.c", "error_string") + r'''
int main(void) {
    char bytes[3]={1,2,3};
    assert(file_replace(77,bytes,3)==MG_ERR_STALE_GENERATION);
    assert(number==89 && arg0==77 && arg1==(unsigned long)bytes && arg2==3);
    assert(file_replace(77,NULL,0)==MG_ERR_STALE_GENERATION);
    assert(number==89 && arg1==0 && arg2==0);
    assert(strcmp(error_string(MG_ERR_STALE_GENERATION),"unknown error"));
    assert(strcmp(error_string(MG_ERR_REPLACEMENT_TOO_LARGE),"unknown error"));
    assert(strcmp(error_string(MG_ERR_NO_SPACE),"unknown error"));
    puts("file replace libc: syscall 89 ABI and errors passed");
    return 0;
}
'''
run(libc, "libc")

assert "SYSCALL_FILE_REPLACE = 89" in (ROOT / "kernel/include/syscall.h").read_text()
assert "case SYSCALL_FILE_REPLACE:" in (ROOT / "kernel/src/syscall.c").read_text()
