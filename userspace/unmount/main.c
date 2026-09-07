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
        command_usage_error(argv[0], "unmount <device>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }
    result = volume_client_call(MG_VOLUME_OP_UNMOUNT, argv[1], &response);
    if (result == MG_OK) {
        printf("Unmounted %s\n", argv[1]);
        return 0;
    }
    if (result == MG_ERR_PRIVILEGE_REQUIRED)
        printf("unmount: administrator privileges required\n");
    else if (result == MG_ERR_BUSY)
        printf("unmount: %s: volume is busy\n", argv[1]);
    else if (result == MG_ERR_NOT_FOUND)
        printf("unmount: %s: volume is not mounted or was not found\n",
               argv[1]);
    else if (result == MG_ERR_DEVICE_GONE)
        printf("unmount: %s: device is no longer present\n", argv[1]);
    else
        printf("unmount: %s: %s\n", argv[1], error_string(result));
    return 1;
}
