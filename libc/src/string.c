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
