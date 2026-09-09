#ifndef S3_S3_H
#define S3_S3_H

#include <chttp/chttp.h>
#include <s3/s3_credentials.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S3_ABI_VERSION 1u

enum {
  S3_MAX_REGION_BYTES = 63,
  S3_DEFAULT_MAX_BUCKET_NAME_BYTES = 63,
  S3_DEFAULT_MAX_OBJECT_KEY_BYTES = 1024,
  S3_DEFAULT_MAX_QUERY_COUNT = 64,
  S3_DEFAULT_MAX_HEADER_COUNT = 64,
  S3_DEFAULT_MAX_XML_NODES = 65536,
  S3_DEFAULT_MAX_LIST_ENTRIES = 10000
};

#define S3_DEFAULT_MAX_TARGET_BYTES ((size_t)8u * 1024u)
#define S3_DEFAULT_MAX_HEADER_BYTES ((size_t)32u * 1024u)
#define S3_DEFAULT_MAX_XML_BYTES ((size_t)4u * 1024u * 1024u)

typedef struct s3_client {
  void *impl;
} s3_client;

typedef struct s3_async_client {
  void *impl;
} s3_async_client;

typedef enum s3_addressing_style {
  S3_ADDRESSING_PATH = 0,
  S3_ADDRESSING_VIRTUAL_HOSTED = 1
} s3_addressing_style;

typedef enum s3_method {
  S3_METHOD_GET = 1,
  S3_METHOD_HEAD,
  S3_METHOD_POST,
  S3_METHOD_PUT,
  S3_METHOD_DELETE
} s3_method;

typedef int64_t (*s3_clock_fn)(void *user);

/** Shared bounded configuration. Strings are copied; pointer fields remain borrowed. */
typedef struct s3_client_config {
  size_t size;
  const char *connection_uri;
  const char *authority;
  const char *region;
  s3_addressing_style addressing_style;
  chttp_protocol protocol;
  const chttp_tls_profile *tls;
  s3_credentials_provider credentials;
  s3_clock_fn clock;
  void *clock_user;
  uint32_t timeout_ms;
  size_t max_bucket_name_bytes;
  size_t max_object_key_bytes;
  size_t max_target_bytes;
  size_t max_query_count;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_xml_bytes;
  size_t max_xml_nodes;
  size_t max_list_entries;
  size_t max_multipart_parts;
  size_t max_multipart_part_bytes;
} s3_client_config;

typedef struct s3_query_param {
  const char *name;
  const char *value;
} s3_query_param;

/**
 * One generic S3 operation. Input views are borrowed for the blocking call.
 * `body` and `body_source` are mutually exclusive. A streamed request must
 * supply the lowercase SHA-256 digest of the exact source bytes.
 */
typedef struct s3_request_options {
  size_t size;
  s3_method method;
  const char *bucket;
  const char *key;
  const s3_query_param *query;
  size_t query_count;
  const chttp_header *headers;
  size_t header_count;
  const void *body;
  size_t body_size;
  const chttp_body_source *body_source;
  const chttp_body_sink *body_sink;
  const char *payload_sha256;
} s3_request_options;

/** Parsed fields from a non-2xx S3 XML error response. Every string is owned. */
typedef struct s3_service_error {
  char *code;
  char *message;
  char *request_id;
  char *host_id;
} s3_service_error;

/** Owning HTTP response plus an optional parsed S3 service error. */
typedef struct s3_response {
  chttp_response http;
  s3_service_error service_error;
} s3_response;

/** Error context. `stage` is a stable library-owned string. */
typedef struct s3_error {
  int status;
  int native_status;
  const char *stage;
} s3_error;

/** Callback-scoped parsed fields from an S3 service error. */
typedef struct s3_service_error_view {
  const char *code;
  const char *message;
  const char *request_id;
  const char *host_id;
} s3_service_error_view;

/** Callback-scoped HTTP response and optional service-error view. */
typedef struct s3_response_view {
  const chttp_response_view *http;
  s3_service_error_view service_error;
} s3_response_view;

/** Generation-checked request handle; never a pointer or transport handle. */
typedef struct s3_request_handle {
  uint32_t slot;
  uint32_t generation;
} s3_request_handle;

typedef void (*s3_complete_fn)(void *user, s3_request_handle request,
                               const s3_response_view *response, const s3_error *error);

/** The request descriptor is consumed by submit; callback/user remain borrowed until completion. */
typedef struct s3_async_request_options {
  size_t size;
  const s3_request_options *request;
  s3_complete_fn on_complete;
  void *user;
} s3_async_request_options;

/**
 * Initializes a single-owner requests-style S3 adapter. The CHTTP client and
 * borrowed pointer fields in `config` must remain valid until destroy.
 */
int s3_client_init(s3_client *client, chttp_client *http_client, const s3_client_config *config);

/** Performs one signed blocking request and returns an owning response. */
int s3_request(s3_client *client, const s3_request_options *options, s3_response *out_response,
               s3_error *out_error);

/** Releases copied endpoint/configuration state; it never destroys CHTTP. */
int s3_client_destroy(s3_client *client);

/** Releases the HTTP response and parsed service-error strings. */
void s3_response_destroy(s3_response *response);

/** Initializes an advanced adapter borrowing one caller-owned CHTTP async client. */
int s3_async_client_init(s3_async_client *client, chttp_async_client *http_client,
                         const s3_client_config *config);

/** Success admits exactly one terminal callback. Immediate failure admits none. */
int s3_async_client_submit(s3_async_client *client, const s3_async_request_options *options,
                           s3_request_handle *out_request);

/** Requests cancellation; the terminal callback remains mandatory. */
int s3_async_request_cancel(s3_async_client *client, s3_request_handle request);

/** Advances the borrowed CHTTP owner on the calling thread. */
int s3_async_client_poll(s3_async_client *client, uint32_t timeout_ms, size_t *out_completions);

/** Requires all admitted S3 requests and callbacks to be quiescent. */
int s3_async_client_destroy(s3_async_client *client);

#ifdef __cplusplus
}
#endif

#endif /* S3_S3_H */
