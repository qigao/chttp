#ifndef S3_OBJECT_H
#define S3_OBJECT_H

#include <s3/s3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s3_list_objects_options {
  size_t size;
  const char *prefix;
  const char *delimiter;
  const char *continuation_token;
  uint32_t max_keys;
} s3_list_objects_options;

typedef struct s3_object_info {
  char *key;
  char *last_modified;
  char *etag;
  char *storage_class;
  uint64_t size;
} s3_object_info;

typedef struct s3_object_list {
  s3_object_info *items;
  size_t count;
  char *next_continuation_token;
  int is_truncated;
} s3_object_list;

typedef enum s3_sse_mode { S3_SSE_NONE = 0, S3_SSE_S3, S3_SSE_KMS, S3_SSE_CUSTOMER } s3_sse_mode;

/**
 * Borrowed server-side encryption input.
 *
 * SSE-S3 accepts no extra fields. SSE-KMS accepts an optional key id and an
 * optional raw UTF-8 encryption context, which the library base64-encodes.
 * SSE-C requires exactly 32 raw key bytes; the library emits the base64 key
 * and MD5 integrity header and never retains either after the call.
 */
typedef struct s3_sse_options {
  size_t size;
  s3_sse_mode mode;
  const char *kms_key_id;
  const void *kms_context;
  size_t kms_context_size;
  const void *customer_key;
  size_t customer_key_size;
} s3_sse_options;

typedef struct s3_put_object_options {
  size_t size;
  const char *content_type;
  const s3_sse_options *sse;
} s3_put_object_options;

typedef struct s3_get_object_options {
  size_t size;
  /** Only SSE-C is valid for reads; NULL selects ordinary retrieval. */
  const s3_sse_options *sse;
} s3_get_object_options;

int s3_put_object(s3_client *client, const char *bucket, const char *key, const void *data,
                  size_t size, const char *content_type, s3_response *out_response,
                  s3_error *out_error);
int s3_put_object_with_options(s3_client *client, const char *bucket, const char *key,
                               const void *data, size_t size, const s3_put_object_options *options,
                               s3_response *out_response, s3_error *out_error);
int s3_get_object(s3_client *client, const char *bucket, const char *key, s3_response *out_response,
                  s3_error *out_error);
int s3_get_object_with_options(s3_client *client, const char *bucket, const char *key,
                               const s3_get_object_options *options, s3_response *out_response,
                               s3_error *out_error);
int s3_head_object(s3_client *client, const char *bucket, const char *key,
                   s3_response *out_response, s3_error *out_error);
int s3_head_object_with_options(s3_client *client, const char *bucket, const char *key,
                                const s3_get_object_options *options, s3_response *out_response,
                                s3_error *out_error);
int s3_delete_object(s3_client *client, const char *bucket, const char *key,
                     s3_response *out_response, s3_error *out_error);
int s3_copy_object(s3_client *client, const char *source_bucket, const char *source_key,
                   const char *destination_bucket, const char *destination_key,
                   s3_response *out_response, s3_error *out_error);
int s3_list_objects(s3_client *client, const char *bucket, const s3_list_objects_options *options,
                    s3_object_list *out_list, s3_response *out_response, s3_error *out_error);
void s3_object_list_destroy(s3_object_list *list);

/**
 * Hashes the file in one bounded-memory pass, then CHTTP streams it with native
 * asynchronous file reads. The caller must keep the file unchanged for both passes.
 * The progress callback runs on the blocking client's owner thread and must not
 * reenter or destroy that S3/CHTTP client.
 */
int s3_put_object_file(s3_client *client, const char *bucket, const char *key, const char *path,
                       const s3_put_object_options *options, chttp_progress_fn progress,
                       void *progress_user, s3_response *out_response, s3_error *out_error);

/**
 * Streams a successful GET through native asynchronous writes to a temporary
 * file, then atomically replaces output_path. HTTP errors leave it unchanged.
 * The progress callback runs on the blocking client's owner thread and must not
 * reenter or destroy that S3/CHTTP client.
 */
int s3_get_object_file(s3_client *client, const char *bucket, const char *key,
                       const char *output_path, const s3_get_object_options *options,
                       chttp_progress_fn progress, void *progress_user, s3_response *out_response,
                       s3_error *out_error);

/** Returns one owned HTTP(S) URL. Expiration is 1 through 604800 seconds. */
int s3_presign_url(s3_client *client, s3_method method, const char *bucket, const char *key,
                   const s3_query_param *query, size_t query_count, uint32_t expires_seconds,
                   char **out_url, s3_error *out_error);
void s3_string_free(char *string);

#ifdef __cplusplus
}
#endif

#endif /* S3_OBJECT_H */
