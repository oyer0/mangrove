/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/device_service.h>

/* A command receives a fresh copied snapshot for each invocation.  The
 * capacity is supplied by the caller so inspection tools remain bounded. */
mg_result_t device_query_category(u32 category, mg_device_info_t *devices,
                                  u32 capacity, u32 *out_count);
