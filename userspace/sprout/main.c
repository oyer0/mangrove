#include <mg/error.h>
#include <mg/object.h>
#include <mg/process.h>
#include <mangrove_version.h>
#include <stdio.h>

static void print_system_welcome(void)
{
    printf("%s %s\n\nType 'help' for commands.\n\n",
           MANGROVE_NAME, MANGROVE_VERSION);
}

static void report_spawn_failure(mg_result_t result)
{
    printf("Sprout: Shoot spawn failed: %s\n", error_string(result));
}

static void report_wait_failure(mg_result_t result)
{
    printf("Sprout: Shoot wait failed: %s\n", error_string(result));
}

static void supervise_loop(void)
{
    bool first_run = true;

    for (;;) {
        mg_result_t spawn_result;
        mg_result_t wait_result;
        mg_result_t close_result;
        mg_handle_t shoot;
        i32 status = 0;

        if (first_run) {
            print_system_welcome();
            first_run = false;
        }

        spawn_result = process_spawn("/bin/shoot");
        if (result_is_error(spawn_result)) {
            report_spawn_failure(spawn_result);
            (void)process_yield();
            continue;
        }

        shoot = (mg_handle_t)spawn_result;
        wait_result = process_wait(shoot, &status);
        close_result = handle_close(shoot);

        if (result_is_error(wait_result)) {
            report_wait_failure(wait_result);
            (void)process_yield();
            continue;
        }

        if (result_is_error(close_result)) {
            printf("Sprout: Shoot handle close failed: %s\n",
                   error_string(close_result));
            (void)process_yield();
            continue;
        }

        printf("Sprout: Shoot exited with status %d; restarting\n", status);
    }
}

int main(void)
{
    supervise_loop();
    process_exit(0);
}
