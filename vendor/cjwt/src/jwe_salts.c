/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <salts/crypto.h>
#include <fmt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "cjwt.h"
#include "jwe.h"
#include "jws.h"
#include "utils.h"

static size_t content_key_size(cjwt_enc_t enc) {
    return enc == enc_a128gcm ? 16 : enc == enc_a192gcm ? 24 : enc == enc_a256gcm ? 32 : 0;
}

static size_t wrapping_key_size(cjwt_alg_t alg) {
    switch (alg) {
    case alg_a128kw: case alg_pbes2_hs256_a128kw: return 16;
    case alg_a192kw: case alg_pbes2_hs384_a192kw: return 24;
    case alg_a256kw: case alg_pbes2_hs512_a256kw: return 32;
    default: return 0;
    }
}

static int is_pbes2(cjwt_alg_t alg) {
    return alg == alg_pbes2_hs256_a128kw || alg == alg_pbes2_hs384_a192kw || alg == alg_pbes2_hs512_a256kw;
}

static cjwt_code_t derive_kek(const cjwt_t *jwt, const uint8_t *password, size_t password_size,
                             uint8_t kek[32], size_t *kek_size) {
    const char *name = alg_to_string(jwt->header.alg);
    const char *encoded = json_get_string(jwt->header.private_headers, "p2s");
    json_value_t *iterations = json_object_get(jwt->header.private_headers, "p2c");
    salts_crypto_hash hash;
    uint8_t *raw = NULL, *salt = NULL;
    size_t raw_size = 0, name_size, salt_size = 0;
    double count;
    cjwt_code_t result = CJWTE_SIGNATURE_VALIDATION_FAILED;
    if (!encoded || !iterations || json_type(iterations) != JSON_NUMBER) return CJWTE_HEADER_MISSING;
    count = json_number(iterations);
    if (!(count >= 1 && count <= INT_MAX) || (double)(uint32_t)count != count || password_size > INT_MAX)
        return CJWTE_INVALID_PARAMETERS;
    raw = b64url_decode_with_alloc((const uint8_t *)encoded, strlen(encoded), &raw_size);
    if (!raw) return CJWTE_HEADER_INVALID_BASE64;
    name_size = strlen(name);
    if (raw_size > SIZE_MAX - name_size - 1 || name_size + 1 + raw_size > INT_MAX) {
        result = CJWTE_INVALID_PARAMETERS; goto done;
    }
    salt_size = name_size + 1 + raw_size;
    salt = malloc(salt_size);
    if (!salt) { result = CJWTE_OUT_OF_MEMORY; goto done; }
    memcpy(salt, name, name_size); salt[name_size] = 0;
    memcpy(salt + name_size + 1, raw, raw_size);
    *kek_size = wrapping_key_size(jwt->header.alg);
    hash = *kek_size == 16 ? SALTS_CRYPTO_SHA256 : *kek_size == 24 ? SALTS_CRYPTO_SHA384 : SALTS_CRYPTO_SHA512;
    if (salts_crypto_pbkdf2(hash, password, password_size, salt, salt_size,
        (uint32_t)count, kek, *kek_size) == SALTS_CRYPTO_OK) result = CJWTE_OK;
done:
    salts_crypto_clear(salt, salt_size); free(salt); free(raw);
    return result;
}

static cjwt_code_t crypt_cek(const cjwt_t *jwt, int decrypt,
    const uint8_t *key_data, size_t key_size, const cjwt_jwk_t *jwk,
    const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size) {
    salts_crypto_key *key = NULL;
    uint8_t kek[32], *buffer = NULL;
    const uint8_t *wrapping_key = key_data;
    size_t capacity = 0, size = 0, kek_size = 0;
    cjwt_code_t result = CJWTE_SIGNATURE_VALIDATION_FAILED;
    int status;
    *output = NULL; *output_size = 0;
    if (jwt->header.alg == alg_dir) {
        if (!decrypt) return CJWTE_OK;
        buffer = malloc(key_size ? key_size : 1);
        if (!buffer) return CJWTE_OUT_OF_MEMORY;
        if (key_size) memcpy(buffer, key_data, key_size);
        *output = buffer; *output_size = key_size; return CJWTE_OK;
    }
    if (jwt->header.alg == alg_rsa_oaep || jwt->header.alg == alg_rsa_oaep_256) {
        salts_crypto_hash hash = jwt->header.alg == alg_rsa_oaep ? SALTS_CRYPTO_SHA1 : SALTS_CRYPTO_SHA256;
        result = jws_load_key(key_data, key_size, jwk, decrypt, &key);
        if (result != CJWTE_OK) goto done;
        if (salts_crypto_key_type(key) != SALTS_CRYPTO_KEY_RSA) { result = CJWTE_SIGNATURE_INVALID_KEY; goto done; }
        capacity = salts_crypto_key_signature_size(key);
        buffer = malloc(capacity);
        if (!buffer) { result = CJWTE_OUT_OF_MEMORY; goto done; }
        status = decrypt ? salts_crypto_key_oaep_decrypt(key, hash, input, input_size, buffer, capacity, &size) :
                           salts_crypto_key_oaep_encrypt(key, hash, input, input_size, buffer, capacity, &size);
    } else {
        kek_size = wrapping_key_size(jwt->header.alg);
        if (!kek_size) { result = CJWTE_SIGNATURE_UNSUPPORTED_ALG; goto done; }
        if (is_pbes2(jwt->header.alg)) {
            result = derive_kek(jwt, key_data, key_size, kek, &kek_size);
            if (result != CJWTE_OK) goto done;
            wrapping_key = kek;
        } else if (key_size != kek_size) { result = CJWTE_SIGNATURE_INVALID_KEY; goto done; }
        if (decrypt) {
            if (input_size < 24 || input_size % 8) { result = CJWTE_SIGNATURE_VALIDATION_FAILED; goto done; }
            capacity = input_size - 8;
        } else {
            if (input_size > SIZE_MAX - 8) { result = CJWTE_INVALID_PARAMETERS; goto done; }
            capacity = input_size + 8;
        }
        buffer = malloc(capacity);
        if (!buffer) { result = CJWTE_OUT_OF_MEMORY; goto done; }
        status = decrypt ? salts_crypto_aes_key_unwrap(wrapping_key, kek_size, input, input_size, buffer, capacity, &size) :
                           salts_crypto_aes_key_wrap(wrapping_key, kek_size, input, input_size, buffer, capacity, &size);
    }
    if (status != SALTS_CRYPTO_OK) { result = CJWTE_SIGNATURE_VALIDATION_FAILED; goto done; }
    *output = buffer; buffer = NULL; *output_size = size; result = CJWTE_OK;
done:
    salts_crypto_clear(buffer, buffer ? capacity : 0); free(buffer);
    salts_crypto_clear(kek, sizeof(kek)); salts_crypto_key_destroy(key);
    return result;
}

cjwt_code_t jwe_decrypt(const cjwt_t *jwt, const struct section *header,
    const struct section *enc_key, const struct section *iv_section,
    const struct section *ciphertext_section, const struct section *tag_section,
    const uint8_t *key_data, size_t key_size, const cjwt_jwk_t *jwk,
    uint8_t **plaintext, size_t *plaintext_size) {
    uint8_t *encrypted_key = NULL, *iv = NULL, *ciphertext = NULL, *tag = NULL, *cek = NULL, *result = NULL;
    size_t encrypted_size = 0, iv_size = 0, ciphertext_size = 0, tag_size = 0, cek_size = 0, required;
    cjwt_code_t status = CJWTE_PAYLOAD_INVALID_BASE64;
    if (!jwt || !header || !enc_key || !iv_section || !ciphertext_section || !tag_section ||
        (!key_data && key_size) || !plaintext || !plaintext_size) return CJWTE_INVALID_PARAMETERS;
    *plaintext = NULL; *plaintext_size = 0;
    required = content_key_size(jwt->header.enc);
    if (!required) return CJWTE_HEADER_UNSUPPORTED_ALG;
    if (jwt->header.alg != alg_dir) {
        encrypted_key = b64url_decode_with_alloc((const uint8_t *)enc_key->data, enc_key->len, &encrypted_size);
        if (!encrypted_key) return CJWTE_HEADER_INVALID_BASE64;
    }
    status = crypt_cek(jwt, 1, key_data, key_size, jwk, encrypted_key, encrypted_size, &cek, &cek_size);
    if (status != CJWTE_OK) goto done;
    if (cek_size < required) { status = CJWTE_SIGNATURE_INVALID_KEY; goto done; }
    iv = b64url_decode_with_alloc((const uint8_t *)iv_section->data, iv_section->len, &iv_size);
    ciphertext = b64url_decode_with_alloc((const uint8_t *)ciphertext_section->data, ciphertext_section->len, &ciphertext_size);
    tag = b64url_decode_with_alloc((const uint8_t *)tag_section->data, tag_section->len, &tag_size);
    if (!iv || !ciphertext || !tag) { status = CJWTE_PAYLOAD_INVALID_BASE64; goto done; }
    if (ciphertext_size == SIZE_MAX) { status = CJWTE_INVALID_PARAMETERS; goto done; }
    result = malloc(ciphertext_size + 1);
    if (!result) { status = CJWTE_OUT_OF_MEMORY; goto done; }
    if (salts_crypto_aes_gcm_decrypt(cek, required, iv, iv_size, header->data, header->len,
        ciphertext, ciphertext_size, tag, tag_size, result, ciphertext_size) != SALTS_CRYPTO_OK) {
        status = CJWTE_SIGNATURE_VALIDATION_FAILED; goto done;
    }
    result[ciphertext_size] = 0;
    *plaintext = result; *plaintext_size = ciphertext_size; result = NULL; status = CJWTE_OK;
done:
    salts_crypto_clear(result, result ? ciphertext_size + 1 : 0); free(result);
    salts_crypto_clear(cek, cek_size); free(cek);
    free(encrypted_key); free(iv); free(ciphertext); free(tag);
    return status;
}

cjwt_code_t jwe_pbes2_prepare(cjwt_t *jwt) {
    const char *p2s;
    json_value_t *p2c;
    if (!jwt) return CJWTE_INVALID_PARAMETERS;
    if (!is_pbes2(jwt->header.alg)) return CJWTE_OK;
    if (!jwt->header.private_headers) jwt->header.private_headers = json_create_object();
    if (!jwt->header.private_headers) return CJWTE_OUT_OF_MEMORY;
    p2s = json_get_string(jwt->header.private_headers, "p2s");
    p2c = json_object_get(jwt->header.private_headers, "p2c");
    if (!p2s) {
        uint8_t salt[16];
        char *encoded;
        if (salts_crypto_random(salt, sizeof(salt)) != SALTS_CRYPTO_OK) return CJWTE_SIGNATURE_VALIDATION_FAILED;
        encoded = b64url_encode_with_alloc(salt, sizeof(salt), NULL);
        salts_crypto_clear(salt, sizeof(salt));
        if (!encoded) return CJWTE_OUT_OF_MEMORY;
        json_object_set_string(jwt->header.private_headers, "p2s", encoded);
        free(encoded);
        if (!json_get_string(jwt->header.private_headers, "p2s")) return CJWTE_OUT_OF_MEMORY;
    }
    if (!p2c) {
        json_object_set_number(jwt->header.private_headers, "p2c", 4096);
        if (!json_object_get(jwt->header.private_headers, "p2c")) return CJWTE_OUT_OF_MEMORY;
    }
    return CJWTE_OK;
}

cjwt_code_t jwe_encrypt(const cjwt_t *jwt, const char *header,
    const uint8_t *plaintext, size_t plaintext_size, const uint8_t *key_data, size_t key_size,
    const cjwt_jwk_t *jwk, char **output) {
    static const uint8_t empty = 0;
    uint8_t cek[32] = {0}, iv[12], tag[16], *encrypted_key = NULL, *ciphertext = NULL;
    size_t cek_size, encrypted_size = 0;
    char *encoded_key = NULL, *encoded_iv = NULL, *encoded_content = NULL, *encoded_tag = NULL;
    tstr token = NULL;
    cjwt_code_t status = CJWTE_OUT_OF_MEMORY;
    if (!jwt || !header || (!plaintext && plaintext_size) || (!key_data && key_size) || !output)
        return CJWTE_INVALID_PARAMETERS;
    *output = NULL;
    cek_size = content_key_size(jwt->header.enc);
    if (!cek_size) return CJWTE_HEADER_UNSUPPORTED_ALG;
    if (jwt->header.alg == alg_dir) {
        if (key_size < cek_size) return CJWTE_SIGNATURE_INVALID_KEY;
        memcpy(cek, key_data, cek_size);
    } else if (salts_crypto_random(cek, cek_size) != SALTS_CRYPTO_OK) {
        status = CJWTE_SIGNATURE_VALIDATION_FAILED; goto done;
    }
    status = crypt_cek(jwt, 0, key_data, key_size, jwk, cek, cek_size, &encrypted_key, &encrypted_size);
    if (status != CJWTE_OK) goto done;
    status = CJWTE_OUT_OF_MEMORY;
    if (salts_crypto_random(iv, sizeof(iv)) != SALTS_CRYPTO_OK) {
        status = CJWTE_SIGNATURE_VALIDATION_FAILED; goto done;
    }
    ciphertext = malloc(plaintext_size ? plaintext_size : 1);
    if (!ciphertext) goto done;
    if (salts_crypto_aes_gcm_encrypt(cek, cek_size, iv, sizeof(iv), header, strlen(header),
        plaintext, plaintext_size, ciphertext, plaintext_size, tag, sizeof(tag)) != SALTS_CRYPTO_OK) {
        status = CJWTE_SIGNATURE_VALIDATION_FAILED; goto done;
    }
    encoded_key = b64url_encode_with_alloc(encrypted_key ? encrypted_key : &empty, encrypted_size, NULL);
    encoded_iv = b64url_encode_with_alloc(iv, sizeof(iv), NULL);
    encoded_content = b64url_encode_with_alloc(ciphertext, plaintext_size, NULL);
    encoded_tag = b64url_encode_with_alloc(tag, sizeof(tag), NULL);
    if (!encoded_key || !encoded_iv || !encoded_content || !encoded_tag) goto done;
    token = tstr_format("{}.{}.{}.{}.{}", header, encoded_key, encoded_iv, encoded_content, encoded_tag);
    if (!token) goto done;
    *output = tstr_to_cstr(token);
    if (*output) status = CJWTE_OK;
done:
    tstr_free(token); free(encoded_key); free(encoded_iv); free(encoded_content); free(encoded_tag);
    salts_crypto_clear(cek, sizeof(cek)); salts_crypto_clear(tag, sizeof(tag));
    free(ciphertext); free(encrypted_key);
    return status;
}
