#pragma once

#include <stddef.h>

size_t strlen(const char *str);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *str, int character);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, usize n);
char *strncat(char *dest, const char *src, usize n);

void *memset(void *dest, int value, size_t size);
void *memcpy(void *dest, const void *src, size_t size);
void *memmove(void *dest, const void *src, size_t size);
int memcmp(const void *left, const void *right, size_t size);
