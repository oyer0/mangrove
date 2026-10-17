/* SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(RUNTIME_MG_FIRST)
#include <mg/types.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>
#include <assert.h>
#include <iso646.h>
#include <stdalign.h>
#include <stdnoreturn.h>
#if !defined(RUNTIME_MG_FIRST)
#include <mg/types.h>
#endif

struct layout_probe { char prefix; long member; };
alignas(32) static unsigned char aligned_header_probe;
noreturn static void noreturn_header_probe(void) { __builtin_trap(); }

_Static_assert(__builtin_types_compatible_p(size_t, unsigned long), "size_t ABI");
_Static_assert(__builtin_types_compatible_p(ptrdiff_t, long), "ptrdiff_t ABI");
_Static_assert(__builtin_types_compatible_p(intptr_t, long), "intptr_t ABI");
_Static_assert(__builtin_types_compatible_p(uintptr_t, unsigned long), "uintptr_t ABI");
_Static_assert(__builtin_types_compatible_p(int64_t, long), "int64_t ABI");
_Static_assert(__builtin_types_compatible_p(uint64_t, unsigned long), "uint64_t ABI");
_Static_assert(__builtin_types_compatible_p(intmax_t, long), "intmax_t ABI");
_Static_assert(__builtin_types_compatible_p(uintmax_t, unsigned long), "uintmax_t ABI");
_Static_assert(__builtin_types_compatible_p(u64, unsigned long long), "native u64 ABI");
_Static_assert(sizeof(size_t) == 8 && sizeof(ptrdiff_t) == 8, "LP64 widths");
_Static_assert(_Alignof(max_align_t) >= 16, "max_align_t alignment");
_Static_assert(__alignof__(aligned_header_probe) >= 32,
               "alignas macro maps to the compiler keyword");
_Static_assert(offsetof(struct layout_probe, member) == 8, "offsetof");
_Static_assert(sizeof(wchar_t) == 4 && WCHAR_MAX == 0x7fffffff, "wchar_t ABI");
_Static_assert(sizeof(va_list) == 24, "x86-64 SysV va_list");
_Static_assert(sizeof(PRId64) == 3 && sizeof(PRIu64) == 3, "LP64 inttypes formats");
_Static_assert(sizeof(PRIu8) == 4 && sizeof(PRIu16) == 3, "promoted small unsigned formats");
_Static_assert(INT_MAX == 2147483647 && LONG_MAX == 9223372036854775807L,
               "integer limits");
_Static_assert(MB_LEN_MAX == 4 && MB_CUR_MAX == 4,
               "fixed UTF-8 multibyte maximum");
_Static_assert(__builtin_types_compatible_p(wint_t, __WINT_TYPE__),
               "wint_t compiler ABI");
_Static_assert(WCHAR_MIN == __WCHAR_MIN__ && WCHAR_MAX == __WCHAR_MAX__,
               "wchar_t limits match compiler ABI");
_Static_assert(WINT_MIN == __WINT_MIN__ && WINT_MAX == __WINT_MAX__,
               "wint_t limits match compiler ABI");
_Static_assert(__builtin_types_compatible_p(time_t, long), "time_t ABI");
_Static_assert(__builtin_types_compatible_p(clock_t, long), "clock_t ABI");
_Static_assert(sizeof(struct timespec) == 16, "timespec ABI");
_Static_assert(sizeof(struct tm) == 9 * sizeof(int), "struct tm layout");
_Static_assert(CLOCKS_PER_SEC == 1000L && TIME_UTC == 1, "time constants");
_Static_assert(alignof(max_align_t) >= 16, "stdalign operators");
_Static_assert(__STDC_VERSION_STDALIGN_H__ == 201112L &&
               __STDC_VERSION_STD_NORETURN_H__ == 201112L,
               "C11 language support headers");

static int copy_argument(int count, ...)
{
    va_list original, duplicate;
    int first, second;
    va_start(original, count);
    va_copy(duplicate, original);
    first = va_arg(original, int);
    second = va_arg(duplicate, int);
    va_end(duplicate);
    va_end(original);
    return first == second ? second : 0;
}

int main(void)
{
    bool value = true;
    int bits = 1 bitand 3;
    bits or_eq 2;
    if (!value) noreturn_header_probe();
    return value and (false == 0) and (bits == 3) and
           (copy_argument(0, 27) == 27) ? 0 : 1;
}
