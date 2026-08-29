#include <mg/error.h>
#include <mg/object.h>
#include <mg/process.h>
#include <mg/identity.h>
#include <mangrove.h>
#include <mangrove_version.h>
#include <stdio.h>
#include <string.h>
#include "../common/secret_input.h"

#define LOGIN_TEXT_CAPACITY 129U
#define LOGIN_RETRY_DELAY_MS 500U

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

static void login_retry_delay(void)
{
    u64 deadline = uptime_ms() + LOGIN_RETRY_DELAY_MS;
    while (uptime_ms() < deadline) (void)process_yield();
}

static bool authenticate_session(void)
{
    char username[LOGIN_TEXT_CAPACITY];
    char password[LOGIN_TEXT_CAPACITY];
    mg_identity_t identity;

    for (;;) {
        mg_result_t result;

        memset(username, 0, sizeof(username));
        memset(password, 0, sizeof(password));
        printf("Username: ");
        result = read_console_line(username, sizeof(username), true);
        if (result_is_error(result)) {
            clear_secret(username, sizeof(username));
            clear_secret(password, sizeof(password));
            return false;
        }
        printf("Password: ");
        result = read_hidden_line(password, sizeof(password));
        if (result_is_error(result)) {
            clear_secret(username, sizeof(username));
            clear_secret(password, sizeof(password));
            return false;
        }
        result = session_login(username, password);
        clear_secret(username, sizeof(username));
        clear_secret(password, sizeof(password));
        if (!result_is_error(result)) {
            if (!result_is_error(process_get_identity(&identity)))
                (void)process_chdir(identity.home);
            return true;
        }
        printf("Authentication failed.\n");
        login_retry_delay();
    }
}

static bool current_session_is_system(void)
{
    mg_identity_t identity;
    return !result_is_error(process_get_identity(&identity)) &&
           identity.uid == MG_UID_SYSTEM;
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

        if (first_run && current_session_is_system()) {
            if (!authenticate_session()) return;
        }
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
