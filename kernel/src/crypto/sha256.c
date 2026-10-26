/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <crypto/sha256.h>

#include <string.h>

static const u32 sha256_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

void crypto_secure_clear(void *buffer, usize size)
{
    volatile u8 *bytes = (volatile u8 *)buffer;
    if (!bytes) return;
    while (size--) *bytes++ = 0;
}

static u32 rotate_right(u32 value, u32 count)
{
    return (value >> count) | (value << (32U - count));
}

static u32 read_be32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) |
           ((u32)data[2] << 8) | data[3];
}

static void write_be32(u8 *data, u32 value)
{
    data[0] = (u8)(value >> 24);
    data[1] = (u8)(value >> 16);
    data[2] = (u8)(value >> 8);
    data[3] = (u8)value;
}

static void sha256_transform(crypto_sha256_context_t *context,
                             const u8 block[64])
{
    u32 words[64];
    u32 a, b, c, d, e, f, g, h;

    for (u32 index = 0; index < 16U; index++)
        words[index] = read_be32(block + index * 4U);
    for (u32 index = 16U; index < 64U; index++) {
        u32 s0 = rotate_right(words[index - 15U], 7U) ^
                 rotate_right(words[index - 15U], 18U) ^
                 (words[index - 15U] >> 3);
        u32 s1 = rotate_right(words[index - 2U], 17U) ^
                 rotate_right(words[index - 2U], 19U) ^
                 (words[index - 2U] >> 10);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (u32 index = 0; index < 64U; index++) {
        u32 s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                 rotate_right(e, 25U);
        u32 choose = (e & f) ^ ((~e) & g);
        u32 temporary1 = h + s1 + choose + sha256_constants[index] +
                         words[index];
        u32 s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                 rotate_right(a, 22U);
        u32 majority = (a & b) ^ (a & c) ^ (b & c);
        u32 temporary2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
    crypto_secure_clear(words, sizeof(words));
}

void crypto_sha256_init(crypto_sha256_context_t *context)
{
    static const u32 initial_state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    if (!context) return;
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->bit_count = 0;
    context->block_length = 0;
    crypto_secure_clear(context->block, sizeof(context->block));
}

void crypto_sha256_update(crypto_sha256_context_t *context,
                          const void *data, usize length)
{
    const u8 *bytes = (const u8 *)data;

    if (!context || (!data && length) || !length) return;
    context->bit_count += (u64)length * 8ULL;
    while (length != 0) {
        u32 available = 64U - context->block_length;
        u32 amount = length < available ? (u32)length : available;
        memcpy(context->block + context->block_length, bytes, amount);
        context->block_length += amount;
        bytes += amount;
        length -= amount;
        if (context->block_length == 64U) {
            sha256_transform(context, context->block);
            context->block_length = 0;
        }
    }
}

void crypto_sha256_final(crypto_sha256_context_t *context, u8 digest[32])
{
    u64 bit_count;
    u8 length[8];

    if (!context || !digest) return;
    bit_count = context->bit_count;
    context->block[context->block_length++] = 0x80;
    if (context->block_length > 56U) {
        while (context->block_length < 64U)
            context->block[context->block_length++] = 0;
        sha256_transform(context, context->block);
        context->block_length = 0;
    }
    while (context->block_length < 56U)
        context->block[context->block_length++] = 0;
    for (u32 index = 0; index < 8U; index++)
        length[7U - index] = (u8)(bit_count >> (index * 8U));
    memcpy(context->block + 56U, length, sizeof(length));
    sha256_transform(context, context->block);
    for (u32 index = 0; index < 8U; index++)
        write_be32(digest + index * 4U, context->state[index]);
    crypto_secure_clear(length, sizeof(length));
    crypto_secure_clear(context, sizeof(*context));
}

void crypto_hmac_sha256_key_init(crypto_hmac_sha256_key_t *prepared,
                                 const u8 *key, usize key_length)
{
    crypto_sha256_context_t context;
    u8 key_block[64];

    if (!prepared || (!key && key_length)) return;
    crypto_secure_clear(key_block, sizeof(key_block));
    if (key_length > sizeof(key_block)) {
        crypto_sha256_init(&context);
        crypto_sha256_update(&context, key, key_length);
        crypto_sha256_final(&context, key_block);
    } else if (key_length) {
        memcpy(key_block, key, key_length);
    }
    for (u32 index = 0; index < sizeof(key_block); index++)
        key_block[index] ^= 0x36;
    crypto_sha256_init(&prepared->inner);
    crypto_sha256_update(&prepared->inner, key_block, sizeof(key_block));

    for (u32 index = 0; index < sizeof(key_block); index++)
        key_block[index] ^= (u8)(0x36 ^ 0x5c);
    crypto_sha256_init(&prepared->outer);
    crypto_sha256_update(&prepared->outer, key_block, sizeof(key_block));
    crypto_secure_clear(key_block, sizeof(key_block));
}

void crypto_hmac_sha256_key_run(const crypto_hmac_sha256_key_t *prepared,
                                const u8 *data, usize data_length,
                                u8 output[32])
{
    crypto_sha256_context_t context;
    u8 inner[32];

    if (!prepared || (!data && data_length) || !output) return;
    memcpy(&context, &prepared->inner, sizeof(context));
    crypto_sha256_update(&context, data, data_length);
    crypto_sha256_final(&context, inner);

    memcpy(&context, &prepared->outer, sizeof(context));
    crypto_sha256_update(&context, inner, sizeof(inner));
    crypto_sha256_final(&context, output);
    crypto_secure_clear(inner, sizeof(inner));
}

void crypto_hmac_sha256_key_clear(crypto_hmac_sha256_key_t *prepared)
{
    crypto_secure_clear(prepared, prepared ? sizeof(*prepared) : 0);
}
