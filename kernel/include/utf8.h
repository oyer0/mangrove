#pragma once

#include <types.h>

/* A decoder emits at most two code points for one input byte: a replacement
 * for an interrupted sequence, followed by the byte when it is also a new
 * ASCII character.  This keeps recovery bounded without requiring pushback. */
#define UTF8_DECODE_OUTPUT_MAX 2U
#define UTF8_REPLACEMENT_CODEPOINT 0xFFFDU

typedef struct {
    u32 value;
    u32 minimum;
    u8 remaining;
} utf8_decoder_t;

typedef struct {
    u32 codepoints[UTF8_DECODE_OUTPUT_MAX];
    u8 count;
} utf8_decode_result_t;

void utf8_decoder_init(utf8_decoder_t *decoder);
utf8_decode_result_t utf8_decoder_feed(utf8_decoder_t *decoder, u8 byte);
