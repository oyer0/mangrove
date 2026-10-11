#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production handle reservation and create syscall with controlled VFS."""
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
    raise AssertionError(f"missing {path}:{name}")


vfs_codes = "\n".join(line for line in
    (ROOT / "kernel/include/vfs.h").read_text().splitlines()
    if line.startswith("#define VFS_OK ") or line.startswith("#define VFS_ERR_"))
mg_codes = "\n".join(line for line in
    (ROOT / "libc/include/mangrove_errors.h").read_text().splitlines()
    if line.startswith("#define MG_OK ") or line.startswith("#define MG_ERR_"))

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
typedef u32 process_handle_t; typedef u32 mg_handle_t;
typedef enum {PROCESS_STATE_ACTIVE, PROCESS_STATE_TERMINATED} process_state_t;
typedef enum {OBJECT_TYPE_INVALID, OBJECT_TYPE_FILE} kernel_object_type_t;
typedef struct kernel_object kernel_object_t;
typedef struct process process_t;
typedef struct {pthread_mutex_t native;} spinlock_t;
typedef struct {kernel_object_t *object; u32 rights; u16 generation;
                bool active, reserved;} process_handle_slot_t;
typedef struct {u64 offset; u64 save_generation;} vfs_file_handle_t;
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
typedef struct mg_file_create_contents_request {
    const char *path; const void *data; usize length; mg_handle_t *out_handle;
} mg_file_create_contents_request_t;
#define OBJECT_RIGHT_READ (1U << 0)
#define OBJECT_RIGHT_WRITE (1U << 1)
#define PROCESS_HANDLE_SLOTS 16U
#define HANDLE_INDEX_MASK 0xffffU
#define FILE_REPLACE_CHUNK_BYTES 4096U
#define MG_FILE_REPLACE_MAX_BYTES (2U * 1024U * 1024U)
#define FILE_REPLACE_CHUNKS (MG_FILE_REPLACE_MAX_BYTES / FILE_REPLACE_CHUNK_BYTES)
typedef struct {u8 **chunks; usize length; usize count;}
    syscall_replace_input_t;
static process_handle_slot_t slots[PROCESS_HANDLE_SLOTS];
static process_t process;
static u8 user_bytes[MG_FILE_REPLACE_MAX_BYTES];
static u8 expected_bytes[MG_FILE_REPLACE_MAX_BYTES];
static char user_path[256]="/parent/new";
static mg_file_create_contents_request_t request;
static mg_handle_t user_handle;
static usize readable_bytes = sizeof(user_bytes);
static bool output_writable=true, request_valid=true, path_valid=true;
static bool change_user_before_source;
static int allocation_budget=-1, vfs_result;
static unsigned live_allocations, destroyed, vfs_calls, published;
static u64 last_length;
static u64 spin_lock_irqsave(spinlock_t *lock) {
    assert(!pthread_mutex_lock(&lock->native)); return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock,u64 flags) {
    (void)flags; assert(!pthread_mutex_unlock(&lock->native));
}
static bool vmm_user_range_valid(const void *address,usize length) {
    uintptr_t start=(uintptr_t)address, base=(uintptr_t)user_bytes;
    if (address==&request) return request_valid && length==sizeof(request);
    if (start>=base && start-base<=readable_bytes)
        return length<=readable_bytes-(start-base);
    return false;
}
static bool vmm_user_range_writable(const void *address,usize length) {
    return output_writable && address==&user_handle && length==sizeof(user_handle);
}
static void *kmalloc(usize size) {
    if (allocation_budget==0) return NULL;
    if (allocation_budget>0) allocation_budget--;
    void *p=malloc(size);
    if (p) __atomic_add_fetch(&live_allocations,1,__ATOMIC_RELAXED);
    return p;
}
static void kfree(void *p) {
    if (!p) return;
    assert(__atomic_load_n(&live_allocations,__ATOMIC_RELAXED));
    __atomic_sub_fetch(&live_allocations,1,__ATOMIC_RELAXED); free(p);
}
static void panic(const char *reason) {
    fprintf(stderr,"panic: %s\n",reason); abort();
}
kernel_object_t *process_handle_lookup_retain(process_t *,
    process_handle_t, kernel_object_type_t, u32);
bool object_reference(kernel_object_t *);
void object_release(kernel_object_t *);
static i64 file_read(kernel_object_t *object,void *b,u64 n) {
    (void)object;(void)b;(void)n;return 0;
}
static i64 file_write(kernel_object_t *object,const void *b,u64 n) {
    (void)object;(void)b;(void)n;return 0;
}
static void file_destroy(kernel_object_t *object) {
    file_object_t *file=(file_object_t *)object;
    destroyed++;
    if (file->handle) { assert(file->handle->offset==0); kfree(file->handle); }
    kfree(file);
}
static bool syscall_user_buffer_valid(const void *p,u64 n) {
    return n==0 || vmm_user_range_valid(p,(usize)n);
}
static bool syscall_resolve_parent(process_t *p,const char *path,
    char *parent,usize parent_size,char *name,usize name_size) {
    assert(p==&process);
    if (!path_valid || path!=user_path || !*path ||
        parent_size<8 || name_size<4) return false;
    strcpy(parent,"/parent"); strcpy(name,"new"); return true;
}
static int vfs_file_create_exclusive(const char *parent,const char *name,
    u64 length,vfs_replace_source_t source,void *context,
    vfs_file_handle_t **out) {
    u8 sample[7];
    assert(!strcmp(parent,"/parent") && !strcmp(name,"new"));
    assert(!process_handle_lookup_retain(&process, (u32)user_handle,
                                         OBJECT_TYPE_FILE,0));
    vfs_calls++; last_length=length;
    if (length) {
        if (change_user_before_source)
            memset(user_bytes,'x',length<7?length:7);
        assert(source(context,0,sample,length<7?(u32)length:7)==
               (length<7?(u32)length:7));
        assert(!memcmp(sample,expected_bytes,length<7?length:7));
    }
    if (vfs_result) return vfs_result;
    *out=kmalloc(sizeof(**out)); assert(*out);
    (*out)->offset=0; (*out)->save_generation=1;
    published++; return VFS_OK;
}
'''

parts = "".join(function(path, name) for path, name in (
    ("kernel/src/process.c", "handle_slots"),
    ("kernel/src/process.c", "encode_handle"),
    ("kernel/src/process.c", "decode_handle"),
    ("kernel/src/process.c", "process_handle_install"),
    ("kernel/src/process.c", "process_handle_reserve"),
    ("kernel/src/process.c", "process_handle_activate"),
    ("kernel/src/process.c", "process_handle_cancel"),
    ("kernel/src/process.c", "process_handle_lookup_reference"),
    ("kernel/src/process.c", "process_handle_lookup_retain"),
    ("kernel/src/process.c", "process_handle_close"),
    ("kernel/src/process.c", "process_handle_close_all"),
    ("kernel/src/object.c", "object_init"),
    ("kernel/src/object.c", "object_reference"),
    ("kernel/src/object.c", "object_release"),
    ("kernel/src/object.c", "object_file_prepare"),
    ("kernel/src/object.c", "object_file_bind_prepared"),
    ("kernel/src/syscall.c", "syscall_vfs_error"),
    ("kernel/src/syscall.c", "syscall_replace_input_release"),
    ("kernel/src/syscall.c", "syscall_replace_input_copy"),
    ("kernel/src/syscall.c", "syscall_replace_source"),
    ("kernel/src/syscall.c", "syscall_file_create_contents"),
))

suffix = r'''
static void reset(void) {
    assert(!live_allocations);
    memset(slots,0,sizeof(slots));
    process.state=PROCESS_STATE_ACTIVE;
    process.handle_table=slots;
    process.handle_capacity=PROCESS_HANDLE_SLOTS;
    readable_bytes=sizeof(user_bytes);
    output_writable=request_valid=path_valid=true;
    change_user_before_source=false;
    allocation_budget=-1; vfs_result=0;
    vfs_calls=published=destroyed=0;
    user_handle=0;
    memcpy(expected_bytes,user_bytes,sizeof(user_bytes));
    request=(mg_file_create_contents_request_t){
        user_path,user_bytes,0,&user_handle};
}
static void assert_clean(void) {
    assert(live_allocations==0);
    for (unsigned i=0;i<PROCESS_HANDLE_SLOTS;i++)
        assert(!slots[i].active && !slots[i].reserved && !slots[i].object);
}
static void test_success(usize length) {
    reset(); request.length=length;
    if (!length) request.data=NULL;
    for (usize i=0;i<length;i++)
        expected_bytes[i]=user_bytes[i]=(u8)i;
    change_user_before_source=length!=0;
    assert(syscall_file_create_contents(&process,&request)==MG_OK);
    assert(vfs_calls==1 && published==1 && last_length==length);
    assert(user_handle);
    kernel_object_t *retained=process_handle_lookup_retain(&process,user_handle,
        OBJECT_TYPE_FILE,OBJECT_RIGHT_READ|OBJECT_RIGHT_WRITE);
    assert(retained);
    assert(!process_handle_cancel(&process,user_handle));
    assert(process_handle_close(&process,user_handle));
    assert(destroyed==0);
    object_release(retained);
    assert(destroyed==1);
    assert_clean();
}
static void test_failure(int failure, i64 expected) {
    reset(); request.length=5000; vfs_result=failure;
    assert(syscall_file_create_contents(&process,&request)==expected);
    assert(vfs_calls==1 && published==0 && user_handle==0);
    assert(destroyed==1); assert_clean();
}
static void test_reservation(void) {
    reset();
    kernel_object_t *object=object_file_prepare(); assert(object);
    process_handle_t reserved=0;
    assert(process_handle_reserve(&process,object,OBJECT_RIGHT_READ,&reserved));
    assert(!process_handle_lookup_retain(&process,reserved,OBJECT_TYPE_FILE,0));
    assert(!process_handle_close(&process,reserved));
    assert(process_handle_cancel(&process,reserved));
    assert(!process_handle_cancel(&process,reserved) && destroyed==1);
    object=object_file_prepare(); assert(object);
    process_handle_t next=0;
    assert(process_handle_reserve(&process,object,OBJECT_RIGHT_READ,&next));
    assert(next!=reserved);
    assert(!process_handle_activate(&process,reserved));
    assert(process_handle_activate(&process,next));
    assert(!process_handle_activate(&process,next));
    assert(process_handle_close(&process,next));
    assert(destroyed==2); assert_clean();
}
static void test_exhaustion(void) {
    reset();
    process_handle_t handles[PROCESS_HANDLE_SLOTS];
    for (unsigned i=0;i<PROCESS_HANDLE_SLOTS;i++) {
        kernel_object_t *o=object_file_prepare(); assert(o);
        assert(process_handle_reserve(&process,o,OBJECT_RIGHT_READ,&handles[i]));
    }
    request.length=0;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_NO_MEMORY);
    assert(vfs_calls==0 && user_handle==0);
    process_handle_close_all(&process);
    assert(destroyed==PROCESS_HANDLE_SLOTS+1);
    assert_clean();
}
static void *reserve_worker(void *argument) {
    process_handle_t *handle=(process_handle_t *)argument;
    kernel_object_t *object=object_file_prepare(); assert(object);
    assert(process_handle_reserve(&process,object,OBJECT_RIGHT_READ,handle));
    return NULL;
}
static void test_concurrent_and_teardown(void) {
    reset();
    pthread_t threads[PROCESS_HANDLE_SLOTS];
    process_handle_t handles[PROCESS_HANDLE_SLOTS]={0};
    for (unsigned i=0;i<PROCESS_HANDLE_SLOTS;i++)
        assert(!pthread_create(&threads[i],NULL,reserve_worker,&handles[i]));
    for (unsigned i=0;i<PROCESS_HANDLE_SLOTS;i++)
        assert(!pthread_join(threads[i],NULL));
    for (unsigned i=0;i<PROCESS_HANDLE_SLOTS;i++) {
        assert(handles[i]);
        for (unsigned j=i+1;j<PROCESS_HANDLE_SLOTS;j++)
            assert(handles[i]!=handles[j]);
        assert(!process_handle_close(&process,handles[i]));
    }
    process_handle_close_all(&process);
    assert(destroyed==PROCESS_HANDLE_SLOTS);
    for (unsigned i=0;i<PROCESS_HANDLE_SLOTS;i++)
        assert(!process_handle_cancel(&process,handles[i]) &&
               !process_handle_activate(&process,handles[i]));
    assert_clean();
}
static void test_preflight_failures(void) {
    reset(); request.length=1; request.out_handle=NULL;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_BAD_ARGUMENT);
    request.out_handle=&user_handle; output_writable=false;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_BAD_ARGUMENT);
    output_writable=true; readable_bytes=0;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_BAD_ARGUMENT);
    readable_bytes=sizeof(user_bytes); path_valid=false;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_BAD_ARGUMENT);
    path_valid=true; request_valid=false;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_BAD_ARGUMENT);
    request_valid=true; request.length=MG_FILE_REPLACE_MAX_BYTES+1;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_REPLACEMENT_TOO_LARGE);
    assert(vfs_calls==0 && destroyed==0); assert_clean();

    reset(); request.length=5000; allocation_budget=2;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_NO_MEMORY);
    assert(vfs_calls==0 && destroyed==1); assert_clean();
    reset(); request.length=5000; allocation_budget=1;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_NO_MEMORY);
    assert(vfs_calls==0 && destroyed==1); assert_clean();
    reset(); allocation_budget=0;
    assert(syscall_file_create_contents(&process,&request)==MG_ERR_NO_MEMORY);
    assert(vfs_calls==0 && destroyed==0); assert_clean();
}
int main(void) {
    assert(!pthread_mutex_init(&process.handle_lock.native,NULL));
    test_reservation(); test_exhaustion(); test_concurrent_and_teardown();
    test_preflight_failures();
    test_success(0); test_success(19);
    test_success(512U*1024U); test_success(512U*1024U+1U);
    test_success(MG_FILE_REPLACE_MAX_BYTES-1U);
    test_success(MG_FILE_REPLACE_MAX_BYTES);
    test_failure(VFS_ERR_ALREADY_EXISTS,MG_ERR_ALREADY_EXISTS);
    test_failure(VFS_ERR_DEVICE_GONE,MG_ERR_DEVICE_GONE);
    test_failure(VFS_ERR_UNCERTAIN,MG_ERR_UNCERTAIN);
    test_failure(VFS_ERR_ACCESS_DENIED,MG_ERR_ACCESS_DENIED);
    puts("production create syscall and reservation: ok");
    return 0;
}
'''

source = vfs_codes + "\n" + mg_codes + "\n" + prefix + parts + suffix
with tempfile.TemporaryDirectory(prefix="mangrove-file-create-") as temp:
    src = Path(temp) / "fixture.c"
    binary = Path(temp) / "fixture"
    src.write_text(source)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-pthread", str(src),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

libc = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t mg_handle_t; typedef int64_t mg_result_t;
typedef size_t usize;
typedef struct mg_file_create_contents_request {
    const char *path; const void *data; usize length; mg_handle_t *out_handle;
} mg_file_create_contents_request_t;
static unsigned long number, arg0, arg1, arg2;
long mg_syscall(unsigned long n,unsigned long a,unsigned long b,unsigned long c) {
    mg_file_create_contents_request_t *request=(void *)a;
    assert(request->path && request->data && request->length==7);
    assert(request->out_handle);
    number=n;arg0=a;arg1=b;arg2=c;
    return 0;
}
''' + function("libc/src/mangrove_syscall.c", "file_create_contents") + r'''
int main(void) {
    mg_handle_t handle=0;
    assert(file_create_contents("/parent/new","content",7,&handle)==0);
    assert(number==90 && arg0 && !arg1 && !arg2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="mangrove-create-libc-") as temp:
    src = Path(temp) / "libc.c"
    binary = Path(temp) / "libc"
    src.write_text(libc)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

assert "SYSCALL_FILE_CREATE_CONTENTS = 90" in (
    ROOT / "kernel/include/syscall.h").read_text()
assert "case SYSCALL_FILE_CREATE_CONTENTS:" in (
    ROOT / "kernel/src/syscall.c").read_text()
