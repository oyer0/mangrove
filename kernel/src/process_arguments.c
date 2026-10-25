/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <process_arguments.h>
#include <heap.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == 8U, "native argument slots are 64-bit");
_Static_assert(MG_PROCESS_MAX_ARGUMENTS <= 0x7fffffffU, "argc must fit native main's int");
_Static_assert((PROCESS_USER_ARGUMENT_BASE & 4095U) == 0, "argument region must be page aligned");

void process_arguments_destroy(process_arguments_t *args)
{
    if (!args) return;
    process_argument_chunk_t *chunk = args->first;
    while (chunk) {
        process_argument_chunk_t *next = chunk->next;
        kfree(chunk);
        chunk = next;
    }
    kfree(args->offsets);
    memset(args, 0, sizeof(*args));
}

mg_result_t process_arguments_collect(process_arguments_t *args,
    uintptr_t vector, usize argc, bool (*readable)(const void *, usize))
{
    mg_result_t result = MG_ERR_BAD_ARGUMENT;
    usize vector_bytes;
    usize budget;

    if (!args) return result;
    memset(args, 0, sizeof(*args));
    if (!vector || !argc || argc > MG_PROCESS_MAX_ARGUMENTS ||
        argc > (~(usize)0 / sizeof(uintptr_t)) - 1U) return result;
    vector_bytes = (argc + 1U) * sizeof(uintptr_t);
    if (vector > ~(uintptr_t)0 - argc * sizeof(uintptr_t) ||
        (readable && !readable((const void *)vector,
                               argc * sizeof(uintptr_t)))) return result;
    budget = MG_PROCESS_ARGUMENT_BYTES_MAX - vector_bytes;
    args->offsets = kmalloc(argc * sizeof(*args->offsets));
    if (!args->offsets) return MG_ERR_NO_MEMORY;
    args->argc = argc;
    for (usize index = 0; index < argc; index++) {
        uintptr_t source;
        memcpy(&source, (const void *)(vector + index * sizeof(source)),
               sizeof(source));
        if (!source) goto failed;
        args->offsets[index] = args->bytes;
        for (;;) {
            if (args->bytes == budget) {
                result = MG_ERR_BUFFER_TOO_SMALL;
                goto failed;
            }
            if (!args->last || args->last->used == PROCESS_ARGUMENT_CHUNK_BYTES) {
                process_argument_chunk_t *chunk = kmalloc(sizeof(*chunk));
                if (!chunk) { result = MG_ERR_NO_MEMORY; goto failed; }
                chunk->next = NULL;
                chunk->used = 0;
                if (args->last) args->last->next = chunk;
                else args->first = chunk;
                args->last = chunk;
            }
            /* Validate at most the remainder of one source page. Scan only
             * through the first NUL, including for trusted kernel strings. */
            usize count = 4096U - (source & 4095U);
            usize space = PROCESS_ARGUMENT_CHUNK_BYTES - args->last->used;
            if (count > space) count = space;
            if (count > budget - args->bytes) count = budget - args->bytes;
            if (source > ~(uintptr_t)0 - count ||
                (readable && !readable((const void *)source, count))) goto failed;
            const volatile u8 *text = (const volatile u8 *)source;
            u8 *destination = args->last->data + args->last->used;
            usize length = 0;
            bool done = false;
            /* Read each byte once and test its captured value. This avoids
             * both a mutable-terminator race and copying an entire page for
             * every short argument. Kernel strings are not over-read either. */
            while (length < count) {
                u8 byte = text[length];
                destination[length++] = byte;
                if (!byte) { done = true; break; }
            }
            args->last->used += length;
            args->bytes += length;
            source += length;
            if (done) break;
        }
    }
    return MG_OK;
failed:
    process_arguments_destroy(args);
    return result;
}
