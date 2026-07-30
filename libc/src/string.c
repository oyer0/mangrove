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

char *strcpy(char *dest, const char *src)
{
    char *orig = dest;
    while ((*dest++ = *src++));
    return orig;
}

char *strncpy(char *dest, const char *src, usize n)
{
    usize i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strncat(char *dest, const char *src, usize n)
{
    char *ptr = dest + strlen(dest);
    while (*src && n--) {
        *ptr++ = *src++;
    }
    *ptr = '\0';
    return dest;
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

void *memcpy(void *dest, const void *src, usize size)
{
    u8 *d = (u8 *)dest;
    const u8 *s = (const u8 *)src;

    for (usize i = 0; i < size; i++)
    {
        d[i] = s[i];
    }

    return dest;
}
