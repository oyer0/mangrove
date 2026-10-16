/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/utf8.h>

bool mg_utf8_is_scalar(u32 scalar)
{
    return scalar <= 0x10ffffU &&
           !(scalar >= 0xd800U && scalar <= 0xdfffU);
}

static u8 sequence_length(u8 lead)
{
    if (lead <= 0x7fU) return 1U;
    if (lead >= 0xc2U && lead <= 0xdfU) return 2U;
    if (lead >= 0xe0U && lead <= 0xefU) return 3U;
    if (lead >= 0xf0U && lead <= 0xf4U) return 4U;
    return 0U;
}

static u32 lead_value(u8 lead, u8 length)
{
    if (length == 2U) return lead & 0x1fU;
    if (length == 3U) return lead & 0x0fU;
    if (length == 4U) return lead & 0x07U;
    return lead;
}

static u32 minimum_value(u8 length)
{
    if (length == 2U) return 0x80U;
    if (length == 3U) return 0x800U;
    if (length == 4U) return 0x10000U;
    return 0U;
}

static bool continuation_allowed(u8 lead, u8 position, u8 byte)
{
    if ((byte & 0xc0U) != 0x80U) return false;
    if (position != 1U) return true;
    if (lead == 0xe0U) return byte >= 0xa0U;
    if (lead == 0xedU) return byte <= 0x9fU;
    if (lead == 0xf0U) return byte >= 0x90U;
    if (lead == 0xf4U) return byte <= 0x8fU;
    return true;
}

mg_utf8_result_t mg_utf8_decode_one(const u8 *input, usize length,
                                    u32 *out_scalar, usize *out_consumed)
{
    u8 lead, count;
    u32 value;

    if (out_consumed) *out_consumed = 0;
    if (!input || !length || !out_scalar || !out_consumed)
        return length ? MG_UTF8_INVALID : MG_UTF8_INCOMPLETE;
    lead = input[0];
    count = sequence_length(lead);
    if (!count) {
        *out_consumed = 1U;
        return MG_UTF8_INVALID;
    }
    value = lead_value(lead, count);
    for (u8 index = 1U; index < count && (usize)index < length; index++) {
        if (!continuation_allowed(lead, index, input[index])) {
            *out_consumed = 1U;
            return MG_UTF8_INVALID;
        }
        value = (value << 6U) | (input[index] & 0x3fU);
    }
    if ((usize)count > length) return MG_UTF8_INCOMPLETE;
    if (value < minimum_value(count) || !mg_utf8_is_scalar(value)) {
        *out_consumed = 1U;
        return MG_UTF8_INVALID;
    }
    *out_scalar = value;
    *out_consumed = count;
    return MG_UTF8_OK;
}

mg_utf8_result_t mg_utf8_decode_display_one(const u8 *input, usize length,
                                            u32 *out_scalar,
                                            usize *out_consumed)
{
    mg_utf8_state_t state = { 0U, 0U, 0U, 0U };
    usize offset = 0U;

    if (out_consumed) *out_consumed = 0U;
    if (!input || !out_scalar || !out_consumed)
        return length ? MG_UTF8_INVALID : MG_UTF8_INCOMPLETE;
    if (!length) return MG_UTF8_INCOMPLETE;

    while (offset < length) {
        mg_utf8_feed_result_t decoded = mg_utf8_state_feed(&state,
                                                            input[offset]);
        if (decoded.consumed) offset++;
        if (decoded.status == MG_UTF8_OK) {
            *out_scalar = decoded.scalar;
            *out_consumed = offset;
            return MG_UTF8_OK;
        }
        if (decoded.status == MG_UTF8_INVALID) {
            *out_scalar = 0xfffdU;
            *out_consumed = offset;
            return MG_UTF8_INVALID;
        }
    }

    /* This function presents a bounded text item as a complete unit. Unlike
     * the terminal's persistent stream state, an incomplete final prefix has
     * no later byte that can complete it, so render it as one replacement. */
    *out_scalar = 0xfffdU;
    *out_consumed = offset;
    return MG_UTF8_INVALID;
}

mg_utf8_result_t mg_utf8_encode_one(u32 scalar, u8 *output, usize capacity,
                                    usize *out_produced)
{
    usize count;

    if (out_produced) *out_produced = 0;
    if (!out_produced || !mg_utf8_is_scalar(scalar)) return MG_UTF8_INVALID;
    if (scalar <= 0x7fU) count = 1U;
    else if (scalar <= 0x7ffU) count = 2U;
    else if (scalar <= 0xffffU) count = 3U;
    else count = 4U;
    if (!output || capacity < count) return MG_UTF8_NO_SPACE;
    if (count == 1U) output[0] = (u8)scalar;
    else if (count == 2U) {
        output[0] = (u8)(0xc0U | (scalar >> 6U));
        output[1] = (u8)(0x80U | (scalar & 0x3fU));
    } else if (count == 3U) {
        output[0] = (u8)(0xe0U | (scalar >> 12U));
        output[1] = (u8)(0x80U | ((scalar >> 6U) & 0x3fU));
        output[2] = (u8)(0x80U | (scalar & 0x3fU));
    } else {
        output[0] = (u8)(0xf0U | (scalar >> 18U));
        output[1] = (u8)(0x80U | ((scalar >> 12U) & 0x3fU));
        output[2] = (u8)(0x80U | ((scalar >> 6U) & 0x3fU));
        output[3] = (u8)(0x80U | (scalar & 0x3fU));
    }
    *out_produced = count;
    return MG_UTF8_OK;
}

bool mg_utf8_validate(const u8 *input, usize length)
{
    usize offset = 0;

    if (length && !input) return false;
    while (offset < length) {
        usize consumed;
        u32 scalar;
        if (mg_utf8_decode_one(input + offset, length - offset, &scalar,
                               &consumed) != MG_UTF8_OK || !consumed)
            return false;
        offset += consumed;
    }
    return true;
}

void mg_utf8_state_reset(mg_utf8_state_t *state)
{
    if (!state) return;
    state->value = 0U;
    state->minimum = 0U;
    state->remaining = 0U;
    state->lead = 0U;
}

bool mg_utf8_state_is_initial(const mg_utf8_state_t *state)
{
    return !state || state->remaining == 0U;
}

mg_utf8_feed_result_t mg_utf8_state_feed(mg_utf8_state_t *state, u8 byte)
{
    mg_utf8_feed_result_t result = { MG_UTF8_INVALID, 0U, true };

    if (!state) return result;
    if (!state->remaining) {
        u8 count = sequence_length(byte);
        if (!count) return result;
        if (count == 1U) {
            result.status = MG_UTF8_OK;
            result.scalar = byte;
            return result;
        }
        state->lead = byte;
        state->remaining = (u8)(count - 1U);
        state->value = lead_value(byte, count);
        state->minimum = minimum_value(count);
        result.status = MG_UTF8_INCOMPLETE;
        return result;
    }

    if (!continuation_allowed(state->lead,
                              (u8)(sequence_length(state->lead) -
                                   state->remaining), byte)) {
        mg_utf8_state_reset(state);
        result.consumed = false;
        return result;
    }
    state->value = (state->value << 6U) | (byte & 0x3fU);
    state->remaining--;
    if (state->remaining) {
        result.status = MG_UTF8_INCOMPLETE;
        return result;
    }
    if (state->value < state->minimum || !mg_utf8_is_scalar(state->value)) {
        mg_utf8_state_reset(state);
        return result;
    }
    result.status = MG_UTF8_OK;
    result.scalar = state->value;
    mg_utf8_state_reset(state);
    return result;
}
