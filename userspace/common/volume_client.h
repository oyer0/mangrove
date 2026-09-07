/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/volume_service.h>

mg_result_t volume_client_call(u16 operation, const char *target,
                               mg_volume_operation_response_t *response);
