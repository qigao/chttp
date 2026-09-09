#include "s3_internal.h"

#include <openssl/crypto.h>

#include <salts_fs.h>
#include <salts_uuid.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct s3_multipart_part_internal {
  char *etag;
  size_t size;
} s3_multipart_part_internal;

typedef struct s3_multipart_impl {
  tstr bucket;
  tstr key;
  char *upload_id;
  s3_multipart_part_internal *parts;
  size_t part_capacity;
  size_t part_count;
  s3_sse_headers part_sse_headers;
  s3_multipart_state state;
} s3_multipart_impl;

typedef struct s3_multipart_memory_source {
  const unsigned char *data;
  size_t size;
  size_t offset;
} s3_multipart_memory_source;

enum { S3_MULTIPART_PART_NUMBER_TEXT_BYTES = 10 };
enum { S3_MULTIPART_CHECKPOINT_MODE = 0600 };

static const char s3_multipart_checkpoint_root[] = "S3MultipartResume";
static const char s3_multipart_checkpoint_version[] = "1";

static char *s3_multipart_text_dup(const char *text) {
  const size_t size = text != NULL ? strlen(text) : 0u;
  char *copy;
  if (text == NULL || size == SIZE_MAX) return NULL;
  copy = (char *)malloc(size + 1u);
  if (copy != NULL) memcpy(copy, text, size + 1u);
  return copy;
}

static int s3_multipart_memory_read(void *user, void *buffer, size_t capacity, size_t *out_size) {
  s3_multipart_memory_source *source = (s3_multipart_memory_source *)user;
  size_t available;
  size_t copied;
  if (source == NULL || buffer == NULL || capacity == 0u || out_size == NULL ||
      source->offset > source->size)
    return SALTS_EINVAL;
  available = source->size - source->offset;
  copied = available < capacity ? available : capacity;
  if (copied != 0u) memcpy(buffer, source->data + source->offset, copied);
  source->offset += copied;
  *out_size = copied;
  return SALTS_OK;
}

static int s3_multipart_error(s3_error *error, int status, const char *stage) {
  if (error != NULL) *error = (s3_error){.status = status, .stage = stage};
  return status;
}

static void s3_multipart_impl_free(s3_multipart_impl *upload) {
  size_t index;
  if (upload == NULL) return;
  for (index = 0u; index < upload->part_capacity; ++index)
    free(upload->parts[index].etag);
  free(upload->parts);
  free(upload->upload_id);
  s3_sse_headers_destroy(&upload->part_sse_headers);
  tstr_free(upload->bucket);
  tstr_free(upload->key);
  free(upload);
}

static int s3_multipart_part_sse_headers_init(const s3_client_base *client,
                                              const s3_put_object_options *options,
                                              s3_multipart_impl *upload) {
  s3_sse_headers headers = {0};
  int status;
  if (client == NULL || upload == NULL || (options != NULL && options->size != sizeof(*options)))
    return SALTS_EINVAL;
  status = s3_sse_headers_build(options != NULL ? options->sse : NULL, 1,
                                client->config.max_header_bytes, &headers);
  if (status == SALTS_OK && options != NULL && options->sse != NULL &&
      options->sse->mode == S3_SSE_CUSTOMER) {
    upload->part_sse_headers = headers;
    headers = (s3_sse_headers){0};
  }
  s3_sse_headers_destroy(&headers);
  return status;
}

static const char *s3_multipart_customer_key_md5(const s3_multipart_impl *upload) {
  if (upload != NULL && upload->part_sse_headers.count == 3u)
    return upload->part_sse_headers.items[2].value;
  return "";
}

static int s3_multipart_headers(const s3_client_impl *client, const s3_put_object_options *options,
                                s3_sse_headers *sse_headers, chttp_header headers[4],
                                size_t *out_count) {
  size_t count = 0u;
  size_t index;
  int status;
  if (client == NULL || sse_headers == NULL || headers == NULL || out_count == NULL ||
      (options != NULL && options->size != sizeof(*options)))
    return SALTS_EINVAL;
  status = s3_sse_headers_build(options != NULL ? options->sse : NULL, 1,
                                client->base.config.max_header_bytes, sse_headers);
  if (status != SALTS_OK) return status;
  if (options != NULL && options->content_type != NULL)
    headers[count++] = (chttp_header){"Content-Type", options->content_type};
  for (index = 0u; index < sse_headers->count; ++index)
    headers[count++] = sse_headers->items[index];
  *out_count = count;
  return SALTS_OK;
}

static int s3_multipart_upload_id_parse(const s3_client_impl *client, const s3_response *response,
                                        char **out_upload_id) {
  salts_xml_document document = {0};
  salts_xml_node root = {0};
  salts_xml_node upload_id;
  int status;
  if (client == NULL || response == NULL || out_upload_id == NULL || *out_upload_id != NULL)
    return SALTS_EINVAL;
  status = s3_xml_parse_root(response->http.body, response->http.body_size,
                             client->base.config.max_xml_bytes, client->base.config.max_xml_nodes,
                             "InitiateMultipartUploadResult", &document, &root);
  if (status == SALTS_OK) {
    upload_id = s3_xml_child(root, "UploadId");
    status = upload_id.impl != NULL ? s3_xml_text_dup(upload_id, out_upload_id) : SALTS_EPROTO;
    if (status == SALTS_OK && (*out_upload_id)[0] == '\0') {
      free(*out_upload_id);
      *out_upload_id = NULL;
      status = SALTS_EPROTO;
    }
  }
  salts_xml_document_destroy(&document);
  return status;
}

int s3_multipart_initiate(s3_client *client, const char *bucket, const char *key,
                          const s3_put_object_options *options, s3_multipart *out_upload,
                          s3_response *out_response, s3_error *out_error) {
  static const s3_query_param query[] = {{"uploads", ""}};
  s3_client_impl *client_impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_multipart_impl *upload = NULL;
  s3_sse_headers sse_headers = {0};
  chttp_header headers[4];
  size_t header_count = 0u;
  size_t parts_bytes = 0u;
  s3_request_options request;
  int status;
  if (out_error == NULL || out_upload == NULL || out_upload->impl != NULL || out_response == NULL ||
      !s3_response_is_empty(out_response))
    return SALTS_EINVAL;
  *out_error = (s3_error){0};
  if (client_impl == NULL ||
      !s3_bucket_name_valid(bucket, client_impl != NULL
                                        ? client_impl->base.config.max_bucket_name_bytes
                                        : S3_DEFAULT_MAX_BUCKET_NAME_BYTES) ||
      key == NULL || key[0] == '\0' ||
      (client_impl != NULL && strlen(key) > client_impl->base.config.max_object_key_bytes))
    return s3_multipart_error(out_error, SALTS_EINVAL, "s3-multipart-initiate");
  status = s3_multipart_headers(client_impl, options, &sse_headers, headers, &header_count);
  if (status == SALTS_OK &&
      s3_checked_multiply(client_impl->base.config.max_multipart_parts,
                          sizeof(s3_multipart_part_internal), &parts_bytes) != SALTS_OK)
    status = SALTS_ERANGE;
  if (status == SALTS_OK) {
    upload = (s3_multipart_impl *)calloc(1u, sizeof(*upload));
    if (upload == NULL) status = SALTS_ENOMEM;
  }
  if (status == SALTS_OK) {
    upload->parts = (s3_multipart_part_internal *)calloc(1u, parts_bytes);
    upload->bucket = tstr_dup(bucket);
    upload->key = tstr_dup(key);
    if (upload->parts == NULL || upload->bucket == NULL || upload->key == NULL)
      status = SALTS_ENOMEM;
  }
  request = (s3_request_options){.size = sizeof(request),
                                 .method = S3_METHOD_POST,
                                 .bucket = bucket,
                                 .key = key,
                                 .query = query,
                                 .query_count = 1u,
                                 .headers = headers,
                                 .header_count = header_count};
  if (status == SALTS_OK) status = s3_request(client, &request, out_response, out_error);
  if (status == SALTS_OK)
    status = s3_multipart_upload_id_parse(client_impl, out_response, &upload->upload_id);
  if (status == SALTS_OK) {
    if (options != NULL && options->sse != NULL && options->sse->mode == S3_SSE_CUSTOMER) {
      upload->part_sse_headers = sse_headers;
      sse_headers = (s3_sse_headers){0};
    }
    upload->part_capacity = client_impl->base.config.max_multipart_parts;
    upload->state = S3_MULTIPART_ACTIVE;
    out_upload->impl = upload;
    upload = NULL;
  } else if (out_error->status == SALTS_OK) {
    s3_multipart_error(out_error, status, "s3-multipart-initiate");
  }
  s3_multipart_impl_free(upload);
  s3_sse_headers_destroy(&sse_headers);
  return status;
}

static int s3_multipart_active(s3_multipart *upload, s3_multipart_impl **out_impl,
                               s3_error *out_error, const char *stage) {
  s3_multipart_impl *impl = upload != NULL ? (s3_multipart_impl *)upload->impl : NULL;
  if (impl == NULL || impl->state != S3_MULTIPART_ACTIVE) {
    s3_multipart_error(out_error, SALTS_EINVAL, stage);
    return SALTS_EINVAL;
  }
  *out_impl = impl;
  return SALTS_OK;
}

int s3_multipart_upload_part(s3_client *client, s3_multipart *upload, uint32_t part_number,
                             const void *data, size_t size, s3_response *out_response,
                             s3_error *out_error) {
  s3_multipart_impl *impl = NULL;
  s3_query_param query[2];
  s3_request_options request;
  s3_multipart_memory_source memory_source = {
      .data = (const unsigned char *)data, .size = size, .offset = 0u};
  chttp_body_source body_source = {.read = s3_multipart_memory_read,
                                   .user = &memory_source,
                                   .content_length = size,
                                   .content_length_known = 1};
  char payload_sha256[S3_SIGNER_SHA256_HEX_SIZE + 1u] = {0};
  char part_text[16];
  const char *etag;
  char *owned_etag = NULL;
  int text_size;
  int status;
  if (out_error == NULL || out_response == NULL || !s3_response_is_empty(out_response))
    return SALTS_EINVAL;
  *out_error = (s3_error){0};
  status = s3_multipart_active(upload, &impl, out_error, "s3-multipart-part");
  if (status != SALTS_OK) return status;
  if (client == NULL || client->impl == NULL || part_number == 0u ||
      part_number > impl->part_capacity || (data == NULL && size != 0u) ||
      (uint64_t)size > S3_MULTIPART_MAX_PART_BYTES)
    return s3_multipart_error(out_error, SALTS_EINVAL, "s3-multipart-part");
  text_size = snprintf(part_text, sizeof(part_text), "%u", (unsigned int)part_number);
  if (text_size <= 0 || (size_t)text_size >= sizeof(part_text))
    return s3_multipart_error(out_error, SALTS_ERANGE, "s3-multipart-part");
  query[0] = (s3_query_param){"partNumber", part_text};
  query[1] = (s3_query_param){"uploadId", impl->upload_id};
  status = s3_signer_sha256_hex(data, size, payload_sha256);
  if (status != SALTS_OK) return s3_multipart_error(out_error, status, "s3-multipart-part-hash");
  request = (s3_request_options){.size = sizeof(request),
                                 .method = S3_METHOD_PUT,
                                 .bucket = impl->bucket,
                                 .key = impl->key,
                                 .query = query,
                                 .query_count = 2u,
                                 .headers = impl->part_sse_headers.items,
                                 .header_count = impl->part_sse_headers.count,
                                 .body_source = &body_source,
                                 .payload_sha256 = payload_sha256};
  status = s3_request(client, &request, out_response, out_error);
  if (status != SALTS_OK) return status;
  etag = chttp_response_header(&out_response->http, "etag");
  if (etag == NULL || etag[0] == '\0')
    return s3_multipart_error(out_error, SALTS_EPROTO, "s3-multipart-etag");
  owned_etag = s3_multipart_text_dup(etag);
  if (owned_etag == NULL) return s3_multipart_error(out_error, SALTS_ENOMEM, "s3-multipart-etag");
  if (impl->parts[part_number - 1u].etag == NULL) ++impl->part_count;
  free(impl->parts[part_number - 1u].etag);
  impl->parts[part_number - 1u].etag = owned_etag;
  impl->parts[part_number - 1u].size = size;
  return SALTS_OK;
}

static int s3_multipart_append_xml_text(s3_text_builder *builder, const char *text) {
  size_t index;
  int status = SALTS_OK;
  for (index = 0u; text[index] != '\0' && status == SALTS_OK; ++index) {
    switch (text[index]) {
    case '&':
      status = s3_text_builder_append_cstr(builder, "&amp;");
      break;
    case '<':
      status = s3_text_builder_append_cstr(builder, "&lt;");
      break;
    case '>':
      status = s3_text_builder_append_cstr(builder, "&gt;");
      break;
    case '"':
      status = s3_text_builder_append_cstr(builder, "&quot;");
      break;
    case '\'':
      status = s3_text_builder_append_cstr(builder, "&apos;");
      break;
    default:
      status = s3_text_builder_append(builder, &text[index], 1u);
      break;
    }
  }
  return status;
}

static int s3_multipart_append_element(s3_text_builder *builder, const char *name,
                                       const char *value) {
  int status;
  if (builder == NULL || name == NULL || value == NULL) return SALTS_EINVAL;
  status = s3_text_builder_append(builder, "<", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(builder, name);
  if (status == SALTS_OK) status = s3_text_builder_append(builder, ">", 1u);
  if (status == SALTS_OK) status = s3_multipart_append_xml_text(builder, value);
  if (status == SALTS_OK) status = s3_text_builder_append(builder, "</", 2u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(builder, name);
  if (status == SALTS_OK) status = s3_text_builder_append(builder, ">", 1u);
  return status;
}

static int s3_multipart_capacity_add_text(size_t *capacity, const char *text) {
  size_t text_capacity;
  if (capacity == NULL || text == NULL || strlen(text) > SIZE_MAX / 6u) return SALTS_ERANGE;
  text_capacity = strlen(text) * 6u;
  return s3_checked_add(*capacity, text_capacity, capacity);
}

static int s3_multipart_checkpoint_capacity(const s3_client_base *client,
                                            const s3_multipart_impl *upload,
                                            const char *source_path, size_t *out_capacity) {
  size_t capacity = 1024u;
  size_t index;
  const char *fields[] = {client->connection_uri,
                          client->authority,
                          client->region,
                          upload->bucket,
                          upload->key,
                          upload->upload_id,
                          source_path,
                          s3_multipart_customer_key_md5(upload)};
  int status = SALTS_OK;
  for (index = 0u; index < sizeof(fields) / sizeof(fields[0]) && status == SALTS_OK; ++index)
    status = s3_multipart_capacity_add_text(&capacity, fields[index]);
  for (index = 0u; index < upload->part_capacity && status == SALTS_OK; ++index) {
    if (upload->parts[index].etag == NULL) continue;
    status = s3_checked_add(capacity, 128u, &capacity);
    if (status == SALTS_OK)
      status = s3_multipart_capacity_add_text(&capacity, upload->parts[index].etag);
  }
  if (status == SALTS_OK && capacity > client->config.max_xml_bytes) status = SALTS_EMSGSIZE;
  if (status == SALTS_OK) *out_capacity = capacity;
  return status;
}

static int s3_multipart_checkpoint_xml(const s3_client_base *client,
                                       const s3_multipart_impl *upload, const char *source_path,
                                       const salts_fs_stat_t *file_stat, size_t part_size,
                                       tstr *out_xml) {
  s3_text_builder builder = {0};
  char number[32];
  size_t capacity = 0u;
  size_t index;
  int status;
  if (client == NULL || upload == NULL || source_path == NULL || file_stat == NULL ||
      out_xml == NULL || *out_xml != NULL)
    return SALTS_EINVAL;
  status = s3_multipart_checkpoint_capacity(client, upload, source_path, &capacity);
  if (status == SALTS_OK) status = s3_text_builder_init(&builder, capacity);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, "<S3MultipartResume>");
  if (status == SALTS_OK)
    status = s3_multipart_append_element(&builder, "Version", s3_multipart_checkpoint_version);
  if (status == SALTS_OK)
    status = s3_multipart_append_element(&builder, "ConnectionUri", client->connection_uri);
  if (status == SALTS_OK)
    status = s3_multipart_append_element(&builder, "Authority", client->authority);
  if (status == SALTS_OK) status = s3_multipart_append_element(&builder, "Region", client->region);
  if (status == SALTS_OK) {
    (void)snprintf(number, sizeof(number), "%u", (unsigned int)client->config.addressing_style);
    status = s3_multipart_append_element(&builder, "AddressingStyle", number);
  }
  if (status == SALTS_OK) status = s3_multipart_append_element(&builder, "Bucket", upload->bucket);
  if (status == SALTS_OK) status = s3_multipart_append_element(&builder, "Key", upload->key);
  if (status == SALTS_OK) status = s3_multipart_append_element(&builder, "SourcePath", source_path);
  if (status == SALTS_OK) {
    (void)snprintf(number, sizeof(number), "%llu", (unsigned long long)file_stat->size);
    status = s3_multipart_append_element(&builder, "FileSize", number);
  }
  if (status == SALTS_OK) {
    (void)snprintf(number, sizeof(number), "%llu", (unsigned long long)file_stat->mtime);
    status = s3_multipart_append_element(&builder, "FileMtime", number);
  }
  if (status == SALTS_OK) {
    (void)snprintf(number, sizeof(number), "%llu", (unsigned long long)file_stat->ctime);
    status = s3_multipart_append_element(&builder, "FileCtime", number);
  }
  if (status == SALTS_OK) {
    (void)snprintf(number, sizeof(number), "%llu", (unsigned long long)part_size);
    status = s3_multipart_append_element(&builder, "PartSize", number);
  }
  if (status == SALTS_OK)
    status = s3_multipart_append_element(&builder, "SseCustomerKeyMd5",
                                         s3_multipart_customer_key_md5(upload));
  if (status == SALTS_OK)
    status = s3_multipart_append_element(&builder, "UploadId", upload->upload_id);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, "<Parts>");
  for (index = 0u; index < upload->part_capacity && status == SALTS_OK; ++index) {
    if (upload->parts[index].etag == NULL) continue;
    status = s3_text_builder_append_cstr(&builder, "<Part>");
    if (status == SALTS_OK) {
      (void)snprintf(number, sizeof(number), "%u", (unsigned int)(index + 1u));
      status = s3_multipart_append_element(&builder, "Number", number);
    }
    if (status == SALTS_OK) {
      (void)snprintf(number, sizeof(number), "%llu", (unsigned long long)upload->parts[index].size);
      status = s3_multipart_append_element(&builder, "Size", number);
    }
    if (status == SALTS_OK)
      status = s3_multipart_append_element(&builder, "ETag", upload->parts[index].etag);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, "</Part>");
  }
  if (status == SALTS_OK)
    status = s3_text_builder_append_cstr(&builder, "</Parts></S3MultipartResume>");
  if (status == SALTS_OK) *out_xml = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_multipart_checkpoint_temp_path(const char *checkpoint_path, char **out_path) {
  static const char prefix[] = ".s3-";
  static const char suffix[] = ".tmp";
  salts_uuid_t uuid;
  char uuid_text[SALTS_UUID_STRING_SIZE];
  size_t base_size;
  size_t total_size;
  char *path;
  int status;
  if (checkpoint_path == NULL || out_path == NULL || *out_path != NULL) return SALTS_EINVAL;
  status = salts_uuid_v4_generate(&uuid);
  if (status == SALTS_OK) status = salts_uuid_format(&uuid, uuid_text, sizeof(uuid_text));
  if (status != SALTS_OK) return status;
  base_size = strlen(checkpoint_path);
  if (base_size >
      SIZE_MAX - (sizeof(prefix) - 1u) - SALTS_UUID_STRING_LENGTH - (sizeof(suffix) - 1u))
    return SALTS_ERANGE;
  total_size = base_size + sizeof(prefix) - 1u + SALTS_UUID_STRING_LENGTH + sizeof(suffix) - 1u;
  path = (char *)malloc(total_size + 1u);
  if (path == NULL) return SALTS_ENOMEM;
  memcpy(path, checkpoint_path, base_size);
  memcpy(path + base_size, prefix, sizeof(prefix) - 1u);
  memcpy(path + base_size + sizeof(prefix) - 1u, uuid_text, SALTS_UUID_STRING_LENGTH);
  memcpy(path + total_size - (sizeof(suffix) - 1u), suffix, sizeof(suffix));
  *out_path = path;
  return SALTS_OK;
}

static int s3_multipart_checkpoint_write_all(salts_file_t file, const char *data, size_t size) {
  size_t offset = 0u;
  while (offset < size) {
    const size_t remaining = size - offset;
    const size_t chunk = remaining < (size_t)INT_MAX ? remaining : (size_t)INT_MAX;
    const int written = salts_fs_write(file, data + offset, chunk);
    if (written <= 0) return written < 0 ? written : SALTS_EIO;
    offset += (size_t)written;
  }
  return SALTS_OK;
}

static int s3_multipart_checkpoint_store(const s3_client_base *client,
                                         const s3_multipart_impl *upload, const char *source_path,
                                         const salts_fs_stat_t *file_stat, size_t part_size,
                                         const char *checkpoint_path) {
  tstr xml = NULL;
  char *temporary_path = NULL;
  salts_file_t file = SALTS_INVALID_FILE;
  int close_status = SALTS_OK;
  int status = s3_multipart_checkpoint_xml(client, upload, source_path, file_stat, part_size, &xml);
  if (status == SALTS_OK)
    status = s3_multipart_checkpoint_temp_path(checkpoint_path, &temporary_path);
  if (status == SALTS_OK) {
    file = salts_fs_open(temporary_path, SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_O_TRUNC,
                         S3_MULTIPART_CHECKPOINT_MODE);
    if (file == SALTS_INVALID_FILE) status = SALTS_EIO;
  }
  if (status == SALTS_OK) status = s3_multipart_checkpoint_write_all(file, xml, tstr_len(xml));
  if (status == SALTS_OK) status = salts_fs_fsync(file);
  if (file != SALTS_INVALID_FILE) close_status = salts_fs_close(file);
  if (status == SALTS_OK) status = close_status;
  if (status == SALTS_OK) status = salts_fs_rename(temporary_path, checkpoint_path);
  if (status != SALTS_OK && temporary_path != NULL) (void)salts_fs_unlink(temporary_path);
  free(temporary_path);
  tstr_free(xml);
  return status == SALTS_OK ? SALTS_OK : SALTS_EIO;
}

static int s3_multipart_parse_u64(const char *text, uint64_t *out_value) {
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

static int s3_multipart_child_text(salts_xml_node parent, const char *name, char **out_text) {
  const salts_xml_node child = s3_xml_child(parent, name);
  if (child.impl == NULL || out_text == NULL || *out_text != NULL) return SALTS_EPROTO;
  return s3_xml_text_dup(child, out_text);
}

static int s3_multipart_checkpoint_part_parse(s3_multipart_impl *upload, salts_xml_node node) {
  char *number_text = NULL;
  char *size_text = NULL;
  char *etag = NULL;
  uint64_t number = 0u;
  uint64_t size = 0u;
  int status = s3_multipart_child_text(node, "Number", &number_text);
  if (status == SALTS_OK) status = s3_multipart_child_text(node, "Size", &size_text);
  if (status == SALTS_OK) status = s3_multipart_child_text(node, "ETag", &etag);
  if (status == SALTS_OK) status = s3_multipart_parse_u64(number_text, &number);
  if (status == SALTS_OK) status = s3_multipart_parse_u64(size_text, &size);
  if (status == SALTS_OK && (number == 0u || number > upload->part_capacity || size > SIZE_MAX ||
                             etag[0] == '\0' || upload->parts[number - 1u].etag != NULL))
    status = SALTS_EPROTO;
  if (status == SALTS_OK) {
    upload->parts[number - 1u].etag = etag;
    upload->parts[number - 1u].size = (size_t)size;
    ++upload->part_count;
    etag = NULL;
  }
  free(number_text);
  free(size_text);
  free(etag);
  return status;
}

static int s3_multipart_checkpoint_parts_parse(s3_multipart_impl *upload, salts_xml_node parts) {
  size_t index;
  int status = SALTS_OK;
  if (parts.impl == NULL) return SALTS_EPROTO;
  for (index = 0u; index < salts_xml_node_child_count(parts) && status == SALTS_OK; ++index) {
    const salts_xml_node child = salts_xml_node_child_at(parts, index);
    if (salts_xml_node_type(child) == SALTS_XML_ELEMENT) {
      if (!s3_xml_node_name_equal(child, "Part")) return SALTS_EPROTO;
      status = s3_multipart_checkpoint_part_parse(upload, child);
    }
  }
  return status;
}

static int s3_multipart_checkpoint_identity(const s3_client_base *client, salts_xml_node root,
                                            const char *bucket, const char *key,
                                            const char *source_path,
                                            const salts_fs_stat_t *file_stat, size_t part_size,
                                            const s3_multipart_impl *upload, char **out_upload_id) {
  static const char *const names[] = {
      "Version",   "ConnectionUri", "Authority",         "Region",   "AddressingStyle",
      "Bucket",    "Key",           "SourcePath",        "FileSize", "FileMtime",
      "FileCtime", "PartSize",      "SseCustomerKeyMd5", "UploadId"};
  char *values[sizeof(names) / sizeof(names[0])] = {0};
  char expected[32];
  size_t index;
  int status = SALTS_OK;
  for (index = 0u; index < sizeof(names) / sizeof(names[0]) && status == SALTS_OK; ++index)
    status = s3_multipart_child_text(root, names[index], &values[index]);
  if (status == SALTS_OK && strcmp(values[0], s3_multipart_checkpoint_version) != 0)
    status = SALTS_EPROTO;
  if (status == SALTS_OK && strcmp(values[1], client->connection_uri) != 0) status = SALTS_EINVAL;
  if (status == SALTS_OK && strcmp(values[2], client->authority) != 0) status = SALTS_EINVAL;
  if (status == SALTS_OK && strcmp(values[3], client->region) != 0) status = SALTS_EINVAL;
  (void)snprintf(expected, sizeof(expected), "%u", (unsigned int)client->config.addressing_style);
  if (status == SALTS_OK && strcmp(values[4], expected) != 0) status = SALTS_EINVAL;
  if (status == SALTS_OK && strcmp(values[5], bucket) != 0) status = SALTS_EINVAL;
  if (status == SALTS_OK && strcmp(values[6], key) != 0) status = SALTS_EINVAL;
  if (status == SALTS_OK && strcmp(values[7], source_path) != 0) status = SALTS_EINVAL;
  (void)snprintf(expected, sizeof(expected), "%llu", (unsigned long long)file_stat->size);
  if (status == SALTS_OK && strcmp(values[8], expected) != 0) status = SALTS_EINVAL;
  (void)snprintf(expected, sizeof(expected), "%llu", (unsigned long long)file_stat->mtime);
  if (status == SALTS_OK && strcmp(values[9], expected) != 0) status = SALTS_EINVAL;
  (void)snprintf(expected, sizeof(expected), "%llu", (unsigned long long)file_stat->ctime);
  if (status == SALTS_OK && strcmp(values[10], expected) != 0) status = SALTS_EINVAL;
  (void)snprintf(expected, sizeof(expected), "%llu", (unsigned long long)part_size);
  if (status == SALTS_OK && strcmp(values[11], expected) != 0) status = SALTS_EINVAL;
  if (status == SALTS_OK && strcmp(values[12], s3_multipart_customer_key_md5(upload)) != 0)
    status = SALTS_EINVAL;
  if (status == SALTS_OK && values[13][0] == '\0') status = SALTS_EPROTO;
  if (status == SALTS_OK) {
    *out_upload_id = values[13];
    values[13] = NULL;
  }
  for (index = 0u; index < sizeof(values) / sizeof(values[0]); ++index)
    free(values[index]);
  return status;
}

static int s3_multipart_checkpoint_load(const s3_client_base *client, const char *bucket,
                                        const char *key, const char *source_path,
                                        const salts_fs_stat_t *file_stat, size_t part_size,
                                        const char *checkpoint_path,
                                        const s3_put_object_options *options,
                                        s3_multipart *out_upload) {
  salts_fs_stat_t checkpoint_stat = {0};
  salts_fs_buf_t file = {0};
  salts_xml_document document = {0};
  salts_xml_node root = {0};
  salts_xml_node parts;
  s3_multipart_impl *upload = NULL;
  size_t parts_bytes = 0u;
  int status;
  status = salts_fs_stat(checkpoint_path, &checkpoint_stat);
  if (status != SALTS_OK) return status == -ENOENT ? SALTS_ENOENT : SALTS_EIO;
  if (!checkpoint_stat.is_file) return SALTS_EINVAL;
  if (checkpoint_stat.size > client->config.max_xml_bytes) return SALTS_EMSGSIZE;
  status = salts_fs_read_file(checkpoint_path, &file);
  if (status != SALTS_OK) return SALTS_EIO;
  status = s3_xml_parse_root(file.base, file.len, client->config.max_xml_bytes,
                             client->config.max_xml_nodes, s3_multipart_checkpoint_root, &document,
                             &root);
  if (status == SALTS_OK &&
      s3_checked_multiply(client->config.max_multipart_parts, sizeof(s3_multipart_part_internal),
                          &parts_bytes) != SALTS_OK)
    status = SALTS_ERANGE;
  if (status == SALTS_OK) {
    upload = (s3_multipart_impl *)calloc(1u, sizeof(*upload));
    if (upload == NULL) status = SALTS_ENOMEM;
  }
  if (status == SALTS_OK) {
    upload->part_capacity = client->config.max_multipart_parts;
    upload->parts = (s3_multipart_part_internal *)calloc(1u, parts_bytes);
    upload->bucket = tstr_dup(bucket);
    upload->key = tstr_dup(key);
    if (upload->parts == NULL || upload->bucket == NULL || upload->key == NULL)
      status = SALTS_ENOMEM;
  }
  if (status == SALTS_OK) status = s3_multipart_part_sse_headers_init(client, options, upload);
  if (status == SALTS_OK)
    status = s3_multipart_checkpoint_identity(client, root, bucket, key, source_path, file_stat,
                                              part_size, upload, &upload->upload_id);
  if (status == SALTS_OK) {
    parts = s3_xml_child(root, "Parts");
    status = s3_multipart_checkpoint_parts_parse(upload, parts);
  }
  if (status == SALTS_OK) {
    upload->state = S3_MULTIPART_ACTIVE;
    out_upload->impl = upload;
    upload = NULL;
  }
  s3_multipart_impl_free(upload);
  salts_xml_document_destroy(&document);
  salts_fs_buf_free(&file);
  return status;
}

static int s3_multipart_completion_xml(const s3_client_impl *client,
                                       const s3_multipart_impl *upload, tstr *out_xml) {
  static const char prefix[] = "<CompleteMultipartUpload>";
  static const char suffix[] = "</CompleteMultipartUpload>";
  static const char part_prefix[] = "<Part><PartNumber>";
  static const char part_middle[] = "</PartNumber><ETag>";
  static const char part_suffix[] = "</ETag></Part>";
  s3_text_builder builder = {0};
  size_t capacity = sizeof(prefix) - 1u + sizeof(suffix) - 1u;
  size_t index;
  int status = SALTS_OK;
  for (index = 0u; index < upload->part_capacity && status == SALTS_OK; ++index) {
    size_t etag_limit;
    if (upload->parts[index].etag == NULL) continue;
    if (strlen(upload->parts[index].etag) > (SIZE_MAX / 6u)) return SALTS_ERANGE;
    etag_limit = strlen(upload->parts[index].etag) * 6u;
    if (s3_checked_add(capacity,
                       sizeof(part_prefix) - 1u + S3_MULTIPART_PART_NUMBER_TEXT_BYTES +
                           sizeof(part_middle) - 1u + sizeof(part_suffix) - 1u,
                       &capacity) != SALTS_OK ||
        s3_checked_add(capacity, etag_limit, &capacity) != SALTS_OK ||
        capacity > client->base.config.max_xml_bytes)
      return SALTS_EMSGSIZE;
  }
  status = s3_text_builder_init(&builder, capacity);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, prefix);
  for (index = 0u; index < upload->part_capacity && status == SALTS_OK; ++index) {
    char part_number[16];
    int written;
    if (upload->parts[index].etag == NULL) continue;
    written = snprintf(part_number, sizeof(part_number), "%u", (unsigned int)(index + 1u));
    if (written <= 0 || (size_t)written >= sizeof(part_number)) {
      status = SALTS_ERANGE;
      break;
    }
    status = s3_text_builder_append_cstr(&builder, part_prefix);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, part_number);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, part_middle);
    if (status == SALTS_OK)
      status = s3_multipart_append_xml_text(&builder, upload->parts[index].etag);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, part_suffix);
  }
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, suffix);
  if (status == SALTS_OK) *out_xml = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_multipart_validate_completion(const s3_multipart_impl *upload) {
  size_t highest;
  size_t index;
  if (upload->part_count == 0u) return SALTS_EINVAL;
  highest = upload->part_capacity;
  while (highest != 0u && upload->parts[highest - 1u].etag == NULL)
    --highest;
  if (highest == 0u || highest != upload->part_count) return SALTS_EINVAL;
  for (index = 0u; index + 1u < highest; ++index) {
    if (upload->parts[index].etag == NULL ||
        upload->parts[index].size < S3_MULTIPART_MIN_PART_BYTES)
      return SALTS_EINVAL;
  }
  return SALTS_OK;
}

static int s3_multipart_complete_response_validate(const s3_client_impl *client,
                                                   s3_response *response, s3_error *out_error) {
  salts_xml_document document = {0};
  salts_xml_node root = {0};
  if (response->http.body == NULL || response->http.body_size == 0u)
    return s3_multipart_error(out_error, SALTS_EPROTO, "s3-multipart-complete-parse");
  int status = s3_xml_parse_root(
      response->http.body, response->http.body_size, client->base.config.max_xml_bytes,
      client->base.config.max_xml_nodes, "CompleteMultipartUploadResult", &document, &root);
  salts_xml_document_destroy(&document);
  if (status == SALTS_OK) return SALTS_OK;
  if (status != SALTS_EPROTO)
    return s3_multipart_error(out_error, status, "s3-multipart-complete-parse");
  status = s3_xml_parse_root(response->http.body, response->http.body_size,
                             client->base.config.max_xml_bytes, client->base.config.max_xml_nodes,
                             "Error", &document, &root);
  salts_xml_document_destroy(&document);
  if (status != SALTS_OK)
    return s3_multipart_error(out_error, status, "s3-multipart-complete-parse");
  status = s3_response_parse_service_error(response, client->base.config.max_xml_bytes,
                                           client->base.config.max_xml_nodes);
  if (status != SALTS_OK)
    return s3_multipart_error(out_error, status, "s3-multipart-complete-parse");
  return s3_multipart_error(out_error, SALTS_EPROTO, "s3-multipart-complete-service");
}

int s3_multipart_complete(s3_client *client, s3_multipart *upload, s3_response *out_response,
                          s3_error *out_error) {
  s3_client_impl *client_impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_multipart_impl *impl = NULL;
  s3_query_param query;
  chttp_header header = {"Content-Type", "application/xml"};
  s3_request_options request;
  tstr xml = NULL;
  int status;
  if (out_error == NULL || out_response == NULL || !s3_response_is_empty(out_response))
    return SALTS_EINVAL;
  *out_error = (s3_error){0};
  status = s3_multipart_active(upload, &impl, out_error, "s3-multipart-complete");
  if (status != SALTS_OK) return status;
  if (client_impl == NULL)
    return s3_multipart_error(out_error, SALTS_EINVAL, "s3-multipart-complete");
  status = s3_multipart_validate_completion(impl);
  if (status == SALTS_OK) status = s3_multipart_completion_xml(client_impl, impl, &xml);
  if (status != SALTS_OK) return s3_multipart_error(out_error, status, "s3-multipart-complete");
  query = (s3_query_param){"uploadId", impl->upload_id};
  request = (s3_request_options){.size = sizeof(request),
                                 .method = S3_METHOD_POST,
                                 .bucket = impl->bucket,
                                 .key = impl->key,
                                 .query = &query,
                                 .query_count = 1u,
                                 .headers = &header,
                                 .header_count = 1u,
                                 .body = xml,
                                 .body_size = tstr_len(xml)};
  status = s3_request(client, &request, out_response, out_error);
  if (status == SALTS_OK)
    status = s3_multipart_complete_response_validate(client_impl, out_response, out_error);
  if (status == SALTS_OK) impl->state = S3_MULTIPART_COMPLETED;
  tstr_free(xml);
  return status;
}

int s3_multipart_abort(s3_client *client, s3_multipart *upload, s3_response *out_response,
                       s3_error *out_error) {
  s3_multipart_impl *impl = NULL;
  s3_query_param query;
  s3_request_options request;
  int status;
  if (out_error == NULL || out_response == NULL || !s3_response_is_empty(out_response))
    return SALTS_EINVAL;
  *out_error = (s3_error){0};
  status = s3_multipart_active(upload, &impl, out_error, "s3-multipart-abort");
  if (status != SALTS_OK) return status;
  query = (s3_query_param){"uploadId", impl->upload_id};
  request = (s3_request_options){.size = sizeof(request),
                                 .method = S3_METHOD_DELETE,
                                 .bucket = impl->bucket,
                                 .key = impl->key,
                                 .query = &query,
                                 .query_count = 1u};
  status = s3_request(client, &request, out_response, out_error);
  if (status == SALTS_OK) impl->state = S3_MULTIPART_ABORTED;
  return status;
}

static int s3_multipart_file_read_part(salts_file_t file, unsigned char *buffer, size_t size,
                                       uint64_t offset, s3_error *out_error) {
  size_t consumed = 0u;
  if (file == SALTS_INVALID_FILE || buffer == NULL || size == 0u || offset > INT64_MAX)
    return s3_multipart_error(out_error, SALTS_EINVAL, "multipart-file-read");
  while (consumed < size) {
    if ((uint64_t)consumed > (uint64_t)INT64_MAX - offset)
      return s3_multipart_error(out_error, SALTS_ERANGE, "multipart-file-read");
    const int read_size = salts_fs_pread(file, (char *)buffer + consumed, size - consumed,
                                         (int64_t)(offset + consumed));
    if (read_size <= 0) return s3_multipart_error(out_error, SALTS_EIO, "multipart-file-read");
    consumed += (size_t)read_size;
  }
  return SALTS_OK;
}

static int s3_multipart_file_state_validate(const s3_multipart_impl *upload, uint64_t file_size,
                                            size_t part_size, size_t part_count) {
  size_t index;
  for (index = 0u; index < upload->part_capacity; ++index) {
    const uint64_t offset = (uint64_t)index * (uint64_t)part_size;
    size_t expected_size;
    if (upload->parts[index].etag == NULL) continue;
    if (index >= part_count || offset >= file_size) return SALTS_EPROTO;
    expected_size =
        file_size - offset < (uint64_t)part_size ? (size_t)(file_size - offset) : part_size;
    if (upload->parts[index].size != expected_size) return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int s3_multipart_file_failure(s3_client *client, s3_multipart *upload,
                                     const char *checkpoint_path, int preserve_on_failure,
                                     int failure_status, s3_error *out_error) {
  s3_response abort_response = {0};
  s3_error abort_error = {0};
  s3_multipart_impl *impl = upload != NULL ? (s3_multipart_impl *)upload->impl : NULL;
  if (impl == NULL) return failure_status;
  if (preserve_on_failure) {
    (void)s3_multipart_detach(upload);
  } else {
    const int abort_status = s3_multipart_abort(client, upload, &abort_response, &abort_error);
    s3_response_destroy(&abort_response);
    if (abort_status == SALTS_OK) {
      (void)salts_fs_unlink(checkpoint_path);
    } else {
      (void)s3_multipart_detach(upload);
      *out_error = abort_error;
      failure_status = abort_status;
    }
  }
  (void)s3_multipart_destroy(upload);
  return failure_status;
}

int s3_put_object_multipart_file(s3_client *client, const char *bucket, const char *key,
                                 const char *path, const s3_multipart_file_options *options,
                                 s3_response *out_response, s3_error *out_error) {
  s3_client_impl *client_impl = client != NULL ? (s3_client_impl *)client->impl : NULL;
  s3_multipart upload = {0};
  s3_multipart_impl *upload_impl;
  salts_fs_stat_t file_stat = {0};
  salts_fs_stat_t final_stat = {0};
  salts_fs_stat_t checkpoint_stat = {0};
  salts_file_t file = SALTS_INVALID_FILE;
  unsigned char *buffer = NULL;
  size_t part_size;
  size_t part_count;
  size_t part_index;
  size_t transferred = 0u;
  int close_status = SALTS_OK;
  int checkpoint_available = 0;
  int status;
  if (out_response == NULL || out_error == NULL || !s3_response_is_empty(out_response))
    return SALTS_EINVAL;
  *out_error = (s3_error){0};
  if (client_impl == NULL || bucket == NULL || key == NULL || path == NULL || path[0] == '\0' ||
      options == NULL || options->size != sizeof(*options) || options->checkpoint_path == NULL ||
      options->checkpoint_path[0] == '\0' ||
      (options->resume_existing != 0 && options->resume_existing != 1) ||
      (options->preserve_on_failure != 0 && options->preserve_on_failure != 1))
    return s3_multipart_error(out_error, SALTS_EINVAL, "multipart-file-options");
  if (client_impl->operation_active)
    return s3_multipart_error(out_error, SALTS_EBUSY, "multipart-file-options");
  part_size = options->part_size != 0u ? options->part_size : S3_MULTIPART_DEFAULT_PART_BYTES;
  if (part_size < S3_MULTIPART_MIN_PART_BYTES ||
      part_size > client_impl->base.config.max_multipart_part_bytes)
    return s3_multipart_error(out_error, SALTS_EINVAL, "multipart-file-part-size");
  status = salts_fs_stat(path, &file_stat);
  if (status != SALTS_OK || !file_stat.is_file || file_stat.size == 0u ||
      file_stat.size > SIZE_MAX || file_stat.size > INT64_MAX)
    return s3_multipart_error(out_error, status != SALTS_OK ? SALTS_EIO : SALTS_EINVAL,
                              "multipart-file-stat");
  part_count = (size_t)(file_stat.size / (uint64_t)part_size);
  if (file_stat.size % (uint64_t)part_size != 0u) ++part_count;
  if (part_count == 0u || part_count > client_impl->base.config.max_multipart_parts)
    return s3_multipart_error(out_error, SALTS_ENOBUFS, "multipart-file-parts");
  file = salts_fs_open(path, SALTS_FS_O_RDONLY, 0);
  if (file == SALTS_INVALID_FILE)
    return s3_multipart_error(out_error, SALTS_EIO, "multipart-file-open");
  buffer = (unsigned char *)malloc(part_size);
  if (buffer == NULL) {
    (void)salts_fs_close(file);
    return s3_multipart_error(out_error, SALTS_ENOMEM, "multipart-file-buffer");
  }
  if (options->resume_existing) {
    status =
        s3_multipart_checkpoint_load(&client_impl->base, bucket, key, path, &file_stat, part_size,
                                     options->checkpoint_path, options->put_options, &upload);
    if (status == SALTS_OK) checkpoint_available = 1;
    if (status != SALTS_OK) s3_multipart_error(out_error, status, "multipart-checkpoint-load");
  } else {
    const int checkpoint_status = salts_fs_stat(options->checkpoint_path, &checkpoint_stat);
    if (checkpoint_status == SALTS_OK) {
      status = s3_multipart_error(out_error, SALTS_EALREADY, "multipart-checkpoint-exists");
    } else if (checkpoint_status != -ENOENT) {
      status = s3_multipart_error(out_error, SALTS_EIO, "multipart-checkpoint-stat");
    } else {
      status = s3_multipart_initiate(client, bucket, key, options->put_options, &upload,
                                     out_response, out_error);
      if (status == SALTS_OK) {
        s3_response_destroy(out_response);
        upload_impl = (s3_multipart_impl *)upload.impl;
        status = s3_multipart_checkpoint_store(&client_impl->base, upload_impl, path, &file_stat,
                                               part_size, options->checkpoint_path);
        if (status == SALTS_OK) checkpoint_available = 1;
        if (status != SALTS_OK) s3_multipart_error(out_error, status, "multipart-checkpoint-store");
      }
    }
  }
  upload_impl = (s3_multipart_impl *)upload.impl;
  if (status == SALTS_OK)
    status = s3_multipart_file_state_validate(upload_impl, file_stat.size, part_size, part_count);
  if (status != SALTS_OK) goto failed;
  for (part_index = 0u; part_index < part_count; ++part_index) {
    const uint64_t offset = (uint64_t)part_index * (uint64_t)part_size;
    const size_t current_size = file_stat.size - offset < (uint64_t)part_size
                                    ? (size_t)(file_stat.size - offset)
                                    : part_size;
    if (upload_impl->parts[part_index].etag == NULL) {
      status = s3_multipart_file_read_part(file, buffer, current_size, offset, out_error);
      if (status == SALTS_OK)
        status = s3_multipart_upload_part(client, &upload, (uint32_t)(part_index + 1u), buffer,
                                          current_size, out_response, out_error);
      if (status != SALTS_OK) goto failed;
      s3_response_destroy(out_response);
      status = s3_multipart_checkpoint_store(&client_impl->base, upload_impl, path, &file_stat,
                                             part_size, options->checkpoint_path);
      if (status != SALTS_OK) {
        s3_multipart_error(out_error, status, "multipart-checkpoint-store");
        goto failed;
      }
    }
    transferred += current_size;
    if (options->progress != NULL) {
      client_impl->operation_active = 1;
      options->progress(options->progress_user, transferred, (size_t)file_stat.size);
      client_impl->operation_active = 0;
    }
  }
  status = salts_fs_stat(path, &final_stat);
  if (status != SALTS_OK || !final_stat.is_file || final_stat.size != file_stat.size ||
      final_stat.mtime != file_stat.mtime || final_stat.ctime != file_stat.ctime) {
    status = s3_multipart_error(out_error, SALTS_EBUSY, "multipart-file-changed");
    goto failed;
  }
  close_status = salts_fs_close(file);
  file = SALTS_INVALID_FILE;
  if (close_status != SALTS_OK) {
    status = s3_multipart_error(out_error, SALTS_EIO, "multipart-file-close");
    goto failed;
  }
  status = s3_multipart_complete(client, &upload, out_response, out_error);
  if (status != SALTS_OK) goto failed;
  if (salts_fs_unlink(options->checkpoint_path) != SALTS_OK)
    status = s3_multipart_error(out_error, SALTS_EIO, "multipart-checkpoint-remove");
  OPENSSL_cleanse(buffer, part_size);
  free(buffer);
  (void)s3_multipart_destroy(&upload);
  return status;

failed:
  if (file != SALTS_INVALID_FILE) (void)salts_fs_close(file);
  OPENSSL_cleanse(buffer, part_size);
  free(buffer);
  return s3_multipart_file_failure(client, &upload, options->checkpoint_path,
                                   options->preserve_on_failure && checkpoint_available, status,
                                   out_error);
}

int s3_multipart_detach(s3_multipart *upload) {
  s3_multipart_impl *impl = upload != NULL ? (s3_multipart_impl *)upload->impl : NULL;
  if (impl == NULL || impl->state != S3_MULTIPART_ACTIVE) return SALTS_EINVAL;
  impl->state = S3_MULTIPART_DETACHED_FOR_RESUME;
  return SALTS_OK;
}

int s3_multipart_state_get(const s3_multipart *upload, s3_multipart_state *out_state) {
  const s3_multipart_impl *impl = upload != NULL ? (const s3_multipart_impl *)upload->impl : NULL;
  if (impl == NULL || out_state == NULL) return SALTS_EINVAL;
  *out_state = impl->state;
  return SALTS_OK;
}

const char *s3_multipart_upload_id(const s3_multipart *upload) {
  const s3_multipart_impl *impl = upload != NULL ? (const s3_multipart_impl *)upload->impl : NULL;
  return impl != NULL ? impl->upload_id : NULL;
}

int s3_multipart_destroy(s3_multipart *upload) {
  s3_multipart_impl *impl;
  if (upload == NULL) return SALTS_EINVAL;
  impl = (s3_multipart_impl *)upload->impl;
  if (impl == NULL) return SALTS_OK;
  if (impl->state == S3_MULTIPART_ACTIVE) return SALTS_EBUSY;
  s3_multipart_impl_free(impl);
  upload->impl = NULL;
  return SALTS_OK;
}
