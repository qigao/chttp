#include <s3/s3_signer.h>

#include "s3_internal.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S3_SHA256_SIZE = 32 };
enum { S3_SIGNER_COMPONENT_LIMIT = 1024 };

typedef struct s3_normalized_header {
  tstr name;
  tstr value;
} s3_normalized_header;

static void s3_cleanse_tstr_free(tstr value) {
  if (value == NULL) return;
  OPENSSL_cleanse(value, tstr_len(value));
  tstr_free(value);
}

static void s3_hex_lower(const unsigned char *input, size_t size, char *output) {
  static const char hex[] = "0123456789abcdef";
  size_t index;
  for (index = 0u; index < size; ++index) {
    output[index * 2u] = hex[input[index] >> 4u];
    output[index * 2u + 1u] = hex[input[index] & 0x0fu];
  }
  output[size * 2u] = '\0';
}

int s3_signer_sha256_hex(const void *data, size_t size,
                         char out_hex[S3_SIGNER_SHA256_HEX_SIZE + 1]) {
  unsigned char digest[S3_SHA256_SIZE];
  unsigned int digest_size = 0u;

  if (out_hex == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;
  if (EVP_Digest(data != NULL ? data : "", size, digest, &digest_size, EVP_sha256(), NULL) != 1 ||
      digest_size != sizeof(digest)) {
    OPENSSL_cleanse(digest, sizeof(digest));
    return SALTS_EIO;
  }
  s3_hex_lower(digest, sizeof(digest), out_hex);
  OPENSSL_cleanse(digest, sizeof(digest));
  return SALTS_OK;
}

static int s3_hmac_sha256(const unsigned char *key, size_t key_size, const void *data,
                          size_t data_size, unsigned char output[S3_SHA256_SIZE]) {
  unsigned int output_size = 0u;
  const unsigned char *input = (const unsigned char *)(data != NULL ? data : "");
  if (key == NULL || (data == NULL && data_size != 0u) || key_size > INT_MAX) return SALTS_EINVAL;
  if (HMAC(EVP_sha256(), key, (int)key_size, input, data_size, output, &output_size) == NULL ||
      output_size != S3_SHA256_SIZE)
    return SALTS_EIO;
  return SALTS_OK;
}

static int s3_signer_date_valid(const char *date) {
  size_t index;
  if (date == NULL || strlen(date) != S3_SIGNER_AMZ_DATE_SIZE || date[8] != 'T' || date[15] != 'Z')
    return 0;
  for (index = 0u; index < S3_SIGNER_AMZ_DATE_SIZE; ++index) {
    if (index != 8u && index != 15u && !isdigit((unsigned char)date[index])) return 0;
  }
  return 1;
}

static int s3_signer_method_valid(const char *method) {
  size_t index;
  if (method == NULL || method[0] == '\0' || strlen(method) > 16u) return 0;
  for (index = 0u; method[index] != '\0'; ++index) {
    if (method[index] < 'A' || method[index] > 'Z') return 0;
  }
  return 1;
}

static int s3_signer_hash_valid(const char *hash) {
  size_t index;
  if (hash == NULL || strlen(hash) != S3_SIGNER_SHA256_HEX_SIZE) return 0;
  for (index = 0u; index < S3_SIGNER_SHA256_HEX_SIZE; ++index) {
    if (!((hash[index] >= '0' && hash[index] <= '9') || (hash[index] >= 'a' && hash[index] <= 'f')))
      return 0;
  }
  return 1;
}

static int s3_header_name_byte(unsigned char value) {
  static const char separators[] = "()<>@,;:\\\"/[]?={} \t";
  return value > 32u && value < 127u && strchr(separators, (int)value) == NULL;
}

static int s3_header_name_normalize(const char *name, tstr *out) {
  size_t size;
  size_t index;
  if (name == NULL || out == NULL || *out != NULL || name[0] == '\0') return SALTS_EINVAL;
  size = strlen(name);
  *out = tstr_dup_len(name, size);
  if (*out == NULL) return SALTS_ENOMEM;
  for (index = 0u; index < size; ++index) {
    const unsigned char value = (unsigned char)(*out)[index];
    if (!s3_header_name_byte(value)) {
      tstr_freep(out);
      return SALTS_EINVAL;
    }
    if (value >= 'A' && value <= 'Z') (*out)[index] = (char)(value + ('a' - 'A'));
  }
  return SALTS_OK;
}

static int s3_header_value_normalize(const char *value, tstr *out) {
  s3_text_builder builder = {0};
  size_t begin = 0u;
  size_t end;
  size_t index;
  int in_whitespace = 0;
  int status;

  if (value == NULL || out == NULL || *out != NULL) return SALTS_EINVAL;
  end = strlen(value);
  while (begin < end && (value[begin] == ' ' || value[begin] == '\t'))
    ++begin;
  while (end > begin && (value[end - 1u] == ' ' || value[end - 1u] == '\t'))
    --end;
  status = s3_text_builder_init(&builder, end - begin);
  for (index = begin; index < end && status == SALTS_OK; ++index) {
    const unsigned char byte = (unsigned char)value[index];
    if (byte == '\r' || byte == '\n' || byte == 0x7fu || (byte < 0x20u && byte != '\t')) {
      status = SALTS_EINVAL;
      break;
    }
    if (byte == ' ' || byte == '\t') {
      in_whitespace = 1;
    } else {
      if (in_whitespace && tstr_len(builder.text) != 0u)
        status = s3_text_builder_append(&builder, " ", 1u);
      if (status == SALTS_OK) status = s3_text_builder_append(&builder, &value[index], 1u);
      in_whitespace = 0;
    }
  }
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_header_compare(const void *left, const void *right) {
  return strcmp(((const s3_normalized_header *)left)->name,
                ((const s3_normalized_header *)right)->name);
}

static int s3_header_forbidden(const char *name) {
  return strcmp(name, "authorization") == 0 || strcmp(name, "x-amz-date") == 0 ||
         strcmp(name, "x-amz-content-sha256") == 0 || strcmp(name, "x-amz-security-token") == 0;
}

static void s3_headers_destroy(s3_normalized_header *headers, size_t count) {
  size_t index;
  for (index = 0u; index < count; ++index) {
    if (headers[index].value != NULL)
      OPENSSL_cleanse(headers[index].value, tstr_len(headers[index].value));
    tstr_free(headers[index].name);
    tstr_free(headers[index].value);
  }
  free(headers);
}

static int s3_headers_normalize(const s3_signer_request *request, size_t max_count,
                                size_t max_bytes, s3_normalized_header **out_headers,
                                size_t *out_count) {
  s3_normalized_header *headers = NULL;
  size_t count;
  size_t allocation_size;
  size_t aggregate = 0u;
  size_t index;
  int status;

  if (s3_checked_add(request->header_count, 2u, &count) != SALTS_OK) return SALTS_ERANGE;
  if (request->session_token != NULL && request->session_token[0] != '\0' &&
      s3_checked_add(count, 1u, &count) != SALTS_OK)
    return SALTS_ERANGE;
  if (count > max_count) return SALTS_ENOBUFS;
  if (request->header_count != 0u && request->headers == NULL) return SALTS_EINVAL;
  if (s3_checked_multiply(count, sizeof(*headers), &allocation_size) != SALTS_OK)
    return SALTS_ERANGE;
  headers = (s3_normalized_header *)calloc(1u, allocation_size);
  if (headers == NULL) return SALTS_ENOMEM;
  status = SALTS_OK;
  for (index = 0u; index < request->header_count && status == SALTS_OK; ++index) {
    status = s3_header_name_normalize(request->headers[index].name, &headers[index].name);
    if (status == SALTS_OK && s3_header_forbidden(headers[index].name)) status = SALTS_EINVAL;
    if (status == SALTS_OK)
      status = s3_header_value_normalize(request->headers[index].value, &headers[index].value);
  }
  index = request->header_count;
  if (status == SALTS_OK) {
    headers[index].name = tstr_dup("x-amz-content-sha256");
    headers[index++].value = tstr_dup(request->payload_sha256);
    headers[index].name = tstr_dup("x-amz-date");
    headers[index++].value = tstr_dup(request->amz_date);
    if (index < count) {
      headers[index].name = tstr_dup("x-amz-security-token");
      headers[index++].value = tstr_dup(request->session_token);
    }
    while (index-- > request->header_count) {
      if (headers[index].name == NULL || headers[index].value == NULL) status = SALTS_ENOMEM;
    }
  }
  if (status == SALTS_OK) {
    qsort(headers, count, sizeof(*headers), s3_header_compare);
    for (index = 0u; index < count; ++index) {
      if (index != 0u && strcmp(headers[index - 1u].name, headers[index].name) == 0) {
        status = SALTS_EINVAL;
        break;
      }
      if (s3_checked_add(aggregate, tstr_len(headers[index].name), &aggregate) != SALTS_OK ||
          s3_checked_add(aggregate, tstr_len(headers[index].value), &aggregate) != SALTS_OK ||
          s3_checked_add(aggregate, 2u, &aggregate) != SALTS_OK || aggregate > max_bytes) {
        status = SALTS_EMSGSIZE;
        break;
      }
    }
  }
  if (status == SALTS_OK) {
    *out_headers = headers;
    *out_count = count;
    return SALTS_OK;
  }
  s3_headers_destroy(headers, count);
  return status;
}

static int s3_build_canonical_headers(const s3_normalized_header *headers, size_t count,
                                      size_t max_header_bytes, tstr *out_signed,
                                      tstr *out_canonical) {
  s3_text_builder signed_builder = {0};
  s3_text_builder canonical_builder = {0};
  size_t signed_size = 0u;
  size_t canonical_size = 0u;
  size_t index;
  int status = SALTS_OK;

  for (index = 0u; index < count; ++index) {
    if (s3_checked_add(signed_size, tstr_len(headers[index].name), &signed_size) != SALTS_OK ||
        (index != 0u && s3_checked_add(signed_size, 1u, &signed_size) != SALTS_OK) ||
        s3_checked_add(canonical_size, tstr_len(headers[index].name), &canonical_size) !=
            SALTS_OK ||
        s3_checked_add(canonical_size, tstr_len(headers[index].value), &canonical_size) !=
            SALTS_OK ||
        s3_checked_add(canonical_size, 2u, &canonical_size) != SALTS_OK ||
        canonical_size > max_header_bytes)
      return SALTS_EMSGSIZE;
  }
  status = s3_text_builder_init(&signed_builder, signed_size);
  if (status == SALTS_OK) status = s3_text_builder_init(&canonical_builder, canonical_size);
  for (index = 0u; index < count && status == SALTS_OK; ++index) {
    if (index != 0u) status = s3_text_builder_append(&signed_builder, ";", 1u);
    if (status == SALTS_OK)
      status = s3_text_builder_append_cstr(&signed_builder, headers[index].name);
    if (status == SALTS_OK)
      status = s3_text_builder_append_cstr(&canonical_builder, headers[index].name);
    if (status == SALTS_OK) status = s3_text_builder_append(&canonical_builder, ":", 1u);
    if (status == SALTS_OK)
      status = s3_text_builder_append_cstr(&canonical_builder, headers[index].value);
    if (status == SALTS_OK) status = s3_text_builder_append(&canonical_builder, "\n", 1u);
  }
  if (status == SALTS_OK) {
    *out_signed = s3_text_builder_release(&signed_builder);
    *out_canonical = s3_text_builder_release(&canonical_builder);
  }
  s3_text_builder_destroy(&signed_builder);
  s3_text_builder_destroy(&canonical_builder);
  return status;
}

static int s3_build_canonical_request(const s3_signer_request *request, const char *canonical_query,
                                      const char *canonical_headers, const char *signed_headers,
                                      size_t max_size, tstr *out) {
  s3_text_builder builder = {0};
  size_t required = strlen(request->method);
  int status;

  if (s3_checked_add(required, strlen(request->canonical_uri), &required) != SALTS_OK ||
      s3_checked_add(required, strlen(canonical_query), &required) != SALTS_OK ||
      s3_checked_add(required, strlen(canonical_headers), &required) != SALTS_OK ||
      s3_checked_add(required, strlen(signed_headers), &required) != SALTS_OK ||
      s3_checked_add(required, strlen(request->payload_sha256), &required) != SALTS_OK ||
      s3_checked_add(required, 5u, &required) != SALTS_OK)
    return SALTS_ERANGE;
  if (required > max_size) return SALTS_EMSGSIZE;
  status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, request->method);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "\n", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, request->canonical_uri);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "\n", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, canonical_query);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "\n", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, canonical_headers);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "\n", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, signed_headers);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "\n", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, request->payload_sha256);
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_build_scope(const s3_signer_request *request, tstr *out) {
  s3_text_builder builder = {0};
  const size_t required = 8u + 1u + strlen(request->region) + sizeof("/s3/aws4_request") - 1u;
  int status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, request->amz_date, 8u);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "/", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, request->region);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, "/s3/aws4_request");
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_build_string_to_sign(const s3_signer_request *request, const char *scope,
                                   const char canonical_hash[S3_SIGNER_SHA256_HEX_SIZE + 1],
                                   tstr *out) {
  s3_text_builder builder = {0};
  const size_t required = sizeof("AWS4-HMAC-SHA256\n") - 1u + S3_SIGNER_AMZ_DATE_SIZE + 1u +
                          strlen(scope) + 1u + S3_SIGNER_SHA256_HEX_SIZE;
  int status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, "AWS4-HMAC-SHA256\n");
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, request->amz_date);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "\n", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, scope);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "\n", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, canonical_hash);
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_derive_signature(const s3_signer_request *request, const char *string_to_sign,
                               char signature[S3_SIGNER_SHA256_HEX_SIZE + 1]) {
  unsigned char secret_prefix[S3_SIGNER_COMPONENT_LIMIT + sizeof("AWS4")];
  unsigned char date_key[S3_SHA256_SIZE];
  unsigned char region_key[S3_SHA256_SIZE];
  unsigned char service_key[S3_SHA256_SIZE];
  unsigned char signing_key[S3_SHA256_SIZE];
  unsigned char raw_signature[S3_SHA256_SIZE];
  const size_t secret_size = strlen(request->secret_key);
  int status;

  memcpy(secret_prefix, "AWS4", 4u);
  memcpy(secret_prefix + 4u, request->secret_key, secret_size);
  status = s3_hmac_sha256(secret_prefix, secret_size + 4u, request->amz_date, 8u, date_key);
  if (status == SALTS_OK)
    status = s3_hmac_sha256(date_key, sizeof(date_key), request->region, strlen(request->region),
                            region_key);
  if (status == SALTS_OK)
    status = s3_hmac_sha256(region_key, sizeof(region_key), "s3", 2u, service_key);
  if (status == SALTS_OK)
    status = s3_hmac_sha256(service_key, sizeof(service_key), "aws4_request", 12u, signing_key);
  if (status == SALTS_OK)
    status = s3_hmac_sha256(signing_key, sizeof(signing_key), string_to_sign,
                            strlen(string_to_sign), raw_signature);
  if (status == SALTS_OK) s3_hex_lower(raw_signature, sizeof(raw_signature), signature);
  OPENSSL_cleanse(secret_prefix, sizeof(secret_prefix));
  OPENSSL_cleanse(date_key, sizeof(date_key));
  OPENSSL_cleanse(region_key, sizeof(region_key));
  OPENSSL_cleanse(service_key, sizeof(service_key));
  OPENSSL_cleanse(signing_key, sizeof(signing_key));
  OPENSSL_cleanse(raw_signature, sizeof(raw_signature));
  return status;
}

static int s3_build_authorization(const s3_signer_request *request, const char *scope,
                                  const char *signed_headers, const char *signature, tstr *out) {
  static const char algorithm[] = "AWS4-HMAC-SHA256 Credential=";
  static const char signed_prefix[] = ",SignedHeaders=";
  static const char signature_prefix[] = ",Signature=";
  s3_text_builder builder = {0};
  const size_t required = sizeof(algorithm) - 1u + strlen(request->access_key) + 1u +
                          strlen(scope) + sizeof(signed_prefix) - 1u + strlen(signed_headers) +
                          sizeof(signature_prefix) - 1u + strlen(signature);
  int status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, algorithm);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, request->access_key);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "/", 1u);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, scope);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, signed_prefix);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, signed_headers);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, signature_prefix);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, signature);
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  s3_text_builder_destroy(&builder);
  return status;
}

static int s3_signer_request_validate(const s3_signer_request *request, size_t max_target_bytes) {
  size_t index;
  if (request == NULL || request->size != sizeof(*request) ||
      !s3_signer_method_valid(request->method) || request->canonical_uri == NULL ||
      request->canonical_uri[0] != '/' || strchr(request->canonical_uri, '?') != NULL ||
      strchr(request->canonical_uri, '#') != NULL ||
      strlen(request->canonical_uri) > max_target_bytes || request->region == NULL ||
      request->access_key == NULL || request->secret_key == NULL || request->region[0] == '\0' ||
      request->access_key[0] == '\0' || request->secret_key[0] == '\0' ||
      strlen(request->region) > S3_SIGNER_COMPONENT_LIMIT ||
      strlen(request->access_key) > S3_SIGNER_COMPONENT_LIMIT ||
      strlen(request->secret_key) > S3_SIGNER_COMPONENT_LIMIT ||
      (request->session_token != NULL &&
       strlen(request->session_token) > S3_SIGNER_DEFAULT_MAX_HEADER_BYTES) ||
      !s3_signer_hash_valid(request->payload_sha256) || !s3_signer_date_valid(request->amz_date))
    return SALTS_EINVAL;
  for (index = 0u; request->canonical_uri[index] != '\0'; ++index) {
    const unsigned char value = (unsigned char)request->canonical_uri[index];
    if (value <= 0x20u || value == 0x7fu) return SALTS_EINVAL;
  }
  return SALTS_OK;
}

int s3_signer_sign(const s3_signer_request *request, s3_signer_result *out_result) {
  s3_signer_result result = {0};
  s3_normalized_header *headers = NULL;
  size_t header_count = 0u;
  tstr canonical_headers = NULL;
  tstr scope = NULL;
  char canonical_hash[S3_SIGNER_SHA256_HEX_SIZE + 1] = {0};
  char signature[S3_SIGNER_SHA256_HEX_SIZE + 1] = {0};
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_query_count;
  size_t max_target_bytes;
  size_t canonical_limit;
  int status;

  if (out_result == NULL || out_result->authorization != NULL ||
      out_result->signed_headers != NULL || out_result->signature != NULL ||
      out_result->canonical_query != NULL || out_result->canonical_request != NULL ||
      out_result->string_to_sign != NULL)
    return SALTS_EINVAL;
  max_header_count = request != NULL && request->max_header_count != 0u
                         ? request->max_header_count
                         : S3_SIGNER_DEFAULT_MAX_HEADER_COUNT;
  max_header_bytes = request != NULL && request->max_header_bytes != 0u
                         ? request->max_header_bytes
                         : S3_SIGNER_DEFAULT_MAX_HEADER_BYTES;
  max_query_count = request != NULL && request->max_query_count != 0u
                        ? request->max_query_count
                        : S3_SIGNER_DEFAULT_MAX_QUERY_COUNT;
  max_target_bytes = request != NULL && request->max_target_bytes != 0u
                         ? request->max_target_bytes
                         : S3_SIGNER_DEFAULT_MAX_TARGET_BYTES;
  status = s3_signer_request_validate(request, max_target_bytes);
  if (status != SALTS_OK) return status;
  status =
      s3_headers_normalize(request, max_header_count, max_header_bytes, &headers, &header_count);
  if (status == SALTS_OK)
    status = s3_build_canonical_headers(headers, header_count, max_header_bytes,
                                        (tstr *)&result.signed_headers, &canonical_headers);
  if (status == SALTS_OK)
    status = s3_query_canonicalize(request->query, request->query_count, max_query_count,
                                   max_target_bytes, (tstr *)&result.canonical_query);
  if (s3_checked_add(max_header_bytes, max_target_bytes, &canonical_limit) != SALTS_OK ||
      s3_checked_add(canonical_limit, max_target_bytes, &canonical_limit) != SALTS_OK)
    status = SALTS_ERANGE;
  if (status == SALTS_OK)
    status = s3_build_canonical_request(request, result.canonical_query, canonical_headers,
                                        result.signed_headers, canonical_limit,
                                        (tstr *)&result.canonical_request);
  if (status == SALTS_OK)
    status = s3_signer_sha256_hex(result.canonical_request, strlen(result.canonical_request),
                                  canonical_hash);
  if (status == SALTS_OK) status = s3_build_scope(request, &scope);
  if (status == SALTS_OK)
    status =
        s3_build_string_to_sign(request, scope, canonical_hash, (tstr *)&result.string_to_sign);
  if (status == SALTS_OK) status = s3_derive_signature(request, result.string_to_sign, signature);
  if (status == SALTS_OK) {
    result.signature = tstr_dup(signature);
    if (result.signature == NULL) status = SALTS_ENOMEM;
  }
  if (status == SALTS_OK)
    status = s3_build_authorization(request, scope, result.signed_headers, result.signature,
                                    (tstr *)&result.authorization);
  if (status == SALTS_OK) {
    *out_result = result;
    memset(&result, 0, sizeof(result));
  }
  s3_signer_result_destroy(&result);
  s3_headers_destroy(headers, header_count);
  s3_cleanse_tstr_free(canonical_headers);
  tstr_free(scope);
  OPENSSL_cleanse(canonical_hash, sizeof(canonical_hash));
  OPENSSL_cleanse(signature, sizeof(signature));
  return status;
}

void s3_signer_result_destroy(s3_signer_result *result) {
  if (result == NULL) return;
  if (result->authorization != NULL)
    OPENSSL_cleanse(result->authorization, tstr_len((tstr)result->authorization));
  if (result->signature != NULL)
    OPENSSL_cleanse(result->signature, tstr_len((tstr)result->signature));
  if (result->canonical_query != NULL)
    OPENSSL_cleanse(result->canonical_query, tstr_len((tstr)result->canonical_query));
  if (result->canonical_request != NULL)
    OPENSSL_cleanse(result->canonical_request, tstr_len((tstr)result->canonical_request));
  if (result->string_to_sign != NULL)
    OPENSSL_cleanse(result->string_to_sign, tstr_len((tstr)result->string_to_sign));
  tstr_free(result->authorization);
  tstr_free(result->signed_headers);
  tstr_free(result->signature);
  tstr_free(result->canonical_query);
  tstr_free(result->canonical_request);
  tstr_free(result->string_to_sign);
  memset(result, 0, sizeof(*result));
}

void s3_presign_result_destroy(s3_presign_result *result) {
  if (result == NULL) return;
  if (result->canonical_query != NULL)
    OPENSSL_cleanse(result->canonical_query, tstr_len((tstr)result->canonical_query));
  if (result->signature != NULL)
    OPENSSL_cleanse(result->signature, tstr_len((tstr)result->signature));
  tstr_free(result->canonical_query);
  tstr_free(result->signature);
  memset(result, 0, sizeof(*result));
}

static int s3_ascii_case_equal(const char *left, const char *right) {
  size_t index;
  if (left == NULL || right == NULL) return 0;
  for (index = 0u; left[index] != '\0' && right[index] != '\0'; ++index) {
    unsigned char lhs = (unsigned char)left[index];
    unsigned char rhs = (unsigned char)right[index];
    if (lhs >= 'A' && lhs <= 'Z') lhs = (unsigned char)(lhs + ('a' - 'A'));
    if (rhs >= 'A' && rhs <= 'Z') rhs = (unsigned char)(rhs + ('a' - 'A'));
    if (lhs != rhs) return 0;
  }
  return left[index] == right[index];
}

static int s3_presign_query_reserved(const char *name) {
  static const char *const reserved[] = {
      "X-Amz-Algorithm",     "X-Amz-Credential",     "X-Amz-Date",     "X-Amz-Expires",
      "X-Amz-SignedHeaders", "X-Amz-Security-Token", "X-Amz-Signature"};
  size_t index;
  for (index = 0u; index < sizeof(reserved) / sizeof(reserved[0]); ++index) {
    if (s3_ascii_case_equal(name, reserved[index])) return 1;
  }
  return 0;
}

static int s3_presign_validate(const s3_presign_request *request, size_t max_target_bytes) {
  size_t index;
  if (request == NULL || request->size != sizeof(*request) ||
      !s3_signer_method_valid(request->method) || request->canonical_uri == NULL ||
      request->canonical_uri[0] != '/' || strchr(request->canonical_uri, '?') != NULL ||
      strchr(request->canonical_uri, '#') != NULL ||
      strlen(request->canonical_uri) > max_target_bytes || request->authority == NULL ||
      request->authority[0] == '\0' || request->region == NULL || request->region[0] == '\0' ||
      request->access_key == NULL || request->access_key[0] == '\0' ||
      request->secret_key == NULL || request->secret_key[0] == '\0' ||
      strlen(request->authority) > S3_SIGNER_DEFAULT_MAX_HEADER_BYTES ||
      strlen(request->region) > S3_SIGNER_COMPONENT_LIMIT ||
      strlen(request->access_key) > S3_SIGNER_COMPONENT_LIMIT ||
      strlen(request->secret_key) > S3_SIGNER_COMPONENT_LIMIT ||
      (request->session_token != NULL &&
       strlen(request->session_token) > S3_SIGNER_DEFAULT_MAX_HEADER_BYTES) ||
      !s3_signer_date_valid(request->amz_date) || request->expires_seconds == 0u ||
      request->expires_seconds > 604800u || (request->query_count != 0u && request->query == NULL))
    return SALTS_EINVAL;
  for (index = 0u; request->canonical_uri[index] != '\0'; ++index) {
    const unsigned char value = (unsigned char)request->canonical_uri[index];
    if (value <= 0x20u || value == 0x7fu) return SALTS_EINVAL;
  }
  for (index = 0u; index < request->query_count; ++index) {
    if (request->query[index].name == NULL || request->query[index].value == NULL ||
        s3_presign_query_reserved(request->query[index].name))
      return SALTS_EINVAL;
  }
  return SALTS_OK;
}

static int s3_presign_canonical_headers(const char *authority, tstr *out) {
  s3_text_builder builder = {0};
  tstr normalized = NULL;
  size_t required;
  int status = s3_header_value_normalize(authority, &normalized);
  if (status != SALTS_OK) return status;
  required = sizeof("host:\n") - 1u + tstr_len(normalized);
  if (required > S3_SIGNER_DEFAULT_MAX_HEADER_BYTES) status = SALTS_EMSGSIZE;
  if (status == SALTS_OK) status = s3_text_builder_init(&builder, required);
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, "host:");
  if (status == SALTS_OK) status = s3_text_builder_append_cstr(&builder, normalized);
  if (status == SALTS_OK) status = s3_text_builder_append(&builder, "\n", 1u);
  if (status == SALTS_OK) *out = s3_text_builder_release(&builder);
  tstr_free(normalized);
  s3_text_builder_destroy(&builder);
  return status;
}

int s3_signer_presign(const s3_presign_request *request, s3_presign_result *out_result) {
  static const char unsigned_payload[] = "UNSIGNED-PAYLOAD";
  s3_presign_result result = {0};
  s3_signer_request signer_request = {0};
  s3_signer_query *query = NULL;
  tstr scope = NULL;
  tstr credential = NULL;
  tstr canonical_query = NULL;
  tstr canonical_headers = NULL;
  tstr canonical_request = NULL;
  tstr string_to_sign = NULL;
  s3_text_builder credential_builder = {0};
  char expires[16];
  char canonical_hash[S3_SIGNER_SHA256_HEX_SIZE + 1u] = {0};
  char signature[S3_SIGNER_SHA256_HEX_SIZE + 1u] = {0};
  size_t max_query_count;
  size_t max_target_bytes;
  size_t generated_count = 6u;
  size_t signing_count;
  size_t final_count;
  size_t allocation_size;
  size_t canonical_limit;
  size_t index;
  int written;
  int status;
  if (out_result == NULL || out_result->canonical_query != NULL || out_result->signature != NULL)
    return SALTS_EINVAL;
  max_query_count = request != NULL && request->max_query_count != 0u
                        ? request->max_query_count
                        : S3_SIGNER_DEFAULT_MAX_QUERY_COUNT;
  max_target_bytes = request != NULL && request->max_target_bytes != 0u
                         ? request->max_target_bytes
                         : S3_SIGNER_DEFAULT_MAX_TARGET_BYTES;
  status = s3_presign_validate(request, max_target_bytes);
  if (status != SALTS_OK) return status;
  if (request->session_token != NULL && request->session_token[0] != '\0') ++generated_count;
  if (s3_checked_add(request->query_count, generated_count, &final_count) != SALTS_OK)
    return SALTS_ERANGE;
  if (final_count > max_query_count ||
      s3_checked_multiply(final_count, sizeof(*query), &allocation_size) != SALTS_OK)
    return SALTS_ENOBUFS;
  query = (s3_signer_query *)calloc(1u, allocation_size);
  if (query == NULL) return SALTS_ENOMEM;
  for (index = 0u; index < request->query_count; ++index)
    query[index] = request->query[index];
  signer_request = (s3_signer_request){.size = sizeof(signer_request),
                                       .method = request->method,
                                       .canonical_uri = request->canonical_uri,
                                       .region = request->region,
                                       .access_key = request->access_key,
                                       .secret_key = request->secret_key,
                                       .session_token = request->session_token,
                                       .payload_sha256 = unsigned_payload,
                                       .amz_date = request->amz_date};
  status = s3_build_scope(&signer_request, &scope);
  if (status == SALTS_OK) {
    const size_t required = strlen(request->access_key) + 1u + tstr_len(scope);
    status = s3_text_builder_init(&credential_builder, required);
    if (status == SALTS_OK)
      status = s3_text_builder_append_cstr(&credential_builder, request->access_key);
    if (status == SALTS_OK) status = s3_text_builder_append(&credential_builder, "/", 1u);
    if (status == SALTS_OK) status = s3_text_builder_append_cstr(&credential_builder, scope);
    if (status == SALTS_OK) credential = s3_text_builder_release(&credential_builder);
  }
  written = snprintf(expires, sizeof(expires), "%u", request->expires_seconds);
  if (status == SALTS_OK && (written <= 0 || (size_t)written >= sizeof(expires)))
    status = SALTS_ERANGE;
  signing_count = final_count - 1u;
  index = request->query_count;
  if (status == SALTS_OK) query[index++] = (s3_signer_query){"X-Amz-Algorithm", "AWS4-HMAC-SHA256"};
  if (status == SALTS_OK) query[index++] = (s3_signer_query){"X-Amz-Credential", credential};
  if (status == SALTS_OK) query[index++] = (s3_signer_query){"X-Amz-Date", request->amz_date};
  if (status == SALTS_OK) query[index++] = (s3_signer_query){"X-Amz-Expires", expires};
  if (status == SALTS_OK) query[index++] = (s3_signer_query){"X-Amz-SignedHeaders", "host"};
  if (status == SALTS_OK && index < signing_count)
    query[index++] = (s3_signer_query){"X-Amz-Security-Token", request->session_token};
  if (status == SALTS_OK)
    status = s3_query_canonicalize(query, signing_count, max_query_count, max_target_bytes,
                                   &canonical_query);
  if (status == SALTS_OK)
    status = s3_presign_canonical_headers(request->authority, &canonical_headers);
  if (s3_checked_add(S3_SIGNER_DEFAULT_MAX_HEADER_BYTES, max_target_bytes, &canonical_limit) !=
          SALTS_OK ||
      s3_checked_add(canonical_limit, max_target_bytes, &canonical_limit) != SALTS_OK)
    status = SALTS_ERANGE;
  if (status == SALTS_OK)
    status = s3_build_canonical_request(&signer_request, canonical_query, canonical_headers, "host",
                                        canonical_limit, &canonical_request);
  if (status == SALTS_OK)
    status = s3_signer_sha256_hex(canonical_request, tstr_len(canonical_request), canonical_hash);
  if (status == SALTS_OK)
    status = s3_build_string_to_sign(&signer_request, scope, canonical_hash, &string_to_sign);
  if (status == SALTS_OK) status = s3_derive_signature(&signer_request, string_to_sign, signature);
  if (status == SALTS_OK) {
    result.signature = tstr_dup(signature);
    if (result.signature == NULL) status = SALTS_ENOMEM;
  }
  if (status == SALTS_OK) {
    query[signing_count] = (s3_signer_query){"X-Amz-Signature", result.signature};
    status = s3_query_canonicalize(query, final_count, max_query_count, max_target_bytes,
                                   (tstr *)&result.canonical_query);
  }
  if (status == SALTS_OK) {
    *out_result = result;
    result = (s3_presign_result){0};
  }
  s3_presign_result_destroy(&result);
  s3_text_builder_destroy(&credential_builder);
  tstr_free(scope);
  s3_cleanse_tstr_free(credential);
  s3_cleanse_tstr_free(canonical_query);
  s3_cleanse_tstr_free(canonical_headers);
  s3_cleanse_tstr_free(canonical_request);
  s3_cleanse_tstr_free(string_to_sign);
  free(query);
  OPENSSL_cleanse(canonical_hash, sizeof(canonical_hash));
  OPENSSL_cleanse(signature, sizeof(signature));
  return status;
}
