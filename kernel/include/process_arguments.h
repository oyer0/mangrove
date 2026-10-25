/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <types.h>
#include <mg/error.h>
#include <mg/process_limits.h>

#define PROCESS_USER_STACK_TOP 0x00007ffffff00000ULL
#define PROCESS_USER_STACK_PAGES 8U
#define PROCESS_USER_ARGUMENT_BASE (PROCESS_USER_STACK_TOP - \
    (PROCESS_USER_STACK_PAGES + 1ULL) * 4096ULL - MG_PROCESS_ARGUMENT_BYTES_MAX)
#define PROCESS_ARGUMENT_CHUNK_BYTES 4096U

typedef struct process_argument_chunk {
    struct process_argument_chunk *next;
    usize used;
    u8 data[PROCESS_ARGUMENT_CHUNK_BYTES];
} process_argument_chunk_t;

typedef struct process_arguments {
    usize argc;
    usize bytes;
    usize *offsets;
    process_argument_chunk_t *first;
    process_argument_chunk_t *last;
} process_arguments_t;

/* The validator must authorize every read against the caller's address space.
 * NULL is reserved for trusted kernel strings. All input bytes are snapshotted
 * once; no user pointer is retained after this function returns. */
mg_result_t process_arguments_collect(process_arguments_t *args,
    uintptr_t vector, usize argc,
    bool (*readable)(const void *, usize));
void process_arguments_destroy(process_arguments_t *args);
