/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define EOF (-1)
#define BUFSIZ 4096
#define FOPEN_MAX 16
#define FILENAME_MAX 256
#define L_tmpnam 32
#define TMP_MAX 0x7fffffff
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct __mg_FILE FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *restrict path, const char *restrict mode);
FILE *freopen(const char *restrict path, const char *restrict mode,
              FILE *restrict stream);
int fclose(FILE *stream);
int fflush(FILE *stream);
size_t fread(void *restrict buffer, size_t size, size_t count,
             FILE *restrict stream);
size_t fwrite(const void *restrict buffer, size_t size, size_t count,
              FILE *restrict stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
int putchar(int character);
char *fgets(char *restrict buffer, int size, FILE *restrict stream);
int fputs(const char *restrict string, FILE *restrict stream);
int puts(const char *string);
int ungetc(int character, FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int setvbuf(FILE *restrict stream, char *restrict buffer, int mode,
            size_t size);
void setbuf(FILE *restrict stream, char *restrict buffer);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *restrict stream, fpos_t *restrict position);
int fsetpos(FILE *stream, const fpos_t *position);
int remove(const char *path);
int rename(const char *old_path, const char *new_path);
void perror(const char *message);
FILE *tmpfile(void);
char *tmpnam(char *buffer);

int vfprintf(FILE *restrict stream, const char *restrict format, va_list arguments);
int fprintf(FILE *restrict stream, const char *restrict format, ...);
int printf(const char *restrict format, ...);
int vprintf(const char *restrict format, va_list arguments);
int vsnprintf(char *restrict buffer, size_t size,
              const char *restrict format, va_list arguments);
int snprintf(char *restrict buffer, size_t size,
             const char *restrict format, ...);
int vsprintf(char *restrict buffer, const char *restrict format,
             va_list arguments);
int sprintf(char *restrict buffer, const char *restrict format, ...);

int vfscanf(FILE *restrict stream, const char *restrict format, va_list arguments);
int fscanf(FILE *restrict stream, const char *restrict format, ...);
int scanf(const char *restrict format, ...);
int vscanf(const char *restrict format, va_list arguments);
int vsscanf(const char *restrict input, const char *restrict format,
            va_list arguments);
int sscanf(const char *restrict input, const char *restrict format, ...);

/* Internal normal-exit hook. Applications should use exit(), not call it. */
void __stdio_exit_flush(void);
