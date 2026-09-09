#ifndef CRPC_INTERNAL_H
#define CRPC_INTERNAL_H

#include <crpc/crpc.h>

#include <stdbool.h>

typedef struct json_value_s json_value_t;

typedef struct crpc_encoded_request {
  unsigned char *data;
  size_t size;
} crpc_encoded_request;

typedef struct crpc_decoded_response {
  void *json_root;
  cserde_reader *reader;
  crpc_response_view response;
  cmeta_callable callable;
  bool has_callable;
} crpc_decoded_response;

typedef struct crpc_prepared_call {
  crpc_encoded_request encoded;
  chttp_header *headers;
  size_t header_count;
  cmeta_callable callable;
  bool has_callable;
} crpc_prepared_call;

bool crpc_client_config_valid(const crpc_client_config *config);

int crpc_bind_callable(const cmeta_callable *input, cmeta_callable *out, bool *out_present);

int crpc_method_format(const crpc_method *method, size_t max_method_bytes, char *out,
                       size_t out_capacity);

int crpc_prepare_call(const crpc_options *options, size_t max_method_bytes, size_t max_json_depth,
                      size_t max_body_bytes, size_t max_http_header_count, crpc_prepared_call *out);

void crpc_prepared_call_destroy(crpc_prepared_call *call);

int crpc_json_encode_request(const crpc_method *method, uint64_t request_id,
                             crpc_encode_params_fn encode_params, void *params_user,
                             size_t max_method_bytes, size_t max_json_depth, size_t max_body_bytes,
                             crpc_encoded_request *out);

int crpc_json_encode_result(uint64_t request_id, crpc_encode_value_fn encode, void *user,
                            size_t max_json_depth, size_t max_body_bytes,
                            crpc_encoded_request *out);

int crpc_json_encode_error(bool null_id, uint64_t request_id, int64_t code, const char *message,
                           crpc_encode_value_fn encode_data, void *data_user, size_t max_json_depth,
                           size_t max_body_bytes, crpc_encoded_request *out);

int crpc_json_encode_batch(const crpc_encoded_request *items, size_t item_count,
                           size_t max_body_bytes, crpc_encoded_request *out);

bool crpc_json_depth_valid(const unsigned char *data, size_t size, size_t max_depth);
json_value_t *crpc_json_unique_member(const json_value_t *object, const char *name,
                                      size_t *out_count);
bool crpc_json_uint64(const json_value_t *value, uint64_t *out);

void crpc_encoded_request_destroy(crpc_encoded_request *request);

int crpc_json_decode_response(const void *data, size_t size, uint64_t expected_id,
                              unsigned int http_status, size_t max_json_depth,
                              const cmeta_callable *callable, crpc_decoded_response *out,
                              const char **out_stage);

void crpc_decoded_response_destroy(crpc_decoded_response *response);

#endif /* CRPC_INTERNAL_H */
