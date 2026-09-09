#include "s3_internal.h"

#include <s3/s3_object.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s3_object_request(s3_client *client, s3_method method, const char *bucket,
                             const char *key, const chttp_header *headers, size_t header_count,
                             const void *body, size_t body_size, s3_response *out_response,
                             s3_error *out_error) {
  const s3_request_options options = {.size = sizeof(options),
                                      .method = method,
                                      .bucket = bucket,
                                      .key = key,
                                      .headers = headers,
                                      .header_count = header_count,
                                      .body = body,
                                      .body_size = body_size};
  return s3_request(client, &options, out_response, out_error);
}

int s3_put_object(s3_client *client, const char *bucket, const char *key, const void *data,
                  size_t size, const char *content_type, s3_response *out_response,
                  s3_error *out_error) {
  const s3_put_object_options options = {
      .size = sizeof(options), .content_type = content_type, .sse = NULL};
  return s3_put_object_with_options(client, bucket, key, data, size, &options, out_response,
                                    out_error);
}

int s3_put_object_with_options(s3_client *client, const char *bucket, const char *key,
                               const void *data, size_t size, const s3_put_object_options *options,
                               s3_response *out_response, s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_sse_headers sse_headers = {0};
  chttp_header headers[4];
  size_t header_count = 0u;
  size_t index;
  int status;
  if (out_error != NULL) *out_error = (s3_error){0};
  if (data == NULL && size != 0u) {
    if (out_error != NULL)
      *out_error = (s3_error){.status = SALTS_EINVAL, .stage = "s3-put-object"};
    return SALTS_EINVAL;
  }
  if (impl == NULL || options == NULL || options->size != sizeof(*options)) {
    if (out_error != NULL)
      *out_error = (s3_error){.status = SALTS_EINVAL, .stage = "s3-put-object"};
    return SALTS_EINVAL;
  }
  status = s3_sse_headers_build(options->sse, 1, impl->base.config.max_header_bytes, &sse_headers);
  if (status == SALTS_OK && options->content_type != NULL)
    headers[header_count++] = (chttp_header){"Content-Type", options->content_type};
  for (index = 0u; index < sse_headers.count && status == SALTS_OK; ++index)
    headers[header_count++] = sse_headers.items[index];
  if (status == SALTS_OK)
    status = s3_object_request(client, S3_METHOD_PUT, bucket, key, headers, header_count, data,
                               size, out_response, out_error);
  if (status != SALTS_OK && out_error != NULL && out_error->status == SALTS_OK)
    *out_error = (s3_error){.status = status, .stage = "s3-put-object"};
  s3_sse_headers_destroy(&sse_headers);
  return status;
}

int s3_get_object(s3_client *client, const char *bucket, const char *key, s3_response *out_response,
                  s3_error *out_error) {
  return s3_get_object_with_options(client, bucket, key, NULL, out_response, out_error);
}

static int s3_read_object_with_options(s3_client *client, s3_method method, const char *bucket,
                                       const char *key, const s3_get_object_options *options,
                                       s3_response *out_response, s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_sse_headers sse_headers = {0};
  int status;
  if (out_error != NULL) *out_error = (s3_error){0};
  if (impl == NULL || (options != NULL && options->size != sizeof(*options))) {
    if (out_error != NULL)
      *out_error = (s3_error){.status = SALTS_EINVAL, .stage = "s3-read-object"};
    return SALTS_EINVAL;
  }
  status = s3_sse_headers_build(options != NULL ? options->sse : NULL, 0,
                                impl->base.config.max_header_bytes, &sse_headers);
  if (status == SALTS_OK)
    status = s3_object_request(client, method, bucket, key, sse_headers.items, sse_headers.count,
                               NULL, 0u, out_response, out_error);
  if (status != SALTS_OK && out_error != NULL && out_error->status == SALTS_OK)
    *out_error = (s3_error){.status = status, .stage = "s3-read-object"};
  s3_sse_headers_destroy(&sse_headers);
  return status;
}

int s3_get_object_with_options(s3_client *client, const char *bucket, const char *key,
                               const s3_get_object_options *options, s3_response *out_response,
                               s3_error *out_error) {
  return s3_read_object_with_options(client, S3_METHOD_GET, bucket, key, options, out_response,
                                     out_error);
}

int s3_head_object(s3_client *client, const char *bucket, const char *key,
                   s3_response *out_response, s3_error *out_error) {
  return s3_head_object_with_options(client, bucket, key, NULL, out_response, out_error);
}

int s3_head_object_with_options(s3_client *client, const char *bucket, const char *key,
                                const s3_get_object_options *options, s3_response *out_response,
                                s3_error *out_error) {
  return s3_read_object_with_options(client, S3_METHOD_HEAD, bucket, key, options, out_response,
                                     out_error);
}

int s3_delete_object(s3_client *client, const char *bucket, const char *key,
                     s3_response *out_response, s3_error *out_error) {
  return s3_object_request(client, S3_METHOD_DELETE, bucket, key, NULL, 0u, NULL, 0u, out_response,
                           out_error);
}

int s3_copy_object(s3_client *client, const char *source_bucket, const char *source_key,
                   const char *destination_bucket, const char *destination_key,
                   s3_response *out_response, s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_text_builder builder = {0};
  tstr encoded_bucket = NULL;
  tstr encoded_key = NULL;
  chttp_header header;
  size_t required = 2u;
  int status;
  if (out_error != NULL) *out_error = (s3_error){0};
  if (impl == NULL || out_response == NULL || out_error == NULL || source_bucket == NULL ||
      !s3_bucket_name_valid(source_bucket, impl != NULL ? impl->base.config.max_bucket_name_bytes
                                                        : S3_DEFAULT_MAX_BUCKET_NAME_BYTES) ||
      source_key == NULL || source_key[0] == '\0' ||
      (impl != NULL && strlen(source_key) > impl->base.config.max_object_key_bytes)) {
    if (out_error != NULL)
      *out_error = (s3_error){.status = SALTS_EINVAL, .stage = "s3-copy-object"};
    return SALTS_EINVAL;
  }
  status = s3_uri_encode(source_bucket, 0, impl->base.config.max_target_bytes, &encoded_bucket);
  if (status == SALTS_OK)
    status = s3_uri_encode(source_key, 1, impl->base.config.max_target_bytes, &encoded_key);
  if (status == SALTS_OK &&
      (s3_checked_add(required, tstr_len(encoded_bucket), &required) != SALTS_OK ||
       s3_checked_add(required, tstr_len(encoded_key), &required) != SALTS_OK ||
       required > impl->base.config.max_target_bytes))
    status = SALTS_EMSGSIZE;
  if (status == SALTS_OK) status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "/", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, encoded_bucket);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "/", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, encoded_key);
  if (status == SALTS_OK) {
    header = (chttp_header){"X-Amz-Copy-Source", builder.text};
    status = s3_object_request(client, S3_METHOD_PUT, destination_bucket, destination_key, &header,
                               1u, NULL, 0u, out_response, out_error);
  }
  tstr_free(encoded_bucket);
  tstr_free(encoded_key);
  s3_text_builder_destroy(&builder);
  if (status != SALTS_OK && out_error != NULL && out_error->status == SALTS_OK)
    *out_error = (s3_error){.status = status, .stage = "s3-copy-object"};
  return status;
}

void s3_object_list_destroy(s3_object_list *list) {
  size_t index;
  if (list == NULL) return;
  for (index = 0u; index < list->count; ++index) {
    free(list->items[index].key);
    free(list->items[index].last_modified);
    free(list->items[index].etag);
    free(list->items[index].storage_class);
  }
  free(list->items);
  free(list->next_continuation_token);
  *list = (s3_object_list){0};
}

static int s3_parse_u64(const char *text, uint64_t *out_value) {
  uint64_t value = 0u;
  size_t index;
  if (text == NULL || text[0] == '\0' || out_value == NULL) return SALTS_EPROTO;
  for (index = 0u; text[index] != '\0'; ++index) {
    const unsigned int digit = (unsigned int)(text[index] - '0');
    if (text[index] < '0' || text[index] > '9' ||
        value > (UINT64_MAX - (uint64_t)digit) / UINT64_C(10))
      return SALTS_EPROTO;
    value = value * UINT64_C(10) + (uint64_t)digit;
  }
  *out_value = value;
  return SALTS_OK;
}

static int s3_object_info_parse(salts_xml_node node, s3_object_info *out) {
  const salts_xml_node key = s3_xml_child(node, "Key");
  const salts_xml_node modified = s3_xml_child(node, "LastModified");
  const salts_xml_node etag = s3_xml_child(node, "ETag");
  const salts_xml_node size = s3_xml_child(node, "Size");
  const salts_xml_node storage = s3_xml_child(node, "StorageClass");
  char *size_text;
  int status;
  if (key.impl == NULL || size.impl == NULL || out == NULL) return SALTS_EPROTO;
  size_text = NULL;
  status = s3_xml_text_dup(key, &out->key);
  if (status == SALTS_OK && modified.impl != NULL)
    status = s3_xml_text_dup(modified, &out->last_modified);
  if (status == SALTS_OK && etag.impl != NULL) status = s3_xml_text_dup(etag, &out->etag);
  if (status == SALTS_OK && storage.impl != NULL)
    status = s3_xml_text_dup(storage, &out->storage_class);
  if (status == SALTS_OK) status = s3_xml_text_dup(size, &size_text);
  if (status != SALTS_OK) {
    free(size_text);
    return status;
  }
  status = s3_parse_u64(size_text, &out->size);
  free(size_text);
  return status;
}

static int s3_object_list_parse(const s3_client_impl *client, const s3_response *response,
                                s3_object_list *out) {
  salts_xml_document document = {0};
  salts_xml_node root = {0};
  salts_xml_node truncated;
  salts_xml_node next_token;
  size_t count = 0u;
  size_t index;
  size_t output_index = 0u;
  int status = s3_xml_parse_root(
      response->http.body, response->http.body_size, client->base.config.max_xml_bytes,
      client->base.config.max_xml_nodes, "ListBucketResult", &document, &root);
  if (status != SALTS_OK) return status;
  for (index = 0u; index < salts_xml_node_child_count(root); ++index) {
    const salts_xml_node child = salts_xml_node_child_at(root, index);
    if (salts_xml_node_type(child) == SALTS_XML_ELEMENT &&
        s3_xml_node_name_equal(child, "Contents"))
      ++count;
  }
  if (count > client->base.config.max_list_entries ||
      (count != 0u && count > SIZE_MAX / sizeof(*out->items))) {
    status = SALTS_ENOBUFS;
    goto done;
  }
  if (count != 0u) {
    out->items = (s3_object_info *)calloc(count, sizeof(*out->items));
    if (out->items == NULL) {
      status = SALTS_ENOMEM;
      goto done;
    }
  }
  out->count = count;
  for (index = 0u; index < salts_xml_node_child_count(root) && status == SALTS_OK; ++index) {
    const salts_xml_node child = salts_xml_node_child_at(root, index);
    if (salts_xml_node_type(child) == SALTS_XML_ELEMENT &&
        s3_xml_node_name_equal(child, "Contents"))
      status = s3_object_info_parse(child, &out->items[output_index++]);
  }
  truncated = s3_xml_child(root, "IsTruncated");
  if (status == SALTS_OK && truncated.impl != NULL) {
    char *value = NULL;
    status = s3_xml_text_dup(truncated, &value);
    if (status == SALTS_OK && strcmp(value, "true") == 0) out->is_truncated = 1;
    else if (status == SALTS_OK && strcmp(value, "false") != 0) status = SALTS_EPROTO;
    free(value);
  }
  next_token = s3_xml_child(root, "NextContinuationToken");
  if (status == SALTS_OK && next_token.impl != NULL)
    status = s3_xml_text_dup(next_token, &out->next_continuation_token);

done:
  salts_xml_document_destroy(&document);
  if (status != SALTS_OK) s3_object_list_destroy(out);
  return status;
}

int s3_list_objects(s3_client *client, const char *bucket, const s3_list_objects_options *options,
                    s3_object_list *out_list, s3_response *out_response, s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_query_param query[5];
  s3_request_options request;
  char max_keys[16];
  size_t count = 0u;
  int status;
  if (impl == NULL || bucket == NULL || out_list == NULL || out_list->items != NULL ||
      out_list->next_continuation_token != NULL || out_response == NULL || out_error == NULL ||
      (options != NULL && options->size != sizeof(*options)) ||
      (options != NULL && options->max_keys > 1000u))
    return SALTS_EINVAL;
  *out_list = (s3_object_list){0};
  query[count++] = (s3_query_param){"list-type", "2"};
  if (options != NULL && options->prefix != NULL)
    query[count++] = (s3_query_param){"prefix", options->prefix};
  if (options != NULL && options->delimiter != NULL)
    query[count++] = (s3_query_param){"delimiter", options->delimiter};
  if (options != NULL && options->continuation_token != NULL)
    query[count++] = (s3_query_param){"continuation-token", options->continuation_token};
  if (options != NULL && options->max_keys != 0u) {
    const int written = snprintf(max_keys, sizeof(max_keys), "%u", options->max_keys);
    if (written <= 0 || (size_t)written >= sizeof(max_keys)) return SALTS_ERANGE;
    query[count++] = (s3_query_param){"max-keys", max_keys};
  }
  request = (s3_request_options){.size = sizeof(request),
                                 .method = S3_METHOD_GET,
                                 .bucket = bucket,
                                 .query = query,
                                 .query_count = count};
  status = s3_request(client, &request, out_response, out_error);
  if (status != SALTS_OK) return status;
  status = s3_object_list_parse(impl, out_response, out_list);
  if (status != SALTS_OK)
    *out_error = (s3_error){.status = status, .stage = "s3-list-objects-parse"};
  return status;
}

int s3_presign_url(s3_client *client, s3_method method, const char *bucket, const char *key,
                   const s3_query_param *query, size_t query_count, uint32_t expires_seconds,
                   char **out_url, s3_error *out_error) {
  s3_client_impl *impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  int status;
  if (out_error == NULL) return SALTS_EINVAL;
  *out_error = (s3_error){0};
  if (impl == NULL || out_url == NULL || *out_url != NULL) return SALTS_EINVAL;
  if (impl->operation_active) return SALTS_EBUSY;
  impl->operation_active = 1;
  status = s3_presign_url_build(&impl->base, method, bucket, key, query, query_count,
                                expires_seconds, out_url);
  impl->operation_active = 0;
  if (status != SALTS_OK) *out_error = (s3_error){.status = status, .stage = "s3-presign"};
  return status;
}

void s3_string_free(char *string) { tstr_free(string); }
