/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>

static int in_range(int value, int first, int last)
{
    return value >= first && value <= last;
}

int isupper(int c) { return in_range(c, 'A', 'Z'); }
int islower(int c) { return in_range(c, 'a', 'z'); }
int isalpha(int c) { return isupper(c) || islower(c); }
int isdigit(int c) { return in_range(c, '0', '9'); }
int isxdigit(int c)
{
    return isdigit(c) || in_range(c, 'A', 'F') || in_range(c, 'a', 'f');
}
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return in_range(c, 0, 0x1f) || c == 0x7f; }
int isspace(int c)
{
    return c == ' ' || in_range(c, '\t', '\r');
}
int isprint(int c) { return in_range(c, ' ', '~'); }
int isgraph(int c) { return in_range(c, '!', '~'); }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }
