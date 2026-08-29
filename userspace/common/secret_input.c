#include "secret_input.h"

#include <mangrove.h>
#include <mg/object.h>

mg_result_t read_console_line(char *buffer, usize capacity, bool echo)
{
    usize length = 0;
    bool overflow = false;
    bool secure = !echo;
    mg_result_t result;

    if (!buffer || capacity < 2U) return MG_ERR_BAD_ARGUMENT;
    if (secure) {
        result = console_set_secure_input(true);
        if (result < 0) return result;
    }
    /* Prompts are commonly entered while Shoot has an output
     * transaction open.  Make the prompt visible before blocking. */
    (void)console_end_transaction();
    for (;;) {
        char character;
        result = object_read(MG_CONSOLE_HANDLE, &character, 1);

        if (result < 0) {
            if (secure) (void)console_set_secure_input(false);
            return result;
        }
        if (result != 1) {
            if (secure) (void)console_set_secure_input(false);
            return MG_ERR_END_OF_FILE;
        }
        if (character == '\r') character = '\n';
        if (character == '\n') break;
        if (character == '\b' || (unsigned char)character == 0x7f) {
            if (length != 0) {
                length--;
                (void)console_write_string("\b \b");
            }
            continue;
        }
        if ((unsigned char)character < 0x20U) continue;
        if (length + 1U >= capacity) {
            overflow = true;
            continue;
        }
        buffer[length++] = character;
        if (echo) {
            (void)console_write(&character, 1);
        } else {
            (void)console_write_string("*");
        }
    }
    buffer[length] = '\0';
    (void)console_write_string("\n");
    if (secure) {
        result = console_set_secure_input(false);
        if (result < 0) return result;
    }
    return overflow ? MG_ERR_BAD_ARGUMENT : MG_OK;
}

mg_result_t read_hidden_line(char *buffer, usize capacity)
{
    return read_console_line(buffer, capacity, false);
}

void clear_secret(void *buffer, usize size)
{
    volatile u8 *bytes = (volatile u8 *)buffer;
    if (!bytes) return;
    while (size--) *bytes++ = 0;
}
