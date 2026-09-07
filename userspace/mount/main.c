/* SPDX-License-Identifier: GPL-3.0-only */
#include <mangrove.h>
#include <mg/volume_service.h>
#include <stdio.h>
#include "../common/help.h"
#include "../common/volume_client.h"

int main(int argc, char **argv)
{
    mg_volume_operation_response_t response = {0};
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 2) {
        command_usage_error(argv[0], "mount <device>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }
    result = volume_client_call(MG_VOLUME_OP_MOUNT, argv[1], &response);
    if (result == MG_OK) {
        if (response.flags & MG_VOLUME_RESULT_ALREADY_MOUNTED)
            printf("%s is already mounted at %s\n", argv[1],
                   response.mount_point);
        else
            printf("Mounted %s at %s\n", argv[1], response.mount_point);
        return 0;
    }
    if (result == MG_ERR_PRIVILEGE_REQUIRED)
        printf("mount: administrator privileges required\n");
    else if (result == MG_ERR_UNSUPPORTED)
        printf("mount: %s: unsupported or unmountable filesystem\n", argv[1]);
    else if (result == MG_ERR_NOT_FOUND)
        printf("mount: %s: volume not found\n", argv[1]);
    else if (result == MG_ERR_DEVICE_GONE)
        printf("mount: %s: device is no longer present\n", argv[1]);
    else
        printf("mount: %s: %s\n", argv[1], error_string(result));
    return 1;
}
