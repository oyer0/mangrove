/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit native random syscall qualification; never installed normally. */
#include <mangrove.h>
#include <mg/random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LARGE_BYTES (2U * 1024U * 1024U)
#define CHILD_COUNT 4U

#define REQUIRE(condition, stage) do { if (!(condition)) { \
    printf("RANDOM1 FAIL %s\n", stage); return 1; } } while (0)

static int child_check(void)
{
    u8 bytes[64];
    REQUIRE(mg_random_status() == MG_OK, "child status");
    REQUIRE(mg_random_bytes(bytes, sizeof(bytes)) == MG_OK, "child bytes");
    puts("RANDOM1 child process request PASS");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "child")) return child_check();
    REQUIRE(argc == 1, "arguments");
    REQUIRE(mg_random_status() == MG_OK, "status");
    REQUIRE(mg_random_bytes(NULL, 0) == MG_OK, "zero length");
    REQUIRE(mg_random_bytes(NULL, 1) == MG_ERR_BAD_ARGUMENT,
            "null destination");
    u8 *bytes = malloc(LARGE_BYTES);
    REQUIRE(bytes, "allocation");
    u64 started = uptime_ms();
    REQUIRE(mg_random_bytes(bytes, LARGE_BYTES) == MG_OK, "large request");
    u64 elapsed = uptime_ms() - started;
    u8 combined = 0;
    for (usize index = 0; index < LARGE_BYTES; index++)
        combined |= bytes[index];
    REQUIRE(combined != 0, "large output not all zero");
    free(bytes);

    const char *child_argv[] = {"/bin/randomtest", "child"};
    mg_handle_t children[CHILD_COUNT];
    for (unsigned index = 0; index < CHILD_COUNT; index++) {
        mg_result_t child = process_spawn_argv(child_argv, 2);
        REQUIRE(child > 0, "spawn child");
        children[index] = (mg_handle_t)child;
    }
    for (unsigned index = 0; index < CHILD_COUNT; index++) {
        i32 status = -1;
        REQUIRE(process_wait(children[index], &status) == MG_OK,
                "wait child");
        REQUIRE(handle_close(children[index]) == MG_OK, "close child");
        REQUIRE(status == 0, "child status result");
    }
    printf("RANDOM1 kernel CSPRNG bytes=2097152 concurrent-processes=4 generation_ms=%llu PASS\n",
           (unsigned long long)elapsed);
    return 0;
}
