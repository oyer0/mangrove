#include <authorization.h>

#include <console.h>
#include <kprint.h>
#include <process.h>
#include <string.h>
#include <terminal.h>

#define AUTHORIZATION_PROMPT_MAX 320U

static bool authorization_text_valid(const char *text)
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
        /* Reject every terminal control character.  The description is
         * presentation-only and must never be able to inject an escape. */
        if (value < 0x20U || value == 0x7fU) return false;
        if (length >= AUTHORIZATION_MESSAGE_MAX) return false;
    }
    return !ended_line;
}

static bool authorization_append(char *buffer, usize capacity, usize *length,
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

static bool authorization_read_answer(void)
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
            /* Only printable input is echoed.  Control characters are
             * rejected without allowing terminal-control injection. */
            if ((unsigned char)value >= 0x20U &&
                (unsigned char)value < 0x7fU) {
                char echoed[2] = {value, '\0'};
                terminal_write(echoed);
            }
        }
    }
}

int authorization_confirm_current(identity_privilege_t privilege,
                                  const char *description)
{
    process_t *requester = process_current();
    process_credentials_t credentials;
    char prompt[AUTHORIZATION_PROMPT_MAX];
    usize length = 0;
    bool confirmed;

    if (!requester || !process_get_credentials(requester, &credentials) ||
        !identity_credentials_has_privilege(&credentials, privilege))
        return MG_ERR_PRIVILEGE_REQUIRED;
    if (!authorization_text_valid(description) ||
        !authorization_append(prompt, sizeof(prompt), &length,
                              requester->name) ||
        !authorization_append(prompt, sizeof(prompt), &length,
                              " wants to perform an administrator action:\n\n") ||
        !authorization_append(prompt, sizeof(prompt), &length, description) ||
        !authorization_append(prompt, sizeof(prompt), &length,
                              "\n\n[y/N] "))
        return MG_ERR_BAD_ARGUMENT;

    /* Shoot keeps a presentation transaction open while it waits for an
     * external child.  End it before blocking so this system-owned prompt is
     * visible and cannot be hidden behind the shell's batch. */
    terminal_force_end_batch();
    terminal_cursor_disable();
    terminal_write(prompt);
    confirmed = authorization_read_answer();
    terminal_cursor_enable();
    return confirmed ? MG_OK : MG_ERR_CANCELLED;
}
