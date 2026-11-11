#include "cjwt_crypto.h"
#include <gmssl/aes.h>
#include <gmssl/digest.h>
#include <gmssl/hmac.h>
#include <gmssl/pbkdf2.h>
#include <gmssl/mem.h>
#include <gmssl/rand.h>
#include <string.h>

static const DIGEST *crypto_digest(cjwt_crypto_hash hash) {
  switch (hash) {
  case SALTS_CRYPTO_SHA1: return DIGEST_sha1();
  case SALTS_CRYPTO_SHA256: return DIGEST_sha256();
  case SALTS_CRYPTO_SHA384: return DIGEST_sha384();
  case SALTS_CRYPTO_SHA512: return DIGEST_sha512();
  default: return NULL;
  }
}

void cjwt_crypto_clear(void *data, size_t size) {
  if (data && size) gmssl_secure_clear(data, size);
}

int cjwt_crypto_equal(const void *a, const void *b, size_t size) {
  if ((!a || !b) && size) return SALTS_CRYPTO_EINVAL;
  return !size || gmssl_secure_memcmp(a, b, size) == 0 ? SALTS_CRYPTO_OK : SALTS_CRYPTO_EVERIFY;
}

int cjwt_crypto_random(void *output, size_t size) {
  uint8_t *p = output;
  size_t left = size;
  if (!output && size) return SALTS_CRYPTO_EINVAL;
  while (left) {
    size_t take = left > 256 ? 256 : left;
    if (rand_bytes(p, take) != 1) {
      cjwt_crypto_clear(output, size);
      return SALTS_CRYPTO_ERANDOM;
    }
    p += take;
    left -= take;
  }
  return SALTS_CRYPTO_OK;
}

int cjwt_crypto_digest(cjwt_crypto_hash hash, const void *data, size_t size,
    uint8_t *output, size_t capacity, size_t *output_size) {
  const DIGEST *md = crypto_digest(hash);
  uint8_t result[DIGEST_MAX_SIZE];
  size_t n = 0;
  int status = SALTS_CRYPTO_ECRYPTO;
  if (output_size) *output_size = 0;
  if (!md || (!data && size) || !output || !output_size || capacity < md->digest_size)
    return SALTS_CRYPTO_EINVAL;
  if (digest(md, data, size, result, &n) == 1 && n == md->digest_size) {
    memcpy(output, result, n);
    *output_size = n;
    status = SALTS_CRYPTO_OK;
  }
  cjwt_crypto_clear(result, sizeof(result));
  return status;
}

int cjwt_crypto_hmac(cjwt_crypto_hash hash, const void *key, size_t key_size,
    const void *data, size_t size, uint8_t *output, size_t capacity, size_t *output_size) {
  const DIGEST *md = crypto_digest(hash);
  uint8_t result[DIGEST_MAX_SIZE];
  size_t n = 0;
  int status = SALTS_CRYPTO_ECRYPTO;
  if (output_size) *output_size = 0;
  if (!md || (!key && key_size) || (!data && size) || !output || !output_size ||
      capacity < md->digest_size) return SALTS_CRYPTO_EINVAL;
  if (hmac(md, key, key_size, data, size, result, &n) == 1 && n == md->digest_size) {
    memcpy(output, result, n);
    *output_size = n;
    status = SALTS_CRYPTO_OK;
  }
  cjwt_crypto_clear(result, sizeof(result));
  return status;
}

int cjwt_crypto_pbkdf2(cjwt_crypto_hash hash, const void *password, size_t password_size,
    const void *salt, size_t salt_size, uint32_t iterations, uint8_t *output, size_t size) {
  const DIGEST *md = crypto_digest(hash);
  if (!md || (!password && password_size) || (!salt && salt_size) ||
      !iterations || !output || !size ||
      (uint64_t)size > (uint64_t)UINT32_MAX * md->digest_size) return SALTS_CRYPTO_EINVAL;
  if (pbkdf2_hmac_genkey(md, password, password_size, salt, salt_size, iterations, size, output) == 1)
    return SALTS_CRYPTO_OK;
  cjwt_crypto_clear(output, size);
  return SALTS_CRYPTO_ECRYPTO;
}

static int gcm_arguments(const uint8_t *key, size_t key_size, const uint8_t *iv, size_t iv_size,
    const void *aad, size_t aad_size, const uint8_t *input, size_t size,
    uint8_t *output, size_t capacity, const uint8_t *tag, size_t tag_size) {
  return key && (key_size == 16 || key_size == 24 || key_size == 32) &&
    iv && iv_size && (uint64_t)iv_size < AES_GCM_IV_MAX_SIZE &&
    (aad || !aad_size) && (uint64_t)aad_size < AES_GCM_MAX_AAD_SIZE &&
    (input || !size) && (output || !size) && capacity >= size &&
    (uint64_t)size <= AES_GCM_MAX_PLAINTEXT_SIZE && tag && tag_size && tag_size <= 16;
}

int cjwt_crypto_aes_gcm_encrypt(const uint8_t *key, size_t key_size,
    const uint8_t *iv, size_t iv_size, const void *aad, size_t aad_size,
    const uint8_t *input, size_t size, uint8_t *output, size_t capacity,
    uint8_t *tag, size_t tag_size) {
  AES_KEY state;
  int ret;
  if (!gcm_arguments(key, key_size, iv, iv_size, aad, aad_size, input, size,
                    output, capacity, tag, tag_size)) return SALTS_CRYPTO_EINVAL;
  ret = aes_set_encrypt_key(&state, key, key_size);
  if (ret == 1) ret = aes_gcm_encrypt(&state, iv, iv_size, aad, aad_size, input, size, output, tag_size, tag);
  cjwt_crypto_clear(&state, sizeof(state));
  return ret == 1 ? SALTS_CRYPTO_OK : SALTS_CRYPTO_ECRYPTO;
}

int cjwt_crypto_aes_gcm_decrypt(const uint8_t *key, size_t key_size,
    const uint8_t *iv, size_t iv_size, const void *aad, size_t aad_size,
    const uint8_t *input, size_t size, const uint8_t *tag, size_t tag_size,
    uint8_t *output, size_t capacity) {
  AES_KEY state;
  int ret;
  if (!gcm_arguments(key, key_size, iv, iv_size, aad, aad_size, input, size,
                    output, capacity, tag, tag_size)) return SALTS_CRYPTO_EINVAL;
  ret = aes_set_encrypt_key(&state, key, key_size);
  if (ret == 1) ret = aes_gcm_decrypt(&state, iv, iv_size, aad, aad_size, input, size, tag, tag_size, output);
  cjwt_crypto_clear(&state, sizeof(state));
  return ret == 1 ? SALTS_CRYPTO_OK : SALTS_CRYPTO_EVERIFY;
}
