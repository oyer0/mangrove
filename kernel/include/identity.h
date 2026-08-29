#pragma once

#include <mg/identity.h>
#include <mg/account.h>
#include <password.h>

#define IDENTITY_USERNAME_CAPACITY MG_IDENTITY_USERNAME_CAPACITY
#define IDENTITY_HOME_CAPACITY     MG_IDENTITY_HOME_CAPACITY

#define IDENTITY_ACCOUNT_DB_PATH "/state/accounts/users"
#define IDENTITY_ACCOUNT_DB_LEGACY_PATH "/core/accounts/users"
#define IDENTITY_ACCOUNT_DB_OLD_PATH "/core/accounts/users.db"
#define IDENTITY_ACCOUNT_DIR_PATH "/state/accounts"
#define IDENTITY_LEGACY_ACCOUNT_DIR_PATH "/core/accounts"
#define IDENTITY_AUTOLOGIN_PATH "/core/session/autologin"
#define IDENTITY_ACCOUNT_DB_MAX_BYTES 16384U
#define IDENTITY_ACCOUNT_MAX_RECORDS  MG_ACCOUNT_MAX_RECORDS
#define IDENTITY_FIRST_USER_UID       1001U

#define IDENTITY_ACCOUNT_FLAG_INITIAL ((u32)1U << 0)
#define IDENTITY_ACCOUNT_FLAG_KNOWN IDENTITY_ACCOUNT_FLAG_INITIAL

typedef enum {
    IDENTITY_PRIVILEGE_MANAGE_USERS = 1U,
    IDENTITY_PRIVILEGE_MANAGE_NETWORK,
} identity_privilege_t;

/* Only stable execution credentials live in each process. */
typedef struct {
    mg_uid_t uid;
    mg_identity_role_t role;
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

int identity_authenticate(const char *username, const char *password,
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
