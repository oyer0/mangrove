/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>
#include <mangrove_errors.h>

const char *error_string(mg_result_t error);
bool result_is_error(mg_result_t result);
bool result_is_success(mg_result_t result);
