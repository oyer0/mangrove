#include <mangrove.h>
#include <stdio.h>
#include <string.h>

static char output[512];
static usize output_length;

mg_result_t object_write(mg_handle_t handle, const void *buffer, usize length)
{
    if (handle != MG_CONSOLE_HANDLE || length > sizeof(output) - output_length)
        return MG_ERR_IO;
    memcpy(output + output_length, buffer, length);
    output_length += length;
    return (mg_result_t)length;
}

static int check(int condition)
{
    return condition ? 0 : 1;
}

int main(void)
{
    char buffer[64];
    char pointer[32];
    i64 minimum = (i64)(~(u64)0 / 2 + 1);
    int failures = 0;
    int result;

    failures += check(snprintf(buffer, sizeof(buffer), "literal") == 7 &&
                      strcmp(buffer, "literal") == 0);
    failures += check(snprintf(buffer, sizeof(buffer), "%s %c %d %u %x %X %%",
                               "ok", 'A', -12, 34U, 0xabU, 0xcdU) == 19 &&
                      strcmp(buffer, "ok A -12 34 ab CD %") == 0);
    failures += check(snprintf(buffer, sizeof(buffer), "%08x", 0x12U) == 8 &&
                      strcmp(buffer, "00000012") == 0);
    failures += check(snprintf(buffer, sizeof(buffer), "%s", (char *)0) == 6 &&
                      strcmp(buffer, "(null)") == 0);
    failures += check(snprintf(buffer, 4, "abcdef") == 6 &&
                      strcmp(buffer, "abc") == 0);
    failures += check(snprintf(buffer, 1, "x") == 1 && buffer[0] == '\0');
    failures += check(snprintf((char *)0, 0, "abc%d", 12) == 5);
    failures += check(snprintf(buffer, sizeof(buffer), "%lld", minimum) == 20 &&
                      strcmp(buffer, "-9223372036854775808") == 0);
    failures += check(snprintf(pointer, sizeof(pointer), "%p", (void *)0x1234) == 18 &&
                      strcmp(pointer, "0x0000000000001234") == 0);

    output_length = 0;
    result = printf("%s %d", "value", 7);
    failures += check(result == 7 && output_length == 7 &&
                      memcmp(output, "value 7", 7) == 0);
    failures += check(putchar('!') == '!' && puts("\n") == 0 &&
                      output_length == 10 && memcmp(output + 7, "!\n\n", 3) == 0);
    return failures ? 1 : 0;
}
