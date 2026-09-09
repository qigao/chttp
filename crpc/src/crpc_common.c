#include "crpc_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

bool crpc_client_config_valid(const crpc_client_config *config) {
  if (config == NULL || config->request_capacity == 0u ||
      config->request_capacity > config->http.request_capacity ||
      config->request_capacity > UINT32_MAX || config->max_method_bytes == 0u ||
      config->max_json_depth < 2u || config->http.max_request_body_bytes == 0u ||
      config->http.max_request_body_bytes == SIZE_MAX || config->http.max_header_count < 5u)
    return false;
  if (config->max_json_depth - 1u > SIZE_MAX / sizeof(size_t)) return false;
  return true;
}

static unsigned char crpc_ascii_lower(unsigned char value) {
  return value >= (unsigned char)'A' && value <= (unsigned char)'Z'
             ? (unsigned char)(value + ('a' - 'A'))
             : value;
}

static bool crpc_ascii_equal(const char *left, const char *right) {
  size_t index = 0u;
  if (left == NULL || right == NULL) return false;
  while (left[index] != '\0' && right[index] != '\0') {
    if (crpc_ascii_lower((unsigned char)left[index]) !=
        crpc_ascii_lower((unsigned char)right[index]))
      return false;
    ++index;
  }
  return left[index] == '\0' && right[index] == '\0';
}

static bool crpc_metadata_reserved(const crpc_metadata *metadata) {
  return metadata == NULL || metadata->name == NULL || metadata->value == NULL ||
         crpc_ascii_equal(metadata->name, "content-type") ||
         crpc_ascii_equal(metadata->name, "accept");
}

int crpc_bind_callable(const cmeta_callable *input, cmeta_callable *out, bool *out_present) {
  const cmeta_sig_desc *signature;
  if (out == NULL || out_present == NULL) return SALTS_EINVAL;
  *out = (cmeta_callable){0};
  *out_present = false;
  if (input == NULL) return SALTS_OK;
  if (!cmeta_callable_bind(*input, out)) return SALTS_EINVAL;
  signature = cmeta_callable_signature(*out);
  if (signature == NULL) return SALTS_EINVAL;
  if (signature->protocol != CMETA_FN_PROTOCOL_VALUE) return SALTS_ENOTSUP;
  *out_present = true;
  return SALTS_OK;
}

int crpc_prepare_call(const crpc_options *options, size_t max_method_bytes, size_t max_json_depth,
                      size_t max_body_bytes, size_t max_http_header_count,
                      crpc_prepared_call *out) {
  static const char content_type[] = "application/json";
  size_t header_count;
  size_t index;
  int status;

  if (out == NULL) return SALTS_EINVAL;
  *out = (crpc_prepared_call){0};
  if (options == NULL || options->connection_uri == NULL || options->authority == NULL ||
      options->target == NULL || (options->metadata_count != 0u && options->metadata == NULL))
    return SALTS_EINVAL;
  if (options->metadata_count > SIZE_MAX - 2u ||
      options->metadata_count + 5u > max_http_header_count)
    return SALTS_EMSGSIZE;
  for (index = 0u; index < options->metadata_count; ++index)
    if (crpc_metadata_reserved(&options->metadata[index])) return SALTS_EINVAL;

  status = crpc_bind_callable(options->method.callable, &out->callable, &out->has_callable);
  if (status == SALTS_OK)
    status = crpc_json_encode_request(&options->method, options->request_id, options->encode_params,
                                      options->params_user, max_method_bytes, max_json_depth,
                                      max_body_bytes, &out->encoded);
  if (status != SALTS_OK) goto fail;

  header_count = options->metadata_count + 2u;
  out->headers = (chttp_header *)calloc(header_count, sizeof(*out->headers));
  if (out->headers == NULL) {
    status = SALTS_ENOMEM;
    goto fail;
  }
  out->headers[0] = (chttp_header){"Content-Type", content_type};
  out->headers[1] = (chttp_header){"Accept", content_type};
  for (index = 0u; index < options->metadata_count; ++index)
    out->headers[index + 2u] =
        (chttp_header){options->metadata[index].name, options->metadata[index].value};
  out->header_count = header_count;
  return SALTS_OK;

fail:
  crpc_prepared_call_destroy(out);
  return status;
}

void crpc_prepared_call_destroy(crpc_prepared_call *call) {
  if (call == NULL) return;
  free(call->headers);
  crpc_encoded_request_destroy(&call->encoded);
  *call = (crpc_prepared_call){0};
}
