#include "turbo_crypto.h"

#include <limits.h>
#include <string.h>

#include <libecc/curves/curves.h>
#include <libecc/external_deps/rand.h>
#include <libecc/sig/eddsa.h>
#include <libecc/sig/sig_algs.h>

static const uint8_t turbo_crypto_empty_message = 0U;

/* libecc uses randomness to blind secret scalar multiplications. */
int get_random(unsigned char* buf, u16 len) {
    return turbo_crypto_random(buf, (size_t)len) == TURBO_CRYPTO_OK ? 0 : -1;
}

static int buffers_overlap(const void* left, const void* right, size_t size) {
    const uintptr_t left_address = (uintptr_t)left;
    const uintptr_t right_address = (uintptr_t)right;

    return left_address < right_address
               ? right_address - left_address < size
               : left_address - right_address < size;
}

static int ed448_init_params(ec_params* params) {
    const ec_str_params* string_params = NULL;

    memset(params, 0, sizeof(*params));
    if (ec_get_curve_params_by_type(WEI448, &string_params) != 0 ||
        string_params == NULL || import_params(params, string_params) != 0) {
        return TURBO_CRYPTO_ECRYPTO;
    }
    return TURBO_CRYPTO_OK;
}

static int ed448_import_key_pair(
    ec_key_pair* key_pair, const ec_params* params,
    const uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE]) {
    memset(key_pair, 0, sizeof(*key_pair));
    return eddsa_import_key_pair_from_priv_key_buf(
               key_pair, private_key, TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE,
               params, EDDSA448) == 0
               ? TURBO_CRYPTO_OK
               : TURBO_CRYPTO_ECRYPTO;
}

int turbo_crypto_ed448_public_key(
    const uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE]) {
    ec_params params;
    ec_key_pair key_pair;
    int rc;

    if (private_key == NULL || public_key == NULL) {
        return TURBO_CRYPTO_EINVAL;
    }

    memset(&key_pair, 0, sizeof(key_pair));
    rc = ed448_init_params(&params);
    if (rc == TURBO_CRYPTO_OK) {
        rc = ed448_import_key_pair(&key_pair, &params, private_key);
    }
    if (rc == TURBO_CRYPTO_OK &&
        eddsa_export_pub_key(&key_pair.pub_key, public_key,
                             TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE) != 0) {
        rc = TURBO_CRYPTO_ECRYPTO;
    }

    turbo_crypto_wipe(&key_pair, sizeof(key_pair));
    turbo_crypto_wipe(&params, sizeof(params));
    if (rc != TURBO_CRYPTO_OK) {
        turbo_crypto_wipe(public_key, TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE);
    }
    return rc;
}

int turbo_crypto_ed448_keygen(
    uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE]) {
    int rc;

    if (private_key == NULL || public_key == NULL ||
        buffers_overlap(private_key, public_key,
                        TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE)) {
        return TURBO_CRYPTO_EINVAL;
    }

    rc = turbo_crypto_random(private_key, TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE);
    if (rc == TURBO_CRYPTO_OK) {
        rc = turbo_crypto_ed448_public_key(private_key, public_key);
    }
    if (rc != TURBO_CRYPTO_OK) {
        turbo_crypto_wipe(private_key, TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE);
        turbo_crypto_wipe(public_key, TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE);
    }
    return rc;
}

int turbo_crypto_ed448_sign(
    const uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    const void* data, size_t data_len,
    uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE]) {
    ec_params params;
    ec_key_pair key_pair;
    const uint8_t* message = (const uint8_t*)data;
    u8 signature_len = 0U;
    int rc;

    if (private_key == NULL || signature == NULL ||
        (data == NULL && data_len != 0U) || data_len > UINT32_MAX) {
        return TURBO_CRYPTO_EINVAL;
    }
    if (message == NULL) {
        message = &turbo_crypto_empty_message;
    }

    memset(&key_pair, 0, sizeof(key_pair));
    rc = ed448_init_params(&params);
    if (rc == TURBO_CRYPTO_OK) {
        rc = ed448_import_key_pair(&key_pair, &params, private_key);
    }
    if (rc == TURBO_CRYPTO_OK &&
        (ec_get_sig_len(&params, EDDSA448, SHAKE256, &signature_len) != 0 ||
         signature_len != TURBO_CRYPTO_ED448_SIGNATURE_SIZE)) {
        rc = TURBO_CRYPTO_ECRYPTO;
    }
    if (rc == TURBO_CRYPTO_OK &&
        ec_sign(signature, signature_len, &key_pair, message, (u32)data_len,
                EDDSA448, SHAKE256, NULL, 0U) != 0) {
        rc = TURBO_CRYPTO_ECRYPTO;
    }

    turbo_crypto_wipe(&key_pair, sizeof(key_pair));
    turbo_crypto_wipe(&params, sizeof(params));
    if (rc != TURBO_CRYPTO_OK) {
        turbo_crypto_wipe(signature, TURBO_CRYPTO_ED448_SIGNATURE_SIZE);
    }
    return rc;
}

int turbo_crypto_ed448_verify(
    const uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE],
    const void* data, size_t data_len,
    const uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE]) {
    ec_params params;
    ec_pub_key imported_public_key;
    const uint8_t* message = (const uint8_t*)data;
    int rc;

    if (public_key == NULL || signature == NULL ||
        (data == NULL && data_len != 0U) || data_len > UINT32_MAX) {
        return TURBO_CRYPTO_EINVAL;
    }
    if (message == NULL) {
        message = &turbo_crypto_empty_message;
    }

    memset(&imported_public_key, 0, sizeof(imported_public_key));
    rc = ed448_init_params(&params);
    if (rc == TURBO_CRYPTO_OK &&
        eddsa_import_pub_key(&imported_public_key, public_key,
                             TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE, &params,
                             EDDSA448) != 0) {
        rc = TURBO_CRYPTO_EVERIFY;
    }
    if (rc == TURBO_CRYPTO_OK &&
        ec_verify(signature, TURBO_CRYPTO_ED448_SIGNATURE_SIZE,
                  &imported_public_key, message, (u32)data_len, EDDSA448,
                  SHAKE256, NULL, 0U) != 0) {
        rc = TURBO_CRYPTO_EVERIFY;
    }

    turbo_crypto_wipe(&imported_public_key, sizeof(imported_public_key));
    turbo_crypto_wipe(&params, sizeof(params));
    return rc;
}
