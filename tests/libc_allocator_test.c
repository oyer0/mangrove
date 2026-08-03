#include <mangrove.h>
#include <stdlib.h>

static unsigned char arena[1024 * 1024] __attribute__((aligned(4096)));
static usize arena_used;

mg_result_t memory_map(usize size, void **out_address)
{
    usize aligned;
    if (!size || !out_address) return MG_ERR_BAD_ARGUMENT;
    aligned = (size + MG_PAGE_SIZE - 1) & ~(MG_PAGE_SIZE - 1);
    if (aligned < size || aligned > sizeof(arena) ||
        arena_used > sizeof(arena) - aligned)
        return MG_ERR_NO_MEMORY;
    *out_address = arena + arena_used;
    arena_used += aligned;
    return MG_OK;
}

mg_result_t memory_unmap(void *address)
{
    (void)address;
    return MG_OK;
}

static int equal_bytes(const unsigned char *bytes, unsigned char value,
                       usize count)
{
    for (usize i = 0; i < count; i++) if (bytes[i] != value) return 0;
    return 1;
}

int main(void)
{
    unsigned char *a = malloc(32);
    unsigned char *b = malloc(64);
    unsigned char *c = malloc(32);
    unsigned char *d;
    unsigned char *e;
    unsigned char *many[256];
    usize i;

    if (!a || !b || !c || ((uintptr_t)a & 15U) != 0) return 1;
    for (i = 0; i < 32; i++) a[i] = (unsigned char)i;
    free(b);
    d = malloc(16);
    if (d != b) return 2;
    free(a);
    free(d);
    e = malloc(120);
    if (e != a) return 3;
    for (i = 0; i < 32; i++) if (e[i] != (unsigned char)i) return 4;

    free(e);
    free(c);
    e = calloc(24, 4);
    if (!e || !equal_bytes(e, 0, 96)) return 5;
    e = realloc(e, 240);
    if (!e) return 6;
    for (i = 0; i < 96; i++) if (e[i] != 0) return 7;
    e = realloc(e, 16);
    if (!e) return 8;
    free(e);
    e = malloc(16);
    if (!e || realloc(e, 0) != (void *)0) return 9;
    if (calloc(~(usize)0, 2) != (void *)0) return 10;
    if (realloc((void *)0, 48) == (void *)0) return 11;

    for (i = 0; i < 256; i++) {
        many[i] = malloc(1024);
        if (!many[i] || ((uintptr_t)many[i] & 15U) != 0) return 12;
    }
    for (i = 0; i < 256; i += 2) free(many[i]);
    for (i = 1; i < 256; i += 2) free(many[i]);
    if (malloc(2 * 1024 * 1024) != (void *)0) return 13;
    return 0;
}
