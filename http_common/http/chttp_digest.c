#include "chttp_digest.h"

#include <core/error_codes.h>
#include <gmssl/digest.h>
#include <gmssl/hmac.h>
#include <gmssl/md5.h>
#include <gmssl/mem.h>
#include <gmssl/sha1.h>
#include <gmssl/sha2.h>
#include <stdlib.h>

int chttp_md5(const void *data, size_t size,
              uint8_t out[SALTS_MD5_DIGEST_BYTES]) {
  MD5_CTX context;

  if (out == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;

  md5_init(&context);
  if (size != 0u) md5_update(&context, (const uint8_t *)data, size);
  md5_finish(&context, out);
  gmssl_secure_clear(&context, sizeof(context));
  return SALTS_OK;
}

int chttp_sha1(const void *data, size_t size,
               uint8_t out[SALTS_SHA1_DIGEST_BYTES]) {
  SHA1_CTX context;

  if (out == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;
  sha1_init(&context);
  if (size != 0u) sha1_update(&context, (const uint8_t *)data, size);
  sha1_finish(&context, out);
  gmssl_secure_clear(&context, sizeof(context));
  return SALTS_OK;
}

struct chttp_sha256_stream {
  SHA256_CTX context;
  int finished;
};

int chttp_sha256(const void *data, size_t size,
                 uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  SHA256_CTX context;

  if (out == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;
  sha256_init(&context);
  if (size != 0u) sha256_update(&context, (const uint8_t *)data, size);
  sha256_finish(&context, out);
  gmssl_secure_clear(&context, sizeof(context));
  return SALTS_OK;
}

int chttp_sha256_stream_create(chttp_sha256_stream **out_stream) {
  chttp_sha256_stream *stream;
  if (out_stream == NULL) return SALTS_EINVAL;
  *out_stream = NULL;
  stream = (chttp_sha256_stream *)calloc(1u, sizeof(*stream));
  if (stream == NULL) return SALTS_ENOMEM;
  sha256_init(&stream->context);
  *out_stream = stream;
  return SALTS_OK;
}

int chttp_sha256_stream_update(chttp_sha256_stream *stream,
                               const void *data, size_t size) {
  if (stream == NULL || stream->finished ||
      (data == NULL && size != 0u))
    return SALTS_EINVAL;
  if (size != 0u) sha256_update(&stream->context, (const uint8_t *)data, size);
  return SALTS_OK;
}

int chttp_sha256_stream_finish(
    chttp_sha256_stream *stream,
    uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  if (stream == NULL || out == NULL || stream->finished) return SALTS_EINVAL;
  sha256_finish(&stream->context, out);
  gmssl_secure_clear(&stream->context, sizeof(stream->context));
  stream->finished = 1;
  return SALTS_OK;
}

void chttp_sha256_stream_destroy(chttp_sha256_stream *stream) {
  if (stream == NULL) return;
  gmssl_secure_clear(stream, sizeof(*stream));
  free(stream);
}

static int chttp_hmac(const DIGEST *digest, size_t expected_size,
                      const void *key, size_t key_size,
                      const void *data, size_t data_size,
                      uint8_t *out) {
  size_t output_size = 0u;

  if (digest == NULL || out == NULL ||
      (key == NULL && key_size != 0u) ||
      (data == NULL && data_size != 0u))
    return SALTS_EINVAL;

  if (hmac(digest, (const uint8_t *)key, key_size,
           (const uint8_t *)data, data_size, out, &output_size) != 1 ||
      output_size != expected_size)
    return SALTS_EIO;

  return SALTS_OK;
}

int chttp_hmac_sha256(const void *key, size_t key_size,
                      const void *data, size_t data_size,
                      uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  return chttp_hmac(DIGEST_sha256(), SALTS_SHA256_DIGEST_BYTES,
                    key, key_size, data, data_size, out);
}

int chttp_crypto_equal(const void *lhs, const void *rhs, size_t size,
                       int *out_equal) {
  if (out_equal == NULL || (size != 0u && (lhs == NULL || rhs == NULL)))
    return SALTS_EINVAL;

  if (size == 0u) {
    *out_equal = 1;
    return SALTS_OK;
  }

  *out_equal = gmssl_secure_memcmp(lhs, rhs, size) == 0 ? 1 : 0;
  return SALTS_OK;
}

void chttp_crypto_clear(void *data, size_t size) {
  if (data == NULL || size == 0u) return;
  gmssl_secure_clear(data, size);
}
