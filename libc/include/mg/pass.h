/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/identity.h>

/* PASS authentication is exposed only as the login frontend operation.  The
 * kernel keeps credential records and performs verification. */
mg_result_t pass_authenticate_account(const char *username,
                                      const char *password,
                                      mg_identity_t *identity);
