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
        command_usage_error(argv[0], "eject <device>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }
    result = volume_client_call(MG_VOLUME_OP_EJECT, argv[1], &response);
    if (result == MG_OK) {
        printf("Ejected %s\n", argv[1]);
        return 0;
    }
    if (result == MG_ERR_PRIVILEGE_REQUIRED)
        printf("eject: administrator privileges required\n");
    else if (result == MG_ERR_ACCESS_DENIED)
        printf("eject: %s: system or non-removable device\n", argv[1]);
    else if (result == MG_ERR_BUSY)
        printf("eject: %s: a volume is busy\n", argv[1]);
    else if (result == MG_ERR_NOT_FOUND)
        printf("eject: %s: device not found\n", argv[1]);
    else if (result == MG_ERR_DEVICE_GONE)
        printf("eject: %s: device is no longer present\n", argv[1]);
    else
        printf("eject: %s: %s\n", argv[1], error_string(result));
    return 1;
}
