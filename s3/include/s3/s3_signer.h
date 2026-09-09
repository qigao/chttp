#ifndef S3_SIGNER_H
#define S3_SIGNER_H

#include <salts/error_codes.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  S3_SIGNER_DEFAULT_MAX_HEADER_COUNT = 64,
  S3_SIGNER_DEFAULT_MAX_QUERY_COUNT = 64,
  S3_SIGNER_SHA256_HEX_SIZE = 64,
  S3_SIGNER_AMZ_DATE_SIZE = 16
};

#define S3_SIGNER_DEFAULT_MAX_HEADER_BYTES ((size_t)32u * 1024u)
#define S3_SIGNER_DEFAULT_MAX_TARGET_BYTES ((size_t)8u * 1024u)

typedef struct s3_signer_header {
  const char *name;
  const char *value;
} s3_signer_header;

typedef struct s3_signer_query {
  const char *name;
  const char *value;
} s3_signer_query;

/** All pointers are borrowed for the call. `canonical_uri` is already URI encoded. */
typedef struct s3_signer_request {
  size_t size;
  const char *method;
  const char *canonical_uri;
  const char *region;
  const char *access_key;
  const char *secret_key;
  const char *session_token;
  const char *payload_sha256;
  /** Exact UTC basic-format timestamp: YYYYMMDDTHHMMSSZ. */
  const char *amz_date;
  const s3_signer_header *headers;
  size_t header_count;
  const s3_signer_query *query;
  size_t query_count;
  /** Zero selects the bounded default. Counts include signing-added headers. */
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_query_count;
  size_t max_target_bytes;
} s3_signer_request;

/** Owning signer result. Destroy it even when a future field is added. */
typedef struct s3_signer_result {
  char *authorization;
  char *signed_headers;
  char *signature;
  char *canonical_query;
  char *canonical_request;
  char *string_to_sign;
} s3_signer_result;

typedef struct s3_presign_request {
  size_t size;
  const char *method;
  const char *canonical_uri;
  const char *authority;
  const char *region;
  const char *access_key;
  const char *secret_key;
  const char *session_token;
  const char *amz_date;
  uint32_t expires_seconds;
  const s3_signer_query *query;
  size_t query_count;
  size_t max_query_count;
  size_t max_target_bytes;
} s3_presign_request;

/** Owned canonical query including X-Amz-Signature. */
typedef struct s3_presign_result {
  char *canonical_query;
  char *signature;
} s3_presign_result;

/** Computes the lowercase hexadecimal SHA-256 digest into 65 bytes including NUL. */
int s3_signer_sha256_hex(const void *data, size_t size,
                         char out_hex[S3_SIGNER_SHA256_HEX_SIZE + 1]);

/** Signs an S3 request with AWS Signature Version 4 and atomically publishes owned output. */
int s3_signer_sign(const s3_signer_request *request, s3_signer_result *out_result);

/** Releases all owning fields and zeros the result. */
void s3_signer_result_destroy(s3_signer_result *result);

/** Produces a SigV4 query-auth payload with an expiration from 1 through 604800 seconds. */
int s3_signer_presign(const s3_presign_request *request, s3_presign_result *out_result);
void s3_presign_result_destroy(s3_presign_result *result);

#ifdef __cplusplus
}
#endif

#endif /* S3_SIGNER_H */
