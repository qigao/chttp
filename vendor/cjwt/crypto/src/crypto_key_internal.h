#ifndef SALTS_CRYPTO_KEY_INTERNAL_H
#define SALTS_CRYPTO_KEY_INTERNAL_H
#include "cjwt_crypto.h"
#include <gmssl/rsa_components.h>
struct cjwt_crypto_key {
  cjwt_crypto_key_kind kind;
  cjwt_crypto_ec_curve curve;
  size_t scalar_size;
  int has_private;
  union {
    RSA_COMPONENTS rsa;
    struct { uint8_t public_key[132], private_key[66]; } ec;
  } value;
};
/* libecc adapter TU has its own compile definitions, unlike GmSSL adapters. */
cjwt_crypto_ec_curve cjwt_crypto_ec_identify(cjwt_crypto_bytes p, cjwt_crypto_bytes a,
    cjwt_crypto_bytes b, cjwt_crypto_bytes generator, cjwt_crypto_bytes order,
    cjwt_crypto_bytes cofactor);
int cjwt_crypto_ec_decode(cjwt_crypto_ec_curve curve, const uint8_t *input, size_t size,
    uint8_t *output, size_t capacity);
int cjwt_crypto_ec_sign_hash(cjwt_crypto_ec_curve curve, cjwt_crypto_hash hash,
    const uint8_t *key, size_t key_size, const void *message, size_t size, uint8_t *signature, size_t signature_size);
int cjwt_crypto_ec_verify_hash(cjwt_crypto_ec_curve curve, cjwt_crypto_hash hash,
    const uint8_t *key, size_t key_size, const void *message, size_t size, const uint8_t *signature, size_t signature_size);
#endif
