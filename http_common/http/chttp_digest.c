#include "chttp_digest.h"

#include <core/error_codes.h>
#include <crypto/sha256.h>
#include <crypto/symmetric.h>
#include <stdlib.h>

static int digest_status(int status) {
  switch (status) {
  case SALTS_CRYPTO_OK:
    return SALTS_OK;
  case SALTS_CRYPTO_EINVAL:
  case SALTS_CRYPTO_ESTATE:
    return SALTS_EINVAL;
  case SALTS_CRYPTO_ENOMEM:
    return SALTS_ENOMEM;
  default:
    return SALTS_EIO;
  }
}

int chttp_md5(const void *data, size_t size, uint8_t out[SALTS_MD5_DIGEST_BYTES]) {
  return digest_status(salts_crypto_md5(data, size, out));
}

int chttp_sha1(const void *data, size_t size, uint8_t out[SALTS_SHA1_DIGEST_BYTES]) {
  size_t output_size;
  return digest_status(salts_crypto_digest(SALTS_CRYPTO_SHA1, data, size, out,
                                           SALTS_SHA1_DIGEST_BYTES, &output_size));
}

struct chttp_sha256_stream {
  salts_crypto_sha256_ctx_t context;
};

int chttp_sha256(const void *data, size_t size, uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  return digest_status(salts_crypto_sha256(data, size, out));
}

int chttp_sha256_stream_create(chttp_sha256_stream **out_stream) {
  chttp_sha256_stream *stream;
  int status;
  if (out_stream == NULL) return SALTS_EINVAL;
  *out_stream = NULL;
  stream = (chttp_sha256_stream *)calloc(1u, sizeof(*stream));
  if (stream == NULL) return SALTS_ENOMEM;
  status = salts_crypto_sha256_init(&stream->context);
  if (status != SALTS_CRYPTO_OK) {
    free(stream);
    return digest_status(status);
  }
  *out_stream = stream;
  return SALTS_OK;
}

int chttp_sha256_stream_update(chttp_sha256_stream *stream, const void *data, size_t size) {
  if (stream == NULL) return SALTS_EINVAL;
  return digest_status(salts_crypto_sha256_update(&stream->context, data, size));
}

int chttp_sha256_stream_finish(chttp_sha256_stream *stream,
                               uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  if (stream == NULL) return SALTS_EINVAL;
  return digest_status(salts_crypto_sha256_final(&stream->context, out));
}

void chttp_sha256_stream_destroy(chttp_sha256_stream *stream) {
  if (stream == NULL) return;
  salts_crypto_clear(stream, sizeof(*stream));
  free(stream);
}

int chttp_hmac_sha256(const void *key, size_t key_size, const void *data, size_t data_size,
                      uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  size_t output_size;
  return digest_status(salts_crypto_hmac(SALTS_CRYPTO_SHA256, key, key_size, data, data_size, out,
                                         SALTS_SHA256_DIGEST_BYTES, &output_size));
}

int chttp_crypto_equal(const void *lhs, const void *rhs, size_t size, int *out_equal) {
  int status;
  if (out_equal == NULL) return SALTS_EINVAL;
  status = salts_crypto_equal(lhs, rhs, size);
  if (status != SALTS_CRYPTO_OK && status != SALTS_CRYPTO_EVERIFY) return digest_status(status);
  *out_equal = status == SALTS_CRYPTO_OK;
  return SALTS_OK;
}

void chttp_crypto_clear(void *data, size_t size) { salts_crypto_clear(data, size); }
