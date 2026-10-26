/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../userspace/fetch/fetch_sink_buffer.h"

typedef struct write_capture {
    u8 *bytes;
    usize capacity;
    usize length;
    usize calls;
    usize largest;
    usize fail_call;
} write_capture_t;

static mg_result_t capture_write(void *opaque, const u8 *bytes, usize length)
{
    write_capture_t *capture = (write_capture_t *)opaque;
    if (!capture || (!bytes && length)) return MG_ERR_BAD_ARGUMENT;
    capture->calls++;
    if (capture->fail_call && capture->calls >= capture->fail_call)
        return MG_ERR_IO;
    if (length > capture->capacity - capture->length)
        return MG_ERR_NO_SPACE;
    memcpy(capture->bytes + capture->length, bytes, length);
    capture->length += length;
    if (length > capture->largest) capture->largest = length;
    return MG_OK;
}

static void test_fragment_coalescing_and_large_stream(void)
{
    const usize source_length = 3U * 1024U * 1024U + 177U;
    u8 *source = (u8 *)malloc(source_length);
    u8 *destination = (u8 *)malloc(source_length);
    assert(source && destination);
    for (usize index = 0; index < source_length; index++)
        source[index] = (u8)(index * 31U + index / 257U);
    write_capture_t capture = {.bytes = destination,
        .capacity = source_length};
    fetch_sink_buffer_t buffer;
    fetch_sink_buffer_init(&buffer, capture_write, &capture);

    for (usize offset = 0; offset < 31743U;) {
        usize amount = 67U;
        if (amount > 31743U - offset) amount = 31743U - offset;
        assert(fetch_sink_buffer_append(&buffer, source + offset, amount) ==
               MG_OK);
        offset += amount;
    }
    assert(capture.calls == 31743U / FETCH_SINK_BUFFER_CAPACITY);
    assert(buffer.length == 31743U % FETCH_SINK_BUFFER_CAPACITY);
    assert(fetch_sink_buffer_flush(&buffer) == MG_OK);
    assert(capture.calls ==
               (31743U + FETCH_SINK_BUFFER_CAPACITY - 1U) /
                   FETCH_SINK_BUFFER_CAPACITY &&
           capture.length == 31743U);

    usize offset = 31743U;
    while (offset < source_length) {
        usize amount = offset % 97U + 1U;
        if (amount > source_length - offset) amount = source_length - offset;
        assert(fetch_sink_buffer_append(&buffer, source + offset, amount) ==
               MG_OK);
        offset += amount;
    }
    assert(fetch_sink_buffer_flush(&buffer) == MG_OK);
    assert(capture.length == source_length);
    assert(capture.largest == FETCH_SINK_BUFFER_CAPACITY);
    assert(capture.calls < source_length / 1000U);
    assert(memcmp(source, destination, source_length) == 0);
    free(destination);
    free(source);
}

static void test_write_failure_retains_uncommitted_chunk(void)
{
    u8 destination[FETCH_SINK_BUFFER_CAPACITY * 2U];
    u8 input[sizeof(destination)];
    memset(input, 0x5a, sizeof(input));
    write_capture_t capture = {.bytes = destination,
        .capacity = sizeof(destination), .fail_call = 2U};
    fetch_sink_buffer_t buffer;
    fetch_sink_buffer_init(&buffer, capture_write, &capture);

    assert(fetch_sink_buffer_append(&buffer, input, sizeof(input)) ==
           MG_ERR_IO);
    assert(capture.calls == 2U);
    assert(capture.length == FETCH_SINK_BUFFER_CAPACITY);
    assert(buffer.length == FETCH_SINK_BUFFER_CAPACITY);
    assert(fetch_sink_buffer_flush(&buffer) == MG_ERR_IO);
    assert(buffer.length == FETCH_SINK_BUFFER_CAPACITY);
}

int main(void)
{
    test_fragment_coalescing_and_large_stream();
    test_write_failure_retains_uncommitted_chunk();
    puts("fetch sink buffer: fragmented writes coalesce, large content preserves bytes, and failures remain explicit");
    return 0;
}
