#include <identity.h>

#include <heap.h>
#include <authorization.h>
#include <process.h>
#include <string.h>
#include <vfs.h>

typedef struct {
    user_identity_t users[IDENTITY_ACCOUNT_MAX_RECORDS];
    identity_authentication_t authentication[IDENTITY_ACCOUNT_MAX_RECORDS];
    u32 count;
    mg_uid_t next_uid;
} identity_registry_t;

static identity_registry_t registry_slots[2];
static volatile u32 active_registry_slot;
static volatile bool registry_loaded;
static volatile bool identity_update_busy;
static const char *registry_error_message = "account database unavailable";

static int account_persist_registry(const identity_registry_t *registry);
static int account_vfs_error(int result);
static int account_find(const identity_registry_t *registry,
                        const char *username);

static const user_identity_t system_identity = {
    MG_UID_SYSTEM,
    "system",
    MG_IDENTITY_ROLE_ADMIN,
    "/",
    0,
};

process_credentials_t identity_system_credentials(void)
{
    process_credentials_t credentials = {
        system_identity.uid,
        system_identity.role,
    };
    return credentials;
}

static u64 identity_irq_save(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void identity_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static void set_registry_error(const char *message)
{
    registry_error_message = message;
}

static bool identity_string_valid(const char *value, usize capacity,
                                  bool require_absolute)
{
    usize length = 0;

    if (!value || capacity == 0 ||
        (require_absolute && value[0] != '/')) return false;
    while (length < capacity && value[length] != '\0') length++;
    return length != 0 && length < capacity;
}

static bool username_valid(const char *username)
{
    if (!identity_string_valid(username, IDENTITY_USERNAME_CAPACITY, false))
        return false;
    if (username[0] < 'a' || username[0] > 'z') return false;
    for (usize index = 0; username[index] != '\0'; index++) {
        char value = username[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_' || value == '-'))
            return false;
    }
    return true;
}

static bool home_path_valid(const char *home)
{
    usize index;

    if (!identity_string_valid(home, IDENTITY_HOME_CAPACITY, true) ||
        strcmp(home, "/") == 0 || home[1] == '/') return false;
    for (index = 1; home[index] != '\0';) {
        usize start = index;
        while (home[index] != '\0' && home[index] != '/') {
            char value = home[index];
            if (!((value >= 'a' && value <= 'z') ||
                  (value >= 'A' && value <= 'Z') ||
                  (value >= '0' && value <= '9') || value == '_' ||
                  value == '-' || value == '.')) return false;
            index++;
        }
        if (index - start == 0 ||
            (index - start == 1 && home[start] == '.') ||
            (index - start == 2 && home[start] == '.' &&
             home[start + 1] == '.')) return false;
        if (home[index] == '/') {
            if (home[index + 1] == '\0' || home[index + 1] == '/') return false;
            index++;
        }
    }
    return true;
}

static bool account_home_matches(const user_identity_t *identity)
{
    char expected[IDENTITY_HOME_CAPACITY];
    usize username_length;

    if (!identity || !username_valid(identity->username)) return false;
    username_length = strlen(identity->username);
    if (username_length + 6U >= sizeof(expected)) return false;
    memcpy(expected, "/user/", 6);
    memcpy(expected + 6, identity->username, username_length + 1U);
    return strcmp(identity->home, expected) == 0;
}

bool identity_user_valid(const user_identity_t *identity)
{
    if (!identity || identity->uid == MG_UID_SYSTEM ||
        (identity->role != MG_IDENTITY_ROLE_REGULAR &&
         identity->role != MG_IDENTITY_ROLE_ADMIN) ||
        (identity->flags & ~IDENTITY_ACCOUNT_FLAG_KNOWN) ||
        !username_valid(identity->username) ||
        !home_path_valid(identity->home) ||
        !account_home_matches(identity)) {
        return false;
    }
    return true;
}

bool identity_credentials_valid(const process_credentials_t *credentials)
{
    return credentials &&
           (credentials->role == MG_IDENTITY_ROLE_REGULAR ||
            credentials->role == MG_IDENTITY_ROLE_ADMIN);
}

bool identity_credentials_is_system(const process_credentials_t *credentials)
{
    return credentials && credentials->uid == MG_UID_SYSTEM;
}

bool identity_credentials_is_admin(const process_credentials_t *credentials)
{
    return identity_credentials_valid(credentials) &&
           !identity_credentials_is_system(credentials) &&
           credentials->role == MG_IDENTITY_ROLE_ADMIN;
}

bool identity_credentials_effective(const process_credentials_t *credentials,
                                    process_credentials_t *effective)
{
    user_identity_t identity;

    if (!credentials || !effective ||
        !identity_credentials_valid(credentials)) return false;
    if (identity_credentials_is_system(credentials)) {
        *effective = identity_system_credentials();
        return true;
    }
    if (!identity_registry_lookup_uid(credentials->uid, &identity)) return false;
    effective->uid = identity.uid;
    effective->role = identity.role;
    return true;
}

bool identity_credentials_has_privilege(
    const process_credentials_t *credentials, identity_privilege_t privilege)
{
    user_identity_t identity;

    if (!credentials ||
        (privilege != IDENTITY_PRIVILEGE_MANAGE_USERS &&
         privilege != IDENTITY_PRIVILEGE_MANAGE_NETWORK) ||
        identity_credentials_is_system(credentials) ||
        !identity_registry_lookup_uid(credentials->uid, &identity)) {
        return false;
    }
    /* Authority is resolved from the live registry, so role demotion takes
     * effect for existing processes without mutating arbitrary process state. */
    return identity.role == MG_IDENTITY_ROLE_ADMIN;
}

bool identity_credentials_from_user(const user_identity_t *identity,
                                    process_credentials_t *credentials)
{
    if (!identity_user_valid(identity) || !credentials)
        return false;
    credentials->uid = identity->uid;
    credentials->role = identity->role;
    return identity_credentials_valid(credentials);
}

static bool span_equals(const char *text, usize length, const char *value)
{
    usize value_length;

    if (!text || !value) return false;
    value_length = strlen(value);
    return length == value_length && memcmp(text, value, length) == 0;
}

static bool copy_span(const char *text, usize length, char *output,
                     usize capacity)
{
    if (!text || !output || capacity == 0 || length == 0 ||
        length >= capacity) return false;
    memcpy(output, text, length);
    output[length] = '\0';
    return true;
}

static bool parse_uid(const char *text, usize length, mg_uid_t *uid)
{
    u32 value = 0;

    if (!text || !length || !uid) return false;
    for (usize index = 0; index < length; index++) {
        u32 digit;
        if (text[index] < '0' || text[index] > '9') return false;
        digit = (u32)(text[index] - '0');
        if (value > (~(u32)0 - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *uid = value;
    return true;
}

static bool parse_role(const char *text, usize length,
                       mg_identity_role_t *role)
{
    if (span_equals(text, length, "regular")) {
        *role = MG_IDENTITY_ROLE_REGULAR;
        return true;
    }
    if (span_equals(text, length, "admin")) {
        *role = MG_IDENTITY_ROLE_ADMIN;
        return true;
    }
    return false;
}

static bool parse_flags(const char *text, usize length, u32 *flags)
{
    if (!text || !length || !flags) return false;
    if (span_equals(text, length, "none")) {
        *flags = 0;
        return true;
    }
    if (span_equals(text, length, "initial")) {
        *flags = IDENTITY_ACCOUNT_FLAG_INITIAL;
        return true;
    }
    return false;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *text, usize length, u8 *output,
                      usize output_length)
{
    if (!text || !output || length != output_length * 2U) return false;
    for (usize index = 0; index < output_length; index++) {
        int high = hex_digit(text[index * 2U]);
        int low = hex_digit(text[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        output[index] = (u8)((high << 4) | low);
    }
    return true;
}

static bool trim_line(const char *text, usize *start, usize *end)
{
    usize left = *start;
    usize right = *end;
    bool quote = false;

    while (left < right && (text[left] == ' ' || text[left] == '\t' ||
                            text[left] == '\r')) left++;
    while (right > left && (text[right - 1U] == ' ' ||
                            text[right - 1U] == '\t' ||
                            text[right - 1U] == '\r')) right--;
    for (usize index = left; index + 1U < right; index++) {
        if (text[index] == '"') quote = !quote;
        if (!quote && text[index] == '/' && text[index + 1U] == '/') {
            right = index;
            while (right > left && (text[right - 1U] == ' ' ||
                                    text[right - 1U] == '\t')) right--;
            break;
        }
    }
    *start = left;
    *end = right;
    return left < right;
}

static bool parse_account_line(const char *line, usize length, u32 version,
                               user_identity_t *identity,
                               identity_authentication_t *authentication)
{
    usize position = 0;
    u32 seen = 0;
    char algorithm[32];

    if (!line || !identity || !authentication) return false;
    memset(identity, 0, sizeof(*identity));
    memset(authentication, 0, sizeof(*authentication));
    memset(algorithm, 0, sizeof(algorithm));
    while (position < length && (line[position] == ' ' ||
                                 line[position] == '\t')) position++;
    if (position + 7U > length ||
        memcmp(line + position, "account", 7) != 0 ||
        (position + 7U < length && line[position + 7U] != ' ' &&
         line[position + 7U] != '\t')) return false;
    position += 7U;

    while (position < length) {
        usize key_start;
        usize key_length;
        usize value_start;
        usize value_length;

        while (position < length && (line[position] == ' ' ||
                                     line[position] == '\t')) position++;
        if (position >= length) break;
        key_start = position;
        while (position < length && line[position] != '=' &&
               line[position] != ' ' && line[position] != '\t') position++;
        key_length = position - key_start;
        if (key_length == 0 || position >= length || line[position] != '=')
            return false;
        position++;
        value_start = position;
        while (position < length && line[position] != ' ' &&
               line[position] != '\t') position++;
        value_length = position - value_start;
        if (!value_length) return false;

        if (span_equals(line + key_start, key_length, "uid")) {
            if (seen & 1U || !parse_uid(line + value_start, value_length,
                                         &identity->uid)) return false;
            seen |= 1U;
        } else if (span_equals(line + key_start, key_length, "username")) {
            if (seen & 2U || !copy_span(line + value_start, value_length,
                                         identity->username,
                                         sizeof(identity->username))) return false;
            seen |= 2U;
        } else if (span_equals(line + key_start, key_length, "role")) {
            if (seen & 4U || !parse_role(line + value_start, value_length,
                                          &identity->role)) return false;
            seen |= 4U;
        } else if (span_equals(line + key_start, key_length, "home")) {
            if (seen & 8U || !copy_span(line + value_start, value_length,
                                         identity->home,
                                         sizeof(identity->home))) return false;
            seen |= 8U;
        } else if (span_equals(line + key_start, key_length, "flags")) {
            if (seen & 16U || !parse_flags(line + value_start, value_length,
                                           &identity->flags)) return false;
            seen |= 16U;
        } else if (version == 2U &&
                   span_equals(line + key_start, key_length, "auth")) {
            if (seen & 32U || !copy_span(line + value_start, value_length,
                                          algorithm, sizeof(algorithm)))
                return false;
            seen |= 32U;
        } else if (version == 2U &&
                   span_equals(line + key_start, key_length, "salt")) {
            if (seen & 64U || !parse_hex(
                    line + value_start, value_length, authentication->salt,
                    sizeof(authentication->salt))) return false;
            seen |= 64U;
        } else if (version == 2U &&
                   span_equals(line + key_start, key_length, "iterations")) {
            if (seen & 128U || !parse_uid(line + value_start, value_length,
                                           &authentication->iterations))
                return false;
            seen |= 128U;
        } else if (version == 2U &&
                   span_equals(line + key_start, key_length, "hash")) {
            if (seen & 256U || !parse_hex(
                    line + value_start, value_length, authentication->hash,
                    sizeof(authentication->hash))) return false;
            seen |= 256U;
        } else {
            return false;
        }
    }
    if ((seen & 31U) != 31U || !identity_user_valid(identity)) return false;
    if (version == 1U) {
        authentication->algorithm = IDENTITY_AUTH_NONE;
        return password_auth_valid(authentication);
    }
    if (!(seen & 32U)) return false;
    if (span_equals(algorithm, strlen(algorithm), "none")) {
        authentication->algorithm = IDENTITY_AUTH_NONE;
        return password_auth_valid(authentication) &&
               !(seen & (64U | 128U | 256U));
    }
    if (!span_equals(algorithm, strlen(algorithm), "pbkdf2-sha256") ||
        (seen & (64U | 128U | 256U)) != (64U | 128U | 256U)) return false;
    authentication->algorithm = IDENTITY_AUTH_PBKDF2_SHA256;
    return password_auth_valid(authentication);
}

static bool parse_database(const char *text, usize length,
                           identity_registry_t *registry,
                           bool *needs_migration)
{
    usize position = 0;
    bool version_seen = false;
    bool next_uid_seen = false;
    u32 initial_count = 0;
    u32 version = 0;

    if (!text || !registry || !needs_migration || length == 0 ||
        length > IDENTITY_ACCOUNT_DB_MAX_BYTES) return false;
    memset(registry, 0, sizeof(*registry));
    *needs_migration = false;
    while (position < length) {
        usize line_start = position;
        usize line_end;
        while (position < length && text[position] != '\n') position++;
        line_end = position;
        if (position < length) position++;
        if (!trim_line(text, &line_start, &line_end)) continue;
        if (!version_seen) {
            if (span_equals(text + line_start, line_end - line_start,
                            "version=1")) {
                version = 1U;
                *needs_migration = true;
            } else if (span_equals(text + line_start, line_end - line_start,
                                   "version=2")) {
                version = 2U;
            } else {
                return false;
            }
            version_seen = true;
            continue;
        }
        if (!next_uid_seen) {
            usize separator = line_start;
            while (separator < line_end && text[separator] != '=') separator++;
            if (!span_equals(text + line_start, separator - line_start,
                             "next_uid") || separator >= line_end ||
                !parse_uid(text + separator + 1U, line_end - separator - 1U,
                           &registry->next_uid)) return false;
            next_uid_seen = true;
            continue;
        }
        if (registry->count >= IDENTITY_ACCOUNT_MAX_RECORDS ||
            !parse_account_line(text + line_start, line_end - line_start,
                                version, &registry->users[registry->count],
                                &registry->authentication[registry->count]))
            return false;
        if (registry->users[registry->count].flags &
            IDENTITY_ACCOUNT_FLAG_INITIAL) initial_count++;
        for (u32 index = 0; index < registry->count; index++) {
            if (registry->users[index].uid == registry->users[registry->count].uid ||
                strcmp(registry->users[index].username,
                       registry->users[registry->count].username) == 0) return false;
        }
        registry->count++;
    }
    if (!version_seen || !next_uid_seen || registry->count == 0 ||
        initial_count != 1U || registry->next_uid < IDENTITY_FIRST_USER_UID) {
        return false;
    }
    for (u32 index = 0; index < registry->count; index++) {
        if (registry->users[index].uid >= registry->next_uid) return false;
    }
    return true;
}

static bool identity_registry_copy_active(identity_registry_t *registry)
{
    u32 slot;

    if (!registry || !identity_registry_ready()) return false;
    slot = __atomic_load_n(&active_registry_slot, __ATOMIC_ACQUIRE);
    *registry = registry_slots[slot];
    return true;
}

static bool identity_registry_publish(const identity_registry_t *registry)
{
    u32 active;
    u32 target;

    if (!registry) return false;
    active = __atomic_load_n(&active_registry_slot, __ATOMIC_ACQUIRE);
    target = registry_loaded ? (active ^ 1U) : 0U;
    registry_slots[target] = *registry;
    __atomic_store_n(&active_registry_slot, target, __ATOMIC_RELEASE);
    __atomic_store_n(&registry_loaded, true, __ATOMIC_RELEASE);
    return true;
}

static bool identity_update_begin(void)
{
    return !__atomic_test_and_set(&identity_update_busy, __ATOMIC_ACQUIRE);
}

static void identity_update_end(void)
{
    __atomic_clear(&identity_update_busy, __ATOMIC_RELEASE);
}

static int identity_read_file(const char *path, usize maximum,
                              char **out_contents, usize *out_length)
{
    vfs_file_handle_t *handle = NULL;
    vfs_node_t *file = NULL;
    char *contents;
    usize length;
    int result;

    if (!path || !out_contents || !out_length) return MG_ERR_BAD_ARGUMENT;
    result = vfs_lookup_trusted(path, &file);
    if (result != VFS_OK || !file || file->type != VFS_TYPE_FILE)
        return result == VFS_ERR_NOT_FOUND ? MG_ERR_NOT_FOUND : MG_ERR_IO;
    if (file->size == 0 || file->size > maximum)
        return MG_ERR_BAD_ARGUMENT;
    length = (usize)file->size;
    contents = (char *)kmalloc(length + 1U);
    if (!contents) return MG_ERR_NO_MEMORY;
    result = vfs_open_trusted(path, VFS_OPEN_READ, &handle);
    if (result == VFS_OK &&
        vfs_file_read_trusted(handle, length, contents) != length)
        result = VFS_ERR_IO;
    if (handle) vfs_close_trusted(handle);
    if (result != VFS_OK) {
        kfree(contents);
        return MG_ERR_IO;
    }
    contents[length] = '\0';
    *out_contents = contents;
    *out_length = length;
    return MG_OK;
}

static int identity_read_database(char **out_contents, usize *out_length,
                                  const char **source_path)
{
    int result;

    if (!source_path) return MG_ERR_BAD_ARGUMENT;
    result = identity_read_file(IDENTITY_ACCOUNT_DB_PATH,
                                IDENTITY_ACCOUNT_DB_MAX_BYTES,
                                out_contents, out_length);
    if (result == MG_OK) {
        *source_path = IDENTITY_ACCOUNT_DB_PATH;
        return MG_OK;
    }
    if (result != MG_ERR_NOT_FOUND) return result;

    result = identity_read_file(IDENTITY_ACCOUNT_DB_LEGACY_PATH,
                                IDENTITY_ACCOUNT_DB_MAX_BYTES,
                                out_contents, out_length);
    if (result == MG_OK) {
        *source_path = IDENTITY_ACCOUNT_DB_LEGACY_PATH;
        return MG_OK;
    }
    if (result != MG_ERR_NOT_FOUND) return result;
    result = identity_read_file(IDENTITY_ACCOUNT_DB_OLD_PATH,
                                IDENTITY_ACCOUNT_DB_MAX_BYTES,
                                out_contents, out_length);
    if (result == MG_OK) *source_path = IDENTITY_ACCOUNT_DB_OLD_PATH;
    return result;
}

static int identity_remove_legacy_database(const char *source_path)
{
    vfs_node_t *accounts = NULL;
    int result;

    if (!source_path || !strcmp(source_path, IDENTITY_ACCOUNT_DB_PATH))
        return MG_OK;
    result = vfs_lookup_trusted(IDENTITY_LEGACY_ACCOUNT_DIR_PATH, &accounts);
    if (result != VFS_OK || !accounts ||
        accounts->type != VFS_TYPE_DIRECTORY)
        return account_vfs_error(result == VFS_OK ? VFS_ERR_NOT_DIRECTORY :
                                  result);
    /* A successful migration makes /state/accounts/users authoritative.
     * Remove both historical names so a stale second file cannot become an
     * alternate database if the new path is later damaged. */
    for (u32 index = 0; index < 2U; index++) {
        const char *name = index == 0U ? "users" : "users.db";
        if (!vfs_finddir_trusted(accounts, name)) continue;
        result = vfs_unlink_trusted(accounts, name);
        if (result != VFS_OK) return account_vfs_error(result);
    }
    return MG_OK;
}

bool identity_registry_reload(void)
{
    char *contents = NULL;
    usize length = 0;
    u32 target;
    bool valid;
    bool needs_migration = false;
    const char *source_path = IDENTITY_ACCOUNT_DB_PATH;
    int result;

    if (!identity_update_begin()) {
        set_registry_error("account registry update busy");
        return false;
    }
    result = identity_read_database(&contents, &length, &source_path);
    if (result != MG_OK) {
        set_registry_error(result == MG_ERR_NOT_FOUND
                               ? "account database not found"
                               : "account database read failed");
        identity_update_end();
        return false;
    }
    target = registry_loaded ? (active_registry_slot ^ 1U) : 0U;
    valid = parse_database(contents, length, &registry_slots[target],
                          &needs_migration);
    kfree(contents);
    if (!valid) {
        set_registry_error("account database validation failed");
        identity_update_end();
        return false;
    }

    if (needs_migration || strcmp(source_path, IDENTITY_ACCOUNT_DB_PATH) != 0) {
        result = account_persist_registry(&registry_slots[target]);
        if (result != MG_OK) {
            set_registry_error("account database migration failed");
            identity_update_end();
            return false;
        }
        if (strcmp(source_path, IDENTITY_ACCOUNT_DB_PATH) != 0 &&
            identity_remove_legacy_database(source_path) != MG_OK) {
            set_registry_error("account database legacy cleanup failed");
            identity_update_end();
            return false;
        }
    }

    __atomic_store_n(&active_registry_slot, target, __ATOMIC_RELEASE);
    __atomic_store_n(&registry_loaded, true, __ATOMIC_RELEASE);
    set_registry_error("account database ready");
    identity_update_end();
    return true;
}

const char *identity_registry_error(void)
{
    return registry_error_message;
}

bool identity_registry_ready(void)
{
    return __atomic_load_n(&registry_loaded, __ATOMIC_ACQUIRE);
}

bool identity_registry_lookup_uid(mg_uid_t uid, user_identity_t *identity)
{
    u64 saved_flags;
    identity_registry_t *registry;

    if (!identity) return false;
    saved_flags = identity_irq_save();
    if (!__atomic_load_n(&registry_loaded, __ATOMIC_ACQUIRE)) {
        identity_irq_restore(saved_flags);
        return false;
    }
    if (uid == MG_UID_SYSTEM) {
        *identity = system_identity;
        identity_irq_restore(saved_flags);
        return true;
    }
    registry = &registry_slots[__atomic_load_n(&active_registry_slot,
                                                __ATOMIC_ACQUIRE)];
    for (u32 index = 0; index < registry->count; index++) {
        if (registry->users[index].uid == uid) {
            *identity = registry->users[index];
            identity_irq_restore(saved_flags);
            return true;
        }
    }
    identity_irq_restore(saved_flags);
    return false;
}

bool identity_registry_lookup_username(const char *username,
                                       user_identity_t *identity)
{
    u64 saved_flags;
    identity_registry_t *registry;

    if (!username || !identity) return false;
    saved_flags = identity_irq_save();
    if (!__atomic_load_n(&registry_loaded, __ATOMIC_ACQUIRE)) {
        identity_irq_restore(saved_flags);
        return false;
    }
    registry = &registry_slots[__atomic_load_n(&active_registry_slot,
                                                __ATOMIC_ACQUIRE)];
    for (u32 index = 0; index < registry->count; index++) {
        if (strcmp(registry->users[index].username, username) == 0) {
            *identity = registry->users[index];
            identity_irq_restore(saved_flags);
            return true;
        }
    }
    identity_irq_restore(saved_flags);
    return false;
}

bool identity_registry_initial_user(user_identity_t *identity)
{
    u64 saved_flags;
    identity_registry_t *registry;
    bool found = false;

    if (!identity) return false;
    saved_flags = identity_irq_save();
    if (!__atomic_load_n(&registry_loaded, __ATOMIC_ACQUIRE)) {
        identity_irq_restore(saved_flags);
        return false;
    }
    registry = &registry_slots[__atomic_load_n(&active_registry_slot,
                                                __ATOMIC_ACQUIRE)];
    for (u32 index = 0; index < registry->count; index++) {
        if (registry->users[index].flags & IDENTITY_ACCOUNT_FLAG_INITIAL) {
            if (found) {
                identity_irq_restore(saved_flags);
                return false;
            }
            *identity = registry->users[index];
            found = true;
        }
    }
    identity_irq_restore(saved_flags);
    return found;
}

bool identity_registry_autologin_user(user_identity_t *identity)
{
    char *contents = NULL;
    usize length = 0;
    user_identity_t candidate;
    int result;

    if (!identity) return false;
    result = identity_read_file(IDENTITY_AUTOLOGIN_PATH, 64U,
                                &contents, &length);
    if (result != MG_OK) return false;
    while (length != 0 && (contents[length - 1U] == '\n' ||
                           contents[length - 1U] == '\r' ||
                           contents[length - 1U] == ' ' ||
                           contents[length - 1U] == '\t')) length--;
    if (!length || !copy_span(contents, length, candidate.username,
                              sizeof(candidate.username)) ||
        !username_valid(candidate.username) ||
        !identity_registry_lookup_username(candidate.username, &candidate) ||
        !(candidate.flags & IDENTITY_ACCOUNT_FLAG_INITIAL)) {
        kfree(contents);
        return false;
    }
    *identity = candidate;
    kfree(contents);
    return true;
}

static bool identity_lookup_authentication(
    const char *username, user_identity_t *identity,
    identity_authentication_t *authentication)
{
    u64 saved_flags;
    identity_registry_t *registry;

    if (!username || !identity || !authentication) return false;
    saved_flags = identity_irq_save();
    if (!__atomic_load_n(&registry_loaded, __ATOMIC_ACQUIRE)) {
        identity_irq_restore(saved_flags);
        return false;
    }
    registry = &registry_slots[__atomic_load_n(&active_registry_slot,
                                                __ATOMIC_ACQUIRE)];
    for (u32 index = 0; index < registry->count; index++) {
        if (strcmp(registry->users[index].username, username) == 0) {
            *identity = registry->users[index];
            *authentication = registry->authentication[index];
            identity_irq_restore(saved_flags);
            return true;
        }
    }
    identity_irq_restore(saved_flags);
    return false;
}

bool identity_query_credentials(const process_credentials_t *credentials,
                                mg_identity_t *identity)
{
    user_identity_t account;

    if (!credentials || !identity ||
        !identity_credentials_valid(credentials) ||
        !identity_registry_lookup_uid(credentials->uid, &account)) return false;
    identity->uid = account.uid;
    identity->role = account.role;
    memcpy(identity->username, account.username, sizeof(identity->username));
    memcpy(identity->home, account.home, sizeof(identity->home));
    return true;
}

static bool identity_registry_valid(const identity_registry_t *registry)
{
    u32 initial_count = 0;

    if (!registry || registry->count == 0 ||
        registry->count > IDENTITY_ACCOUNT_MAX_RECORDS ||
        registry->next_uid < IDENTITY_FIRST_USER_UID) return false;
    for (u32 index = 0; index < registry->count; index++) {
        const user_identity_t *user = &registry->users[index];
        if (!identity_user_valid(user) || user->uid >= registry->next_uid ||
            !password_auth_valid(&registry->authentication[index]))
            return false;
        if (user->flags & IDENTITY_ACCOUNT_FLAG_INITIAL) initial_count++;
        for (u32 other = 0; other < index; other++) {
            if (registry->users[other].uid == user->uid ||
                strcmp(registry->users[other].username, user->username) == 0)
                return false;
        }
    }
    return initial_count == 1U;
}

static int account_vfs_error(int result)
{
    switch (result) {
        case VFS_OK: return MG_OK;
        case VFS_ERR_NOT_FOUND: return MG_ERR_NOT_FOUND;
        case VFS_ERR_NO_MEM: return MG_ERR_NO_MEMORY;
        case VFS_ERR_UNSUPPORTED: return MG_ERR_UNSUPPORTED;
        case VFS_ERR_NOT_EMPTY: return MG_ERR_NOT_EMPTY;
        case VFS_ERR_ACCESS_DENIED: return MG_ERR_ACCESS_DENIED;
        case VFS_ERR_NOT_DIRECTORY: return MG_ERR_NOT_DIRECTORY;
        case VFS_ERR_IO: return MG_ERR_IO;
        default: return MG_ERR_BAD_ARGUMENT;
    }
}

static bool append_bytes(char *buffer, usize capacity, usize *position,
                         const char *text, usize length)
{
    if (!buffer || !position || !text || *position > capacity ||
        length > capacity - *position) return false;
    memcpy(buffer + *position, text, length);
    *position += length;
    return true;
}

static bool append_text(char *buffer, usize capacity, usize *position,
                        const char *text)
{
    return text && append_bytes(buffer, capacity, position, text, strlen(text));
}

static bool append_u32(char *buffer, usize capacity, usize *position,
                       u32 value)
{
    char digits[10];
    usize count = 0;

    if (value == 0) digits[count++] = '0';
    while (value != 0) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (count != 0) {
        count--;
        if (!append_bytes(buffer, capacity, position, &digits[count], 1))
            return false;
    }
    return true;
}

static bool append_hex(char *buffer, usize capacity, usize *position,
                       const u8 *bytes, usize length)
{
    static const char digits[] = "0123456789abcdef";
    for (usize index = 0; index < length; index++) {
        char pair[2] = {digits[bytes[index] >> 4],
                        digits[bytes[index] & 0x0fU]};
        if (!append_bytes(buffer, capacity, position, pair, sizeof(pair)))
            return false;
    }
    return true;
}

static bool serialize_database(const identity_registry_t *registry,
                               char *buffer, usize capacity, usize *length)
{
    usize position = 0;

    if (!identity_registry_valid(registry) || !buffer || !length) return false;
    if (!append_text(buffer, capacity, &position, "version=2\n") ||
        !append_text(buffer, capacity, &position, "next_uid=") ||
        !append_u32(buffer, capacity, &position, registry->next_uid) ||
        !append_text(buffer, capacity, &position, "\n\n")) return false;
    for (u32 index = 0; index < registry->count; index++) {
        const user_identity_t *user = &registry->users[index];
        const identity_authentication_t *authentication =
            &registry->authentication[index];
        if (!append_text(buffer, capacity, &position, "account uid=") ||
            !append_u32(buffer, capacity, &position, user->uid) ||
            !append_text(buffer, capacity, &position, " username=") ||
            !append_text(buffer, capacity, &position, user->username) ||
            !append_text(buffer, capacity, &position, " role=") ||
            !append_text(buffer, capacity, &position,
                         user->role == MG_IDENTITY_ROLE_ADMIN ? "admin" :
                                                               "regular") ||
            !append_text(buffer, capacity, &position, " home=") ||
            !append_text(buffer, capacity, &position, user->home) ||
            !append_text(buffer, capacity, &position, " flags=") ||
            !append_text(buffer, capacity, &position,
                         user->flags & IDENTITY_ACCOUNT_FLAG_INITIAL
                             ? "initial" : "none")) return false;
        if (authentication->algorithm == IDENTITY_AUTH_NONE) {
            if (!append_text(buffer, capacity, &position, " auth=none\n"))
                return false;
        } else if (authentication->algorithm == IDENTITY_AUTH_PBKDF2_SHA256) {
            if (!append_text(buffer, capacity, &position,
                             " auth=pbkdf2-sha256 salt=") ||
                !append_hex(buffer, capacity, &position,
                            authentication->salt,
                            sizeof(authentication->salt)) ||
                !append_text(buffer, capacity, &position, " iterations=") ||
                !append_u32(buffer, capacity, &position,
                            authentication->iterations) ||
                !append_text(buffer, capacity, &position, " hash=") ||
                !append_hex(buffer, capacity, &position,
                            authentication->hash,
                            sizeof(authentication->hash)) ||
                !append_text(buffer, capacity, &position, "\n")) return false;
        } else {
            return false;
        }
    }
    *length = position;
    return true;
}

static int account_storage_directory(vfs_node_t **out_directory)
{
    vfs_node_t *root;
    vfs_node_t *state;
    vfs_node_t *accounts;
    int result;

    if (!out_directory) return VFS_ERR_INVALID_PARAM;
    result = vfs_lookup_trusted(IDENTITY_ACCOUNT_DIR_PATH, &accounts);
    if (result == VFS_OK) {
        if (accounts->type != VFS_TYPE_DIRECTORY)
            return VFS_ERR_NOT_DIRECTORY;
        *out_directory = accounts;
        return VFS_OK;
    }
    if (result != VFS_ERR_NOT_FOUND) return result;
    root = vfs_get_root_node();
    if (!root || root->type != VFS_TYPE_DIRECTORY) return VFS_ERR_NOT_FOUND;
    state = vfs_finddir_trusted(root, "state");
    if (!state) {
        result = vfs_mkdir_owned(root, "state", VFS_UID_SYSTEM,
                                 VFS_DEFAULT_SYSTEM_PERMISSIONS, &state);
        if (result != VFS_OK || !state) return result;
    }
    if (state->type != VFS_TYPE_DIRECTORY) return VFS_ERR_NOT_DIRECTORY;
    accounts = vfs_finddir_trusted(state, "accounts");
    if (!accounts) {
        result = vfs_mkdir_owned(state, "accounts", VFS_UID_SYSTEM,
                                 VFS_DEFAULT_SYSTEM_PERMISSIONS, &accounts);
        if (result != VFS_OK || !accounts) return result;
    }
    if (accounts->type != VFS_TYPE_DIRECTORY) return VFS_ERR_NOT_DIRECTORY;
    *out_directory = accounts;
    return VFS_OK;
}

static int account_persist_registry(const identity_registry_t *registry)
{
    static const char temporary_name[] = ".users.new";
    static const char backup_name[] = ".users.old";
    vfs_node_t *accounts = NULL;
    vfs_node_t *node;
    vfs_node_t *temporary = NULL;
    vfs_file_handle_t *handle = NULL;
    char *contents;
    usize length;
    int result;
    bool moved_old = false;

    contents = (char *)kmalloc(IDENTITY_ACCOUNT_DB_MAX_BYTES);
    if (!contents) return MG_ERR_NO_MEMORY;
    if (!serialize_database(registry, contents, IDENTITY_ACCOUNT_DB_MAX_BYTES,
                            &length)) {
        kfree(contents);
        return MG_ERR_BAD_ARGUMENT;
    }
    result = account_storage_directory(&accounts);
    if (result != VFS_OK || !accounts) {
        kfree(contents);
        return account_vfs_error(result);
    }

    node = vfs_finddir_trusted(accounts, temporary_name);
    if (node) {
        if (node->type != VFS_TYPE_FILE ||
            vfs_unlink_trusted(accounts, temporary_name) != VFS_OK) {
            kfree(contents);
            return MG_ERR_IO;
        }
    }
    node = vfs_finddir_trusted(accounts, backup_name);
    if (node) {
        if (node->type != VFS_TYPE_FILE ||
            vfs_unlink_trusted(accounts, backup_name) != VFS_OK) {
            kfree(contents);
            return MG_ERR_IO;
        }
    }
    result = vfs_create_owned(accounts, temporary_name, VFS_UID_SYSTEM,
                              VFS_DEFAULT_SYSTEM_PERMISSIONS, &temporary);
    if (result != VFS_OK || !temporary) {
        kfree(contents);
        return account_vfs_error(result);
    }
    result = vfs_open_trusted(IDENTITY_ACCOUNT_DIR_PATH "/.users.new",
                              VFS_OPEN_WRITE,
                              &handle);
    if (result == VFS_OK &&
        vfs_file_write_trusted(handle, length, contents) != length) {
        result = VFS_ERR_IO;
    }
    if (handle) vfs_close_trusted(handle);
    kfree(contents);
    if (result != VFS_OK) {
        (void)vfs_unlink_trusted(accounts, temporary_name);
        return account_vfs_error(result);
    }

    node = vfs_finddir_trusted(accounts, "users");
    if (node) {
        if (node->type != VFS_TYPE_FILE) {
            (void)vfs_unlink_trusted(accounts, temporary_name);
            return MG_ERR_BAD_ARGUMENT;
        }
        result = vfs_rename_trusted(accounts, "users", accounts,
                                    backup_name);
        if (result != VFS_OK) {
            (void)vfs_unlink_trusted(accounts, temporary_name);
            return account_vfs_error(result);
        }
        moved_old = true;
    }
    result = vfs_rename_trusted(accounts, temporary_name, accounts, "users");
    if (result != VFS_OK) {
        if (moved_old)
            (void)vfs_rename_trusted(accounts, backup_name, accounts, "users");
        (void)vfs_unlink_trusted(accounts, temporary_name);
        return account_vfs_error(result);
    }
    if (moved_old) (void)vfs_unlink_trusted(accounts, backup_name);
    return MG_OK;
}

static int account_require_manage_users(void)
{
    process_credentials_t credentials;

    if (!process_get_credentials(process_current(), &credentials) ||
        !identity_credentials_has_privilege(
            &credentials, IDENTITY_PRIVILEGE_MANAGE_USERS))
        return MG_ERR_PRIVILEGE_REQUIRED;
    return MG_OK;
}

static bool password_input_valid(const char *password)
{
    usize length;

    if (!password) return false;
    length = strlen(password);
    return length != 0 && length <= IDENTITY_PASSWORD_MAX_LENGTH;
}

static bool account_salt_unique(const identity_registry_t *registry,
                                u32 ignored_index,
                                const identity_authentication_t *candidate)
{
    if (!registry || !candidate) return false;
    for (u32 index = 0; index < registry->count; index++) {
        if (index != ignored_index &&
            registry->authentication[index].algorithm ==
                IDENTITY_AUTH_PBKDF2_SHA256 &&
            memcmp(registry->authentication[index].salt, candidate->salt,
                   sizeof(candidate->salt)) == 0) return false;
    }
    return true;
}

static int account_make_password(const identity_registry_t *registry,
                                 u32 target_index, const char *password,
                                 identity_authentication_t *authentication)
{
    if (!registry || !authentication || !password_input_valid(password))
        return MG_ERR_BAD_ARGUMENT;
    if (!password_auth_available()) return MG_ERR_ENTROPY_UNAVAILABLE;
    for (u32 attempt = 0; attempt < 8U; attempt++) {
        if (!password_auth_generate(authentication, password))
            return MG_ERR_ENTROPY_UNAVAILABLE;
        if (account_salt_unique(registry, target_index, authentication))
            return MG_OK;
    }
    password_secure_clear(authentication, sizeof(*authentication));
    return MG_ERR_BUSY;
}

int identity_authenticate(const char *username, const char *password,
                          user_identity_t *identity)
{
    static const identity_authentication_t dummy_authentication = {
        IDENTITY_AUTH_PBKDF2_SHA256,
        IDENTITY_PASSWORD_ITERATIONS,
        {0x4d, 0x61, 0x6e, 0x67, 0x72, 0x6f, 0x76, 0x65,
         0x2d, 0x61, 0x75, 0x74, 0x68, 0x2d, 0x64, 0x75},
        {0},
    };
    user_identity_t candidate;
    identity_authentication_t authentication;
    bool found;
    bool has_password;
    bool valid;

    if (!username || !password || !identity || !username_valid(username) ||
        !password_input_valid(password)) return MG_ERR_AUTH_FAILED;
    found = identity_lookup_authentication(username, &candidate,
                                           &authentication);
    has_password = found &&
        authentication.algorithm == IDENTITY_AUTH_PBKDF2_SHA256 &&
        password_auth_valid(&authentication);
    if (!has_password) {
        authentication = dummy_authentication;
        valid = password_auth_verify(&authentication, password);
    } else {
        valid = password_auth_verify(&authentication, password);
    }
    if (!found || !has_password || !valid) {
        password_secure_clear(&authentication, sizeof(authentication));
        return MG_ERR_AUTH_FAILED;
    }
    *identity = candidate;
    password_secure_clear(&authentication, sizeof(authentication));
    return MG_OK;
}

int identity_account_set_password(const char *username, const char *password)
{
    identity_registry_t registry;
    process_credentials_t credentials;
    int index;
    int result;

    if (!username || !username_valid(username) ||
        !password_input_valid(password)) return MG_ERR_BAD_ARGUMENT;
    if (!process_get_credentials(process_current(), &credentials))
        return MG_ERR_ACCESS_DENIED;
    if (!identity_update_begin()) return MG_ERR_BUSY;
    if (!identity_registry_copy_active(&registry)) {
        result = MG_ERR_IO;
        goto set_password_done;
    }
    index = account_find(&registry, username);
    if (index < 0) {
        result = MG_ERR_NOT_FOUND;
        goto set_password_done;
    }
    if (credentials.uid != registry.users[index].uid &&
        !identity_credentials_has_privilege(
            &credentials, IDENTITY_PRIVILEGE_MANAGE_USERS)) {
        result = MG_ERR_PRIVILEGE_REQUIRED;
        goto set_password_done;
    }
    if (credentials.uid != registry.users[index].uid) {
        char description[AUTHORIZATION_MESSAGE_MAX];
        usize description_length = 0;

        if (!append_text(description, sizeof(description),
                         &description_length, "Reset the password for account \"") ||
            !append_text(description, sizeof(description), &description_length,
                         username) ||
            !append_text(description, sizeof(description), &description_length,
                         "\".")) {
            result = MG_ERR_BAD_ARGUMENT;
            goto set_password_done;
        }
        description[description_length] = '\0';
        result = authorization_confirm_current(
            IDENTITY_PRIVILEGE_MANAGE_USERS, description);
        if (result != MG_OK) goto set_password_done;
    }
    result = account_make_password(&registry, (u32)index, password,
                                   &registry.authentication[index]);
    if (result != MG_OK) goto set_password_done;
    result = account_persist_registry(&registry);
    if (result == MG_OK) (void)identity_registry_publish(&registry);
set_password_done:
    identity_update_end();
    return result;
}

static void account_info_from_identity(const user_identity_t *identity,
                                       mg_account_info_t *account)
{
    memset(account, 0, sizeof(*account));
    account->uid = identity->uid;
    account->role = identity->role;
    account->flags = identity->flags;
    memcpy(account->username, identity->username,
           sizeof(account->username));
    memcpy(account->home, identity->home, sizeof(account->home));
}

static int account_find(const identity_registry_t *registry, const char *username)
{
    for (u32 index = 0; index < registry->count; index++) {
        if (strcmp(registry->users[index].username, username) == 0)
            return (int)index;
    }
    return -1;
}

static u32 account_admin_count(const identity_registry_t *registry)
{
    u32 count = 0;
    for (u32 index = 0; index < registry->count; index++)
        if (registry->users[index].role == MG_IDENTITY_ROLE_ADMIN) count++;
    return count;
}

static bool account_home_is_safe(const user_identity_t *identity)
{
    return identity && account_home_matches(identity) &&
           strcmp(identity->home, "/user") != 0 &&
           strcmp(identity->home, "/") != 0;
}

static int account_purge_directory(vfs_node_t *directory, u32 depth)
{
    vfs_dirent_t entries[64];
    char names[64][256];
    vfs_node_type_t types[64];
    u32 count = 0;
    u32 index = 0;

    if (!directory || directory->type != VFS_TYPE_DIRECTORY || depth >= 32U)
        return MG_ERR_BAD_ARGUMENT;
    while (count < 64U && vfs_readdir_trusted(directory, index,
                                               &entries[count])) {
        strncpy(names[count], entries[count].name, sizeof(names[count]) - 1);
        names[count][sizeof(names[count]) - 1] = '\0';
        types[count] = entries[count].type;
        count++;
        index++;
    }
    if (count == 64U) return MG_ERR_NO_MEMORY;
    for (u32 child_index = 0; child_index < count; child_index++) {
        vfs_node_t *child = vfs_finddir_trusted(directory, names[child_index]);
        int result;

        if (!child) return MG_ERR_IO;
        if (types[child_index] == VFS_TYPE_DIRECTORY) {
            result = account_purge_directory(child, depth + 1U);
            if (result == MG_OK)
                result = account_vfs_error(
                    vfs_rmdir_trusted(directory, names[child_index]));
        } else {
            result = account_vfs_error(
                vfs_unlink_trusted(directory, names[child_index]));
        }
        if (result != MG_OK) return result;
    }
    return MG_OK;
}

static int account_purge_home(const user_identity_t *identity)
{
    vfs_node_t *user_root = NULL;
    vfs_node_t *home = NULL;
    int result;

    if (!account_home_is_safe(identity)) return MG_ERR_BAD_ARGUMENT;
    result = vfs_lookup_trusted("/user", &user_root);
    if (result != VFS_OK || !user_root ||
        user_root->type != VFS_TYPE_DIRECTORY) return account_vfs_error(result);
    home = vfs_finddir_trusted(user_root, identity->username);
    if (!home || home->type != VFS_TYPE_DIRECTORY ||
        home->owner_uid != identity->uid) return MG_ERR_ACCESS_DENIED;
    /* A freshly created account home has no directory stream yet.  Its
     * validated node size is the authoritative empty-state indication, so
     * avoid a needless device read before removing it. */
    if (home->size != 0) {
        result = account_purge_directory(home, 0);
        if (result != MG_OK) return result;
    }
    result = vfs_rmdir_trusted(user_root, identity->username);
    return account_vfs_error(result);
}

int identity_account_list(mg_account_info_t *accounts, usize capacity,
                          usize *out_count)
{
    identity_registry_t registry;

    if (!out_count || (capacity && !accounts) ||
        !identity_registry_copy_active(&registry)) return MG_ERR_BAD_ARGUMENT;
    *out_count = registry.count;
    if (capacity < registry.count) return MG_ERR_BUFFER_TOO_SMALL;
    for (u32 index = 0; index < registry.count; index++)
        account_info_from_identity(&registry.users[index], &accounts[index]);
    return MG_OK;
}

int identity_account_show(const char *username, mg_account_info_t *account)
{
    user_identity_t identity;

    if (!username || !account || !username_valid(username))
        return MG_ERR_BAD_ARGUMENT;
    if (!identity_registry_lookup_username(username, &identity))
        return MG_ERR_NOT_FOUND;
    account_info_from_identity(&identity, account);
    return MG_OK;
}

int identity_account_create(const char *username, const char *password)
{
    identity_registry_t *registry;
    user_identity_t *user;
    vfs_node_t *user_root = NULL;
    vfs_node_t *home = NULL;
    int result;

    result = account_require_manage_users();
    if (result != MG_OK) return result;
    if (!username || !username_valid(username) ||
        !password_input_valid(password)) return MG_ERR_BAD_ARGUMENT;
    registry = (identity_registry_t *)kmalloc(sizeof(*registry));
    if (!registry) return MG_ERR_NO_MEMORY;
    if (!identity_update_begin()) {
        kfree(registry);
        return MG_ERR_BUSY;
    }
    result = identity_registry_copy_active(registry) ? MG_OK : MG_ERR_IO;
    if (result != MG_OK) goto create_done;
    if (registry->count >= IDENTITY_ACCOUNT_MAX_RECORDS ||
        registry->next_uid == ~(u32)0) {
        result = MG_ERR_BUSY;
        goto create_done;
    }
    if (account_find(registry, username) >= 0) {
        result = MG_ERR_ALREADY_EXISTS;
        goto create_done;
    }
    {
        char description[AUTHORIZATION_MESSAGE_MAX];
        usize description_length = 0;
        char home_path[IDENTITY_HOME_CAPACITY];

        memcpy(home_path, "/user/", 6);
        memcpy(home_path + 6, username, strlen(username) + 1U);
        if (!append_text(description, sizeof(description),
                         &description_length, "Create account \"") ||
            !append_text(description, sizeof(description), &description_length,
                         username) ||
            !append_text(description, sizeof(description), &description_length,
                         "\" with home ") ||
            !append_text(description, sizeof(description), &description_length,
                         home_path) ||
            !append_text(description, sizeof(description), &description_length,
                         ".")) {
            result = MG_ERR_BAD_ARGUMENT;
            goto create_done;
        }
        description[description_length] = '\0';
        result = authorization_confirm_current(
            IDENTITY_PRIVILEGE_MANAGE_USERS, description);
        if (result != MG_OK) goto create_done;
    }
    user = &registry->users[registry->count];
    memset(user, 0, sizeof(*user));
    user->uid = registry->next_uid;
    user->role = MG_IDENTITY_ROLE_REGULAR;
    strncpy(user->username, username, sizeof(user->username) - 1);
    memcpy(user->home, "/user/", 6);
    memcpy(user->home + 6, username, strlen(username) + 1);
    result = account_make_password(registry, registry->count, password,
                                   &registry->authentication[registry->count]);
    if (result != MG_OK) goto create_done;
    registry->count++;
    registry->next_uid++;
    if (!identity_registry_valid(registry)) {
        result = MG_ERR_BAD_ARGUMENT;
        goto create_done;
    }
    result = vfs_lookup_trusted("/user", &user_root);
    if (result != VFS_OK || !user_root ||
        user_root->type != VFS_TYPE_DIRECTORY) {
        result = account_vfs_error(result == VFS_OK ? VFS_ERR_NOT_DIRECTORY
                                                    : result);
        goto create_done;
    }
    if (vfs_finddir_trusted(user_root, username)) {
        result = MG_ERR_ALREADY_EXISTS;
        goto create_done;
    }
    result = vfs_mkdir_owned(user_root, username, user->uid,
                             VFS_DEFAULT_USER_PERMISSIONS, &home);
    if (result != VFS_OK || !home) {
        result = account_vfs_error(result);
        goto create_done;
    }
    result = account_persist_registry(registry);
    if (result != MG_OK) {
        (void)vfs_rmdir_trusted(user_root, username);
        goto create_done;
    }
    (void)identity_registry_publish(registry);
create_done:
    kfree(registry);
    identity_update_end();
    return result;
}

int identity_account_remove(const char *username, bool purge)
{
    identity_registry_t old_registry;
    identity_registry_t registry;
    process_credentials_t credentials;
    int index;
    int result;

    result = account_require_manage_users();
    if (result != MG_OK) return result;
    if (!username || !username_valid(username)) return MG_ERR_BAD_ARGUMENT;
    if (!identity_update_begin()) return MG_ERR_BUSY;
    if (!identity_registry_copy_active(&old_registry)) {
        result = MG_ERR_IO;
        goto remove_done;
    }
    index = account_find(&old_registry, username);
    if (index < 0) {
        result = MG_ERR_NOT_FOUND;
        goto remove_done;
    }
    if (!process_get_credentials(process_current(), &credentials))
        result = MG_ERR_ACCESS_DENIED;
    else if (credentials.uid == old_registry.users[index].uid ||
             (old_registry.users[index].role == MG_IDENTITY_ROLE_ADMIN &&
              account_admin_count(&old_registry) <= 1U))
        result = MG_ERR_ACCESS_DENIED;
    else if (purge && !account_home_is_safe(&old_registry.users[index]))
        result = MG_ERR_BAD_ARGUMENT;
    else
        result = MG_OK;
    if (result != MG_OK) goto remove_done;
    {
        char description[AUTHORIZATION_MESSAGE_MAX];
        usize description_length = 0;

        if (!append_text(description, sizeof(description),
                         &description_length,
                         purge ? "Delete account \"" : "Remove account \"") ||
            !append_text(description, sizeof(description), &description_length,
                         username) ||
            !append_text(description, sizeof(description), &description_length,
                         purge ? "\" and permanently delete\n" : "\" and preserve its home.")) {
            result = MG_ERR_BAD_ARGUMENT;
            goto remove_done;
        }
        if (purge &&
            (!append_text(description, sizeof(description), &description_length,
                           old_registry.users[index].home) ||
             !append_text(description, sizeof(description), &description_length,
                           " and all contents."))) {
            result = MG_ERR_BAD_ARGUMENT;
            goto remove_done;
        }
        description[description_length] = '\0';
        result = authorization_confirm_current(
            IDENTITY_PRIVILEGE_MANAGE_USERS, description);
        if (result != MG_OK) goto remove_done;
    }
    registry = old_registry;
    if (old_registry.users[index].flags & IDENTITY_ACCOUNT_FLAG_INITIAL) {
        int replacement = -1;
        for (u32 i = 0; i < old_registry.count; i++) {
            if ((int)i == index) continue;
            if (old_registry.users[i].role == MG_IDENTITY_ROLE_ADMIN) {
                replacement = (int)i;
                break;
            }
            if (replacement < 0) replacement = (int)i;
        }
        if (replacement < 0) {
            result = MG_ERR_ACCESS_DENIED;
            goto remove_done;
        }
        registry.users[replacement].flags |= IDENTITY_ACCOUNT_FLAG_INITIAL;
    }
    for (u32 i = (u32)index; i + 1U < registry.count; i++)
        registry.users[i] = registry.users[i + 1U];
    for (u32 i = (u32)index; i + 1U < registry.count; i++)
        registry.authentication[i] = registry.authentication[i + 1U];
    registry.count--;
    result = account_persist_registry(&registry);
    if (result != MG_OK) goto remove_done;
    if (purge) {
        result = account_purge_home(&old_registry.users[index]);
        if (result != MG_OK) {
            int restore_result = account_persist_registry(&old_registry);
            if (restore_result != MG_OK)
                result = MG_ERR_IO;
            goto remove_done;
        }
    }
    (void)identity_registry_publish(&registry);
remove_done:
    identity_update_end();
    return result;
}

int identity_account_set_role(const char *username, mg_identity_role_t role)
{
    identity_registry_t registry;
    int index;
    int result;

    result = account_require_manage_users();
    if (result != MG_OK) return result;
    if (!username || !username_valid(username) ||
        (role != MG_IDENTITY_ROLE_REGULAR &&
         role != MG_IDENTITY_ROLE_ADMIN)) return MG_ERR_BAD_ARGUMENT;
    if (!identity_update_begin()) return MG_ERR_BUSY;
    if (!identity_registry_copy_active(&registry)) {
        result = MG_ERR_IO;
        goto role_done;
    }
    index = account_find(&registry, username);
    if (index < 0) {
        result = MG_ERR_NOT_FOUND;
        goto role_done;
    }
    if (registry.users[index].role == role) {
        result = MG_OK;
        goto role_done;
    }
    if (role == MG_IDENTITY_ROLE_REGULAR &&
        registry.users[index].role == MG_IDENTITY_ROLE_ADMIN &&
        account_admin_count(&registry) <= 1U) {
        result = MG_ERR_ACCESS_DENIED;
        goto role_done;
    }
    {
        char description[AUTHORIZATION_MESSAGE_MAX];
        usize description_length = 0;
        const char *role_text = role == MG_IDENTITY_ROLE_ADMIN ?
            "admin" : "regular";

        if (!append_text(description, sizeof(description),
                         &description_length, "Change account \"") ||
            !append_text(description, sizeof(description), &description_length,
                         username) ||
            !append_text(description, sizeof(description), &description_length,
                         "\" role to ") ||
            !append_text(description, sizeof(description), &description_length,
                         role_text) ||
            !append_text(description, sizeof(description), &description_length,
                         ".")) {
            result = MG_ERR_BAD_ARGUMENT;
            goto role_done;
        }
        description[description_length] = '\0';
        result = authorization_confirm_current(
            IDENTITY_PRIVILEGE_MANAGE_USERS, description);
        if (result != MG_OK) goto role_done;
    }
    registry.users[index].role = role;
    if (!identity_registry_valid(&registry)) {
        result = MG_ERR_BAD_ARGUMENT;
        goto role_done;
    }
    result = account_persist_registry(&registry);
    if (result == MG_OK) (void)identity_registry_publish(&registry);
role_done:
    identity_update_end();
    return result;
}
