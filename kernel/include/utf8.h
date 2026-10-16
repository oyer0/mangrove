/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/utf8.h>

/* Kernel terminal compatibility wrapper. Strict scalar conversion lives in
 * the shared freestanding codec; this adapter applies terminal replacement
 * policy to its streaming results. */
#define UTF8_DECODE_OUTPUT_MAX 2U
#define UTF8_REPLACEMENT_CODEPOINT 0xfffdu

typedef mg_utf8_state_t utf8_decoder_t;

typedef struct {
    u32 codepoints[UTF8_DECODE_OUTPUT_MAX];
    u8 count;
} utf8_decode_result_t;

void utf8_decoder_init(utf8_decoder_t *decoder);
utf8_decode_result_t utf8_decoder_feed(utf8_decoder_t *decoder, u8 byte);
