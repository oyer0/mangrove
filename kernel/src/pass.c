#include <pass.h>

#include <config_parser.h>
#include <console.h>
#include <ipc.h>
#include <process.h>
#include <string.h>
#include <terminal.h>
#include <vfs.h>

#define PASS_PROMPT_MAX 320U

typedef pass_result_t (*pass_provider_authorize_t)(
    const process_credentials_t *credentials, const char *requester_name,
    const char *description);

typedef struct {
    pass_provider_t id;
    const char *name;
    pass_provider_authorize_t authorize;
} pass_provider_definition_t;

static pass_provider_t pass_provider_from_value(const char *value,
                                                bool *valid)
{
    if (valid) *valid = true;
    if (!value) {
        if (valid) *valid = false;
        return PASS_PROVIDER_CONFIRM;
    }
    if (strcmp(value, "password") == 0) return PASS_PROVIDER_PASSWORD;
    if (strcmp(value, "confirm") == 0) return PASS_PROVIDER_CONFIRM;
    if (strcmp(value, "scripts") == 0) return PASS_PROVIDER_SCRIPTS;
    if (strcmp(value, "none") == 0) return PASS_PROVIDER_NONE;
    if (valid) *valid = false;
    return PASS_PROVIDER_CONFIRM;
}

bool pass_policy_read(pass_policy_t *policy)
{
    vfs_node_t *node = NULL;
    vfs_file_handle_t *handle = NULL;
    kernel_config_document_t document;
    char contents[PASS_SECURITY_CONFIG_MAX_BYTES + 1U];
    u32 error_line;
    usize length;
    int result;
    bool valid;

    if (!policy) return false;
    policy->provider = PASS_PROVIDER_CONFIRM;
    policy->valid = false;

    result = vfs_lookup_trusted(PASS_SECURITY_CONFIG_PATH, &node);
    if (result != VFS_OK || !node || node->type != VFS_TYPE_FILE ||
        node->size == 0 || node->size > PASS_SECURITY_CONFIG_MAX_BYTES)
        return false;
    length = (usize)node->size;
    result = vfs_open_trusted(PASS_SECURITY_CONFIG_PATH, VFS_OPEN_READ,
                              &handle);
    if (result != VFS_OK || !handle ||
        vfs_file_read_trusted(handle, length, contents) != length) {
        if (handle) vfs_close_trusted(handle);
        return false;
    }
    vfs_close_trusted(handle);
    contents[length] = '\0';
    if (!kernel_config_parse(contents, length, &document, &error_line))
        return false;

    policy->provider = pass_provider_from_value(
        kernel_config_find(&document, "authorization"), &valid);
    policy->valid = valid;
    return valid;
}

pass_provider_t pass_policy_current(void)
{
    pass_policy_t policy;

    if (!pass_policy_read(&policy)) return PASS_PROVIDER_CONFIRM;
    return policy.provider;
}

const char *pass_provider_name(pass_provider_t provider)
{
    switch (provider) {
        case PASS_PROVIDER_PASSWORD: return "password";
        case PASS_PROVIDER_CONFIRM: return "confirm";
        case PASS_PROVIDER_SCRIPTS: return "scripts";
        case PASS_PROVIDER_NONE: return "none";
        default: return "confirm";
    }
}

static bool pass_text_valid(const char *text)
{
    usize length = 0;
    u32 lines = 1;
    bool ended_line = false;

    if (!text || !text[0]) return false;
    while (text[length]) {
        unsigned char value = (unsigned char)text[length++];
        ended_line = false;
        if (value == '\n') {
            if (++lines > 3U) return false;
            ended_line = true;
            continue;
        }
        /* Descriptions are presentation-only and cannot inject terminal
         * control sequences into trusted interaction. */
        if (value < 0x20U || value == 0x7fU) return false;
        if (length >= PASS_MESSAGE_MAX) return false;
    }
    return !ended_line;
}

static bool pass_append(char *buffer, usize capacity, usize *length,
                        const char *text)
{
    usize amount;

    if (!buffer || !length || !text || *length >= capacity) return false;
    amount = strlen(text);
    if (amount >= capacity - *length) return false;
    memcpy(buffer + *length, text, amount);
    *length += amount;
    buffer[*length] = '\0';
    return true;
}

static bool pass_read_confirmation(void)
{
    char answer = 0;
    bool invalid = false;

    for (;;) {
        char value;
        u64 received = console_read_bytes(&value, 1);

        if (received != 1) return false;
        if (value == '\r') value = '\n';
        if (value == '\n') {
            terminal_write("\n");
            if (!invalid && (answer == 'y' || answer == 'Y')) return true;
            if (!invalid && (answer == 'n' || answer == 'N' || !answer))
                return false;
            terminal_write("Invalid response. Enter y or n.\n[y/N] ");
            answer = 0;
            invalid = false;
            continue;
        }

        if (!invalid && !answer &&
            (value == 'y' || value == 'Y' || value == 'n' || value == 'N')) {
            char echoed[2] = {value, '\0'};
            answer = value;
            terminal_write(echoed);
        } else {
            invalid = true;
            if ((unsigned char)value >= 0x20U &&
                (unsigned char)value < 0x7fU) {
                char echoed[2] = {value, '\0'};
                terminal_write(echoed);
            }
        }
    }
}

static bool pass_read_password(const process_credentials_t *credentials)
{
    char password[IDENTITY_PASSWORD_MAX_LENGTH + 1U] = {0};
    usize length = 0;
    bool overflow = false;

    if (!credentials) return false;
    for (;;) {
        char value;
        u64 received = console_read_bytes(&value, 1);

        if (received != 1) {
            password_secure_clear(password, sizeof(password));
            return false;
        }
        if (value == '\r') value = '\n';
        if (value == '\n') {
            bool valid = !overflow && identity_password_verify_current(
                credentials, password);
            terminal_write("\n");
            password_secure_clear(password, sizeof(password));
            return valid;
        }
        if (value == '\b' || (unsigned char)value == 0x7fU) {
            if (length != 0) {
                password[--length] = '\0';
                terminal_write("\b \b");
            }
            continue;
        }
        if ((unsigned char)value < 0x20U ||
            (unsigned char)value >= 0x7fU) continue;
        if (length >= IDENTITY_PASSWORD_MAX_LENGTH) {
            overflow = true;
            continue;
        }
        password[length++] = value;
        password[length] = '\0';
        terminal_write("*");
    }
}

static pass_result_t pass_provider_password(
    const process_credentials_t *credentials, const char *requester_name,
    const char *description)
{
    (void)requester_name;
    (void)description;
    terminal_force_end_batch();
    terminal_cursor_disable();
    terminal_write("Administrator password: ");
    if (pass_read_password(credentials)) {
        terminal_cursor_enable();
        return PASS_RESULT_ALLOWED;
    }
    terminal_cursor_enable();
    return PASS_RESULT_AUTHENTICATION_FAILED;
}

static pass_result_t pass_provider_confirm(
    const process_credentials_t *credentials, const char *requester_name,
    const char *description)
{
    char prompt[PASS_PROMPT_MAX];
    usize length = 0;

    if (!pass_append(prompt, sizeof(prompt), &length, requester_name) ||
        !pass_append(prompt, sizeof(prompt), &length,
                     " wants to perform an administrator action:\n\n") ||
        !pass_append(prompt, sizeof(prompt), &length, description) ||
        !pass_append(prompt, sizeof(prompt), &length, "\n\n[y/N] "))
        return PASS_RESULT_DENIED;

    /* A shell presentation transaction can otherwise hide a system-owned
     * prompt while the child waits for input. */
    (void)credentials;
    terminal_force_end_batch();
    terminal_cursor_disable();
    terminal_write(prompt);
    if (pass_read_confirmation()) {
        terminal_cursor_enable();
        return PASS_RESULT_ALLOWED;
    }
    terminal_cursor_enable();
    return PASS_RESULT_CANCELLED;
}

static pass_result_t pass_provider_scripts(
    const process_credentials_t *credentials, const char *requester_name,
    const char *description)
{
    (void)credentials;
    (void)requester_name;
    (void)description;
    /* Shoot has no trustworthy script/job boundary yet.  Preserve the
     * established direct-admin scripts-mode behavior, while deliberately
     * not inventing a transferable script authorization context. */
    return PASS_RESULT_ALLOWED;
}

static pass_result_t pass_provider_none(
    const process_credentials_t *credentials, const char *requester_name,
    const char *description)
{
    (void)credentials;
    (void)requester_name;
    (void)description;
    return PASS_RESULT_ALLOWED;
}

static const pass_provider_definition_t pass_providers[] = {
    {PASS_PROVIDER_PASSWORD, "password", pass_provider_password},
    {PASS_PROVIDER_CONFIRM, "confirm", pass_provider_confirm},
    {PASS_PROVIDER_SCRIPTS, "scripts", pass_provider_scripts},
    {PASS_PROVIDER_NONE, "none", pass_provider_none},
};

static const pass_provider_definition_t *pass_provider_lookup(
    pass_provider_t provider)
{
    for (usize index = 0;
         index < sizeof(pass_providers) / sizeof(pass_providers[0]); index++) {
        if (pass_providers[index].id == provider) return &pass_providers[index];
    }
    return &pass_providers[1];
}

static pass_result_t pass_authorize_for_requester(
    const process_credentials_t *credentials, const char *requester_name,
    identity_privilege_t privilege, const char *description)
{
    const pass_provider_definition_t *provider;

    if (!credentials || !requester_name ||
        !identity_credentials_has_privilege(credentials, privilege))
        return PASS_RESULT_PRIVILEGE_REQUIRED;
    if (!pass_text_valid(description)) return MG_ERR_BAD_ARGUMENT;

    /* SYSTEM authority is explicit service capability, not a UID 0 bypass.
     * A service's autonomous maintenance therefore does not ask a human. */
    if (identity_credentials_is_system(credentials))
        return PASS_RESULT_ALLOWED;

    provider = pass_provider_lookup(pass_policy_current());
    return provider->authorize(credentials, requester_name, description);
}

pass_result_t pass_authorize_current(identity_privilege_t privilege,
                                     const char *description)
{
    process_t *requester = process_current();
    process_credentials_t credentials;

    if (!requester || !process_get_credentials(requester, &credentials))
        return PASS_RESULT_PRIVILEGE_REQUIRED;
    return pass_authorize_for_requester(&credentials, requester->name,
                                        privilege, description);
}

pass_result_t pass_authorize_request(process_t *service,
                                     process_handle_t request_handle,
                                     identity_privilege_t privilege,
                                     const char *description)
{
    process_credentials_t credentials;
    char requester_name[32];

    if (!ipc_request_context_claim(service, request_handle, &credentials,
                                   requester_name, sizeof(requester_name)))
        return MG_ERR_SERVICE_UNAVAILABLE;
    return pass_authorize_for_requester(&credentials, requester_name,
                                        privilege, description);
}

pass_result_t pass_authenticate_account(const char *username,
                                        const char *password,
                                        user_identity_t *identity)
{
    int result;

    /* The identity subsystem owns account-record parsing and PBKDF2.  PASS
     * is the sole caller-facing authentication boundary and intentionally
     * maps account/credential failures to the normal generic login result. */
    result = identity_password_authenticate(username, password, identity);
    return result == MG_OK ? PASS_RESULT_ALLOWED :
                             PASS_RESULT_AUTHENTICATION_FAILED;
}
