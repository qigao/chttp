#include <http_client/rpc.h>
#ifndef CRPC_CLIENT_INTERNAL_H
#define CRPC_CLIENT_INTERNAL_H

#include "crpc_internal.h"

typedef struct crpc_prepared_call {
  crpc_encoded_request encoded;
  chttp_header *headers;
  size_t header_count;
  cmeta_callable callable;
  bool has_callable;
} crpc_prepared_call;

bool crpc_client_config_valid(const crpc_client_config *config);

int crpc_prepare_call(const crpc_options *options, size_t max_method_bytes, size_t max_json_depth,
                      size_t max_body_bytes, size_t max_http_header_count, crpc_prepared_call *out);

void crpc_prepared_call_destroy(crpc_prepared_call *call);

#endif
