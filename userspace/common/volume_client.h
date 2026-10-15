/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/volume_service.h>

mg_result_t volume_client_call(u16 operation, const char *target,
                               mg_volume_operation_response_t *response);

/* Bind a previously observed selection; never let a reused label/mountpoint
 * redirect the request to another registration. Existing text-only clients
 * intentionally select the current target with volume_client_call(). */
mg_result_t volume_client_call_expected(u16 operation, const char *target,
                                         u64 expected_instance,
                                         mg_volume_operation_response_t *response);
