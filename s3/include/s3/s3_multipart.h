#ifndef S3_MULTIPART_H
#define S3_MULTIPART_H

#include <s3/s3_object.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { S3_MULTIPART_MAX_PARTS = 10000, S3_MULTIPART_MIN_PART_BYTES = 5u * 1024u * 1024u };

#define S3_MULTIPART_MAX_PART_BYTES (UINT64_C(5) * 1024u * 1024u * 1024u)
#define S3_MULTIPART_DEFAULT_PART_BYTES ((size_t)8u * 1024u * 1024u)
#define S3_MULTIPART_DEFAULT_MAX_BUFFER_BYTES ((size_t)64u * 1024u * 1024u)

/** Multipart handles are single-owner and must not be used concurrently. */
typedef struct s3_multipart {
  void *impl;
} s3_multipart;

typedef enum s3_multipart_state {
  S3_MULTIPART_ACTIVE = 1,
  S3_MULTIPART_COMPLETED,
  S3_MULTIPART_ABORTED,
  S3_MULTIPART_DETACHED_FOR_RESUME
} s3_multipart_state;

/**
 * Initiates one upload and owns its returned upload id in out_upload.
 * SSE-C also retains cleansable derived headers required by later part calls.
 */
int s3_multipart_initiate(s3_client *client, const char *bucket, const char *key,
                          const s3_put_object_options *options, s3_multipart *out_upload,
                          s3_response *out_response, s3_error *out_error);

/** Uploads or explicitly replaces one part numbered 1 through 10000. */
int s3_multipart_upload_part(s3_client *client, s3_multipart *upload, uint32_t part_number,
                             const void *data, size_t size, s3_response *out_response,
                             s3_error *out_error);

/**
 * Completes contiguous parts in ascending order; only the final part may be below 5 MiB.
 * An empty, malformed, or Error-root HTTP 200 response returns SALTS_EPROTO and
 * leaves the handle active for retry or abort.
 */
int s3_multipart_complete(s3_client *client, s3_multipart *upload, s3_response *out_response,
                          s3_error *out_error);

/** Aborts the server upload and enters the terminal ABORTED state on success. */
int s3_multipart_abort(s3_client *client, s3_multipart *upload, s3_response *out_response,
                       s3_error *out_error);

/** Marks an active upload as intentionally retained for an external resume record. */
int s3_multipart_detach(s3_multipart *upload);
int s3_multipart_state_get(const s3_multipart *upload, s3_multipart_state *out_state);
const char *s3_multipart_upload_id(const s3_multipart *upload);

/** Active state returns SALTS_EBUSY; terminal or detached state releases all owned data. */
int s3_multipart_destroy(s3_multipart *upload);

typedef struct s3_multipart_file_options {
  size_t size;
  size_t part_size;
  const char *checkpoint_path;
  /** Requires and resumes an existing checkpoint instead of initiating an upload. */
  int resume_existing;
  /** Leaves the checkpoint and server upload available after a part/complete failure. */
  int preserve_on_failure;
  const s3_put_object_options *put_options;
  chttp_progress_fn progress;
  void *progress_user;
} s3_multipart_file_options;

/**
 * Sequential bounded-memory multipart file upload with an atomic resume checkpoint.
 * SSE-C resume requires the same put_options; the checkpoint stores no encryption key.
 * The progress callback runs on the blocking client's owner thread and must not
 * reenter or destroy that S3/CHTTP client. If checkpoint removal fails after a
 * successful CompleteMultipartUpload, the object is committed and the stale
 * checkpoint must be removed by the caller before attempting resume.
 */
int s3_put_object_multipart_file(s3_client *client, const char *bucket, const char *key,
                                 const char *path, const s3_multipart_file_options *options,
                                 s3_response *out_response, s3_error *out_error);

#ifdef __cplusplus
}
#endif

#endif /* S3_MULTIPART_H */
