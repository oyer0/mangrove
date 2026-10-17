/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <wchar.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef char wchar_is_32_bit[(sizeof(wchar_t) == 4U) ? 1 : -1];
typedef char wchar_is_signed[((wchar_t)-1 < (wchar_t)0) ? 1 : -1];
typedef char size_is_64_bit[(sizeof(size_t) == sizeof(usize)) ? 1 : -1];

static mbstate_t mbrtowc_internal;
static mbstate_t mbrlen_internal;
static mbstate_t mbsrtowcs_internal;
static mbstate_t wcsrtombs_internal;
static mbstate_t mbtowc_internal;
static mbstate_t mblen_internal;
static mbstate_t wctomb_internal;

static size_t conversion_error(void)
{
    errno = EILSEQ;
    return (size_t)-1;
}

static mbstate_t *select_state(mbstate_t *state, mbstate_t *internal)
{
    return state ? state : internal;
}

int mbsinit(const mbstate_t *state)
{
    return mg_utf8_state_is_initial(state) ? 1 : 0;
}

size_t mbrtowc(wchar_t *restrict output, const char *restrict input,
               size_t length, mbstate_t *restrict state)
{
    mbstate_t *current = select_state(state, &mbrtowc_internal);
    size_t used = 0;

    if (!input) return mbrtowc(NULL, "", 1U, state);
    if (!length) return (size_t)-2;
    while (used < length) {
        mg_utf8_feed_result_t decoded = mg_utf8_state_feed(
            current, (u8)input[used]);
        if (decoded.consumed) used++;
        if (decoded.status == MG_UTF8_INVALID) {
            mg_utf8_state_reset(current);
            return conversion_error();
        }
        if (decoded.status == MG_UTF8_OK) {
            if (output) *output = (wchar_t)decoded.scalar;
            return decoded.scalar == 0U ? 0U : used;
        }
    }
    return (size_t)-2;
}

size_t mbrlen(const char *restrict input, size_t length,
              mbstate_t *restrict state)
{
    return mbrtowc(NULL, input, length, select_state(state, &mbrlen_internal));
}

size_t wcrtomb(char *restrict output, wchar_t value,
               mbstate_t *restrict state)
{
    mbstate_t *current = select_state(state, &wctomb_internal);
    u8 encoded[4];
    usize produced;
    u32 scalar;

    if (!output) {
        char ignored[4];
        return wcrtomb(ignored, L'\0', state);
    }
    if (!mg_utf8_state_is_initial(current)) {
        mg_utf8_state_reset(current);
        return conversion_error();
    }
    if (value < 0) return conversion_error();
    scalar = (u32)value;
    if (mg_utf8_encode_one(scalar, encoded, sizeof(encoded), &produced) !=
        MG_UTF8_OK)
        return conversion_error();
    for (size_t index = 0; index < produced; index++)
        output[index] = (char)encoded[index];
    mg_utf8_state_reset(current);
    return (size_t)produced;
}

static size_t mbsrtowcs_run(wchar_t *restrict output,
                            const char **restrict input, size_t capacity,
                            mbstate_t *restrict state, bool measure)
{
    const char *source;
    size_t byte_length, offset = 0, converted = 0;
    mbstate_t saved, *current;

    if (!input || !*input) {
        errno = EINVAL;
        return (size_t)-1;
    }
    source = *input;
    byte_length = strlen(source);
    if (byte_length == (size_t)-1) {
        errno = EOVERFLOW;
        return (size_t)-1;
    }
    if (measure) {
        saved = state ? *state : mbsrtowcs_internal;
        current = &saved;
    } else {
        current = select_state(state, &mbsrtowcs_internal);
    }
    while (offset <= byte_length) {
        wchar_t value;
        size_t result;

        if (!measure && converted == capacity) {
            *input = source + offset;
            return converted;
        }
        result = mbrtowc(&value, source + offset, byte_length + 1U - offset,
                         current);
        if (result == (size_t)-1 || result == (size_t)-2) {
            errno = EILSEQ;
            if (!measure) *input = source + offset;
            return (size_t)-1;
        }
        if (value == L'\0') {
            if (!measure) {
                output[converted] = L'\0';
                *input = NULL;
                mg_utf8_state_reset(current);
            }
            return converted;
        }
        if (converted == (size_t)-1) {
            errno = EOVERFLOW;
            if (!measure) *input = source + offset;
            return (size_t)-1;
        }
        if (!measure) output[converted] = value;
        converted++;
        offset += result;
    }
    errno = EILSEQ;
    if (!measure) *input = source + offset;
    return (size_t)-1;
}

size_t mbsrtowcs(wchar_t *restrict output, const char **restrict input,
                 size_t capacity, mbstate_t *restrict state)
{
    return mbsrtowcs_run(output, input, capacity, state, output == NULL);
}

static size_t wcsrtombs_run(char *restrict output,
                            const wchar_t **restrict input, size_t capacity,
                            mbstate_t *restrict state, bool measure)
{
    const wchar_t *source;
    size_t offset = 0, produced_total = 0;
    mbstate_t saved, *current;

    if (!input || !*input) {
        errno = EINVAL;
        return (size_t)-1;
    }
    source = *input;
    if (measure) {
        saved = state ? *state : wcsrtombs_internal;
        current = &saved;
    } else {
        current = select_state(state, &wcsrtombs_internal);
    }
    if (!mg_utf8_state_is_initial(current)) {
        mg_utf8_state_reset(current);
        errno = EILSEQ;
        if (!measure) *input = source;
        return (size_t)-1;
    }
    for (;;) {
        u8 encoded[4];
        usize count;
        u32 scalar;

        if (source[offset] < 0) {
            errno = EILSEQ;
            if (!measure) *input = source + offset;
            return (size_t)-1;
        }
        scalar = (u32)source[offset];
        if (mg_utf8_encode_one(scalar, encoded, sizeof(encoded), &count) !=
            MG_UTF8_OK) {
            errno = EILSEQ;
            if (!measure) *input = source + offset;
            return (size_t)-1;
        }
        if (scalar == 0U) {
            if (!measure && produced_total < capacity) {
                output[produced_total] = '\0';
                *input = NULL;
                mg_utf8_state_reset(current);
            } else if (!measure) {
                *input = source + offset;
            }
            return produced_total;
        }
        if (produced_total > (size_t)-1 - (size_t)count) {
            errno = EOVERFLOW;
            if (!measure) *input = source + offset;
            return (size_t)-1;
        }
        if (!measure && (size_t)count > capacity - produced_total) {
            *input = source + offset;
            return produced_total;
        }
        if (!measure) {
            for (size_t index = 0; index < count; index++)
                output[produced_total + index] = (char)encoded[index];
        }
        produced_total += (size_t)count;
        offset++;
    }
}

size_t wcsrtombs(char *restrict output, const wchar_t **restrict input,
                 size_t capacity, mbstate_t *restrict state)
{
    return wcsrtombs_run(output, input, capacity, state, output == NULL);
}

int mblen(const char *input, size_t length)
{
    wchar_t ignored;
    size_t result;

    if (!input) {
        mg_utf8_state_reset(&mblen_internal);
        return 0;
    }
    mg_utf8_state_reset(&mblen_internal);
    result = mbrtowc(&ignored, input, length, &mblen_internal);
    if (result == (size_t)-1 || result == (size_t)-2) return -1;
    return (int)result;
}

int mbtowc(wchar_t *restrict output, const char *restrict input,
           size_t length)
{
    size_t result;

    if (!input) {
        mg_utf8_state_reset(&mbtowc_internal);
        return 0;
    }
    mg_utf8_state_reset(&mbtowc_internal);
    result = mbrtowc(output, input, length, &mbtowc_internal);
    if (result == (size_t)-1 || result == (size_t)-2) return -1;
    return (int)result;
}

int wctomb(char *output, wchar_t value)
{
    size_t result;

    if (!output) {
        mg_utf8_state_reset(&wctomb_internal);
        return 0;
    }
    result = wcrtomb(output, value, &wctomb_internal);
    return result == (size_t)-1 ? -1 : (int)result;
}

size_t mbstowcs(wchar_t *restrict output, const char *restrict input,
                size_t capacity)
{
    const char *source = input;
    mbstate_t state = { 0 };
    if (!input) { errno = EINVAL; return (size_t)-1; }
    return mbsrtowcs(output, &source, capacity, &state);
}

size_t wcstombs(char *restrict output, const wchar_t *restrict input,
                size_t capacity)
{
    const wchar_t *source = input;
    mbstate_t state = { 0 };
    if (!input) { errno = EINVAL; return (size_t)-1; }
    return wcsrtombs(output, &source, capacity, &state);
}

wint_t btowc(int value)
{
    if (value == EOF) return WEOF;
    if (value < 0 || value > (int)UCHAR_MAX) return WEOF;
    return value <= 0x7f ? (wint_t)value : WEOF;
}

int wctob(wint_t value)
{
    if (value > 0x7fU) return EOF;
    return (int)value;
}

/* Mangrove fixes multibyte conversion to UTF-8; C-locale collation is
 * code-unit order. */
size_t wcslen(const wchar_t *text)
{
    const wchar_t *cursor = text;
    while (*cursor) cursor++;
    return (size_t)(cursor - text);
}

int wcscmp(const wchar_t *left, const wchar_t *right)
{
    while (*left && *left == *right) { left++; right++; }
    return *left < *right ? -1 : *left > *right ? 1 : 0;
}

int wcsncmp(const wchar_t *left, const wchar_t *right, size_t count)
{
    while (count && *left && *left == *right) {
        left++; right++; count--;
    }
    return count ? (*left < *right ? -1 : *left > *right ? 1 : 0) : 0;
}

wchar_t *wcscpy(wchar_t *restrict destination,
                const wchar_t *restrict source)
{
    wchar_t *start = destination;
    do { *destination++ = *source; } while (*source++);
    return start;
}

wchar_t *wcsncpy(wchar_t *restrict destination,
                 const wchar_t *restrict source, size_t count)
{
    wchar_t *start = destination;
    size_t index = 0;
    while (index < count && source[index]) {
        destination[index] = source[index];
        index++;
    }
    while (index < count) destination[index++] = L'\0';
    return start;
}

wchar_t *wcscat(wchar_t *restrict destination,
                const wchar_t *restrict source)
{
    wcscpy(destination + wcslen(destination), source);
    return destination;
}

wchar_t *wcsncat(wchar_t *restrict destination,
                 const wchar_t *restrict source, size_t count)
{
    wchar_t *end = destination + wcslen(destination);
    size_t index = 0;
    while (index < count && source[index]) {
        end[index] = source[index];
        index++;
    }
    end[index] = L'\0';
    return destination;
}

wchar_t *wcstok(wchar_t *restrict text, const wchar_t *restrict delimiters,
                wchar_t **restrict state)
{
    wchar_t *cursor = text ? text : *state;
    if (!cursor) return NULL;
    cursor += wcsspn(cursor, delimiters);
    if (!*cursor) {
        *state = NULL;
        return NULL;
    }
    wchar_t *token = cursor;
    cursor += wcscspn(cursor, delimiters);
    if (*cursor) *cursor++ = L'\0';
    *state = cursor;
    return token;
}

int wcscoll(const wchar_t *left, const wchar_t *right)
{
    return wcscmp(left, right);
}

size_t wcsxfrm(wchar_t *restrict destination,
               const wchar_t *restrict source, size_t capacity)
{
    size_t length = wcslen(source);
    if (capacity) {
        size_t copy = length < capacity - 1U ? length : capacity - 1U;
        for (size_t index = 0; index < copy; index++)
            destination[index] = source[index];
        destination[copy] = L'\0';
    }
    return length;
}

wchar_t *wcschr(const wchar_t *text, wchar_t value)
{
    do { if (*text == value) return (wchar_t *)(void *)text; } while (*text++);
    return NULL;
}

wchar_t *wcsrchr(const wchar_t *text, wchar_t value)
{
    const wchar_t *found = NULL;
    do { if (*text == value) found = text; } while (*text++);
    return (wchar_t *)(void *)found;
}

wchar_t *wcsstr(const wchar_t *text, const wchar_t *substring)
{
    size_t length = wcslen(substring);
    if (!length) return (wchar_t *)(void *)text;
    for (; *text; text++) if (wcsncmp(text, substring, length) == 0)
        return (wchar_t *)(void *)text;
    return NULL;
}

wchar_t *wcspbrk(const wchar_t *text, const wchar_t *accept)
{
    for (; *text; text++) if (wcschr(accept, *text))
        return (wchar_t *)(void *)text;
    return NULL;
}

size_t wcscspn(const wchar_t *text, const wchar_t *reject)
{
    const wchar_t *start = text;
    while (*text && !wcschr(reject, *text)) text++;
    return (size_t)(text - start);
}

size_t wcsspn(const wchar_t *text, const wchar_t *accept)
{
    const wchar_t *start = text;
    while (*text && wcschr(accept, *text)) text++;
    return (size_t)(text - start);
}

wchar_t *wmemchr(const wchar_t *text, wchar_t value, size_t count)
{
    for (size_t index = 0; index < count; index++)
        if (text[index] == value) return (wchar_t *)(void *)(text + index);
    return NULL;
}

int wmemcmp(const wchar_t *left, const wchar_t *right, size_t count)
{
    for (size_t index = 0; index < count; index++)
        if (left[index] != right[index])
            return left[index] < right[index] ? -1 : 1;
    return 0;
}

wchar_t *wmemcpy(wchar_t *restrict destination,
                 const wchar_t *restrict source, size_t count)
{
    for (size_t index = 0; index < count; index++)
        destination[index] = source[index];
    return destination;
}

wchar_t *wmemmove(wchar_t *destination, const wchar_t *source, size_t count)
{
    if ((uintptr_t)destination < (uintptr_t)source) {
        for (size_t index = 0; index < count; index++)
            destination[index] = source[index];
    } else if ((uintptr_t)destination > (uintptr_t)source) {
        while (count) { count--; destination[count] = source[count]; }
    }
    return destination;
}

wchar_t *wmemset(wchar_t *text, wchar_t value, size_t count)
{
    for (size_t index = 0; index < count; index++) text[index] = value;
    return text;
}
