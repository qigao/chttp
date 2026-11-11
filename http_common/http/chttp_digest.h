#ifndef CHTTP_DIGEST_H
#define CHTTP_DIGEST_H

#include <stddef.h>
#include <stdint.h>

enum {
  SALTS_MD5_DIGEST_BYTES = 16,
  SALTS_SHA1_DIGEST_BYTES = 20,
  SALTS_SHA256_DIGEST_BYTES = 32
};

typedef struct chttp_sha256_stream chttp_sha256_stream;

/* Internal synchronous adapters return Salts status codes. Buffers are borrowed
 * for each call. create publishes one owner or NULL on failure. finish consumes
 * the stream state once, and destroy clears and releases that owner's storage.
 * Independent streams may run concurrently. A single stream has one caller. */
int chttp_md5(const void *data, size_t size, uint8_t out[SALTS_MD5_DIGEST_BYTES]);
int chttp_sha1(const void *data, size_t size, uint8_t out[SALTS_SHA1_DIGEST_BYTES]);
int chttp_sha256(const void *data, size_t size, uint8_t out[SALTS_SHA256_DIGEST_BYTES]);
int chttp_hmac_sha256(const void *key, size_t key_size, const void *data,
                     size_t data_size, uint8_t out[SALTS_SHA256_DIGEST_BYTES]);
int chttp_sha256_stream_create(chttp_sha256_stream **out_stream);
int chttp_sha256_stream_update(chttp_sha256_stream *stream, const void *data, size_t size);
int chttp_sha256_stream_finish(chttp_sha256_stream *stream, uint8_t out[SALTS_SHA256_DIGEST_BYTES]);
void chttp_sha256_stream_destroy(chttp_sha256_stream *stream);
int chttp_crypto_equal(const void *lhs, const void *rhs, size_t size, int *out_equal);
void chttp_crypto_clear(void *data, size_t size);

#endif
