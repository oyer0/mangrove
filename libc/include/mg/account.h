#pragma once

#include <mg/identity.h>

#define MG_ACCOUNT_MAX_RECORDS 64U

typedef struct mg_account_info {
    mg_uid_t uid;
    mg_identity_role_t role;
    u32 flags;
    char username[MG_IDENTITY_USERNAME_CAPACITY];
    char home[MG_IDENTITY_HOME_CAPACITY];
} mg_account_info_t;

enum {
    MG_ACCOUNT_OP_LIST = 1U,
    MG_ACCOUNT_OP_SHOW,
    MG_ACCOUNT_OP_CREATE,
    MG_ACCOUNT_OP_REMOVE,
    MG_ACCOUNT_OP_SET_ROLE,
    MG_ACCOUNT_OP_SET_PASSWORD,
};

#define MG_ACCOUNT_REMOVE_PURGE (1U << 0)

/* This request is an internal syscall ABI.  Userspace fills only the fields
 * appropriate to the selected operation; the kernel validates every pointer
 * before using it. */
typedef struct mg_account_request {
    u32 operation;
    u32 flags;
    mg_identity_role_t role;
    u32 reserved;
    const char *username;
    const char *password;
    mg_account_info_t *result;
    usize result_capacity;
    usize *out_count;
} mg_account_request_t;

mg_result_t account_list(mg_account_info_t *accounts, usize capacity,
                         usize *out_count);
mg_result_t account_show(const char *username, mg_account_info_t *account);
mg_result_t account_create(const char *username, const char *password);
mg_result_t account_remove(const char *username, bool purge);
mg_result_t account_set_role(const char *username, mg_identity_role_t role);
mg_result_t account_set_password(const char *username, const char *password);
