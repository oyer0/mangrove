/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <identity.h>
#include <process.h>
#include <types.h>

typedef struct {
    u32 id;
    const char *name;
    const char *path;
    u32 privileges;
    const char *endpoint_name;
} kernel_service_definition_t;

bool service_definition_lookup(u32 id,
                               const kernel_service_definition_t **definition);

/* Sprout is the only caller allowed to authorize its protected lifecycle
 * operations.  The kernel derives the privilege and trusted description. */
int service_authorize_control(process_t *sprout,
                              process_handle_t request_handle,
                              u32 operation, u32 service_id);
