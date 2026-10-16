/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Strict UTF-8 primitives shared by kernel and userspace. The codec has no
 * allocation, locale, libc, or filesystem dependencies. */
typedef enum {
    MG_UTF8_OK = 0,
    MG_UTF8_INCOMPLETE,
    MG_UTF8_INVALID,
    MG_UTF8_NO_SPACE
} mg_utf8_result_t;

typedef struct {
    u32 value;
    u32 minimum;
    u8 remaining;
    u8 lead;
} mg_utf8_state_t;

typedef struct {
    mg_utf8_result_t status;
    u32 scalar;
    /* false only when a byte interrupting a partial sequence must be retried
     * as the start of the next sequence. */
    bool consumed;
} mg_utf8_feed_result_t;

/* Decode one scalar from a bounded byte range. INCOMPLETE leaves consumed at
 * zero; INVALID reports one byte as the malformed subsequence's start. NUL
 * is a valid scalar when it is part of the explicit range. */
mg_utf8_result_t mg_utf8_decode_one(const u8 *input, usize length,
                                    u32 *out_scalar, usize *out_consumed);
/* Presentation decoder: returns valid scalars unchanged and maps each
 * malformed subsequence to U+FFFD. An incomplete prefix at the end of this
 * bounded range is treated as one malformed subsequence. */
mg_utf8_result_t mg_utf8_decode_display_one(const u8 *input, usize length,
                                            u32 *out_scalar,
                                            usize *out_consumed);
mg_utf8_result_t mg_utf8_encode_one(u32 scalar, u8 *output, usize capacity,
                                    usize *out_produced);
bool mg_utf8_is_scalar(u32 scalar);
bool mg_utf8_validate(const u8 *input, usize length);

void mg_utf8_state_reset(mg_utf8_state_t *state);
bool mg_utf8_state_is_initial(const mg_utf8_state_t *state);
mg_utf8_feed_result_t mg_utf8_state_feed(mg_utf8_state_t *state, u8 byte);
