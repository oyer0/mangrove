#include <password.h>

#include <string.h>

typedef struct {
    u32 state[8];
    u64 bit_count;
    u8 block[64];
    u32 block_length;
} sha256_context_t;

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

static void sha256_transform(sha256_context_t *context,
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
    password_secure_clear(words, sizeof(words));
}

static void sha256_init(sha256_context_t *context)
{
    static const u32 initial_state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->bit_count = 0;
    context->block_length = 0;
    password_secure_clear(context->block, sizeof(context->block));
}

static void sha256_update(sha256_context_t *context, const void *data,
                          usize length)
{
    const u8 *bytes = (const u8 *)data;

    if (!length) return;
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

static void sha256_final(sha256_context_t *context, u8 digest[32])
{
    u64 bit_count = context->bit_count;
    u8 length[8];

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
    password_secure_clear(length, sizeof(length));
    password_secure_clear(context, sizeof(*context));
}

static void hmac_sha256(const u8 *key, usize key_length,
                        const u8 *data, usize data_length, u8 output[32])
{
    sha256_context_t context;
    u8 key_block[64];
    u8 inner[32];

    password_secure_clear(key_block, sizeof(key_block));
    if (key_length > sizeof(key_block)) {
        sha256_init(&context);
        sha256_update(&context, key, key_length);
        sha256_final(&context, key_block);
    } else {
        memcpy(key_block, key, key_length);
    }
    for (u32 index = 0; index < sizeof(key_block); index++)
        key_block[index] ^= 0x36;
    sha256_init(&context);
    sha256_update(&context, key_block, sizeof(key_block));
    sha256_update(&context, data, data_length);
    sha256_final(&context, inner);

    for (u32 index = 0; index < sizeof(key_block); index++)
        key_block[index] ^= (u8)(0x36 ^ 0x5c);
    sha256_init(&context);
    sha256_update(&context, key_block, sizeof(key_block));
    sha256_update(&context, inner, sizeof(inner));
    sha256_final(&context, output);
    password_secure_clear(key_block, sizeof(key_block));
    password_secure_clear(inner, sizeof(inner));
}

static void pbkdf2_sha256(const char *password, const u8 *salt,
                          usize salt_length, u32 iterations, u8 output[32])
{
    u8 input[IDENTITY_PASSWORD_SALT_BYTES + 4U];
    u8 current[32];
    u8 accumulated[32];
    usize password_length = strlen(password);

    memcpy(input, salt, salt_length);
    input[salt_length] = 0;
    input[salt_length + 1U] = 0;
    input[salt_length + 2U] = 0;
    input[salt_length + 3U] = 1;
    hmac_sha256((const u8 *)password, password_length, input,
                salt_length + 4U, accumulated);
    memcpy(current, accumulated, sizeof(current));
    for (u32 round = 1U; round < iterations; round++) {
        hmac_sha256((const u8 *)password, password_length, current,
                    sizeof(current), current);
        for (u32 index = 0; index < sizeof(accumulated); index++)
            accumulated[index] ^= current[index];
    }
    memcpy(output, accumulated, sizeof(accumulated));
    password_secure_clear(input, sizeof(input));
    password_secure_clear(current, sizeof(current));
    password_secure_clear(accumulated, sizeof(accumulated));
}

static void read_cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx,
                       u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static bool hardware_rng_available(bool *rdseed)
{
    u32 eax, ebx, ecx, edx;
    u32 max_leaf;

    read_cpuid(0, 0, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf < 1U) return false;
    read_cpuid(1U, 0, &eax, &ebx, &ecx, &edx);
    if (ecx & (1U << 30)) {
        if (rdseed) *rdseed = false;
        return true;
    }
    if (max_leaf < 7U) return false;
    read_cpuid(7U, 0, &eax, &ebx, &ecx, &edx);
    if (ebx & (1U << 18)) {
        if (rdseed) *rdseed = true;
        return true;
    }
    return false;
}

static bool random_u64(u64 *value, bool rdseed)
{
    unsigned char ready;

    if (rdseed) {
        for (u32 attempt = 0; attempt < 16U; attempt++) {
            __asm__ volatile("rdseed %0; setc %1"
                             : "=r"(*value), "=qm"(ready));
            if (ready) return true;
        }
    } else {
        for (u32 attempt = 0; attempt < 16U; attempt++) {
            __asm__ volatile("rdrand %0; setc %1"
                             : "=r"(*value), "=qm"(ready));
            if (ready) return true;
        }
    }
    return false;
}

static bool random_bytes(u8 *output, usize length)
{
    bool rdseed;
    if (!hardware_rng_available(&rdseed)) return false;
    for (usize offset = 0; offset < length; offset += sizeof(u64)) {
        u64 value;
        usize amount = length - offset < sizeof(value)
            ? length - offset : sizeof(value);
        if (!random_u64(&value, rdseed)) return false;
        memcpy(output + offset, &value, amount);
        password_secure_clear(&value, sizeof(value));
    }
    return true;
}

void password_secure_clear(void *buffer, usize size)
{
    volatile u8 *bytes = (volatile u8 *)buffer;
    if (!bytes) return;
    while (size--) *bytes++ = 0;
}

bool password_auth_valid(const identity_authentication_t *authentication)
{
    if (!authentication) return false;
    if (authentication->algorithm == IDENTITY_AUTH_NONE) {
        if (authentication->iterations != 0) return false;
        for (u32 index = 0; index < sizeof(authentication->salt); index++)
            if (authentication->salt[index]) return false;
        for (u32 index = 0; index < sizeof(authentication->hash); index++)
            if (authentication->hash[index]) return false;
        return true;
    }
    return authentication->algorithm == IDENTITY_AUTH_PBKDF2_SHA256 &&
           authentication->iterations >= IDENTITY_PASSWORD_MIN_ITERATIONS &&
           authentication->iterations <= IDENTITY_PASSWORD_MAX_ITERATIONS;
}

bool password_auth_available(void)
{
    return hardware_rng_available(NULL);
}

bool password_auth_generate(identity_authentication_t *authentication,
                            const char *password)
{
    usize length;

    if (!authentication || !password) return false;
    length = strlen(password);
    if (length == 0 || length > IDENTITY_PASSWORD_MAX_LENGTH ||
        !random_bytes(authentication->salt,
                      IDENTITY_PASSWORD_SALT_BYTES)) return false;
    authentication->algorithm = IDENTITY_AUTH_PBKDF2_SHA256;
    authentication->iterations = IDENTITY_PASSWORD_ITERATIONS;
    pbkdf2_sha256(password, authentication->salt,
                  IDENTITY_PASSWORD_SALT_BYTES, authentication->iterations,
                  authentication->hash);
    return password_auth_valid(authentication);
}

bool password_auth_verify(const identity_authentication_t *authentication,
                          const char *password)
{
    u8 computed[IDENTITY_PASSWORD_HASH_BYTES];
    u8 difference = 0;
    usize length;

    if (!authentication || !password ||
        authentication->algorithm != IDENTITY_AUTH_PBKDF2_SHA256 ||
        !password_auth_valid(authentication)) return false;
    length = strlen(password);
    if (length > IDENTITY_PASSWORD_MAX_LENGTH) return false;
    pbkdf2_sha256(password, authentication->salt,
                  IDENTITY_PASSWORD_SALT_BYTES, authentication->iterations,
                  computed);
    for (u32 index = 0; index < sizeof(computed); index++)
        difference |= computed[index] ^ authentication->hash[index];
    password_secure_clear(computed, sizeof(computed));
    return difference == 0;
}
