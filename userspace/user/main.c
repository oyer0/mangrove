#include <mangrove.h>
#include <mg/error.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/secret_input.h"

#define USER_PASSWORD_CAPACITY 129U

static const char *role_name(mg_identity_role_t role)
{
    if (role == MG_IDENTITY_ROLE_REGULAR) return "regular";
    if (role == MG_IDENTITY_ROLE_ADMIN) return "admin";
    return "unknown";
}

static int usage_error(const char *argv0, const char *option)
{
    command_usage_error(argv0, "user <command> [options]", option);
    return 1;
}

static void print_account(const mg_account_info_t *account)
{
    printf("UID: %u\nUsername: %s\nRole: %s\nHome: %s\n",
           account->uid, account->username, role_name(account->role),
           account->home);
}

static int report_failure(const char *action, mg_result_t result)
{
    if (result == MG_ERR_PRIVILEGE_REQUIRED) {
        printf("This action requires administrator privileges.\n");
        return 1;
    }
    if (result == MG_ERR_CANCELLED) {
        printf("Action cancelled.\n");
        return 0;
    }
    printf("Could not %s: %s.\n", action, error_string(result));
    return 1;
}

static int show_current_identity(void)
{
    mg_identity_t identity;
    mg_result_t result = process_get_identity(&identity);

    if (result_is_error(result))
        return report_failure("query identity", result);
    printf("UID: %u\nUsername: %s\nRole: %s\nHome: %s\n",
           identity.uid, identity.username, role_name(identity.role),
           identity.home);
    return 0;
}

static int list_accounts(void)
{
    mg_account_info_t accounts[MG_ACCOUNT_MAX_RECORDS];
    usize count = 0;
    mg_result_t result = account_list(accounts, MG_ACCOUNT_MAX_RECORDS, &count);

    if (result_is_error(result)) return report_failure("list accounts", result);
    for (usize index = 0; index < count; index++) {
        printf("%u %s %s %s\n", accounts[index].uid, accounts[index].username,
               role_name(accounts[index].role), accounts[index].home);
    }
    return 0;
}

static int show_account(const char *username)
{
    mg_account_info_t account;
    mg_result_t result = account_show(username, &account);

    if (result_is_error(result)) return report_failure("show account", result);
    print_account(&account);
    return 0;
}

static int remove_account(int argc, char **argv)
{
    const char *username;
    bool purge = false;
    mg_result_t result;

    if (argc == 3) {
        if (!strcmp(argv[2], "--purge") || !strcmp(argv[2], "-p")) {
            return usage_error(argv[0], argv[2]);
        }
        username = argv[2];
    } else if (argc == 4 && (!strcmp(argv[2], "--purge") ||
                             !strcmp(argv[2], "-p"))) {
        username = argv[3];
        purge = true;
    } else {
        return usage_error(argv[0], argc > 2 && argv[2][0] == '-' ?
                           argv[2] : NULL);
    }
    result = account_remove(username, purge);
    if (result_is_error(result)) return report_failure("remove account", result);
    printf("Removed account \"%s\"%s.\n", username,
           purge ? " and home" : "");
    return 0;
}

static int create_account(const char *username)
{
    char password[USER_PASSWORD_CAPACITY];
    char confirmation[USER_PASSWORD_CAPACITY];
    mg_result_t result;

    memset(password, 0, sizeof(password));
    memset(confirmation, 0, sizeof(confirmation));
    printf("New password: ");
    result = read_hidden_line(password, sizeof(password));
    if (result_is_error(result)) {
        clear_secret(password, sizeof(password));
        clear_secret(confirmation, sizeof(confirmation));
        return report_failure("read password", result);
    }
    printf("Confirm password: ");
    result = read_hidden_line(confirmation, sizeof(confirmation));
    if (result_is_error(result)) {
        clear_secret(password, sizeof(password));
        clear_secret(confirmation, sizeof(confirmation));
        return report_failure("read password confirmation", result);
    }
    if (strcmp(password, confirmation) != 0) {
        clear_secret(password, sizeof(password));
        clear_secret(confirmation, sizeof(confirmation));
        printf("Passwords do not match.\n");
        return 1;
    }
    result = account_create(username, password);
    clear_secret(password, sizeof(password));
    clear_secret(confirmation, sizeof(confirmation));
    if (result_is_error(result)) return report_failure("create account", result);
    printf("Created account \"%s\".\n", username);
    return 0;
}

static int change_password(int argc, char **argv)
{
    char password[USER_PASSWORD_CAPACITY];
    char confirmation[USER_PASSWORD_CAPACITY];
    const char *username;
    mg_result_t result;

    if (argc == 2) {
        mg_identity_t identity;
        result = process_get_identity(&identity);
        if (result_is_error(result))
            return report_failure("query identity", result);
        username = identity.username;
    } else if (argc == 3) {
        username = argv[2];
    } else {
        return usage_error(argv[0], argc > 2 && argv[2][0] == '-' ?
                           argv[2] : NULL);
    }
    memset(password, 0, sizeof(password));
    memset(confirmation, 0, sizeof(confirmation));
    printf("New password: ");
    result = read_hidden_line(password, sizeof(password));
    if (result_is_error(result)) {
        clear_secret(password, sizeof(password));
        clear_secret(confirmation, sizeof(confirmation));
        return report_failure("read password", result);
    }
    printf("Confirm password: ");
    result = read_hidden_line(confirmation, sizeof(confirmation));
    if (result_is_error(result)) {
        clear_secret(password, sizeof(password));
        clear_secret(confirmation, sizeof(confirmation));
        return report_failure("read password confirmation", result);
    }
    if (strcmp(password, confirmation) != 0) {
        clear_secret(password, sizeof(password));
        clear_secret(confirmation, sizeof(confirmation));
        printf("Passwords do not match.\n");
        return 1;
    }
    result = account_set_password(username, password);
    clear_secret(password, sizeof(password));
    clear_secret(confirmation, sizeof(confirmation));
    if (result_is_error(result)) return report_failure("change password", result);
    printf("Password updated for \"%s\".\n", username);
    return 0;
}

static int change_role(int argc, char **argv)
{
    mg_identity_role_t role;
    mg_result_t result;

    if (argc != 4) {
        return usage_error(argv[0], argc > 1 && argv[1][0] == '-' ?
                           argv[1] : NULL);
    }
    if (!strcmp(argv[3], "regular")) role = MG_IDENTITY_ROLE_REGULAR;
    else if (!strcmp(argv[3], "admin")) role = MG_IDENTITY_ROLE_ADMIN;
    else {
        return usage_error(argv[0], argv[3][0] == '-' ? argv[3] : NULL);
    }
    result = account_set_role(argv[2], role);
    if (result_is_error(result)) return report_failure("change account role", result);
    printf("Changed role for \"%s\" to %s.\n", argv[2], role_name(role));
    return 0;
}

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc == 1) return show_current_identity();
    if (argc == 2 && !strcmp(argv[1], "list")) return list_accounts();
    if (argc == 3 && !strcmp(argv[1], "show"))
        return show_account(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "create"))
        return create_account(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "password"))
        return change_password(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "remove"))
        return remove_account(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "role"))
        return change_role(argc, argv);
    return usage_error(argv[0], argc > 1 && argv[1][0] == '-' ?
                       argv[1] : NULL);
}
