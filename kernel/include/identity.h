/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/identity.h>
#include <mg/account.h>
#include <password.h>

#define IDENTITY_USERNAME_CAPACITY MG_IDENTITY_USERNAME_CAPACITY
#define IDENTITY_HOME_CAPACITY     MG_IDENTITY_HOME_CAPACITY

#define IDENTITY_ACCOUNT_DB_PATH "/sys/accounts/users"
#define IDENTITY_ACCOUNT_DIR_PATH "/sys/accounts"
#define IDENTITY_SESSION_CONFIG_PATH "/conf/session/config"
#define IDENTITY_ACCOUNT_DB_MAX_BYTES 16384U
#define IDENTITY_ACCOUNT_MAX_RECORDS  MG_ACCOUNT_MAX_RECORDS
#define IDENTITY_FIRST_USER_UID       1001U

#define IDENTITY_ACCOUNT_FLAG_INITIAL ((u32)1U << 0)
#define IDENTITY_ACCOUNT_FLAG_KNOWN IDENTITY_ACCOUNT_FLAG_INITIAL

typedef enum {
    IDENTITY_PRIVILEGE_MANAGE_USERS = 1U,
    IDENTITY_PRIVILEGE_MANAGE_NETWORK,
    IDENTITY_PRIVILEGE_MANAGE_SESSIONS,
    IDENTITY_PRIVILEGE_MANAGE_CONFIGURATION,
    IDENTITY_PRIVILEGE_MANAGE_SERVICES,
    IDENTITY_PRIVILEGE_MANAGE_DEVICES,
    IDENTITY_PRIVILEGE_MANAGE_STORAGE,
} identity_privilege_t;

#define IDENTITY_PRIVILEGE_MASK(privilege) \
    ((u32)1U << ((u32)(privilege) - 1U))
#define IDENTITY_SERVICE_PRIVILEGES_KNOWN \
    (IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_SESSIONS) | \
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_SERVICES) | \
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_NETWORK) | \
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_CONFIGURATION) | \
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_DEVICES) | \
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_STORAGE))

/* Only stable execution credentials live in each process. */
typedef struct {
    mg_uid_t uid;
    mg_identity_role_t role;
    /* Explicit capabilities are assigned only to trusted system services.
     * Human roles never acquire these bits through userspace. */
    u32 service_privileges;
} process_credentials_t;

/* Kernel-owned identity metadata.  Process credentials deliberately contain
 * only the stable identity fields, not duplicated account strings. */
typedef struct {
    mg_uid_t uid;
    char username[IDENTITY_USERNAME_CAPACITY];
    mg_identity_role_t role;
    char home[IDENTITY_HOME_CAPACITY];
    u32 flags;
} user_identity_t;

process_credentials_t identity_system_credentials(void);
bool identity_init(void);
bool identity_user_valid(const user_identity_t *identity);
bool identity_credentials_valid(const process_credentials_t *credentials);
bool identity_credentials_is_system(const process_credentials_t *credentials);
bool identity_credentials_is_admin(const process_credentials_t *credentials);
bool identity_credentials_effective(const process_credentials_t *credentials,
                                    process_credentials_t *effective);
bool identity_credentials_has_privilege(
    const process_credentials_t *credentials, identity_privilege_t privilege);
bool identity_credentials_from_user(const user_identity_t *identity,
                                    process_credentials_t *credentials);
bool identity_registry_reload(void);
const char *identity_registry_error(void);
bool identity_registry_ready(void);
bool identity_registry_lookup_uid(mg_uid_t uid, user_identity_t *identity);
bool identity_registry_lookup_username(const char *username,
                                       user_identity_t *identity);
bool identity_registry_initial_user(user_identity_t *identity);
bool identity_registry_autologin_user(user_identity_t *identity);
bool identity_query_credentials(const process_credentials_t *credentials,
                                mg_identity_t *identity);
/* Verify a password for the current human identity without exposing account
 * authentication records to the caller. */
bool identity_password_verify_current(
    const process_credentials_t *credentials, const char *password);

int identity_password_authenticate(const char *username, const char *password,
                                   user_identity_t *identity);
int identity_account_set_password(const char *username,
                                  const char *password);

int identity_account_list(mg_account_info_t *accounts, usize capacity,
                          usize *out_count);
int identity_account_show(const char *username, mg_account_info_t *account);
int identity_account_create(const char *username, const char *password);
int identity_account_remove(const char *username, bool purge);
int identity_account_set_role(const char *username,
                              mg_identity_role_t role);
