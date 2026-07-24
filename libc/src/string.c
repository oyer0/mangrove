#include <string.h>

usize strlen(const char *str)
{
    usize len = 0;

    while (str[len] != '\0') {
        len++;
    }

    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }

    return (unsigned char)*s1 - (unsigned char)*s2;
}

void *memset(void *dest, int value, usize size)
{
    u8 *ptr = (u8 *)dest;

    for (usize i = 0; i < size; i++)
    {
        ptr[i] = (u8)value;
    }

    return dest;
}
