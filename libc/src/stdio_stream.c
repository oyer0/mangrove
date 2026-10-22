/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <limits.h>
#include <mangrove.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MG_STREAM_COUNT FOPEN_MAX
#define MG_STREAM_MAGIC UINT32_C(0x4d474649)
#define MG_STREAM_STD_BUFFER 512U
#define MG_STREAM_READ 1
#define MG_STREAM_WRITE 2

struct __mg_FILE {
    uint32_t magic;
    mg_handle_t handle;
    unsigned char *buffer;
    size_t capacity;
    size_t length;
    size_t cursor;
    unsigned char standard_buffer[MG_STREAM_STD_BUFFER];
    unsigned char pushed;
    bool has_pushed;
    bool used;
    bool open;
    bool standard;
    bool owns_handle;
    bool owns_buffer;
    bool readable;
    bool writable;
    bool append;
    bool line_buffered;
    bool unbuffered;
    bool input_buffered;
    bool eof;
    bool error;
    bool poisoned;
    bool io_started;
    int last_operation;
};

static FILE stream_slots[MG_STREAM_COUNT];
FILE *stdin = &stream_slots[0];
FILE *stdout = &stream_slots[1];
FILE *stderr = &stream_slots[2];
static bool streams_initialized;

static void initialize_streams(void)
{
    if (streams_initialized) return;
    memset(stream_slots, 0, sizeof(stream_slots));

    FILE *input = stdin;
    input->magic = MG_STREAM_MAGIC;
    input->handle = MG_STDIN_HANDLE;
    input->used = input->open = input->standard = true;
    input->readable = true;
    input->unbuffered = true;

    FILE *output = stdout;
    output->magic = MG_STREAM_MAGIC;
    output->handle = MG_STDOUT_HANDLE;
    output->used = output->open = output->standard = true;
    output->writable = true;
    output->line_buffered = true;
    output->buffer = output->standard_buffer;
    output->capacity = sizeof(output->standard_buffer);

    FILE *error_stream = stderr;
    error_stream->magic = MG_STREAM_MAGIC;
    error_stream->handle = MG_STDOUT_HANDLE;
    error_stream->used = error_stream->open = error_stream->standard = true;
    error_stream->writable = true;
    error_stream->unbuffered = true;
    streams_initialized = true;
}

static bool valid_stream(const FILE *stream)
{
    initialize_streams();
    uintptr_t address = (uintptr_t)stream;
    uintptr_t first = (uintptr_t)&stream_slots[0];
    uintptr_t last = (uintptr_t)&stream_slots[MG_STREAM_COUNT];
    return stream && address >= first && address < last &&
           (address - first) % sizeof(FILE) == 0U &&
           stream->magic == MG_STREAM_MAGIC && stream->used && stream->open;
}

static bool standard_stream_slot(const FILE *stream)
{
    initialize_streams();
    return stream == stdin || stream == stdout || stream == stderr;
}

static int fail_result(mg_result_t result)
{
    errno = mg_error_to_errno(result);
    return EOF;
}

static int fail_errno(int error_number)
{
    errno = error_number;
    return EOF;
}

static void stream_error(FILE *stream, mg_result_t result)
{
    stream->error = true;
    errno = mg_error_to_errno(result);
}

static int write_direct(FILE *stream, const unsigned char *bytes,
                        size_t length, size_t *out_written)
{
    size_t written = 0;
    *out_written = 0;
    if (stream == stderr && stdout->open && stdout->length &&
        stdout != stream && fflush(stdout) == EOF)
        return EOF;

    while (written < length) {
        mg_result_t result = object_write(stream->handle, bytes + written,
                                          length - written);
        if (result > 0 && (u64)result <= (u64)(length - written)) {
            written += (size_t)result;
            continue;
        }
        if (result == MG_ERR_UNCERTAIN) stream->poisoned = true;
        if (result < 0) stream_error(stream, result);
        else stream_error(stream, MG_ERR_IO);
        *out_written = written;
        return EOF;
    }
    *out_written = written;
    return 0;
}

static int flush_output(FILE *stream)
{
    if (stream->poisoned) {
        stream->error = true;
        errno = EIO;
        return EOF;
    }
    while (stream->length) {
        size_t written = 0;
        if (write_direct(stream, stream->buffer, stream->length,
                         &written) == EOF) {
            if (written) {
                memmove(stream->buffer, stream->buffer + written,
                        stream->length - written);
                stream->length -= written;
            }
            return EOF;
        }
        stream->length = 0;
    }
    return 0;
}

static size_t input_unread(const FILE *stream)
{
    return (stream->input_buffered ? stream->length - stream->cursor : 0U) +
           (stream->has_pushed ? 1U : 0U);
}

static int reset_input_for_write(FILE *stream)
{
    size_t unread = input_unread(stream);
    if (unread) {
        if ((stream->standard && !stream->owns_handle) ||
            unread > (size_t)INT64_MAX ||
            file_seek(stream->handle, -(i64)unread, MG_SEEK_CUR) < 0) {
            stream_error(stream, stream->standard ? MG_ERR_UNSUPPORTED : MG_ERR_IO);
            return EOF;
        }
    }
    stream->length = stream->cursor = 0;
    stream->input_buffered = false;
    stream->has_pushed = false;
    return 0;
}

static int prepare_read(FILE *stream)
{
    if (!valid_stream(stream) || !stream->readable)
        return fail_errno(EBADF);
    if (stream == stdin && stdout->open && stdout->line_buffered &&
        stdout->length &&
        fflush(stdout) == EOF)
        return EOF;
    if (stream->last_operation == MG_STREAM_WRITE && flush_output(stream) == EOF)
        return EOF;
    if (stream->last_operation == MG_STREAM_WRITE) {
        stream->length = 0;
        stream->cursor = 0;
    }
    stream->last_operation = MG_STREAM_READ;
    stream->io_started = true;
    return 0;
}

static int prepare_write(FILE *stream)
{
    if (!valid_stream(stream) || !stream->writable)
        return fail_errno(EBADF);
    if (stream->poisoned) {
        stream->error = true;
        errno = EIO;
        return EOF;
    }
    if (stream->last_operation == MG_STREAM_READ &&
        reset_input_for_write(stream) == EOF)
        return EOF;
    stream->last_operation = MG_STREAM_WRITE;
    stream->input_buffered = false;
    stream->io_started = true;
    return 0;
}

static int fill_input(FILE *stream)
{
    mg_result_t result;
    stream->length = stream->cursor = 0;
    stream->input_buffered = true;
    result = object_read(stream->handle, stream->buffer, stream->capacity);
    if (result < 0) {
        stream->input_buffered = false;
        stream_error(stream, result);
        return EOF;
    }
    if ((u64)result > stream->capacity) {
        stream->input_buffered = false;
        stream_error(stream, MG_ERR_PROTOCOL);
        return EOF;
    }
    if (result == 0) {
        stream->eof = true;
        return EOF;
    }
    stream->length = (size_t)result;
    return 0;
}

int fgetc(FILE *stream)
{
    int result;
    if (prepare_read(stream) == EOF) return EOF;
    if (stream->has_pushed) {
        stream->has_pushed = false;
        return stream->pushed;
    }
    if (stream->unbuffered) {
        unsigned char byte;
        mg_result_t native = object_read(stream->handle, &byte, 1U);
        if (native < 0) { stream_error(stream, native); return EOF; }
        if (native == 0) { stream->eof = true; return EOF; }
        if (native != 1) { stream_error(stream, MG_ERR_PROTOCOL); return EOF; }
        return byte;
    }
    if (!stream->input_buffered || stream->cursor == stream->length) {
        result = fill_input(stream);
        if (result == EOF) return EOF;
    }
    return stream->buffer[stream->cursor++];
}

int getc(FILE *stream) { return fgetc(stream); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int character, FILE *stream)
{
    if (character == EOF || prepare_read(stream) == EOF || stream->has_pushed)
        return EOF;
    stream->pushed = (unsigned char)character;
    stream->has_pushed = true;
    stream->eof = false;
    return stream->pushed;
}

size_t fread(void *restrict buffer, size_t size, size_t count,
             FILE *restrict stream)
{
    size_t wanted, copied = 0;
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) {
        errno = EOVERFLOW;
        if (valid_stream(stream)) stream->error = true;
        return 0;
    }
    wanted = size * count;
    if (!buffer || prepare_read(stream) == EOF) {
        if (!buffer) { errno = EINVAL; if (valid_stream(stream)) stream->error = true; }
        return 0;
    }
    unsigned char *output = buffer;
    while (copied < wanted) {
        if (stream->has_pushed) {
            output[copied++] = stream->pushed;
            stream->has_pushed = false;
            continue;
        }
        if (stream->unbuffered) {
            mg_result_t result = object_read(stream->handle, output + copied,
                                             wanted - copied);
            if (result < 0) { stream_error(stream, result); break; }
            if (result == 0) { stream->eof = true; break; }
            if ((u64)result > (u64)(wanted - copied)) {
                stream_error(stream, MG_ERR_PROTOCOL); break;
            }
            copied += (size_t)result;
            continue;
        }
        if (!stream->input_buffered || stream->cursor == stream->length) {
            if (fill_input(stream) == EOF) break;
        }
        size_t available = stream->length - stream->cursor;
        size_t chunk = wanted - copied < available ? wanted - copied : available;
        memcpy(output + copied, stream->buffer + stream->cursor, chunk);
        stream->cursor += chunk;
        copied += chunk;
    }
    return copied / size;
}

static size_t write_buffered(FILE *stream, const unsigned char *source,
                             size_t length)
{
    size_t accepted = 0;
    while (accepted < length) {
        if (stream->length == stream->capacity && flush_output(stream) == EOF)
            break;
        size_t room = stream->capacity - stream->length;
        size_t chunk = length - accepted < room ? length - accepted : room;
        memcpy(stream->buffer + stream->length, source + accepted, chunk);
        stream->length += chunk;
        accepted += chunk;
        if (stream->line_buffered) {
            bool newline = false;
            for (size_t i = 0; i < chunk; i++) newline |= source[accepted - chunk + i] == '\n';
            if (newline && flush_output(stream) == EOF) break;
        }
    }
    return accepted;
}

size_t fwrite(const void *restrict buffer, size_t size, size_t count,
              FILE *restrict stream)
{
    size_t wanted, written = 0;
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) {
        errno = EOVERFLOW;
        if (valid_stream(stream)) stream->error = true;
        return 0;
    }
    wanted = size * count;
    if (!buffer || prepare_write(stream) == EOF) {
        if (!buffer) { errno = EINVAL; if (valid_stream(stream)) stream->error = true; }
        return 0;
    }
    const unsigned char *input = buffer;
    if (stream->unbuffered) {
        if (write_direct(stream, input, wanted, &written) == EOF)
            return written / size;
        return count;
    }
    if (stream->capacity == 0) {
        stream->error = true;
        errno = EINVAL;
        return 0;
    }
    while (written < wanted) {
        size_t remaining = wanted - written;
        if (!stream->length && !stream->line_buffered &&
            remaining >= stream->capacity) {
            size_t direct = 0;
            if (write_direct(stream, input + written, remaining, &direct) == EOF) {
                written += direct;
                break;
            }
            written += direct;
            continue;
        }
        size_t accepted = write_buffered(stream, input + written, remaining);
        written += accepted;
        if (accepted < remaining) break;
    }
    return written / size;
}

int fputc(int character, FILE *stream)
{
    unsigned char byte = (unsigned char)character;
    bool prior_error = valid_stream(stream) && stream->error;
    if (fwrite(&byte, 1U, 1U, stream) != 1U ||
        (!prior_error && valid_stream(stream) && stream->error)) return EOF;
    return byte;
}

int putc(int character, FILE *stream) { return fputc(character, stream); }
int putchar(int character) { return fputc(character, stdout); }

char *fgets(char *restrict buffer, int size, FILE *restrict stream)
{
    if (!buffer || size <= 0 || !valid_stream(stream)) {
        errno = EINVAL;
        if (valid_stream(stream)) stream->error = true;
        return NULL;
    }
    if (size == 1) { buffer[0] = '\0'; return buffer; }
    int used = 0;
    while (used < size - 1) {
        int character = fgetc(stream);
        if (character == EOF) break;
        buffer[used++] = (char)character;
        if (character == '\n') break;
    }
    if (!used) return NULL;
    buffer[used] = '\0';
    return buffer;
}

int fputs(const char *restrict string, FILE *restrict stream)
{
    if (!string) return fail_errno(EINVAL);
    bool prior_error = valid_stream(stream) && stream->error;
    size_t length = strlen(string);
    return fwrite(string, 1U, length, stream) == length &&
           (prior_error || !valid_stream(stream) || !stream->error) ? 0 : EOF;
}

int puts(const char *string)
{
    if (!string) return fail_errno(EINVAL);
    size_t length = strlen(string);
    if (fwrite(string, 1U, length, stdout) != length ||
        fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

int fflush(FILE *stream)
{
    initialize_streams();
    if (!stream) {
        int result = 0;
        for (size_t i = 0; i < MG_STREAM_COUNT; i++) {
            FILE *current = &stream_slots[i];
            if (current->used && current->open && current->writable &&
                current->last_operation == MG_STREAM_WRITE &&
                flush_output(current) == EOF) result = EOF;
        }
        return result;
    }
    if (!valid_stream(stream)) return fail_errno(EBADF);
    if (!stream->writable || stream->last_operation != MG_STREAM_WRITE) return 0;
    return flush_output(stream);
}

int feof(FILE *stream)
{
    if (!valid_stream(stream)) { errno = EBADF; return 0; }
    return stream->eof;
}

int ferror(FILE *stream)
{
    if (!valid_stream(stream)) { errno = EBADF; return 0; }
    return stream->error;
}

void clearerr(FILE *stream)
{
    if (!valid_stream(stream)) { errno = EBADF; return; }
    stream->eof = false;
    stream->error = false;
}

static bool parse_mode(const char *mode, bool *readable, bool *writable,
                       bool *truncate, bool *append, bool *exclusive)
{
    if (!mode || !mode[0]) return false;
    char first = mode[0];
    if (first != 'r' && first != 'w' && first != 'a') return false;
    bool plus = false, binary = false, xflag = false;
    for (size_t i = 1; mode[i]; i++) {
        if (mode[i] == '+' && !plus) plus = true;
        else if (mode[i] == 'b' && !binary) binary = true;
        else if (mode[i] == 'x' && !xflag) xflag = true;
        else return false;
    }
    (void)binary;
    if (xflag && first != 'w') return false;
    *readable = first == 'r' || plus;
    *writable = first != 'r' || plus;
    *truncate = first == 'w';
    *append = first == 'a';
    *exclusive = xflag;
    return true;
}

static mg_result_t release_stream(FILE *stream, bool close_handle)
{
    mg_result_t close_result = MG_OK;
    if (close_handle && stream->owns_handle)
        close_result = handle_close(stream->handle);
    if (stream->owns_buffer) free(stream->buffer);
    bool standard = stream->standard;
    memset(stream, 0, sizeof(*stream));
    if (standard) {
        stream->magic = MG_STREAM_MAGIC;
        stream->standard = stream->used = true;
        stream->open = false;
    }
    return close_result;
}

static FILE *reserve_stream(void)
{
    initialize_streams();
    for (size_t i = 3; i < MG_STREAM_COUNT; i++) {
        if (!stream_slots[i].used) {
            FILE *stream = &stream_slots[i];
            memset(stream, 0, sizeof(*stream));
            stream->magic = MG_STREAM_MAGIC;
            stream->used = true;
            return stream;
        }
    }
    errno = EMFILE;
    return NULL;
}

static int open_stream(FILE *stream, const char *path, const char *mode)
{
    bool readable, writable, truncate, append, exclusive;
    mg_result_t result;
    u32 flags;
    if (!path || !parse_mode(mode, &readable, &writable, &truncate,
                            &append, &exclusive)) return fail_errno(EINVAL);
    flags = (readable ? MG_OPEN_READ : 0U) |
            (writable ? MG_OPEN_WRITE : 0U) |
            (append ? MG_OPEN_APPEND : 0U);
    stream->readable = readable;
    stream->writable = writable;
    stream->append = append;
    stream->owns_handle = true;
    stream->unbuffered = false;
    stream->line_buffered = false;
    stream->length = stream->cursor = 0;
    stream->eof = stream->error = stream->poisoned = false;
    stream->io_started = false;
    stream->last_operation = 0;
    stream->buffer = malloc(BUFSIZ);
    if (!stream->buffer) return fail_errno(ENOMEM);
    stream->owns_buffer = true;
    stream->capacity = BUFSIZ;

    if (exclusive) {
        result = file_create_stream(path, NULL, 0U, flags, &stream->handle);
    } else {
        result = file_open(path, flags);
        if (result >= 0) stream->handle = (mg_handle_t)result;
        if (result == MG_ERR_NOT_FOUND && (truncate || append)) {
            result = file_create_stream(path, NULL, 0U, flags,
                                        &stream->handle);
            if (result == MG_ERR_ALREADY_EXISTS) {
                result = file_open(path, flags);
                if (result >= 0) stream->handle = (mg_handle_t)result;
            }
        }
        if (result >= 0 && truncate) {
            result = file_truncate(stream->handle);
            if (result < 0) {
                (void)handle_close(stream->handle);
                stream->owns_handle = false;
            }
        }
    }
    if (result < 0) {
        stream->open = false;
        release_stream(stream, false);
        return fail_result(result);
    }
    stream->open = true;
    if (append && !readable) {
        mg_result_t seek_error = file_seek(stream->handle, 0, MG_SEEK_END);
        if (seek_error >= 0) return 0;
        (void)handle_close(stream->handle);
        stream->owns_handle = false;
        stream->open = false;
        release_stream(stream, false);
        return fail_result(seek_error);
    }
    return 0;
}

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    FILE *stream = reserve_stream();
    if (!stream) return NULL;
    if (open_stream(stream, path, mode) < 0) {
        (void)release_stream(stream, false);
        return NULL;
    }
    return stream;
}

int fclose(FILE *stream)
{
    if (!valid_stream(stream)) return fail_errno(EBADF);
    int result = stream->writable &&
                 stream->last_operation == MG_STREAM_WRITE
        ? flush_output(stream) : 0;
    bool standard = stream->standard;
    mg_result_t close_result = release_stream(stream, true);
    if (result == 0 && close_result < 0) {
        errno = mg_error_to_errno(close_result);
        result = EOF;
    }
    if (standard) {
        stream->magic = MG_STREAM_MAGIC;
        stream->used = stream->standard = true;
        stream->open = false;
    }
    return result;
}

FILE *freopen(const char *restrict path, const char *restrict mode,
              FILE *restrict stream)
{
    if (!standard_stream_slot(stream) && !valid_stream(stream)) {
        errno = EBADF; return NULL;
    }
    if (standard_stream_slot(stream) && !stream->open) {
        errno = EBADF; return NULL;
    }
    bool standard = stream->standard;
    if (stream->writable && stream->last_operation == MG_STREAM_WRITE &&
        flush_output(stream) == EOF) {
        release_stream(stream, true);
        if (standard) {
            stream->magic = MG_STREAM_MAGIC;
            stream->used = stream->standard = true;
            stream->open = false;
        }
        return NULL;
    }
    mg_result_t close_result = release_stream(stream, true);
    if (close_result < 0) {
        errno = mg_error_to_errno(close_result);
        if (standard) {
            stream->magic = MG_STREAM_MAGIC;
            stream->used = stream->standard = true;
            stream->open = false;
        }
        return NULL;
    }
    if (standard) {
        stream->magic = MG_STREAM_MAGIC;
        stream->used = stream->standard = true;
    } else {
        stream->magic = MG_STREAM_MAGIC;
        stream->used = true;
    }
    if (open_stream(stream, path, mode) < 0) {
        if (standard) {
            stream->magic = MG_STREAM_MAGIC;
            stream->used = stream->standard = true;
            stream->open = false;
        }
        return NULL;
    }
    stream->standard = standard;
    return stream;
}

int setvbuf(FILE *restrict stream, char *restrict buffer, int mode, size_t size)
{
    if (!valid_stream(stream) || stream->io_started ||
        (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) ||
        (mode != _IONBF && size == 0U)) {
        errno = EINVAL;
        return -1;
    }
    unsigned char *replacement = (unsigned char *)buffer;
    bool owns = false;
    if (mode != _IONBF && !replacement) {
        replacement = malloc(size);
        if (!replacement) { errno = ENOMEM; return -1; }
        owns = true;
    }
    if (stream->owns_buffer) free(stream->buffer);
    stream->buffer = mode == _IONBF ? NULL : replacement;
    stream->capacity = mode == _IONBF ? 0U : size;
    stream->owns_buffer = owns;
    stream->unbuffered = mode == _IONBF;
    stream->line_buffered = mode == _IOLBF;
    stream->length = stream->cursor = 0;
    stream->input_buffered = false;
    return 0;
}

void setbuf(FILE *restrict stream, char *restrict buffer)
{
    (void)setvbuf(stream, buffer, buffer ? _IOFBF : _IONBF,
                  buffer ? BUFSIZ : 0U);
}

int fseek(FILE *stream, long offset, int origin)
{
    if (!valid_stream(stream)) return fail_errno(EBADF);
    if (stream->standard && !stream->owns_handle) return fail_errno(ESPIPE);
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)
        return fail_errno(EINVAL);
    if (stream->writable && stream->last_operation == MG_STREAM_WRITE &&
        flush_output(stream) == EOF) return EOF;
    i64 adjusted = (i64)offset;
    if (origin == SEEK_CUR && stream->last_operation == MG_STREAM_READ) {
        size_t unread = input_unread(stream);
        if (unread > (size_t)INT64_MAX || adjusted < INT64_MIN + (i64)unread)
            return fail_errno(EOVERFLOW);
        adjusted -= (i64)unread;
    }
    mg_seek_whence_t whence = origin == SEEK_SET ? MG_SEEK_SET :
                              origin == SEEK_CUR ? MG_SEEK_CUR : MG_SEEK_END;
    mg_result_t result = file_seek(stream->handle, adjusted, whence);
    if (result < 0) { stream_error(stream, result); return EOF; }
    u64 position;
    result = file_position(stream->handle, &position);
    if (result < 0) { stream_error(stream, result); return EOF; }
    if (position > (u64)LONG_MAX) return fail_errno(EOVERFLOW);
    stream->length = stream->cursor = 0;
    stream->input_buffered = false;
    stream->has_pushed = false;
    stream->eof = false;
    stream->last_operation = 0;
    return 0;
}

long ftell(FILE *stream)
{
    if (!valid_stream(stream)) { errno = EBADF; return -1L; }
    if (stream->standard) { errno = ESPIPE; return -1L; }
    if (stream->writable && stream->last_operation == MG_STREAM_WRITE &&
        flush_output(stream) == EOF) return -1L;
    u64 position;
    mg_result_t result = file_position(stream->handle, &position);
    if (result < 0) { errno = mg_error_to_errno(result); return -1L; }
    if (stream->last_operation == MG_STREAM_READ) {
        size_t unread = input_unread(stream);
        if (position < unread) { errno = EIO; return -1L; }
        position -= unread;
    }
    if (position > (u64)LONG_MAX) { errno = EOVERFLOW; return -1L; }
    return (long)position;
}

void rewind(FILE *stream)
{
    if (fseek(stream, 0L, SEEK_SET) == 0) clearerr(stream);
    else clearerr(stream);
}

int fgetpos(FILE *restrict stream, fpos_t *restrict position)
{
    if (!position) return fail_errno(EINVAL);
    long current = ftell(stream);
    if (current < 0) return EOF;
    *position = (fpos_t)current;
    return 0;
}

int fsetpos(FILE *stream, const fpos_t *position)
{
    if (!position || *position < 0) return fail_errno(EINVAL);
    return fseek(stream, *position, SEEK_SET);
}

int remove(const char *path)
{
    mg_result_t result = path_remove(path);
    return result < 0 ? fail_result(result) : 0;
}

int rename(const char *old_path, const char *new_path)
{
    mg_result_t result = path_move(old_path, new_path);
    /* path_move deliberately preserves an existing destination identity. */
    return result < 0 ? fail_result(result) : 0;
}

FILE *tmpfile(void)
{
    errno = ENOSYS;
    return NULL;
}

char *tmpnam(char *buffer)
{
    (void)buffer;
    errno = ENOSYS;
    return NULL;
}

void __stdio_exit_flush(void)
{
    (void)fflush(NULL);
}
