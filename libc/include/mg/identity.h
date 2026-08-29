#pragma once

#include <mg/types.h>
#include <mg/error.h>

typedef u32 mg_uid_t;
typedef u32 mg_identity_role_t;

/* UID zero is reserved for kernel/system-owned resources. */
#define MG_UID_SYSTEM       ((mg_uid_t)0U)
#define MG_IDENTITY_USERNAME_CAPACITY 32U
#define MG_IDENTITY_HOME_CAPACITY     256U

enum {
    MG_IDENTITY_ROLE_REGULAR = 0U,
    MG_IDENTITY_ROLE_ADMIN = 1U,
};

/* Stable read-only process-identity ABI.  Account metadata remains owned by
 * the kernel; this is a snapshot returned by the identity query. */
typedef struct {
    mg_uid_t uid;
    mg_identity_role_t role;
    char username[MG_IDENTITY_USERNAME_CAPACITY];
    char home[MG_IDENTITY_HOME_CAPACITY];
} mg_identity_t;

mg_result_t process_get_identity(mg_identity_t *identity);

/* Authenticates the kernel-owned parent session.  Credentials are never
 * returned to userspace; successful child processes inherit them normally. */
mg_result_t session_login(const char *username, const char *password);
