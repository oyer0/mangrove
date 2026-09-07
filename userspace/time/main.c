/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"
#include "../common/path.h"

static void print_elapsed(u64 milliseconds)
{
    if (milliseconds < 1000ULL) {
        printf("Elapsed: %llu ms\n", milliseconds);
        return;
    }
    if (milliseconds < 60000ULL) {
        printf("Elapsed: %llu.%03llu s\n",
               milliseconds / 1000ULL, milliseconds % 1000ULL);
        return;
    }
    printf("Elapsed: %llum %llu.%03llus\n",
           milliseconds / 60000ULL,
           (milliseconds / 1000ULL) % 60ULL,
           milliseconds % 1000ULL);
}

int main(int argc, char **argv)
{
    const char *child_argv[MG_PROCESS_MAX_ARGUMENTS];
    char executable[256];
    mg_monotonic_time_t start;
    mg_monotonic_time_t end;
    mg_result_t result;
    mg_handle_t child;
    i32 status;
    u64 elapsed;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc < 2) {
        command_usage_error(argv[0], "time <command> [arguments...]", NULL);
        return 1;
    }
    if ((usize)(argc - 1) > MG_PROCESS_MAX_ARGUMENTS ||
        !command_build_executable_path(argv[1], executable,
                                       sizeof(executable))) {
        printf("Could not find command: %s\n", argv[1]);
        return 1;
    }

    child_argv[0] = executable;
    for (int index = 2; index < argc; index++)
        child_argv[index - 1] = argv[index];

    result = mg_clock_monotonic(&start);
    if (result != MG_OK) {
        printf("Could not read monotonic clock: %s.\n",
               error_string(result));
        return 1;
    }
    result = process_spawn_argv(child_argv, (usize)(argc - 1));
    if (result_is_error(result)) {
        printf("Could not find command: %s\n", argv[1]);
        return 1;
    }
    child = (mg_handle_t)result;
    result = process_wait(child, &status);
    (void)handle_close(child);
    if (result_is_error(result)) {
        printf("Could not wait for \"%s\": %s.\n",
               argv[1], error_string(result));
        return 1;
    }

    result = mg_clock_monotonic(&end);
    if (result != MG_OK) {
        printf("Could not read monotonic clock: %s.\n",
               error_string(result));
        return status;
    }
    elapsed = end.milliseconds >= start.milliseconds
        ? end.milliseconds - start.milliseconds : 0;
    print_elapsed(elapsed);
    return status;
}
