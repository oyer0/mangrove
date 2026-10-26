/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fetch_sink_buffer.h"

#include <string.h>

void fetch_sink_buffer_init(fetch_sink_buffer_t *buffer,
                            fetch_sink_buffer_write_fn write,
                            void *write_context)
{
    if (!buffer) return;
    *buffer = (fetch_sink_buffer_t){
        .write = write,
        .write_context = write_context,
    };
}

mg_result_t fetch_sink_buffer_flush(fetch_sink_buffer_t *buffer)
{
    mg_result_t result;

    if (!buffer || !buffer->write) return MG_ERR_BAD_ARGUMENT;
    if (!buffer->length) return MG_OK;
    result = buffer->write(buffer->write_context, buffer->bytes,
                           buffer->length);
    if (result != MG_OK) return result < 0 ? result : MG_ERR_IO;
    buffer->length = 0U;
    return MG_OK;
}

mg_result_t fetch_sink_buffer_append(fetch_sink_buffer_t *buffer,
                                     const void *data, usize length)
{
    const u8 *bytes = (const u8 *)data;

    if (!buffer || !buffer->write || (!bytes && length))
        return MG_ERR_BAD_ARGUMENT;
    while (length) {
        usize available = FETCH_SINK_BUFFER_CAPACITY - buffer->length;
        usize amount = length < available ? length : available;
        memcpy(buffer->bytes + buffer->length, bytes, amount);
        buffer->length += amount;
        bytes += amount;
        length -= amount;
        if (buffer->length == FETCH_SINK_BUFFER_CAPACITY) {
            mg_result_t result = fetch_sink_buffer_flush(buffer);
            if (result != MG_OK) return result;
        }
    }
    return MG_OK;
}
