#include <mangrove.h>
#include <mg/error.h>
#include <stdio.h>
#include "../common/help.h"

static const char *role_name(u32 role)
{
    if (role == MG_IDENTITY_ROLE_REGULAR) return "regular";
    if (role == MG_IDENTITY_ROLE_ADMIN) return "admin";
    return "unknown";
}

int main(int argc, char **argv)
{
    mg_identity_t identity;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "identity", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    result = process_get_identity(&identity);
    if (result_is_error(result)) {
        printf("Could not query identity: %s.\n", error_string(result));
        return 1;
    }
    printf("UID: %u\nRole: %s\nUsername: %s\nHome: %s\n",
           identity.uid, role_name(identity.role), identity.username,
           identity.home);
    return 0;
}
