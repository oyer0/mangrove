#include <string.h>

static int check(int condition, const char *name)
{
    (void)name;
    return condition;
}

int main(void)
{
    unsigned char a[16] = { 0 };
    unsigned char b[16] = { 0 };
    unsigned char overlap[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    int ok = 1;

    ok &= check(memcpy(b, "abcdef", 6) == b && memcmp(b, "abcdef", 6) == 0,
                "memcpy copy");
    ok &= check(memcpy(b, a, 0) == b, "memcpy zero");
    ok &= check(memmove(overlap + 2, overlap, 6) == overlap + 2 &&
                memcmp(overlap, "ababcdef", 8) == 0, "memmove forward overlap");
    ok &= check(memmove(overlap, overlap + 2, 6) == overlap &&
                memcmp(overlap, "abcdefef", 8) == 0, "memmove backward overlap");
    ok &= check(memmove(overlap, overlap, 8) == overlap, "memmove same");
    ok &= check(memset(a, 0xff, sizeof(a)) == a && a[0] == 0xff &&
                memset(a, 0x80, 0) == a, "memset values and zero");
    ok &= check(memcmp("abc", "abc", 3) == 0 && memcmp("abc", "abd", 3) < 0,
                "memcmp equality and ordering");
    ok &= check(memcmp((unsigned char *)"\xff", "\1", 1) > 0,
                "memcmp unsigned ordering");
    ok &= check(strlen("") == 0 && strlen("abc") == 3, "strlen");
    ok &= check(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0,
                "strcmp");
    ok &= check(strncmp("abc", "abd", 2) == 0 &&
                strncmp("abc", "abd", 3) < 0 && strncmp("a", "b", 0) == 0,
                "strncmp");
    ok &= check(strchr("abcabc", 'b') != (char *)0 &&
                strchr("abc", 'z') == (char *)0 &&
                strchr("abc", '\0') != (char *)0, "strchr");
    return ok ? 0 : 1;
}
