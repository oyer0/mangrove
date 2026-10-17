/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

char *strtok(char *restrict text, const char *restrict delimiters)
{
    static char *next;
    unsigned char set[256] = { 0 };
    char *start;

    while (*delimiters) set[(unsigned char)*delimiters++] = 1U;
    if (text) next = text;
    else if (!next) return NULL;

    while (*next && set[(unsigned char)*next]) next++;
    if (!*next) {
        next = NULL;
        return NULL;
    }
    start = next;
    while (*next && !set[(unsigned char)*next]) next++;
    if (*next) *next++ = '\0';
    else next = NULL;
    return start;
}
