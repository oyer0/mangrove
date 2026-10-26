/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

#define FETCH_SINK_BUFFER_CAPACITY 32768U

typedef mg_result_t (*fetch_sink_buffer_write_fn)(void *context,
                                                  const u8 *bytes,
                                                  usize length);

typedef struct fetch_sink_buffer {
    fetch_sink_buffer_write_fn write;
    void *write_context;
    usize length;
    u8 bytes[FETCH_SINK_BUFFER_CAPACITY];
} fetch_sink_buffer_t;

void fetch_sink_buffer_init(fetch_sink_buffer_t *buffer,
                            fetch_sink_buffer_write_fn write,
                            void *write_context);
mg_result_t fetch_sink_buffer_append(fetch_sink_buffer_t *buffer,
                                     const void *bytes, usize length);
mg_result_t fetch_sink_buffer_flush(fetch_sink_buffer_t *buffer);
