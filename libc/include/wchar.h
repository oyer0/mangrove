/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <mg/utf8.h>

typedef __WINT_TYPE__ wint_t;
typedef mg_utf8_state_t mbstate_t;

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

size_t mbrlen(const char *restrict input, size_t length,
              mbstate_t *restrict state);
size_t mbrtowc(wchar_t *restrict output, const char *restrict input,
               size_t length, mbstate_t *restrict state);
size_t wcrtomb(char *restrict output, wchar_t value,
               mbstate_t *restrict state);
size_t mbsrtowcs(wchar_t *restrict output, const char **restrict input,
                 size_t capacity, mbstate_t *restrict state);
size_t wcsrtombs(char *restrict output, const wchar_t **restrict input,
                 size_t capacity, mbstate_t *restrict state);

int mblen(const char *input, size_t length);
int mbtowc(wchar_t *restrict output, const char *restrict input,
           size_t length);
int wctomb(char *output, wchar_t value);
size_t mbstowcs(wchar_t *restrict output, const char *restrict input,
                size_t capacity);
size_t wcstombs(char *restrict output, const wchar_t *restrict input,
                size_t capacity);
wint_t btowc(int value);
int wctob(wint_t value);
int mbsinit(const mbstate_t *state);

wchar_t *wcscpy(wchar_t *restrict destination,
                const wchar_t *restrict source);
wchar_t *wcsncpy(wchar_t *restrict destination,
                 const wchar_t *restrict source, size_t count);
wchar_t *wcscat(wchar_t *restrict destination,
                const wchar_t *restrict source);
wchar_t *wcsncat(wchar_t *restrict destination,
                 const wchar_t *restrict source, size_t count);
wchar_t *wcstok(wchar_t *restrict text, const wchar_t *restrict delimiters,
                wchar_t **restrict state);
int wcscmp(const wchar_t *left, const wchar_t *right);
int wcsncmp(const wchar_t *left, const wchar_t *right, size_t count);
int wcscoll(const wchar_t *left, const wchar_t *right);
size_t wcsxfrm(wchar_t *restrict destination,
               const wchar_t *restrict source, size_t capacity);
size_t wcslen(const wchar_t *text);
wchar_t *wcschr(const wchar_t *text, wchar_t value);
wchar_t *wcsrchr(const wchar_t *text, wchar_t value);
wchar_t *wcsstr(const wchar_t *text, const wchar_t *substring);
wchar_t *wcspbrk(const wchar_t *text, const wchar_t *accept);
size_t wcscspn(const wchar_t *text, const wchar_t *reject);
size_t wcsspn(const wchar_t *text, const wchar_t *accept);
wchar_t *wmemchr(const wchar_t *text, wchar_t value, size_t count);
int wmemcmp(const wchar_t *left, const wchar_t *right, size_t count);
wchar_t *wmemcpy(wchar_t *restrict destination,
                 const wchar_t *restrict source, size_t count);
wchar_t *wmemmove(wchar_t *destination, const wchar_t *source, size_t count);
wchar_t *wmemset(wchar_t *text, wchar_t value, size_t count);
