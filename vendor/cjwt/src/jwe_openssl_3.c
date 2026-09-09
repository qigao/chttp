/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <string.h>
#include <stdlib.h>

#include "cjwt.h"
#include "jwe.h"
#include "jws.h"
#include "utils.h"
#include <turbo_crypto.h>

static int aes_kw_unwrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t in_len, uint8_t *out) {
    AES_KEY aes_key;
    if ((kek_len != 16U && kek_len != 24U && kek_len != 32U)
        || in_len < 16U || (in_len % 8U) != 0U
        || AES_set_decrypt_key(kek, (unsigned)(kek_len * 8U), &aes_key) != 0) {
        return -1;
    }
    return AES_unwrap_key(&aes_key, NULL, out, in, in_len);
}

static int aes_kw_wrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t in_len, uint8_t *out) {
    AES_KEY aes_key;
    if ((kek_len != 16U && kek_len != 24U && kek_len != 32U)
        || in_len < 8U || (in_len % 8U) != 0U
        || AES_set_encrypt_key(kek, (unsigned)(kek_len * 8U), &aes_key) != 0) {
        return -1;
    }
    return AES_wrap_key(&aes_key, NULL, out, in, in_len);
}

static cjwt_code_t decrypt_cek(const cjwt_t *jwt,
                               const struct section *enc_key,
                               const uint8_t *key_data, size_t key_len,
                               const cjwt_jwk_t *jwk,
                               uint8_t **cek, size_t *cek_len)
{
    cjwt_code_t rv = CJWTE_OK;
    uint8_t *raw_enc_key = NULL;
    size_t raw_enc_key_len = 0;

    if (jwt->header.alg == alg_dir) {
        *cek = malloc(key_len);
        if (!*cek) return CJWTE_OUT_OF_MEMORY;
        memcpy(*cek, key_data, key_len);
        *cek_len = key_len;
        return CJWTE_OK;
    }

    /* Decode encrypted key */
    raw_enc_key = b64url_decode_with_alloc((const uint8_t *)enc_key->data, enc_key->len, &raw_enc_key_len);
    if (!raw_enc_key) return CJWTE_HEADER_INVALID_BASE64;

    if (jwt->header.alg == alg_rsa_oaep || jwt->header.alg == alg_rsa_oaep_256) {
        EVP_PKEY *pkey = NULL;
        EVP_PKEY_CTX *ctx = NULL;
        BIO *bio = NULL;
        jws_pkey_type_t pkey_type = JWS_PKEY_EVP;

        if (jwk) {
            rv = jws_jwk_to_pkey(jwk, (void **)&pkey, &pkey_type);
            if (pkey && pkey_type != JWS_PKEY_EVP) {
                jws_pkey_free(pkey, pkey_type);
                pkey = NULL;
            }
        } else {
            bio = BIO_new_mem_buf(key_data, (int)key_len);
            pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
            BIO_free(bio);
        }

        if (!pkey) { free(raw_enc_key); return CJWTE_SIGNATURE_INVALID_KEY; }

        ctx = EVP_PKEY_CTX_new(pkey, NULL);
        if (!ctx || EVP_PKEY_decrypt_init(ctx) <= 0) {
            rv = CJWTE_SIGNATURE_INVALID_KEY;
            goto rsa_cleanup;
        }

        if (jwt->header.alg == alg_rsa_oaep) {
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING);
            EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha1());
            EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha1());
        } else {
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING);
            EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256());
            EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256());
        }

        if (EVP_PKEY_decrypt(ctx, NULL, cek_len, raw_enc_key, raw_enc_key_len) <= 0) {
            rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
            goto rsa_cleanup;
        }

        *cek = malloc(*cek_len);
        if (!*cek) { rv = CJWTE_OUT_OF_MEMORY; goto rsa_cleanup; }

        if (EVP_PKEY_decrypt(ctx, *cek, cek_len, raw_enc_key, raw_enc_key_len) <= 0) {
            free(*cek); *cek = NULL;
            rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        }
rsa_cleanup:
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
    } else if (jwt->header.alg >= alg_a128kw && jwt->header.alg <= alg_a256kw) {
        size_t expected_kek_len = 0;
        if (jwt->header.alg == alg_a128kw) expected_kek_len = 16;
        else if (jwt->header.alg == alg_a192kw) expected_kek_len = 24;
        else if (jwt->header.alg == alg_a256kw) expected_kek_len = 32;

        if (key_len != expected_kek_len) {
            free(raw_enc_key); return CJWTE_SIGNATURE_INVALID_KEY;
        }

        *cek = malloc(raw_enc_key_len);
        int unwrapped_len = aes_kw_unwrap(key_data, key_len, raw_enc_key, raw_enc_key_len, *cek);
        if (unwrapped_len <= 0) {
            free(*cek); *cek = NULL;
            rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        } else {
            *cek_len = (size_t)unwrapped_len;
        }
    } else if (jwt->header.alg >= alg_pbes2_hs256_a128kw && jwt->header.alg <= alg_pbes2_hs512_a256kw) {
        const char *alg_str = alg_to_string(jwt->header.alg);
        const EVP_MD *md = NULL;
        size_t kek_len = 0;

        if (jwt->header.alg == alg_pbes2_hs256_a128kw) { kek_len = 16; }
        else if (jwt->header.alg == alg_pbes2_hs384_a192kw) { md = EVP_sha384(); kek_len = 24; }
        else if (jwt->header.alg == alg_pbes2_hs512_a256kw) { md = EVP_sha512(); kek_len = 32; }

        /* Extract p2s and p2c */
        const char *p2s_str = json_get_string(jwt->header.private_headers, "p2s");
        json_value_t *p2c_val = json_object_get(jwt->header.private_headers, "p2c");
        if (!p2s_str || !p2c_val || json_type(p2c_val) != JSON_NUMBER) {
            free(raw_enc_key); return CJWTE_HEADER_MISSING;
        }

        size_t p2s_raw_len = 0;
        uint8_t *p2s_raw = b64url_decode_with_alloc((const uint8_t *)p2s_str, strlen(p2s_str), &p2s_raw_len);
        if (!p2s_raw) { free(raw_enc_key); return CJWTE_HEADER_INVALID_BASE64; }

        /* PBKDF2 Salt: alg_str || 0x00 || p2s_raw */
        size_t salt_len = strlen(alg_str) + 1 + p2s_raw_len;
        uint8_t *salt = malloc(salt_len);
        memcpy(salt, alg_str, strlen(alg_str));
        salt[strlen(alg_str)] = 0;
        memcpy(salt + strlen(alg_str) + 1, p2s_raw, p2s_raw_len);

        uint8_t kek[32];
        int p2c = (int)json_number(p2c_val);
        if (p2c <= 0) {
            free(raw_enc_key); free(p2s_raw); free(salt);
            return CJWTE_INVALID_PARAMETERS;
        }
        int kdf_ok = jwt->header.alg == alg_pbes2_hs256_a128kw
                         ? turbo_crypto_pbkdf2_hmac_sha256(
                               key_data, key_len, salt, salt_len, (uint32_t)p2c,
                               kek, kek_len) == TURBO_CRYPTO_OK
                         : PKCS5_PBKDF2_HMAC((const char *)key_data, (int)key_len,
                                             salt, (int)salt_len, p2c, md,
                                             (int)kek_len, kek) > 0;
        if (!kdf_ok) {
            turbo_crypto_wipe(kek, sizeof(kek));
            free(raw_enc_key); free(p2s_raw); free(salt); return CJWTE_SIGNATURE_VALIDATION_FAILED;
        }

        /* Unwrap CEK */
        *cek = malloc(raw_enc_key_len); /* KW unwrap output is smaller or equal */
        int unwrapped_len = aes_kw_unwrap(kek, kek_len, raw_enc_key, raw_enc_key_len, *cek);
        if (unwrapped_len <= 0) {
            free(*cek); *cek = NULL;
            rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        } else {
            *cek_len = (size_t)unwrapped_len;
        }

        turbo_crypto_wipe(kek, sizeof(kek));
        free(p2s_raw); free(salt);
    } else {
        rv = CJWTE_SIGNATURE_UNSUPPORTED_ALG;
    }

    free(raw_enc_key);
    return rv;
}

static cjwt_code_t decrypt_content(const cjwt_t *jwt,
                                   const struct section *iv_s,
                                   const struct section *ct_s,
                                   const struct section *tag_s,
                                   const uint8_t *cek, size_t cek_len,
                                   const struct section *header_s,
                                   uint8_t **plaintext, size_t *plaintext_len)
{
    cjwt_code_t rv = CJWTE_OK;
    uint8_t *iv = NULL, *ct = NULL, *tag = NULL;
    size_t iv_len = 0, ct_len = 0, tag_len = 0;
    const EVP_CIPHER *cipher = NULL;

    iv = b64url_decode_with_alloc((const uint8_t *)iv_s->data, iv_s->len, &iv_len);
    ct = b64url_decode_with_alloc((const uint8_t *)ct_s->data, ct_s->len, &ct_len);
    tag = b64url_decode_with_alloc((const uint8_t *)tag_s->data, tag_s->len, &tag_len);

    if (!ct || !tag) { rv = CJWTE_PAYLOAD_INVALID_BASE64; goto cleanup; }

    if (jwt->header.enc == enc_a128gcm) cipher = EVP_aes_128_gcm();
    else if (jwt->header.enc == enc_a192gcm) cipher = EVP_aes_192_gcm();
    else if (jwt->header.enc == enc_a256gcm) cipher = EVP_aes_256_gcm();
    else { rv = CJWTE_HEADER_UNSUPPORTED_ALG; goto cleanup; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;

    if (!ctx || EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) <= 0) {
        rv = CJWTE_OUT_OF_MEMORY;
        goto gcm_cleanup;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)iv_len, NULL) <= 0) {
        rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        goto gcm_cleanup;
    }

    if (EVP_DecryptInit_ex(ctx, NULL, NULL, cek, iv) <= 0) {
        rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        goto gcm_cleanup;
    }

    /* Set AAD */
    if (EVP_DecryptUpdate(ctx, NULL, &len, (const uint8_t *)header_s->data, (int)header_s->len) <= 0) {
        rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        goto gcm_cleanup;
    }

    *plaintext = malloc(ct_len + 1);
    if (!*plaintext) { rv = CJWTE_OUT_OF_MEMORY; goto gcm_cleanup; }

    if (EVP_DecryptUpdate(ctx, *plaintext, &len, ct, (int)ct_len) <= 0) {
        rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        goto gcm_cleanup;
    }
    *plaintext_len = len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)tag_len, tag) <= 0) {
        rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        goto gcm_cleanup;
    }

    if (EVP_DecryptFinal_ex(ctx, *plaintext + len, &len) <= 0) {
        rv = CJWTE_SIGNATURE_VALIDATION_FAILED;
        goto gcm_cleanup;
    }
    *plaintext_len += len;
    (*plaintext)[*plaintext_len] = '\0';

gcm_cleanup:
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    if (rv != CJWTE_OK && *plaintext) { free(*plaintext); *plaintext = NULL; }

cleanup:
    free(iv); free(ct); free(tag);
    return rv;
}

cjwt_code_t jwe_decrypt(const cjwt_t *jwt,
                        const struct section *header,
                        const struct section *enc_key,
                        const struct section *iv,
                        const struct section *ciphertext,
                        const struct section *tag,
                        const uint8_t *key_data, size_t key_len,
                        const cjwt_jwk_t *jwk,
                        uint8_t **plaintext, size_t *plaintext_len)
{
    uint8_t *cek = NULL;
    size_t cek_len = 0;
    cjwt_code_t rv;

    /* Get CEK */
    rv = decrypt_cek(jwt, enc_key, key_data, key_len, jwk, &cek, &cek_len);
    if (rv != CJWTE_OK) return rv;

    /* Decrypt content */
    rv = decrypt_content(jwt, iv, ciphertext, tag, cek, cek_len, header, plaintext, plaintext_len);
    
    free(cek);
    return rv;
}

cjwt_code_t jwe_pbes2_prepare(cjwt_t *jwt)
{
    if (jwt->header.alg >= alg_pbes2_hs256_a128kw && jwt->header.alg <= alg_pbes2_hs512_a256kw) {
        const char *p2s_str = jwt->header.private_headers ? json_get_string(jwt->header.private_headers, "p2s") : NULL;
        json_value_t *p2c_val = jwt->header.private_headers ? json_object_get(jwt->header.private_headers, "p2c") : NULL;

        if (!p2s_str) {
            uint8_t p2s_raw[16];
            if (turbo_crypto_random(p2s_raw, sizeof(p2s_raw)) != TURBO_CRYPTO_OK) {
                return CJWTE_OUT_OF_MEMORY;
            }
            char *p2s_b64 = b64url_encode_with_alloc(p2s_raw, sizeof(p2s_raw), NULL);
            turbo_crypto_wipe(p2s_raw, sizeof(p2s_raw));
            if (!p2s_b64) return CJWTE_OUT_OF_MEMORY;
            if (!jwt->header.private_headers) jwt->header.private_headers = json_create_object();
            json_object_set_string(jwt->header.private_headers, "p2s", p2s_b64);
            free(p2s_b64);
        }

        if (!p2c_val) {
            if (!jwt->header.private_headers) jwt->header.private_headers = json_create_object();
            json_object_set_number(jwt->header.private_headers, "p2c", 4096);
        }
    }
    return CJWTE_OK;
}

static cjwt_code_t encrypt_cek(const cjwt_t *jwt, const uint8_t *key_data, size_t key_len,
                               const cjwt_jwk_t *jwk, uint8_t *cek, size_t cek_len,
                               uint8_t **enc_cek, size_t *enc_cek_len)
{
    if (jwt->header.alg == alg_dir) {
        *enc_cek = NULL;
        *enc_cek_len = 0;
        return CJWTE_OK;
    }

    if (jwt->header.alg == alg_rsa_oaep || jwt->header.alg == alg_rsa_oaep_256) {
        EVP_PKEY *pkey = NULL;
        EVP_PKEY_CTX *ctx = NULL;
        BIO *bio = NULL;
        jws_pkey_type_t pkey_type = JWS_PKEY_EVP;

        if (jwk) {
            jws_jwk_to_pkey(jwk, (void **)&pkey, &pkey_type);
            if (pkey && pkey_type != JWS_PKEY_EVP) {
                jws_pkey_free(pkey, pkey_type);
                pkey = NULL;
            }
        } else {
            bio = BIO_new_mem_buf(key_data, (int)key_len);
            pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
            BIO_free(bio);
        }

        if (!pkey) return CJWTE_SIGNATURE_INVALID_KEY;

        ctx = EVP_PKEY_CTX_new(pkey, NULL);
        if (!ctx || EVP_PKEY_encrypt_init(ctx) <= 0) {
            EVP_PKEY_free(pkey);
            return CJWTE_SIGNATURE_INVALID_KEY;
        }

        if (jwt->header.alg == alg_rsa_oaep) {
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING);
            EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha1());
            EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha1());
        } else {
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING);
            EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256());
            EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256());
        }

        if (EVP_PKEY_encrypt(ctx, NULL, enc_cek_len, cek, cek_len) <= 0) {
            EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(pkey);
            return CJWTE_SIGNATURE_VALIDATION_FAILED;
        }

        *enc_cek = malloc(*enc_cek_len);
        if (EVP_PKEY_encrypt(ctx, *enc_cek, enc_cek_len, cek, cek_len) <= 0) {
            free(*enc_cek); *enc_cek = NULL;
            EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(pkey);
            return CJWTE_SIGNATURE_VALIDATION_FAILED;
        }

        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return CJWTE_OK;
    }

    if (jwt->header.alg >= alg_a128kw && jwt->header.alg <= alg_a256kw) {
        size_t expected_kek_len = 0;
        if (jwt->header.alg == alg_a128kw) expected_kek_len = 16;
        else if (jwt->header.alg == alg_a192kw) expected_kek_len = 24;
        else if (jwt->header.alg == alg_a256kw) expected_kek_len = 32;

        if (key_len != expected_kek_len) return CJWTE_SIGNATURE_INVALID_KEY;

        *enc_cek_len = cek_len + 8;
        *enc_cek = malloc(*enc_cek_len);
        int wrapped_len = aes_kw_wrap(key_data, key_len, cek, cek_len, *enc_cek);
        *enc_cek_len = (size_t)wrapped_len;
        return CJWTE_OK;
    }

    if (jwt->header.alg >= alg_pbes2_hs256_a128kw && jwt->header.alg <= alg_pbes2_hs512_a256kw) {
        const char *alg_str = alg_to_string(jwt->header.alg);
        const EVP_MD *md = NULL;
        size_t kek_len = 0;

        if (jwt->header.alg == alg_pbes2_hs256_a128kw) { kek_len = 16; }
        else if (jwt->header.alg == alg_pbes2_hs384_a192kw) { md = EVP_sha384(); kek_len = 24; }
        else if (jwt->header.alg == alg_pbes2_hs512_a256kw) { md = EVP_sha512(); kek_len = 32; }

        /* p2s and p2c handling - should already be in headers */
        const char *p2s_str = json_get_string(jwt->header.private_headers, "p2s");
        json_value_t *p2c_val = json_object_get(jwt->header.private_headers, "p2c");

        if (!p2s_str || !p2c_val || json_type(p2c_val) != JSON_NUMBER) {
            return CJWTE_HEADER_MISSING;
        }

        uint8_t p2s_raw[32];
        size_t plen = 0;
        uint8_t *p = b64url_decode_with_alloc((const uint8_t *)p2s_str, strlen(p2s_str), &plen);
        if (!p) return CJWTE_HEADER_INVALID_BASE64;
        memcpy(p2s_raw, p, (plen > 32) ? 32 : plen);
        size_t actual_p2s_len = (plen > 32) ? 32 : plen;
        free(p);

        int p2c = (int)json_number(p2c_val);
        if (p2c <= 0) {
            turbo_crypto_wipe(p2s_raw, sizeof(p2s_raw));
            return CJWTE_INVALID_PARAMETERS;
        }

        /* Derive KEK */
        size_t salt_len = strlen(alg_str) + 1 + actual_p2s_len;
        uint8_t *salt = malloc(salt_len);
        memcpy(salt, alg_str, strlen(alg_str));
        salt[strlen(alg_str)] = 0;
        memcpy(salt + strlen(alg_str) + 1, p2s_raw, actual_p2s_len);

        uint8_t kek[32];
        int kdf_ok = jwt->header.alg == alg_pbes2_hs256_a128kw
                         ? turbo_crypto_pbkdf2_hmac_sha256(
                               key_data, key_len, salt, salt_len, (uint32_t)p2c,
                               kek, kek_len) == TURBO_CRYPTO_OK
                         : PKCS5_PBKDF2_HMAC((const char *)key_data, (int)key_len,
                                             salt, (int)salt_len, p2c, md,
                                             (int)kek_len, kek) > 0;
        if (!kdf_ok) {
            turbo_crypto_wipe(kek, sizeof(kek));
            free(salt);
            return CJWTE_SIGNATURE_VALIDATION_FAILED;
        }

        /* Wrap CEK */
        *enc_cek_len = cek_len + 8; /* AES KW overhead */
        *enc_cek = malloc(*enc_cek_len);
        int wrapped_len = aes_kw_wrap(kek, kek_len, cek, cek_len, *enc_cek);
        *enc_cek_len = (size_t)wrapped_len;

        turbo_crypto_wipe(kek, sizeof(kek));
        turbo_crypto_wipe(p2s_raw, sizeof(p2s_raw));
        free(salt);
        return CJWTE_OK;
    }

    return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
}

cjwt_code_t jwe_encrypt(const cjwt_t *jwt, const char *header_b64,
                        const uint8_t *plaintext, size_t plaintext_len,
                        const uint8_t *key_data, size_t key_len,
                        const cjwt_jwk_t *jwk, char **output)
{
    cjwt_code_t rv = CJWTE_OK;
    uint8_t cek[64];
    size_t cek_len = 0;
    uint8_t *enc_cek = NULL;
    size_t enc_cek_len = 0;
    uint8_t iv[12];
    uint8_t *ciphertext = NULL;
    uint8_t tag[16];
    const EVP_CIPHER *cipher = NULL;

    if (jwt->header.enc == enc_a128gcm) { cipher = EVP_aes_128_gcm(); cek_len = 16; }
    else if (jwt->header.enc == enc_a192gcm) { cipher = EVP_aes_192_gcm(); cek_len = 24; }
    else if (jwt->header.enc == enc_a256gcm) { cipher = EVP_aes_256_gcm(); cek_len = 32; }
    else return CJWTE_HEADER_UNSUPPORTED_ALG;

    if (jwt->header.alg == alg_dir) {
        if (key_len < cek_len) return CJWTE_SIGNATURE_INVALID_KEY;
        memcpy(cek, key_data, cek_len);
    } else {
        if (turbo_crypto_random(cek, cek_len) != TURBO_CRYPTO_OK)
            return CJWTE_OUT_OF_MEMORY;
    }

    rv = encrypt_cek(jwt, key_data, key_len, jwk, cek, cek_len, &enc_cek, &enc_cek_len);
    if (rv != CJWTE_OK) {
        turbo_crypto_wipe(cek, sizeof(cek));
        return rv;
    }

    if (turbo_crypto_random(iv, sizeof(iv)) != TURBO_CRYPTO_OK) {
        free(enc_cek);
        turbo_crypto_wipe(cek, sizeof(cek));
        return CJWTE_OUT_OF_MEMORY;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    EVP_EncryptInit_ex(ctx, cipher, NULL, cek, iv);
    EVP_EncryptUpdate(ctx, NULL, &len, (const uint8_t *)header_b64, (int)strlen(header_b64));
    
    ciphertext = malloc(plaintext_len);
    EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, (int)plaintext_len);
    EVP_EncryptFinal_ex(ctx, ciphertext + len, &len);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);

    char *enc_cek_b64 = b64url_encode_with_alloc(enc_cek, enc_cek_len, NULL);
    char *iv_b64 = b64url_encode_with_alloc(iv, sizeof(iv), NULL);
    char *ct_b64 = b64url_encode_with_alloc(ciphertext, plaintext_len, NULL);
    char *tag_b64 = b64url_encode_with_alloc(tag, sizeof(tag), NULL);

    *output = malloc(strlen(header_b64) + 1 + (enc_cek_b64 ? strlen(enc_cek_b64) : 0) + 1 +
                    strlen(iv_b64) + 1 + strlen(ct_b64) + 1 + strlen(tag_b64) + 1);
    sprintf(*output, "%s.%s.%s.%s.%s", header_b64, enc_cek_b64 ? enc_cek_b64 : "", iv_b64, ct_b64, tag_b64);

    free(enc_cek); free(ciphertext);
    free(enc_cek_b64); free(iv_b64); free(ct_b64); free(tag_b64);
    turbo_crypto_wipe(cek, sizeof(cek));
    turbo_crypto_wipe(tag, sizeof(tag));
    
    return CJWTE_OK;
}
