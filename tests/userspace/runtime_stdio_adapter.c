/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/filesystem.h>
#include <mg/object.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FILES 8U
#define TEST_HANDLES 16U
#define TEST_CAPACITY 65536U

typedef struct test_file {
    bool used;
    char path[256];
    unsigned char bytes[TEST_CAPACITY];
    usize length;
} test_file_t;

typedef struct test_handle {
    bool used;
    test_file_t *file;
    usize position;
    u32 flags;
} test_handle_t;

static test_file_t files[TEST_FILES];
static test_handle_t handles[TEST_HANDLES];
static unsigned char console_output[8192];
static usize console_output_length;
static const unsigned char default_console_input[] = "input line\n";
static const unsigned char *console_input = default_console_input;
static usize console_input_length = sizeof(default_console_input) - 1U;
static usize console_input_position;
static usize console_input_boundaries[16];
static bool console_input_secure[16];
static usize console_input_boundary_count;
static bool console_secure_input;
static int write_mode = 1;
static bool fault_sent;
static int read_error;
static usize read_limit;
static long allocation_countdown = -1;

void test_fail_allocation_after(size_t successful_allocations)
{
    allocation_countdown = (long)successful_allocations;
}

void test_clear_allocation_failure(void)
{
    allocation_countdown = -1;
}

static bool allocation_should_fail(void)
{
    if (allocation_countdown < 0) return false;
    if (allocation_countdown == 0) {
        allocation_countdown = -1;
        return true;
    }
    allocation_countdown--;
    return false;
}

void *test_malloc(size_t size)
{
    return allocation_should_fail() ? NULL : malloc(size);
}

void *test_realloc(void *pointer, size_t size)
{
    return allocation_should_fail() ? NULL : realloc(pointer, size);
}

void test_free(void *pointer)
{
    free(pointer);
}

static mg_handle_t reserve_handle(test_file_t *file, u32 flags)
{
    for (usize i = 0; i < TEST_HANDLES; i++) {
        if (!handles[i].used) {
            handles[i] = (test_handle_t){true, file, 0, flags};
            return (mg_handle_t)(i + 1U);
        }
    }
    return 0;
}

static test_handle_t *lookup(mg_handle_t handle)
{
    if (!handle || handle > TEST_HANDLES || !handles[handle - 1U].used) return NULL;
    return &handles[handle - 1U];
}

static test_file_t *find_file(const char *path)
{
    for (usize i = 0; i < TEST_FILES; i++)
        if (files[i].used && !strcmp(files[i].path, path)) return &files[i];
    return NULL;
}

static test_file_t *create_file(const char *path)
{
    if (find_file(path)) return NULL;
    for (usize i = 0; i < TEST_FILES; i++) {
        if (!files[i].used) {
            files[i].used = true;
            strncpy(files[i].path, path, sizeof(files[i].path) - 1U);
            return &files[i];
        }
    }
    return NULL;
}

mg_result_t file_open(const char *path, u32 flags)
{
    test_file_t *file = find_file(path);
    if (!file) return MG_ERR_NOT_FOUND;
    mg_handle_t handle = reserve_handle(file, flags);
    return handle ? (mg_result_t)handle : MG_ERR_NO_MEMORY;
}

mg_result_t file_create_stream(const char *path, const void *data, usize length,
                               u32 flags, mg_handle_t *out_handle)
{
    (void)data;
    if (!path || !out_handle || length) return MG_ERR_BAD_ARGUMENT;
    if (find_file(path)) return MG_ERR_ALREADY_EXISTS;
    test_file_t *file = create_file(path);
    if (!file) return MG_ERR_NO_MEMORY;
    mg_handle_t handle = reserve_handle(file, flags);
    if (!handle) { file->used = false; return MG_ERR_NO_MEMORY; }
    *out_handle = handle;
    return MG_OK;
}

mg_result_t file_truncate(mg_handle_t handle)
{
    test_handle_t *item = lookup(handle);
    if (!item || !(item->flags & MG_OPEN_WRITE)) return MG_ERR_INVALID_HANDLE;
    item->file->length = item->position = 0;
    return MG_OK;
}

mg_result_t file_seek(mg_handle_t handle, i64 offset, mg_seek_whence_t whence)
{
    test_handle_t *item = lookup(handle);
    if (!item) return MG_ERR_INVALID_HANDLE;
    i64 base = whence == MG_SEEK_SET ? 0 : whence == MG_SEEK_CUR ?
               (i64)item->position : (i64)item->file->length;
    if (offset < -base || offset > (i64)TEST_CAPACITY - base)
        return MG_ERR_BAD_ARGUMENT;
    item->position = (usize)(base + offset);
    return MG_OK;
}

mg_result_t file_position(mg_handle_t handle, u64 *out)
{
    test_handle_t *item = lookup(handle);
    if (!item || !out) return MG_ERR_INVALID_HANDLE;
    *out = item->position;
    return MG_OK;
}

mg_result_t handle_close(mg_handle_t handle)
{
    test_handle_t *item = lookup(handle);
    if (!item) return MG_ERR_INVALID_HANDLE;
    memset(item, 0, sizeof(*item));
    return MG_OK;
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    if (handle == MG_STDIN_HANDLE) {
        if (console_input_position < console_input_length &&
            (console_input_position == 0U ||
             console_input[console_input_position - 1U] == '\n') &&
            console_input_boundary_count <
                sizeof(console_input_boundaries) /
                sizeof(console_input_boundaries[0])) {
            usize boundary = console_input_boundary_count++;
            console_input_boundaries[boundary] = console_output_length;
            console_input_secure[boundary] = console_secure_input;
        }
        usize left = console_input_length - console_input_position;
        usize count = length < left ? length : left;
        memcpy(buffer, console_input + console_input_position, count);
        console_input_position += count;
        return (mg_result_t)count;
    }
    test_handle_t *item = lookup(handle);
    if (!item || !(item->flags & MG_OPEN_READ)) return MG_ERR_INVALID_HANDLE;
    if (read_error) return (mg_result_t)read_error;
    usize left = item->position < item->file->length
        ? item->file->length - item->position : 0;
    usize count = length < left ? length : left;
    if (read_limit && count > read_limit) count = read_limit;
    memcpy(buffer, item->file->bytes + item->position, count);
    item->position += count;
    return (mg_result_t)count;
}

mg_result_t object_write(mg_handle_t handle, const void *buffer, usize length)
{
    if (handle == MG_STDOUT_HANDLE) {
        if (write_mode == -1) return MG_ERR_IO;
        if (write_mode == 0) return 0;
        if (length > sizeof(console_output) - console_output_length) return MG_ERR_IO;
        memcpy(console_output + console_output_length, buffer, length);
        console_output_length += length;
        if (console_output_length < sizeof(console_output))
            console_output[console_output_length] = '\0';
        return (mg_result_t)length;
    }
    test_handle_t *item = lookup(handle);
    if (!item || !(item->flags & MG_OPEN_WRITE)) return MG_ERR_INVALID_HANDLE;
    if (write_mode == -1) return MG_ERR_IO;
    if (write_mode == 2 && fault_sent) return MG_ERR_UNCERTAIN;
    usize count = length;
    if (write_mode == 2 && count > 2U) { count = 2U; fault_sent = true; }
    if (item->flags & MG_OPEN_APPEND) item->position = item->file->length;
    if (count > TEST_CAPACITY - item->position) return MG_ERR_NO_SPACE;
    memcpy(item->file->bytes + item->position, buffer, count);
    item->position += count;
    if (item->position > item->file->length) item->file->length = item->position;
    return (mg_result_t)count;
}

mg_result_t path_move(const char *source, const char *destination)
{
    if (find_file(destination)) return MG_ERR_ALREADY_EXISTS;
    test_file_t *file = find_file(source);
    if (!file) return MG_ERR_NOT_FOUND;
    strncpy(file->path, destination, sizeof(file->path) - 1U);
    return MG_OK;
}

mg_result_t path_remove(const char *path)
{
    test_file_t *file = find_file(path);
    if (!file) return MG_ERR_NOT_FOUND;
    file->used = false;
    return MG_OK;
}

void test_write_mode(int value)
{
    write_mode = value;
    fault_sent = false;
}

void test_read_error(int value) { read_error = value; }
void test_read_limit(size_t value) { read_limit = value; }

const char *test_console_output(size_t *length)
{
    if (length) *length = console_output_length;
    return (const char *)console_output;
}

void test_console_input_reset(const void *input, size_t length)
{
    console_input = (const unsigned char *)input;
    console_input_length = length;
    console_input_position = 0;
    console_output_length = 0;
    console_output[0] = '\0';
    console_input_boundary_count = 0;
    console_secure_input = false;
    memset(console_input_boundaries, 0, sizeof(console_input_boundaries));
    memset(console_input_secure, 0, sizeof(console_input_secure));
}

size_t test_console_input_boundary_count(void)
{
    return console_input_boundary_count;
}

size_t test_console_input_boundary_output_length(size_t boundary)
{
    return boundary < console_input_boundary_count ?
           console_input_boundaries[boundary] : SIZE_MAX;
}

bool test_console_input_boundary_secure(size_t boundary)
{
    return boundary < console_input_boundary_count &&
           console_input_secure[boundary];
}

bool test_console_secure_input(void) { return console_secure_input; }

mg_result_t console_end_transaction(void) { return MG_OK; }

mg_result_t console_set_secure_input(bool secure)
{
    console_secure_input = secure;
    return MG_OK;
}
