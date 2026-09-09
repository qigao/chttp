#include "turbo_crypto.h"

#include <stdlib.h>
#include <string.h>

#include "xxtea/xxtea.h"

static int turbo_crypto_xxtea_key(const void* key, size_t key_len,
                                  uint8_t fixed_key[TURBO_CRYPTO_XXTEA_KEY_SIZE]) {
    if ((!key && key_len != 0U) || key_len > TURBO_CRYPTO_XXTEA_KEY_SIZE) {
        return TURBO_CRYPTO_EINVAL;
    }

    memset(fixed_key, 0, TURBO_CRYPTO_XXTEA_KEY_SIZE);
    if (key_len != 0U) memcpy(fixed_key, key, key_len);
    return TURBO_CRYPTO_OK;
}

int turbo_crypto_xxtea_encrypt_size(size_t plain_text_len,
                                    size_t* cipher_text_len) {
    if (!cipher_text_len || plain_text_len == 0U ||
        plain_text_len > UINT32_MAX || plain_text_len > SIZE_MAX - 7U) {
        return TURBO_CRYPTO_EINVAL;
    }

    *cipher_text_len = ((plain_text_len + 3U) / 4U + 1U) * 4U;
    return TURBO_CRYPTO_OK;
}

int turbo_crypto_xxtea_encrypt(void* cipher_text,
                               size_t cipher_text_capacity,
                               size_t* cipher_text_len,
                               const void* plain_text,
                               size_t plain_text_len,
                               const void* key,
                               size_t key_len) {
    uint8_t fixed_key[TURBO_CRYPTO_XXTEA_KEY_SIZE];
    size_t required = 0U;
    size_t allocated_len = 0U;
    void* allocated = NULL;
    int rc;

    if (cipher_text_len) *cipher_text_len = 0U;
    if (!cipher_text_len || !cipher_text || !plain_text) {
        return TURBO_CRYPTO_EINVAL;
    }

    rc = turbo_crypto_xxtea_key(key, key_len, fixed_key);
    if (rc != TURBO_CRYPTO_OK) return rc;

    rc = turbo_crypto_xxtea_encrypt_size(plain_text_len, &required);
    if (rc != TURBO_CRYPTO_OK) {
        turbo_crypto_wipe(fixed_key, sizeof(fixed_key));
        return rc;
    }
    *cipher_text_len = required;
    if (cipher_text_capacity < required) {
        turbo_crypto_wipe(fixed_key, sizeof(fixed_key));
        return TURBO_CRYPTO_EBUFFER;
    }

    allocated = xxtea_encrypt(plain_text, plain_text_len, fixed_key,
                              &allocated_len);
    turbo_crypto_wipe(fixed_key, sizeof(fixed_key));
    if (!allocated || allocated_len != required) {
        free(allocated);
        *cipher_text_len = 0U;
        return TURBO_CRYPTO_ECRYPTO;
    }

    memcpy(cipher_text, allocated, allocated_len);
    free(allocated);
    return TURBO_CRYPTO_OK;
}

int turbo_crypto_xxtea_decrypt(void* plain_text,
                               size_t plain_text_capacity,
                               size_t* plain_text_len,
                               const void* cipher_text,
                               size_t cipher_text_len,
                               const void* key,
                               size_t key_len) {
    uint8_t fixed_key[TURBO_CRYPTO_XXTEA_KEY_SIZE];
    size_t required_capacity;
    size_t allocated_len = 0U;
    void* allocated = NULL;
    int rc;

    if (plain_text_len) *plain_text_len = 0U;
    if (!plain_text_len || !plain_text || !cipher_text ||
        cipher_text_len < 8U || (cipher_text_len & 3U) != 0U ||
        cipher_text_len / 4U > UINT32_MAX) {
        return TURBO_CRYPTO_EINVAL;
    }

    rc = turbo_crypto_xxtea_key(key, key_len, fixed_key);
    if (rc != TURBO_CRYPTO_OK) return rc;

    required_capacity = cipher_text_len - 4U;
    *plain_text_len = required_capacity;
    if (plain_text_capacity < required_capacity) {
        turbo_crypto_wipe(fixed_key, sizeof(fixed_key));
        return TURBO_CRYPTO_EBUFFER;
    }

    allocated = xxtea_decrypt(cipher_text, cipher_text_len, fixed_key,
                              &allocated_len);
    turbo_crypto_wipe(fixed_key, sizeof(fixed_key));
    if (!allocated || allocated_len == 0U || allocated_len > required_capacity) {
        free(allocated);
        *plain_text_len = 0U;
        return TURBO_CRYPTO_ECRYPTO;
    }

    memcpy(plain_text, allocated, allocated_len);
    turbo_crypto_wipe(allocated, allocated_len);
    free(allocated);
    *plain_text_len = allocated_len;
    return TURBO_CRYPTO_OK;
}
