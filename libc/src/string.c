/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include <stdint.h>

size_t strlen(const char *str)
{
    size_t len = 0;

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

int strncmp(const char *s1, const char *s2, size_t n)
{
    size_t i;
    const unsigned char *left = (const unsigned char *)s1;
    const unsigned char *right = (const unsigned char *)s2;

    for (i = 0; i < n; i++) {
        if (left[i] != right[i] || left[i] == '\0') {
            return (int)left[i] - (int)right[i];
        }
    }
    return 0;
}

char *strchr(const char *str, int character)
{
    unsigned char wanted = (unsigned char)character;
    while ((unsigned char)*str != wanted) {
        if (*str == '\0') return (char *)0;
        str++;
    }
    return (char *)str;
}

char *strcpy(char *dest, const char *src)
{
    char *orig = dest;
    while ((*dest++ = *src++));
    return orig;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *ptr = dest + strlen(dest);
    while (n && *src) {
        *ptr++ = *src++;
        n--;
    }
    *ptr = '\0';
    return dest;
}

void *memset(void *dest, int value, size_t size)
{
    uint8_t *ptr = (uint8_t *)dest;

    for (size_t i = 0; i < size; i++)
    {
        ptr[i] = (uint8_t)value;
    }

    return dest;
}

void *memcpy(void *dest, const void *src, size_t size)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < size; i++)
    {
        d[i] = s[i];
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t size)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    uintptr_t d_addr = (uintptr_t)d;
    uintptr_t s_addr = (uintptr_t)s;

    if (d == s || size == 0) return dest;
    if (d_addr < s_addr || d_addr - s_addr >= size) {
        for (size_t i = 0; i < size; i++) d[i] = s[i];
    } else {
        for (size_t i = size; i != 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    for (size_t i = 0; i < size; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}
