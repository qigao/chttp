#include "turbo_crypto.h"

#include <salts/random.h>
#include <string.h>

#include "monocypher/monocypher.h"
#include "sha2/sha-256.h"

typedef struct {
    struct Sha_256 sha;
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    uint32_t state;
} turbo_crypto_sha256_impl_t;

enum {
    TURBO_CRYPTO_SHA256_STATE_INITIALIZED = 0x53483236U,
    TURBO_CRYPTO_SHA256_STATE_FINALIZED = 0x46494e49U
};

_Static_assert(sizeof(turbo_crypto_sha256_impl_t) <= TURBO_CRYPTO_SHA256_CONTEXT_SIZE,
               "TURBO_CRYPTO_SHA256_CONTEXT_SIZE is too small");
_Static_assert(_Alignof(turbo_crypto_sha256_impl_t) <= _Alignof(turbo_crypto_sha256_ctx_t),
               "turbo_crypto_sha256_ctx_t alignment is insufficient");

static turbo_crypto_sha256_impl_t* sha256_impl(turbo_crypto_sha256_ctx_t* ctx) {
    return (turbo_crypto_sha256_impl_t*)(void*)ctx->bytes;
}

int turbo_crypto_sha256_init(turbo_crypto_sha256_ctx_t* ctx) {
    if (!ctx) return TURBO_CRYPTO_EINVAL;

    memset(ctx, 0, sizeof(*ctx));
    turbo_crypto_sha256_impl_t* impl = sha256_impl(ctx);
    sha_256_init(&impl->sha, impl->digest);
    impl->state = TURBO_CRYPTO_SHA256_STATE_INITIALIZED;
    return TURBO_CRYPTO_OK;
}

int turbo_crypto_sha256_update(turbo_crypto_sha256_ctx_t* ctx,
                               const void* data, size_t len) {
    if (!ctx || (!data && len != 0U)) return TURBO_CRYPTO_EINVAL;

    turbo_crypto_sha256_impl_t* impl = sha256_impl(ctx);
    if (impl->state != TURBO_CRYPTO_SHA256_STATE_INITIALIZED) {
        return TURBO_CRYPTO_ESTATE;
    }
    sha_256_write(&impl->sha, data, len);
    return TURBO_CRYPTO_OK;
}

int turbo_crypto_sha256_final(turbo_crypto_sha256_ctx_t* ctx,
                              uint8_t out[TURBO_CRYPTO_SHA256_SIZE]) {
    if (!ctx || !out) return TURBO_CRYPTO_EINVAL;

    turbo_crypto_sha256_impl_t* impl = sha256_impl(ctx);
    if (impl->state != TURBO_CRYPTO_SHA256_STATE_INITIALIZED) {
        return TURBO_CRYPTO_ESTATE;
    }
    sha_256_close(&impl->sha);
    memcpy(out, impl->digest, TURBO_CRYPTO_SHA256_SIZE);
    impl->state = TURBO_CRYPTO_SHA256_STATE_FINALIZED;
    return TURBO_CRYPTO_OK;
}

int turbo_crypto_sha256(const void* data, size_t len,
                        uint8_t out[TURBO_CRYPTO_SHA256_SIZE]) {
    turbo_crypto_sha256_ctx_t ctx;
    int rc = turbo_crypto_sha256_init(&ctx);
    if (rc == TURBO_CRYPTO_OK) rc = turbo_crypto_sha256_update(&ctx, data, len);
    if (rc == TURBO_CRYPTO_OK) rc = turbo_crypto_sha256_final(&ctx, out);
    crypto_wipe(&ctx, sizeof(ctx));
    return rc;
}

static int hmac_sha256_parts(const void* key, size_t key_len,
                             const void* first, size_t first_len,
                             const void* second, size_t second_len,
                             uint8_t out[TURBO_CRYPTO_SHA256_SIZE]) {
    uint8_t key_block[TURBO_CRYPTO_SHA256_BLOCK_SIZE] = {0};
    uint8_t inner_pad[TURBO_CRYPTO_SHA256_BLOCK_SIZE];
    uint8_t outer_pad[TURBO_CRYPTO_SHA256_BLOCK_SIZE];
    uint8_t inner_digest[TURBO_CRYPTO_SHA256_SIZE];
    turbo_crypto_sha256_ctx_t ctx;
    int rc = TURBO_CRYPTO_OK;

    if ((!key && key_len != 0U) || (!first && first_len != 0U) ||
        (!second && second_len != 0U) || !out) {
        return TURBO_CRYPTO_EINVAL;
    }

    if (key_len > TURBO_CRYPTO_SHA256_BLOCK_SIZE) {
        rc = turbo_crypto_sha256(key, key_len, key_block);
        key_len = TURBO_CRYPTO_SHA256_SIZE;
    } else if (key_len != 0U) {
        memcpy(key_block, key, key_len);
    }

    if (rc == TURBO_CRYPTO_OK) {
        for (size_t i = 0; i < TURBO_CRYPTO_SHA256_BLOCK_SIZE; ++i) {
            inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36U);
            outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5cU);
        }

        rc = turbo_crypto_sha256_init(&ctx);
        if (rc == TURBO_CRYPTO_OK) {
            rc = turbo_crypto_sha256_update(&ctx, inner_pad, sizeof(inner_pad));
        }
        if (rc == TURBO_CRYPTO_OK) rc = turbo_crypto_sha256_update(&ctx, first, first_len);
        if (rc == TURBO_CRYPTO_OK) rc = turbo_crypto_sha256_update(&ctx, second, second_len);
        if (rc == TURBO_CRYPTO_OK) rc = turbo_crypto_sha256_final(&ctx, inner_digest);
    }

    if (rc == TURBO_CRYPTO_OK) {
        rc = turbo_crypto_sha256_init(&ctx);
        if (rc == TURBO_CRYPTO_OK) {
            rc = turbo_crypto_sha256_update(&ctx, outer_pad, sizeof(outer_pad));
        }
        if (rc == TURBO_CRYPTO_OK) {
            rc = turbo_crypto_sha256_update(&ctx, inner_digest, sizeof(inner_digest));
        }
        if (rc == TURBO_CRYPTO_OK) rc = turbo_crypto_sha256_final(&ctx, out);
    }

    crypto_wipe(&ctx, sizeof(ctx));
    crypto_wipe(key_block, sizeof(key_block));
    crypto_wipe(inner_pad, sizeof(inner_pad));
    crypto_wipe(outer_pad, sizeof(outer_pad));
    crypto_wipe(inner_digest, sizeof(inner_digest));
    return rc;
}

int turbo_crypto_hmac_sha256(const void* key, size_t key_len,
                             const void* data, size_t data_len,
                             uint8_t out[TURBO_CRYPTO_SHA256_SIZE]) {
    return hmac_sha256_parts(key, key_len, data, data_len, NULL, 0U, out);
}

int turbo_crypto_pbkdf2_hmac_sha256(
    const void* password, size_t password_len,
    const void* salt, size_t salt_len,
    uint32_t iterations, void* out, size_t out_len) {
    uint8_t* output = (uint8_t*)out;
    uint8_t counter[4];
    uint8_t u[TURBO_CRYPTO_SHA256_SIZE];
    uint8_t block[TURBO_CRYPTO_SHA256_SIZE];
    int rc = TURBO_CRYPTO_OK;

    if ((!password && password_len != 0U) || (!salt && salt_len != 0U) ||
        (!out && out_len != 0U) || iterations == 0U) {
        return TURBO_CRYPTO_EINVAL;
    }
    if (out_len == 0U) return TURBO_CRYPTO_OK;

    const size_t block_count = (out_len - 1U) / TURBO_CRYPTO_SHA256_SIZE + 1U;
    if (block_count > UINT32_MAX) return TURBO_CRYPTO_EINVAL;

    size_t output_offset = 0U;
    for (size_t block_number = 1U; block_number <= block_count; ++block_number) {
        const uint32_t index = (uint32_t)block_number;
        counter[0] = (uint8_t)(index >> 24U);
        counter[1] = (uint8_t)(index >> 16U);
        counter[2] = (uint8_t)(index >> 8U);
        counter[3] = (uint8_t)index;

        rc = hmac_sha256_parts(password, password_len, salt, salt_len,
                               counter, sizeof(counter), u);
        if (rc != TURBO_CRYPTO_OK) break;
        memcpy(block, u, sizeof(block));

        for (uint32_t iteration = 1U; iteration < iterations; ++iteration) {
            rc = turbo_crypto_hmac_sha256(password, password_len, u, sizeof(u), u);
            if (rc != TURBO_CRYPTO_OK) break;
            for (size_t i = 0; i < sizeof(block); ++i) block[i] ^= u[i];
        }
        if (rc != TURBO_CRYPTO_OK) break;

        size_t copy_len = out_len - output_offset;
        if (copy_len > sizeof(block)) copy_len = sizeof(block);
        memcpy(output + output_offset, block, copy_len);
        output_offset += copy_len;
    }

    crypto_wipe(counter, sizeof(counter));
    crypto_wipe(u, sizeof(u));
    crypto_wipe(block, sizeof(block));
    if (rc != TURBO_CRYPTO_OK) crypto_wipe(out, out_len);
    return rc;
}

typedef struct {
    uint32_t state[4];
    uint64_t total_len;
    uint8_t block[64];
    size_t block_len;
} turbo_md5_ctx_t;

static uint32_t rotate_left32(uint32_t value, uint32_t shift) {
    return (value << shift) | (value >> (32U - shift));
}

static uint32_t load_le32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static void store_le32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

static void md5_transform(turbo_md5_ctx_t* ctx, const uint8_t block[64]) {
    static const uint32_t shifts[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    static const uint32_t constants[64] = {
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
        0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
        0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
        0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
        0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
        0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
        0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
        0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
        0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
        0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
        0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
        0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
        0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
        0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
        0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
        0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U
    };
    uint32_t words[16];
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];

    for (size_t i = 0; i < 16U; ++i) words[i] = load_le32(block + i * 4U);

    for (uint32_t i = 0; i < 64U; ++i) {
        uint32_t f;
        uint32_t word_index;
        if (i < 16U) {
            f = (b & c) | ((~b) & d);
            word_index = i;
        } else if (i < 32U) {
            f = (d & b) | ((~d) & c);
            word_index = (5U * i + 1U) & 15U;
        } else if (i < 48U) {
            f = b ^ c ^ d;
            word_index = (3U * i + 5U) & 15U;
        } else {
            f = c ^ (b | (~d));
            word_index = (7U * i) & 15U;
        }

        uint32_t previous_d = d;
        d = c;
        c = b;
        b += rotate_left32(a + f + constants[i] + words[word_index], shifts[i]);
        a = previous_d;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

static void md5_init(turbo_md5_ctx_t* ctx) {
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
    ctx->total_len = 0U;
    ctx->block_len = 0U;
}

static void md5_update(turbo_md5_ctx_t* ctx, const uint8_t* data, size_t len) {
    ctx->total_len += (uint64_t)len;

    if (ctx->block_len != 0U) {
        size_t needed = sizeof(ctx->block) - ctx->block_len;
        size_t copied = len < needed ? len : needed;
        memcpy(ctx->block + ctx->block_len, data, copied);
        ctx->block_len += copied;
        data += copied;
        len -= copied;
        if (ctx->block_len == sizeof(ctx->block)) {
            md5_transform(ctx, ctx->block);
            ctx->block_len = 0U;
        }
    }

    while (len >= sizeof(ctx->block)) {
        md5_transform(ctx, data);
        data += sizeof(ctx->block);
        len -= sizeof(ctx->block);
    }

    if (len != 0U) {
        memcpy(ctx->block, data, len);
        ctx->block_len = len;
    }
}

static void md5_final(turbo_md5_ctx_t* ctx, uint8_t out[TURBO_CRYPTO_MD5_SIZE]) {
    uint64_t bit_len = ctx->total_len << 3U;
    ctx->block[ctx->block_len++] = 0x80U;

    if (ctx->block_len > 56U) {
        memset(ctx->block + ctx->block_len, 0, sizeof(ctx->block) - ctx->block_len);
        md5_transform(ctx, ctx->block);
        ctx->block_len = 0U;
    }
    memset(ctx->block + ctx->block_len, 0, 56U - ctx->block_len);
    for (size_t i = 0; i < 8U; ++i) {
        ctx->block[56U + i] = (uint8_t)(bit_len >> (i * 8U));
    }
    md5_transform(ctx, ctx->block);

    for (size_t i = 0; i < 4U; ++i) store_le32(out + i * 4U, ctx->state[i]);
}

int turbo_crypto_md5(const void* data, size_t len,
                     uint8_t out[TURBO_CRYPTO_MD5_SIZE]) {
    if ((!data && len != 0U) || !out) return TURBO_CRYPTO_EINVAL;

    turbo_md5_ctx_t ctx;
    md5_init(&ctx);
    md5_update(&ctx, (const uint8_t*)data, len);
    md5_final(&ctx, out);
    crypto_wipe(&ctx, sizeof(ctx));
    return TURBO_CRYPTO_OK;
}

int turbo_crypto_random(void* out, size_t len) {
    return salts_platform_secure_random(out, len) == 0 ? TURBO_CRYPTO_OK
                                                        : TURBO_CRYPTO_ERANDOM;
}

int turbo_crypto_verify(const void* expected, const void* actual, size_t len) {
    if ((!expected || !actual) && len != 0U) return TURBO_CRYPTO_EINVAL;
    if (len == 0U) return TURBO_CRYPTO_OK;
    if (len == 16U) {
        return crypto_verify16((const uint8_t*)expected, (const uint8_t*)actual) == 0
                   ? TURBO_CRYPTO_OK : TURBO_CRYPTO_EVERIFY;
    }
    if (len == 32U) {
        return crypto_verify32((const uint8_t*)expected, (const uint8_t*)actual) == 0
                   ? TURBO_CRYPTO_OK : TURBO_CRYPTO_EVERIFY;
    }
    if (len == 64U) {
        return crypto_verify64((const uint8_t*)expected, (const uint8_t*)actual) == 0
                   ? TURBO_CRYPTO_OK : TURBO_CRYPTO_EVERIFY;
    }

    const uint8_t* left = (const uint8_t*)expected;
    const uint8_t* right = (const uint8_t*)actual;
    uint8_t difference = 0U;
    for (size_t i = 0; i < len; ++i) difference |= (uint8_t)(left[i] ^ right[i]);
    return difference == 0U ? TURBO_CRYPTO_OK : TURBO_CRYPTO_EVERIFY;
}

void turbo_crypto_wipe(void* secret, size_t len) {
    if (secret && len != 0U) crypto_wipe(secret, len);
}
