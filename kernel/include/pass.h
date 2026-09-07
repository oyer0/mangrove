/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <identity.h>
#include <mangrove_errors.h>
#include <process.h>

/* PASS (Pluggable Authentication and Security Policy) keeps policy
 * selection, trusted interaction, and credential verification behind one
 * kernel-owned boundary.  Callers choose neither the provider nor the
 * required privilege. */
typedef enum {
    PASS_PROVIDER_PASSWORD = 0U,
    PASS_PROVIDER_CONFIRM,
    PASS_PROVIDER_SCRIPTS,
    PASS_PROVIDER_NONE,
} pass_provider_t;

typedef enum {
    PASS_RESULT_ALLOWED = MG_OK,
    PASS_RESULT_DENIED = MG_ERR_ACCESS_DENIED,
    PASS_RESULT_AUTHENTICATION_FAILED = MG_ERR_AUTH_FAILED,
    PASS_RESULT_CANCELLED = MG_ERR_CANCELLED,
    PASS_RESULT_POLICY_ERROR = MG_ERR_IO,
    PASS_RESULT_ACCOUNT_UNAVAILABLE = MG_ERR_NOT_FOUND,
    PASS_RESULT_PRIVILEGE_REQUIRED = MG_ERR_PRIVILEGE_REQUIRED,
} pass_result_t;

typedef struct {
    pass_provider_t provider;
    bool valid;
} pass_policy_t;

#define PASS_MESSAGE_MAX 192U
#define PASS_SECURITY_CONFIG_PATH "/conf/security/config"
#define PASS_SECURITY_CONFIG_MAX_BYTES 1024U

/* A malformed or missing policy is represented as confirm.  The boolean
 * form is available to diagnostics and future policy consumers without
 * weakening that safe runtime fallback. */
bool pass_policy_read(pass_policy_t *policy);
pass_provider_t pass_policy_current(void);
const char *pass_provider_name(pass_provider_t provider);

/* Protected operations supply their own privilege and trusted description.
 * The requester identity for a service call is recovered from the
 * kernel-owned IPC request context. */
pass_result_t pass_authorize_current(identity_privilege_t privilege,
                                     const char *description);
pass_result_t pass_authorize_request(process_t *service,
                                     process_handle_t request_handle,
                                     identity_privilege_t privilege,
                                     const char *description);

/* Login authentication is distinct from authorization of an already
 * identified human.  The result deliberately does not expose credential
 * records to sessiond or other callers. */
pass_result_t pass_authenticate_account(const char *username,
                                        const char *password,
                                        user_identity_t *identity);
