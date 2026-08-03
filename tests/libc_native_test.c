#include <mangrove.h>
#include <string.h>

static char captured[128];
static usize captured_length;
static usize chunk = 3;

mg_result_t object_write(mg_handle_t handle, const void *buffer, usize length)
{
    usize amount = length < chunk ? length : chunk;
    if (handle != MG_CONSOLE_HANDLE || amount > sizeof(captured) - captured_length)
        return MG_ERR_IO;
    memcpy(captured + captured_length, buffer, amount);
    captured_length += amount;
    return (mg_result_t)amount;
}

int main(void)
{
    const char message[] = "partial console output";
    if (!result_is_success(MG_OK) || result_is_error(MG_OK) ||
        !result_is_error(MG_ERR_IO)) return 1;
    if (object_write_all(MG_CONSOLE_HANDLE, message, sizeof(message) - 1) !=
        (mg_result_t)(sizeof(message) - 1)) return 2;
    if (captured_length != sizeof(message) - 1 ||
        memcmp(captured, message, captured_length) != 0) return 3;
    captured_length = 0;
    if (console_write_string("hello") != 5 ||
        memcmp(captured, "hello", 5) != 0) return 4;
    if (console_write((const void *)0, 0) != 0 ||
        object_write_all(MG_CONSOLE_HANDLE, (const void *)0, 1) !=
        MG_ERR_BAD_ARGUMENT || console_write_string((const char *)0) !=
        MG_ERR_BAD_ARGUMENT) return 5;
    return 0;
}
