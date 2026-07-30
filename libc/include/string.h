#pragma once

#include <types.h>

usize strlen(const char *str);
int strcmp(const char *s1, const char *s2);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, usize n);
char *strncat(char *dest, const char *src, usize n);

void *memset(void *dest, int value, usize size);
void *memcpy(void *dest, const void *src, usize size);
