/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>
#include <mangrove_errors.h>

const char *error_string(mg_result_t error);
bool result_is_error(mg_result_t result);
bool result_is_success(mg_result_t result);
/* Convert at a C/POSIX library boundary; native APIs retain mg_result_t. */
int mg_error_to_errno(mg_result_t result);
void mg_set_errno_from_result(mg_result_t result);
