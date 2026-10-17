/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

typedef long long mg_result_t;
typedef unsigned long long usize;
extern void *probe_malloc(size_t);
extern void *probe_aligned_alloc(size_t, size_t);
extern void *probe_calloc(size_t, size_t);
extern void *probe_realloc(void *, size_t);
extern void probe_free(void *);
extern int *probe_errno_location(void);

static int fail_mapping;

mg_result_t memory_map(usize size, void **result)
{
    if (fail_mapping) {
        fail_mapping = 0;
        return -6;
    }
    *result = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return *result == MAP_FAILED ? -6 : 0;
}

mg_result_t memory_unmap(void *address)
{
    (void)address;
    return 0;
}

int main(void)
{
    unsigned char *a = (unsigned char *)probe_malloc(3);
    unsigned char *b = (unsigned char *)probe_calloc(16, 4);
    assert(a && b && ((uintptr_t)a & 15U) == 0 && ((uintptr_t)b & 15U) == 0);
    for (size_t i = 0; i < 64; i++) assert(b[i] == 0);
    a[0] = 0x5a; a[1] = 0xa5; a[2] = 0x33;
    fail_mapping = 1;
    assert(probe_realloc(a, 8192) == NULL);
    assert(*probe_errno_location() == ENOMEM);
    assert(a[0] == 0x5a && a[1] == 0xa5 && a[2] == 0x33);
    unsigned char *grown = (unsigned char *)probe_realloc(a, 128);
    assert(grown && grown[0] == 0x5a && grown[1] == 0xa5 && grown[2] == 0x33);
    unsigned char *aligned = probe_aligned_alloc(256, 512);
    unsigned char *guard = probe_malloc(128);
    assert(aligned && guard && ((uintptr_t)aligned & 255U) == 0);
    for (size_t i = 0; i < 512; i++) aligned[i] = (unsigned char)i;
    unsigned char *aligned_grown = probe_realloc(aligned, 8192);
    assert(aligned_grown && ((uintptr_t)aligned_grown & 255U) == 0);
    for (size_t i = 0; i < 512; i++) assert(aligned_grown[i] == (unsigned char)i);
    probe_free(aligned_grown);
    probe_free(guard);
    *probe_errno_location() = 0;
    assert(probe_aligned_alloc(0, 0) == NULL &&
           *probe_errno_location() == EINVAL);
    *probe_errno_location() = 0;
    assert(probe_aligned_alloc(24, 96) == NULL &&
           *probe_errno_location() == EINVAL);
    *probe_errno_location() = 0;
    assert(probe_aligned_alloc(64, 65) == NULL &&
           *probe_errno_location() == EINVAL);
    unsigned char *small_aligned = probe_aligned_alloc(8, 8);
    assert(small_aligned && ((uintptr_t)small_aligned & 7U) == 0);
    *probe_errno_location() = 0;
    assert(probe_calloc(SIZE_MAX, 2) == NULL);
    assert(*probe_errno_location() == ENOMEM);
    assert(probe_malloc(0) == NULL);
    probe_free(small_aligned);
    probe_free(grown); probe_free(b);
    return 0;
}
