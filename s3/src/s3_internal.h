#ifndef S3_INTERNAL_H
#define S3_INTERNAL_H

#include <s3/s3.h>
#include <s3/s3_multipart.h>
#include <s3/s3_object.h>
#include <s3/s3_signer.h>

#include <tstr.h>
#include <xml_parser/xml_parser.h>

#include <stddef.h>

typedef struct s3_client_base {
  s3_client_config config;
  tstr connection_uri;
  tstr authority;
  tstr region;
} s3_client_base;

typedef struct s3_client_impl {
  s3_client_base base;
  chttp_client *http_client;
  int operation_active;
} s3_client_impl;

typedef struct s3_async_client_impl {
  s3_client_base base;
  chttp_async_client *http_client;
  size_t active_requests;
  int callback_active;
} s3_async_client_impl;

typedef struct s3_request_plan {
  chttp_method method;
  tstr authority;
  tstr canonical_uri;
  tstr target;
  tstr payload_sha256;
  char amz_date[S3_SIGNER_AMZ_DATE_SIZE + 1u];
  chttp_header *headers;
  size_t header_count;
} s3_request_plan;

typedef struct s3_text_builder {
  tstr text;
  size_t capacity;
} s3_text_builder;

typedef struct s3_sse_headers {
  chttp_header items[3];
  char *owned_values[2];
  size_t count;
} s3_sse_headers;

int s3_checked_add(size_t left, size_t right, size_t *out);
int s3_checked_multiply(size_t left, size_t right, size_t *out);
int s3_text_builder_init(s3_text_builder *builder, size_t capacity);
int s3_text_builder_append(s3_text_builder *builder, const void *data, size_t size);
int s3_text_builder_append_cstr(s3_text_builder *builder, const char *text);
tstr s3_text_builder_release(s3_text_builder *builder);
void s3_text_builder_destroy(s3_text_builder *builder);

int s3_uri_encode(const char *input, int preserve_slash, size_t max_bytes, tstr *out);
int s3_bucket_name_valid(const char *bucket, size_t max_bytes);
int s3_query_canonicalize(const s3_signer_query *query, size_t query_count, size_t max_query_count,
                          size_t max_target_bytes, tstr *out);
int s3_sse_headers_build(const s3_sse_options *options, int for_write, size_t max_header_bytes,
                         s3_sse_headers *out_headers);
void s3_sse_headers_destroy(s3_sse_headers *headers);

int s3_client_base_init(s3_client_base *base, const s3_client_config *config);
void s3_client_base_destroy(s3_client_base *base);
int s3_response_is_empty(const s3_response *response);

int s3_request_plan_build(const s3_client_base *client, const s3_request_options *options,
                          s3_request_plan *out_plan);
void s3_request_plan_destroy(s3_request_plan *plan);
int s3_presign_url_build(const s3_client_base *client, s3_method method, const char *bucket,
                         const char *key, const s3_query_param *query, size_t query_count,
                         uint32_t expires_seconds, char **out_url);
int s3_request_put_file(s3_client *client, const s3_request_options *options, const char *path,
                        chttp_progress_fn progress, void *progress_user, s3_response *out_response,
                        s3_error *out_error);
int s3_request_get_file(s3_client *client, const s3_request_options *options,
                        const char *output_path, chttp_progress_fn progress, void *progress_user,
                        s3_response *out_response, s3_error *out_error);

int s3_service_error_parse(const void *body, size_t body_size, size_t max_xml_bytes,
                           size_t max_xml_nodes, s3_service_error *out_error);
void s3_service_error_destroy(s3_service_error *error);
int s3_response_parse_service_error(s3_response *response, size_t max_xml_bytes,
                                    size_t max_xml_nodes);

int s3_xml_parse_root(const void *body, size_t body_size, size_t max_xml_bytes,
                      size_t max_xml_nodes, const char *expected_root,
                      salts_xml_document *out_document, salts_xml_node *out_root);
int s3_xml_node_name_equal(salts_xml_node node, const char *expected);
salts_xml_node s3_xml_child(salts_xml_node parent, const char *name);
int s3_xml_text_dup(salts_xml_node node, char **out_text);

#endif /* S3_INTERNAL_H */
