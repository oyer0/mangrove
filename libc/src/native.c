#include <mangrove.h>
#include <string.h>

bool result_is_error(mg_result_t result)
{
    return result < 0;
}

bool result_is_success(mg_result_t result)
{
    return result >= 0;
}

mg_result_t object_write_all(mg_handle_t handle, const void *buffer,
                             usize length)
{
    const u8 *bytes = (const u8 *)buffer;
    usize written = 0;

    if (length != 0 && !buffer) return MG_ERR_BAD_ARGUMENT;
    while (written < length) {
        mg_result_t result = object_write(handle, bytes + written,
                                          length - written);
        if (result < 0) return result;
        if (result == 0 || (usize)result > length - written) return MG_ERR_IO;
        written += (usize)result;
    }
    return (mg_result_t)written;
}

mg_result_t console_write(const void *buffer, usize length)
{
    return object_write_all(MG_CONSOLE_HANDLE, buffer, length);
}

mg_result_t console_write_string(const char *string)
{
    if (!string) return MG_ERR_BAD_ARGUMENT;
    return console_write(string, strlen(string));
}
