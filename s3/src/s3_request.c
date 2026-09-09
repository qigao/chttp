#include "s3_internal.h"

#include <openssl/crypto.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct s3_encoded_query {
  tstr name;
  tstr value;
} s3_encoded_query;

int s3_checked_add(size_t left, size_t right, size_t *out) {
  if (out == NULL || right > SIZE_MAX - left) return SALTS_ERANGE;
  *out = left + right;
  return SALTS_OK;
}

int s3_checked_multiply(size_t left, size_t right, size_t *out) {
  if (out == NULL || (left != 0u && right > SIZE_MAX / left)) return SALTS_ERANGE;
  *out = left * right;
  return SALTS_OK;
}

int s3_text_builder_init(s3_text_builder *builder, size_t capacity) {
  tstr reserved;

  if (builder == NULL || capacity == SIZE_MAX) return SALTS_EINVAL;
  memset(builder, 0, sizeof(*builder));
  builder->text = tstr_new();
  if (builder->text == NULL) return SALTS_ENOMEM;
  reserved = tstr_reserve(builder->text, capacity);
  if (reserved == NULL) {
    tstr_free(builder->text);
    memset(builder, 0, sizeof(*builder));
    return SALTS_ENOMEM;
  }
  builder->text = reserved;
  builder->capacity = capacity;
  return SALTS_OK;
}

int s3_text_builder_append(s3_text_builder *builder, const void *data, size_t size) {
  size_t next_size;
  size_t current_size;

  if (builder == NULL || builder->text == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;
  current_size = tstr_len(builder->text);
  if (s3_checked_add(current_size, size, &next_size) != SALTS_OK || next_size > builder->capacity)
    return SALTS_EMSGSIZE;
  if (size != 0u) memcpy(builder->text + current_size, data, size);
  if (!tstr_set_len_checked(builder->text, next_size)) return SALTS_EPROTO;
  return SALTS_OK;
}

int s3_text_builder_append_cstr(s3_text_builder *builder, const char *text) {
  return text != NULL ? s3_text_builder_append(builder, text, strlen(text)) : SALTS_EINVAL;
}

tstr s3_text_builder_release(s3_text_builder *builder) {
  tstr text;
  if (builder == NULL) return NULL;
  text = builder->text;
  memset(builder, 0, sizeof(*builder));
  return text;
}

void s3_text_builder_destroy(s3_text_builder *builder) {
  if (builder == NULL) return;
  tstr_free(builder->text);
  memset(builder, 0, sizeof(*builder));
}

static int s3_uri_unreserved(unsigned char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
         value == '~';
}

int s3_uri_encode(const char *input, int preserve_slash, size_t max_bytes, tstr *out) {
  static const char hex[] = "0123456789ABCDEF";
  s3_text_builder builder = {0};
  size_t encoded_size = 0u;
  size_t index;
  int status;

  if (input == NULL || out == NULL || *out != NULL || max_bytes == 0u) return SALTS_EINVAL;
  for (index = 0u; input[index] != '\0'; ++index) {
    const unsigned char value = (unsigned char)input[index];
    const size_t width = s3_uri_unreserved(value) || (preserve_slash && value == '/') ? 1u : 3u;
    if (s3_checked_add(encoded_size, width, &encoded_size) != SALTS_OK || encoded_size > max_bytes)
      return SALTS_EMSGSIZE;
  }
  status = s3_text_builder_init(&builder, encoded_size);
  if (status != SALTS_OK) return status;
  for (index = 0u; input[index] != '\0' && status == SALTS_OK; ++index) {
    const unsigned char value = (unsigned char)input[index];
    if (s3_uri_unreserved(value) || (preserve_slash && value == '/')) {
      status = s3_text_builder_append(&builder, &input[index], 1u);
    } else {
      const char encoded[3] = {'%', hex[value >> 4u], hex[value & 0x0fu]};
      status = s3_text_builder_append(&builder, encoded, sizeof(encoded));
    }
  }
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_encoded_query_compare(const void *left, const void *right) {
  const s3_encoded_query *a = (const s3_encoded_query *)left;
  const s3_encoded_query *b = (const s3_encoded_query *)right;
  const int name_order = strcmp(a->name, b->name);
  return name_order != 0 ? name_order : strcmp(a->value, b->value);
}

int s3_query_canonicalize(const s3_signer_query *query, size_t query_count, size_t max_query_count,
                          size_t max_target_bytes, tstr *out) {
  s3_encoded_query *encoded = NULL;
  s3_text_builder builder = {0};
  size_t allocation_size = 0u;
  size_t output_size = 0u;
  size_t index;
  int status = SALTS_OK;

  if (out == NULL || *out != NULL || max_query_count == 0u || max_target_bytes == 0u ||
      (query_count != 0u && query == NULL))
    return SALTS_EINVAL;
  if (query_count > max_query_count) return SALTS_ENOBUFS;
  if (query_count == 0u) {
    *out = tstr_new();
    return *out != NULL ? SALTS_OK : SALTS_ENOMEM;
  }
  if (s3_checked_multiply(query_count, sizeof(*encoded), &allocation_size) != SALTS_OK)
    return SALTS_ERANGE;
  encoded = (s3_encoded_query *)calloc(1u, allocation_size);
  if (encoded == NULL) return SALTS_ENOMEM;
  for (index = 0u; index < query_count; ++index) {
    if (query[index].name == NULL || query[index].name[0] == '\0' || query[index].value == NULL) {
      status = SALTS_EINVAL;
      break;
    }
    status = s3_uri_encode(query[index].name, 0, max_target_bytes, &encoded[index].name);
    if (status == SALTS_OK)
      status = s3_uri_encode(query[index].value, 0, max_target_bytes, &encoded[index].value);
    if (status != SALTS_OK) break;
    if (s3_checked_add(output_size, tstr_len(encoded[index].name), &output_size) != SALTS_OK ||
        s3_checked_add(output_size, 1u, &output_size) != SALTS_OK ||
        s3_checked_add(output_size, tstr_len(encoded[index].value), &output_size) != SALTS_OK ||
        (index != 0u && s3_checked_add(output_size, 1u, &output_size) != SALTS_OK)) {
      status = SALTS_ERANGE;
      break;
    }
    if (output_size > max_target_bytes) {
      status = SALTS_EMSGSIZE;
      break;
    }
  }
  if (status == SALTS_OK) {
    qsort(encoded, query_count, sizeof(*encoded), s3_encoded_query_compare);
    status = s3_text_builder_init(&builder, output_size);
  }
  for (index = 0u; index < query_count && status == SALTS_OK; ++index) {
    if (index != 0u) status = s3_text_builder_append(&builder, "&", 1u);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, encoded[index].name);
    if (status == SALTS_OK) status = s3_text_builder_append(&builder, "=", 1u);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, encoded[index].value);
  }
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  for (index = 0u; index < query_count; ++index) {
    tstr_free(encoded[index].name);
    tstr_free(encoded[index].value);
  }
  free(encoded);
  return status;
}

int s3_bucket_name_valid(const char *bucket, size_t max_bytes) {
  size_t size;
  size_t index;
  if (bucket == NULL) return 1;
  size = strlen(bucket);
  if (size < 3u || size > max_bytes) return 0;
  if (!((bucket[0] >= 'a' && bucket[0] <= 'z') || (bucket[0] >= '0' && bucket[0] <= '9')) ||
      !((bucket[size - 1u] >= 'a' && bucket[size - 1u] <= 'z') ||
        (bucket[size - 1u] >= '0' && bucket[size - 1u] <= '9')))
    return 0;
  for (index = 0u; index < size; ++index) {
    const char value = bucket[index];
    if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-' ||
          value == '.'))
      return 0;
    if (index != 0u &&
        ((value == '.' && (bucket[index - 1u] == '.' || bucket[index - 1u] == '-')) ||
         (value == '-' && bucket[index - 1u] == '.')))
      return 0;
  }
  return 1;
}

static int s3_header_name_equal(const char *left, const char *right) {
  size_t index;
  if (left == NULL || right == NULL) return 0;
  for (index = 0u; left[index] != '\0' && right[index] != '\0'; ++index) {
    char lhs = left[index];
    char rhs = right[index];
    if (lhs >= 'A' && lhs <= 'Z') lhs = (char)(lhs + ('a' - 'A'));
    if (rhs >= 'A' && rhs <= 'Z') rhs = (char)(rhs + ('a' - 'A'));
    if (lhs != rhs) return 0;
  }
  return left[index] == right[index];
}

static int s3_application_header_valid(const chttp_header *header) {
  return header != NULL && header->name != NULL && header->value != NULL &&
         !s3_header_name_equal(header->name, "host") &&
         !s3_header_name_equal(header->name, "authorization") &&
         !s3_header_name_equal(header->name, "x-amz-date") &&
         !s3_header_name_equal(header->name, "x-amz-content-sha256") &&
         !s3_header_name_equal(header->name, "x-amz-security-token");
}

static int s3_method_map(s3_method method, chttp_method *out_method, const char **out_name) {
  if (out_method == NULL || out_name == NULL) return SALTS_EINVAL;
  switch (method) {
  case S3_METHOD_GET:
    *out_method = CHTTP_METHOD_GET;
    *out_name = "GET";
    return SALTS_OK;
  case S3_METHOD_HEAD:
    *out_method = CHTTP_METHOD_HEAD;
    *out_name = "HEAD";
    return SALTS_OK;
  case S3_METHOD_POST:
    *out_method = CHTTP_METHOD_POST;
    *out_name = "POST";
    return SALTS_OK;
  case S3_METHOD_PUT:
    *out_method = CHTTP_METHOD_PUT;
    *out_name = "PUT";
    return SALTS_OK;
  case S3_METHOD_DELETE:
    *out_method = CHTTP_METHOD_DELETE;
    *out_name = "DELETE";
    return SALTS_OK;
  default:
    return SALTS_EINVAL;
  }
}

static int s3_request_time(const s3_client_base *client, char out[S3_SIGNER_AMZ_DATE_SIZE + 1u]) {
  const int64_t seconds = client->config.clock != NULL
                              ? client->config.clock(client->config.clock_user)
                              : (int64_t)time(NULL);
  time_t value;
  struct tm utc;
  if (seconds < 0) return SALTS_ERANGE;
  value = (time_t)seconds;
  if ((int64_t)value != seconds) return SALTS_ERANGE;
#if defined(_WIN32)
  if (gmtime_s(&utc, &value) != 0) return SALTS_ERANGE;
#else
  if (gmtime_r(&value, &utc) == NULL) return SALTS_ERANGE;
#endif
  return strftime(out, S3_SIGNER_AMZ_DATE_SIZE + 1u, "%Y%m%dT%H%M%SZ", &utc) ==
                 S3_SIGNER_AMZ_DATE_SIZE
             ? SALTS_OK
             : SALTS_ERANGE;
}

static int s3_request_authority(const s3_client_base *client, const char *bucket, tstr *out) {
  s3_text_builder builder = {0};
  size_t required;
  int status;
  if (client->config.addressing_style == S3_ADDRESSING_PATH || bucket == NULL)
    return (*out = tstr_dup(client->authority)) != NULL ? SALTS_OK : SALTS_ENOMEM;
  required = strlen(bucket);
  if (s3_checked_add(required, 1u, &required) != SALTS_OK ||
      s3_checked_add(required, tstr_len(client->authority), &required) != SALTS_OK ||
      required > client->config.max_header_bytes)
    return SALTS_EMSGSIZE;
  status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, bucket);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, ".", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, client->authority);
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_request_canonical_uri(const s3_client_base *client, const s3_request_options *options,
                                    tstr *out) {
  s3_text_builder builder = {0};
  tstr encoded_bucket = NULL;
  tstr encoded_key = NULL;
  size_t required = 1u;
  int status = SALTS_OK;
  if (options->bucket != NULL && client->config.addressing_style == S3_ADDRESSING_PATH)
    status = s3_uri_encode(options->bucket, 0, client->config.max_target_bytes, &encoded_bucket);
  if (status == SALTS_OK && options->key != NULL)
    status = s3_uri_encode(options->key, 1, client->config.max_target_bytes, &encoded_key);
  if (status == SALTS_OK && encoded_bucket != NULL)
    status = s3_checked_add(required, tstr_len(encoded_bucket), &required);
  if (status == SALTS_OK && encoded_key != NULL) {
    if (encoded_bucket != NULL) status = s3_checked_add(required, 1u, &required);
    if (status == SALTS_OK) status = s3_checked_add(required, tstr_len(encoded_key), &required);
  }
  if (status == SALTS_OK && required > client->config.max_target_bytes) status = SALTS_EMSGSIZE;
  if (status == SALTS_OK) status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "/", 1u);
  if (status == SALTS_OK && encoded_bucket != NULL)
    status = s3_text_builder_append_cstr(&builder, encoded_bucket);
  if (status == SALTS_OK && encoded_key != NULL) {
    if (encoded_bucket != NULL) status = s3_text_builder_append(&builder, "/", 1u);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, encoded_key);
  }
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  tstr_free(encoded_bucket);
  tstr_free(encoded_key);
  return status;
}

static int s3_request_target(const s3_client_base *client, const char *canonical_uri,
                             const char *canonical_query, tstr *out) {
  s3_text_builder builder = {0};
  size_t required = strlen(canonical_uri);
  int status;
  if (canonical_query[0] != '\0' &&
      (s3_checked_add(required, 1u, &required) != SALTS_OK ||
       s3_checked_add(required, strlen(canonical_query), &required) != SALTS_OK))
    return SALTS_ERANGE;
  if (required > client->config.max_target_bytes) return SALTS_EMSGSIZE;
  status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, canonical_uri);
  if (status == SALTS_OK && canonical_query[0] != '\0') {
    status = s3_text_builder_append(&builder, "?", 1u);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, canonical_query);
  }
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_request_copy_header(chttp_header *header, const char *name, const char *value) {
  char *name_copy;
  char *value_copy;
  if (header == NULL || name == NULL || value == NULL) return SALTS_EINVAL;
  name_copy = tstr_dup(name);
  value_copy = tstr_dup(value);
  if (name_copy == NULL || value_copy == NULL) {
    tstr_free(name_copy);
    tstr_free(value_copy);
    return SALTS_ENOMEM;
  }
  *header = (chttp_header){name_copy, value_copy};
  return SALTS_OK;
}

static int s3_request_transport_headers(const s3_client_base *client,
                                        const s3_request_options *options,
                                        const s3_credentials *credentials,
                                        const s3_signer_result *signing, const char *payload_hash,
                                        const char *amz_date, s3_request_plan *plan) {
  size_t count;
  size_t allocation_size;
  size_t aggregate = 0u;
  size_t index;
  int status = SALTS_OK;
  if (s3_checked_add(options->header_count, 3u, &count) != SALTS_OK) return SALTS_ERANGE;
  if (credentials->session_token != NULL && credentials->session_token[0] != '\0' &&
      s3_checked_add(count, 1u, &count) != SALTS_OK)
    return SALTS_ERANGE;
  if (count > client->config.max_header_count ||
      s3_checked_multiply(count, sizeof(*plan->headers), &allocation_size) != SALTS_OK)
    return SALTS_ENOBUFS;
  plan->headers = (chttp_header *)calloc(1u, allocation_size);
  if (plan->headers == NULL) return SALTS_ENOMEM;
  plan->header_count = count;
  for (index = 0u; index < options->header_count && status == SALTS_OK; ++index)
    status = s3_request_copy_header(&plan->headers[index], options->headers[index].name,
                                    options->headers[index].value);
  if (status == SALTS_OK)
    status =
        s3_request_copy_header(&plan->headers[index++], "Authorization", signing->authorization);
  if (status == SALTS_OK)
    status = s3_request_copy_header(&plan->headers[index++], "X-Amz-Date", amz_date);
  if (status == SALTS_OK)
    status = s3_request_copy_header(&plan->headers[index++], "X-Amz-Content-Sha256", payload_hash);
  if (status == SALTS_OK && index < count)
    status = s3_request_copy_header(&plan->headers[index++], "X-Amz-Security-Token",
                                    credentials->session_token);
  for (index = 0u; index < count && status == SALTS_OK; ++index) {
    if (s3_checked_add(aggregate, strlen(plan->headers[index].name), &aggregate) != SALTS_OK ||
        s3_checked_add(aggregate, strlen(plan->headers[index].value), &aggregate) != SALTS_OK ||
        s3_checked_add(aggregate, 4u, &aggregate) != SALTS_OK ||
        aggregate > client->config.max_header_bytes)
      status = SALTS_EMSGSIZE;
  }
  return status;
}

static int s3_request_options_validate(const s3_client_base *client,
                                       const s3_request_options *options) {
  size_t index;
  if (options == NULL || options->size != sizeof(*options) ||
      !s3_bucket_name_valid(options->bucket, client->config.max_bucket_name_bytes) ||
      (options->key != NULL && (options->bucket == NULL || options->key[0] == '\0' ||
                                strlen(options->key) > client->config.max_object_key_bytes)) ||
      (options->query_count != 0u && options->query == NULL) ||
      options->query_count > client->config.max_query_count ||
      (options->header_count != 0u && options->headers == NULL) ||
      options->header_count > client->config.max_header_count ||
      (options->body == NULL && options->body_size != 0u) ||
      (options->body_source != NULL && (options->body != NULL || options->body_size != 0u)) ||
      (options->body_source != NULL && options->payload_sha256 == NULL) ||
      (options->body_source == NULL && options->payload_sha256 != NULL))
    return SALTS_EINVAL;
  for (index = 0u; index < options->header_count; ++index) {
    if (!s3_application_header_valid(&options->headers[index])) return SALTS_EINVAL;
  }
  return SALTS_OK;
}

void s3_request_plan_destroy(s3_request_plan *plan) {
  size_t index;
  if (plan == NULL) return;
  tstr_free(plan->authority);
  tstr_free(plan->canonical_uri);
  tstr_free(plan->target);
  tstr_free(plan->payload_sha256);
  for (index = 0u; index < plan->header_count; ++index) {
    if (plan->headers[index].value != NULL)
      OPENSSL_cleanse((void *)plan->headers[index].value,
                      tstr_len((tstr)plan->headers[index].value));
    tstr_free((tstr)plan->headers[index].name);
    tstr_free((tstr)plan->headers[index].value);
  }
  free(plan->headers);
  *plan = (s3_request_plan){0};
}

int s3_request_plan_build(const s3_client_base *client, const s3_request_options *options,
                          s3_request_plan *out_plan) {
  s3_request_plan plan = {0};
  s3_signer_result signing = {0};
  s3_credentials credentials = {0};
  s3_signer_header *signer_headers = NULL;
  s3_signer_query *signer_query = NULL;
  const char *method_name = NULL;
  size_t signer_header_count = 0u;
  size_t allocation_size = 0u;
  size_t index;
  int fetched = 0;
  int status;
  if (client == NULL || out_plan == NULL || out_plan->target != NULL) return SALTS_EINVAL;
  status = s3_request_options_validate(client, options);
  if (status == SALTS_OK) status = s3_method_map(options->method, &plan.method, &method_name);
  if (status == SALTS_OK) status = s3_request_authority(client, options->bucket, &plan.authority);
  if (status == SALTS_OK) status = s3_request_canonical_uri(client, options, &plan.canonical_uri);
  if (status == SALTS_OK) status = s3_request_time(client, plan.amz_date);
  if (status == SALTS_OK) {
    char hash[S3_SIGNER_SHA256_HEX_SIZE + 1u];
    if (options->body_source != NULL) {
      plan.payload_sha256 = tstr_dup(options->payload_sha256);
      status = plan.payload_sha256 != NULL ? SALTS_OK : SALTS_ENOMEM;
    } else {
      status = s3_signer_sha256_hex(options->body, options->body_size, hash);
      if (status == SALTS_OK) {
        plan.payload_sha256 = tstr_dup(hash);
        if (plan.payload_sha256 == NULL) status = SALTS_ENOMEM;
      }
    }
  }
  if (status == SALTS_OK) {
    status = s3_checked_add(options->header_count, 1u, &signer_header_count);
    if (status == SALTS_OK)
      status = s3_checked_multiply(signer_header_count, sizeof(*signer_headers), &allocation_size);
    if (status == SALTS_OK) {
      signer_headers = (s3_signer_header *)calloc(1u, allocation_size);
      if (signer_headers == NULL) status = SALTS_ENOMEM;
    }
  }
  for (index = 0u; index < options->header_count && status == SALTS_OK; ++index)
    signer_headers[index] =
        (s3_signer_header){options->headers[index].name, options->headers[index].value};
  if (status == SALTS_OK)
    signer_headers[options->header_count] = (s3_signer_header){"Host", plan.authority};
  if (status == SALTS_OK && options->query_count != 0u) {
    status = s3_checked_multiply(options->query_count, sizeof(*signer_query), &allocation_size);
    if (status == SALTS_OK) {
      signer_query = (s3_signer_query *)calloc(1u, allocation_size);
      if (signer_query == NULL) status = SALTS_ENOMEM;
    }
    for (index = 0u; index < options->query_count && status == SALTS_OK; ++index)
      signer_query[index] =
          (s3_signer_query){options->query[index].name, options->query[index].value};
  }
  if (status == SALTS_OK) {
    status = client->config.credentials.fetch(client->config.credentials.user, &credentials);
    if (status == SALTS_OK) fetched = 1;
  }
  if (status == SALTS_OK) {
    const s3_signer_request signer_request = {.size = sizeof(signer_request),
                                              .method = method_name,
                                              .canonical_uri = plan.canonical_uri,
                                              .region = client->region,
                                              .access_key = credentials.access_key,
                                              .secret_key = credentials.secret_key,
                                              .session_token = credentials.session_token,
                                              .payload_sha256 = plan.payload_sha256,
                                              .amz_date = plan.amz_date,
                                              .headers = signer_headers,
                                              .header_count = signer_header_count,
                                              .query = signer_query,
                                              .query_count = options->query_count,
                                              .max_header_count = client->config.max_header_count,
                                              .max_header_bytes = client->config.max_header_bytes,
                                              .max_query_count = client->config.max_query_count,
                                              .max_target_bytes = client->config.max_target_bytes};
    status = s3_signer_sign(&signer_request, &signing);
  }
  if (status == SALTS_OK)
    status = s3_request_target(client, plan.canonical_uri, signing.canonical_query, &plan.target);
  if (status == SALTS_OK)
    status = s3_request_transport_headers(client, options, &credentials, &signing,
                                          plan.payload_sha256, plan.amz_date, &plan);
  if (fetched && client->config.credentials.release != NULL)
    client->config.credentials.release(client->config.credentials.user, &credentials);
  credentials = (s3_credentials){0};
  free(signer_headers);
  free(signer_query);
  s3_signer_result_destroy(&signing);
  if (status == SALTS_OK) {
    *out_plan = plan;
    return SALTS_OK;
  }
  s3_request_plan_destroy(&plan);
  return status;
}

int s3_presign_url_build(const s3_client_base *client, s3_method method, const char *bucket,
                         const char *key, const s3_query_param *query, size_t query_count,
                         uint32_t expires_seconds, char **out_url) {
  s3_request_options options = {.size = sizeof(options),
                                .method = method,
                                .bucket = bucket,
                                .key = key,
                                .query = query,
                                .query_count = query_count};
  s3_presign_request request = {0};
  s3_presign_result result = {0};
  s3_credentials credentials = {0};
  s3_signer_query *signer_query = NULL;
  s3_text_builder url = {0};
  tstr authority = NULL;
  tstr canonical_uri = NULL;
  char amz_date[S3_SIGNER_AMZ_DATE_SIZE + 1u];
  const char *method_name = NULL;
  const char *scheme;
  chttp_method http_method;
  size_t allocation_size;
  size_t target_size = 0u;
  size_t url_size = 0u;
  size_t index;
  int fetched = 0;
  int status;
  if (client == NULL || out_url == NULL || *out_url != NULL) return SALTS_EINVAL;
  status = s3_request_options_validate(client, &options);
  if (status == SALTS_OK) status = s3_method_map(method, &http_method, &method_name);
  (void)http_method;
  if (status == SALTS_OK) status = s3_request_authority(client, bucket, &authority);
  if (status == SALTS_OK) status = s3_request_canonical_uri(client, &options, &canonical_uri);
  if (status == SALTS_OK && query_count != 0u) {
    status = s3_checked_multiply(query_count, sizeof(*signer_query), &allocation_size);
    if (status == SALTS_OK) {
      signer_query = (s3_signer_query *)calloc(1u, allocation_size);
      if (signer_query == NULL) status = SALTS_ENOMEM;
    }
    for (index = 0u; index < query_count && status == SALTS_OK; ++index)
      signer_query[index] = (s3_signer_query){query[index].name, query[index].value};
  }
  if (status == SALTS_OK) {
    status = client->config.credentials.fetch(client->config.credentials.user, &credentials);
    if (status == SALTS_OK) fetched = 1;
  }
  request = (s3_presign_request){.size = sizeof(request),
                                 .method = method_name,
                                 .canonical_uri = canonical_uri,
                                 .authority = authority,
                                 .region = client->region,
                                 .access_key = credentials.access_key,
                                 .secret_key = credentials.secret_key,
                                 .session_token = credentials.session_token,
                                 .amz_date = amz_date,
                                 .expires_seconds = expires_seconds,
                                 .query = signer_query,
                                 .query_count = query_count,
                                 .max_query_count = client->config.max_query_count,
                                 .max_target_bytes = client->config.max_target_bytes};
  if (status == SALTS_OK) status = s3_request_time(client, amz_date);
  if (status == SALTS_OK) status = s3_signer_presign(&request, &result);
  if (status == SALTS_OK) {
    if (strncmp(client->connection_uri, "tls://", 6u) == 0) scheme = "https://";
    else if (strncmp(client->connection_uri, "tcp://", 6u) == 0) scheme = "http://";
    else {
      scheme = NULL;
      status = SALTS_ENOTSUP;
    }
  } else {
    scheme = NULL;
  }
  if (status == SALTS_OK) {
    target_size = strlen(canonical_uri);
    if (s3_checked_add(target_size, 1u, &target_size) != SALTS_OK ||
        s3_checked_add(target_size, strlen(result.canonical_query), &target_size) != SALTS_OK ||
        target_size > client->config.max_target_bytes)
      status = SALTS_EMSGSIZE;
  }
  if (status == SALTS_OK) {
    url_size = strlen(scheme);
    if (s3_checked_add(url_size, strlen(authority), &url_size) != SALTS_OK ||
        s3_checked_add(url_size, target_size, &url_size) != SALTS_OK)
      status = SALTS_ERANGE;
  }
  if (status == SALTS_OK) status = s3_text_builder_init(&url, url_size);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&url, scheme);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&url, authority);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&url, canonical_uri);
  if (status == SALTS_OK) status = s3_text_builder_append(&url, "?", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&url, result.canonical_query);
  if (status == SALTS_OK) *out_url = s3_text_builder_release(&url);
  if (fetched && client->config.credentials.release != NULL)
    client->config.credentials.release(client->config.credentials.user, &credentials);
  credentials = (s3_credentials){0};
  free(signer_query);
  tstr_free(authority);
  tstr_free(canonical_uri);
  s3_presign_result_destroy(&result);
  s3_text_builder_destroy(&url);
  return status;
}
