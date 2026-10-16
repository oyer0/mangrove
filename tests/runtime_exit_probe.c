/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef int i32;
extern void probe_exit(int) __attribute__((noreturn));
extern void probe_quick_exit(int) __attribute__((noreturn));
extern void probe__Exit(int) __attribute__((noreturn));
extern void probe_abort(void) __attribute__((noreturn));
extern int probe_atexit(void (*)(void));
extern int probe_at_quick_exit(void (*)(void));

static jmp_buf exit_point;
static int returned_status;
static int sequence[8];
static size_t sequence_count;

void process_exit(i32 status)
{
    returned_status = status;
    longjmp(exit_point, 1);
}

void __stdio_exit_flush(void) { sequence[sequence_count++] = 7; }

static void record(int value) { sequence[sequence_count++] = value; }
static void first(void) { record(1); }
static void second(void);
static void late(void) { record(4); }
static void second(void) { record(2); assert(probe_atexit(late) == 0); }
static void third(void) { record(3); }
static void qfirst(void) { record(5); }
static void qsecond(void) { record(6); }
static void noop(void) { }

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "normal")) {
        if (!setjmp(exit_point)) {
            assert(probe_atexit(first) == 0);
            assert(probe_atexit(second) == 0);
            assert(probe_atexit(third) == 0);
            probe_exit(41);
        }
        assert(returned_status == 41 && sequence_count == 5);
        assert(sequence[0] == 3 && sequence[1] == 2 &&
               sequence[2] == 4 && sequence[3] == 1 && sequence[4] == 7);
        return 0;
    }
    if (!strcmp(argv[1], "quick")) {
        if (!setjmp(exit_point)) {
            assert(probe_atexit(first) == 0);
            assert(probe_at_quick_exit(qfirst) == 0);
            assert(probe_at_quick_exit(qsecond) == 0);
            probe_quick_exit(42);
        }
        assert(returned_status == 42 && sequence_count == 2);
        assert(sequence[0] == 6 && sequence[1] == 5);
        return 0;
    }
    if (!strcmp(argv[1], "immediate")) {
        if (!setjmp(exit_point)) {
            assert(probe_atexit(first) == 0);
            probe__Exit(43);
        }
        assert(returned_status == 43 && sequence_count == 0);
        return 0;
    }
    if (!strcmp(argv[1], "capacity")) {
        for (size_t i = 0; i < 32; i++) assert(probe_atexit(noop) == 0);
        assert(probe_atexit(noop) != 0);
        for (size_t i = 0; i < 32; i++) assert(probe_at_quick_exit(noop) == 0);
        assert(probe_at_quick_exit(noop) != 0);
        return 0;
    }
    if (!strcmp(argv[1], "abort")) probe_abort();
    return 2;
}
