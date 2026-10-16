/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0
#define RAND_MAX 32767

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
void abort(void) __attribute__((noreturn));
int atexit(void (*function)(void));
int at_quick_exit(void (*function)(void));
void exit(int status) __attribute__((noreturn));
void quick_exit(int status) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
int abs(int value);
long labs(long value);
long long llabs(long long value);
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);
char *getenv(const char *name);
int system(const char *command);
int rand(void);
void srand(unsigned int seed);
long strtol(const char *restrict text, char **restrict end, int base);
unsigned long strtoul(const char *restrict text, char **restrict end, int base);
long long strtoll(const char *restrict text, char **restrict end, int base);
unsigned long long strtoull(const char *restrict text, char **restrict end,
                           int base);
int atoi(const char *text);
long atol(const char *text);
long long atoll(const char *text);
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));
