#include "s3_internal.h"

#include <base64_utils.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { S3_SSE_CUSTOMER_KEY_BYTES = 32, S3_SSE_MD5_BYTES = 16 };

static int s3_sse_has_kms_fields(const s3_sse_options *options) {
  return options->kms_key_id != NULL || options->kms_context != NULL ||
         options->kms_context_size != 0u;
}

static int s3_sse_has_customer_fields(const s3_sse_options *options) {
  return options->customer_key != NULL || options->customer_key_size != 0u;
}

static int s3_sse_base64(const void *data, size_t size, size_t max_header_bytes, char **out_value) {
  tn_base64_string_result_t result;
  if (data == NULL || size == 0u || out_value == NULL || *out_value != NULL ||
      size > (SIZE_MAX - 2u) / 4u * 3u || 4u * ((size + 2u) / 3u) > max_header_bytes)
    return SALTS_EINVAL;
  result = tn_base64_encode_ex((const uint8_t *)data, size);
  if (!result.ok) return result.error == TN_BASE64_ERR_NO_MEMORY ? SALTS_ENOMEM : SALTS_EINVAL;
  *out_value = result.value;
  return SALTS_OK;
}

static int s3_sse_customer_headers(const s3_sse_options *options, size_t max_header_bytes,
                                   s3_sse_headers *headers) {
  unsigned char digest[S3_SSE_MD5_BYTES];
  unsigned int digest_size = 0u;
  int status;
  if (s3_sse_has_kms_fields(options) || options->customer_key == NULL ||
      options->customer_key_size != S3_SSE_CUSTOMER_KEY_BYTES)
    return SALTS_EINVAL;
  status = s3_sse_base64(options->customer_key, options->customer_key_size, max_header_bytes,
                         &headers->owned_values[0]);
  if (status == SALTS_OK && (EVP_Digest(options->customer_key, options->customer_key_size, digest,
                                        &digest_size, EVP_md5(), NULL) != 1 ||
                             digest_size != sizeof(digest)))
    status = SALTS_EIO;
  if (status == SALTS_OK)
    status = s3_sse_base64(digest, sizeof(digest), max_header_bytes, &headers->owned_values[1]);
  OPENSSL_cleanse(digest, sizeof(digest));
  if (status != SALTS_OK) return status;
  headers->items[0] = (chttp_header){"X-Amz-Server-Side-Encryption-Customer-Algorithm", "AES256"};
  headers->items[1] =
      (chttp_header){"X-Amz-Server-Side-Encryption-Customer-Key", headers->owned_values[0]};
  headers->items[2] =
      (chttp_header){"X-Amz-Server-Side-Encryption-Customer-Key-MD5", headers->owned_values[1]};
  headers->count = 3u;
  return SALTS_OK;
}

int s3_sse_headers_build(const s3_sse_options *options, int for_write, size_t max_header_bytes,
                         s3_sse_headers *out_headers) {
  int status;
  if (out_headers == NULL) return SALTS_EINVAL;
  *out_headers = (s3_sse_headers){0};
  if (options == NULL) return SALTS_OK;
  if (options->size != sizeof(*options) || max_header_bytes == 0u) return SALTS_EINVAL;
  switch (options->mode) {
  case S3_SSE_NONE:
    return s3_sse_has_kms_fields(options) || s3_sse_has_customer_fields(options) ? SALTS_EINVAL
                                                                                 : SALTS_OK;
  case S3_SSE_S3:
    if (!for_write || s3_sse_has_kms_fields(options) || s3_sse_has_customer_fields(options))
      return SALTS_EINVAL;
    out_headers->items[0] = (chttp_header){"X-Amz-Server-Side-Encryption", "AES256"};
    out_headers->count = 1u;
    return SALTS_OK;
  case S3_SSE_KMS:
    if (!for_write || s3_sse_has_customer_fields(options) ||
        (options->kms_key_id != NULL && options->kms_key_id[0] == '\0') ||
        (options->kms_context == NULL && options->kms_context_size != 0u) ||
        (options->kms_context != NULL && options->kms_context_size == 0u))
      return SALTS_EINVAL;
    out_headers->items[0] = (chttp_header){"X-Amz-Server-Side-Encryption", "aws:kms"};
    out_headers->count = 1u;
    if (options->kms_key_id != NULL)
      out_headers->items[out_headers->count++] =
          (chttp_header){"X-Amz-Server-Side-Encryption-Aws-Kms-Key-Id", options->kms_key_id};
    if (options->kms_context == NULL) return SALTS_OK;
    status = s3_sse_base64(options->kms_context, options->kms_context_size, max_header_bytes,
                           &out_headers->owned_values[0]);
    if (status != SALTS_OK) return status;
    out_headers->items[out_headers->count++] =
        (chttp_header){"X-Amz-Server-Side-Encryption-Context", out_headers->owned_values[0]};
    return SALTS_OK;
  case S3_SSE_CUSTOMER:
    return s3_sse_customer_headers(options, max_header_bytes, out_headers);
  default:
    return SALTS_EINVAL;
  }
}

void s3_sse_headers_destroy(s3_sse_headers *headers) {
  size_t index;
  if (headers == NULL) return;
  for (index = 0u; index < sizeof(headers->owned_values) / sizeof(headers->owned_values[0]);
       ++index) {
    if (headers->owned_values[index] != NULL) {
      OPENSSL_cleanse(headers->owned_values[index], strlen(headers->owned_values[index]));
      free(headers->owned_values[index]);
    }
  }
  *headers = (s3_sse_headers){0};
}
