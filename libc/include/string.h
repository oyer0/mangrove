/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

size_t strlen(const char *str);
char *strtok(char *restrict str, const char *restrict delimiters);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *str, int character);
char *strrchr(const char *str, int character);
char *strstr(const char *str, const char *substring);
char *strpbrk(const char *str, const char *accept);
size_t strspn(const char *str, const char *accept);
size_t strcspn(const char *str, const char *reject);
char *strcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strncat(char *dest, const char *src, size_t n);
int strcoll(const char *left, const char *right);
size_t strxfrm(char *restrict dest, const char *restrict src, size_t n);

void *memchr(const void *ptr, int value, size_t size);
void *memset(void *dest, int value, size_t size);
void *memcpy(void *dest, const void *src, size_t size);
void *memmove(void *dest, const void *src, size_t size);
int memcmp(const void *left, const void *right, size_t size);
char *strerror(int error_number);
