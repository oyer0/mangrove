#include <mg/error.h>
#include <mg/memory.h>
#include <mg/object.h>
#include <mg/process.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int allocator_self_test(void)
{
    unsigned char *a = (unsigned char *)malloc(32);
    unsigned char *b = (unsigned char *)malloc(64);
    unsigned char *c = (unsigned char *)malloc(32);
    unsigned char *d;
    unsigned char *large;
    unsigned char *zeroed;
    usize i;

    if (!a || !b || !c || ((uintptr_t)a & 15U) != 0 ||
        ((uintptr_t)b & 15U) != 0 || ((uintptr_t)c & 15U) != 0) return 0;
    for (i = 0; i < 32; i++) a[i] = (unsigned char)i;
    free(b);
    d = (unsigned char *)malloc(16);
    if (d != b) return 0;
    free(a);
    free(d);
    d = (unsigned char *)malloc(120);
    if (d != a) return 0;
    free(d);
    free(c);

    zeroed = (unsigned char *)calloc(32, 2);
    if (!zeroed) return 0;
    for (i = 0; i < 64; i++) if (zeroed[i] != 0) return 0;
    for (i = 0; i < 32; i++) zeroed[i] = (unsigned char)(i + 1);
    zeroed = (unsigned char *)realloc(zeroed, 128);
    if (!zeroed) return 0;
    for (i = 0; i < 32; i++) if (zeroed[i] != (unsigned char)(i + 1)) return 0;
    zeroed = (unsigned char *)realloc(zeroed, 16);
    if (!zeroed) return 0;
    free(zeroed);
    if (calloc(~(usize)0, 2) != (void *)0) return 0;

    large = (unsigned char *)malloc(MG_PAGE_SIZE * 4U + 37U);
    if (!large || ((uintptr_t)large & 15U) != 0) return 0;
    large[0] = 0x5a;
    large[MG_PAGE_SIZE * 4U + 36U] = 0xa5;
    if (large[0] != 0x5a || large[MG_PAGE_SIZE * 4U + 36U] != 0xa5)
        return 0;
    free(large);
    return 1;
}

static int libc_self_test(void)
{
    unsigned char source[8] = { 0, 1, 2, 0x7f, 0x80, 0xff, 6, 7 };
    unsigned char copy[8];
    unsigned char overlap[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    char *found;

    memset(copy, 0, sizeof(copy));
    if (memcpy(copy, source, sizeof(source)) != copy ||
        memcmp(copy, source, sizeof(source)) != 0) return 0;
    if (memcpy(copy, source, 0) != copy) return 0;
    if (memmove(overlap + 2, overlap, 6) != overlap + 2 ||
        memcmp(overlap, "ababcdef", 8) != 0) return 0;
    if (memmove(overlap, overlap + 2, 6) != overlap ||
        memcmp(overlap, "abcdefef", 8) != 0) return 0;
    if (memmove(overlap, overlap, sizeof(overlap)) != overlap) return 0;
    memset(copy, 0x80, sizeof(copy));
    for (usize i = 0; i < sizeof(copy); i++) if (copy[i] != 0x80) return 0;
    if (memset(copy, 0, 0) != copy || memcmp(source, source, 0) != 0) return 0;
    if (memcmp((unsigned char[]){ 0xff }, (unsigned char[]){ 0x01 }, 1) <= 0) return 0;
    if (strlen("") != 0 || strlen("abc") != 3) return 0;
    if (strcmp("abc", "abc") != 0 || strcmp("abc", "abd") >= 0) return 0;
    if (strncmp("abc", "abd", 2) != 0 || strncmp("abc", "abd", 3) >= 0 ||
        strncmp("abc", "xyz", 0) != 0) return 0;
    found = strchr("abcabc", 'b');
    if (!found || *found != 'b' || strchr("abc", 'z') != (char *)0 ||
        strchr("abc", '\0') == (char *)0) return 0;
    return 1;
}

static int anonymous_memory_self_test(void)
{
    void *small = (void *)0;
    void *second = (void *)0;
    void *large = (void *)0;
    void *exit_mapping = (void *)0;
    unsigned char *bytes;
    usize large_size = MG_PAGE_SIZE * 3U + 19U;

    if (memory_map(0, &small) != MG_ERR_BAD_ARGUMENT ||
        memory_map(1, (void **)0) != MG_ERR_BAD_ARGUMENT) return 0;
    if (memory_map(1, &small) != MG_OK ||
        ((uintptr_t)small & (MG_PAGE_SIZE - 1)) != 0) return 0;
    if (memory_map(MG_PAGE_SIZE * 2U, &second) != MG_OK ||
        small == second) return 0;
    if (memory_map(large_size, &large) != MG_OK) return 0;

    bytes = (unsigned char *)small;
    bytes[0] = 0x11;
    if (bytes[0] != 0x11) return 0;
    bytes = (unsigned char *)second;
    bytes[0] = 0x22;
    bytes[MG_PAGE_SIZE] = 0x33;
    if (bytes[0] != 0x22 || bytes[MG_PAGE_SIZE] != 0x33) return 0;
    bytes = (unsigned char *)large;
    bytes[0] = 0x44;
    bytes[MG_PAGE_SIZE - 1] = 0x55;
    bytes[MG_PAGE_SIZE] = 0x66;
    bytes[MG_PAGE_SIZE * 2U] = 0x77;
    bytes[large_size - 1] = 0x88;
    (void)process_yield();
    if (bytes[0] != 0x44 || bytes[MG_PAGE_SIZE - 1] != 0x55 ||
        bytes[MG_PAGE_SIZE] != 0x66 || bytes[MG_PAGE_SIZE * 2U] != 0x77 ||
        bytes[large_size - 1] != 0x88) return 0;

    if (memory_unmap(second) != MG_OK ||
        memory_unmap(large) != MG_OK ||
        memory_unmap(small) != MG_OK ||
        memory_unmap(small) != MG_ERR_NOT_FOUND ||
        memory_unmap((void *)0x0000002000000000ULL) != MG_ERR_NOT_FOUND) {
        return 0;
    }
    if (memory_map(MG_PAGE_SIZE, &exit_mapping) != MG_OK) return 0;
    ((unsigned char *)exit_mapping)[0] = 0xa5;
    return ((unsigned char *)exit_mapping)[0] == 0xa5;
}

static int formatted_output_self_test(void)
{
    char buffer[32];
    i64 minimum = (i64)(~(u64)0 / 2 + 1);
    if (snprintf(buffer, sizeof(buffer), "%s %d %08x %%", "ok", -7, 0x2aU) != 16 ||
        strcmp(buffer, "ok -7 0000002a %") != 0) return 0;
    if (snprintf(buffer, 4, "abcdef") != 6 || strcmp(buffer, "abc") != 0)
        return 0;
    if (snprintf((char *)0, 0, "value %u", 42U) != 8) return 0;
    if (snprintf(buffer, sizeof(buffer), "%s", (char *)0) != 6 ||
        strcmp(buffer, "(null)") != 0) return 0;
    if (snprintf(buffer, sizeof(buffer), "%lld", minimum) != 20 ||
        strcmp(buffer, "-9223372036854775808") != 0) return 0;
    return 1;
}

int main(void)
{
    if (!allocator_self_test()) return 45;
    if (!libc_self_test()) return 43;
    if (!anonymous_memory_self_test()) return 44;
    if (!formatted_output_self_test() ||
        strcmp(error_string(MG_ERR_NOT_FOUND), "not found") != 0)
        return 43;
    if (printf("hello child process running\n") < 0) return 43;
    return 0;
}
