#include "crpc_client_internal.h"
#include "../src/chttp_client_internal.h"

#include <salts/clock.h>
#include <stdint.h>
#include <stdlib.h>

/* Bounded by admitted HTTP request slots: no separate transport ownership. */
typedef struct chttp_rpc_direct_call {
  chttp_async_rpc_complete_fn on_complete;
  void *user;
  uint64_t wire_id;
  size_t max_json_depth;
  cmeta_callable callable;
  int has_callable;
} chttp_rpc_direct_call;

static void chttp_rpc_direct_complete(
    void *user, chttp_request request,
    const chttp_response_view *http_response,
    const chttp_error *http_error) {
  chttp_rpc_direct_call *call = (chttp_rpc_direct_call *)user;
  crpc_decoded_response decoded = {0};
  crpc_error error = {0};
  const crpc_response_view *response = NULL;
  const crpc_error *failure = NULL;
  const char *stage = NULL;
  int status;
  if (call == NULL) return;
  if (http_error != NULL) {
    error = (crpc_error){.status = http_error->status,
                         .native_status = http_error->native_status,
                         .stage = http_error->stage};
    failure = &error;
  } else if (http_response == NULL) {
    error = (crpc_error){.status = SALTS_EPROTO, .stage = "http-response"};
    failure = &error;
  } else {
    status = crpc_json_decode_response(
        http_response->body, http_response->body_size,
        call->wire_id, http_response->status_code, call->max_json_depth,
        call->has_callable ? &call->callable : NULL, &decoded, &stage);
    if (status == SALTS_OK) {
      response = &decoded.response;
    } else {
      error = (crpc_error){.status = status,
                           .http_status = http_response->status_code,
                           .stage = stage};
      failure = &error;
    }
  }
  call->on_complete(call->user, request, response, failure);
  crpc_decoded_response_destroy(&decoded);
  free(call);
}

int chttp_async_client_rpc_submit(
    chttp_async_client *client, const chttp_client_rpc_limits *limits,
    const crpc_options *options, chttp_async_rpc_complete_fn on_complete,
    void *user, chttp_request *out_request) {
  crpc_prepared_call prepared = {0};
  chttp_request_options http;
  chttp_rpc_direct_call *call;
  uint64_t start_ms;
  uint64_t deadline_at_ms = 0u;
  int status;

  if (out_request == NULL) return SALTS_EINVAL;
  *out_request = (chttp_request){0};
  if (client == NULL || client->impl == NULL || limits == NULL ||
      limits->size != sizeof(*limits) || options == NULL ||
      on_complete == NULL || limits->max_method_bytes == 0u ||
      limits->max_json_depth < 2u ||
      limits->max_request_body_bytes == 0u ||
      limits->max_http_header_count < 5u)
    return SALTS_EINVAL;
  start_ms = cmeta_monotonic_ms();
  if (options->deadline_ms != 0u) {
    if (UINT64_MAX - start_ms < options->deadline_ms) return SALTS_ERANGE;
    deadline_at_ms = start_ms + options->deadline_ms;
  }
  status = crpc_prepare_call(
      options, limits->max_method_bytes, limits->max_json_depth,
      limits->max_request_body_bytes, limits->max_http_header_count,
      &prepared);
  if (status != SALTS_OK) return status;
  if (deadline_at_ms != 0u && cmeta_monotonic_ms() >= deadline_at_ms) {
    crpc_prepared_call_destroy(&prepared);
    return SALTS_ETIMEDOUT;
  }
  call = (chttp_rpc_direct_call *)calloc(1u, sizeof(*call));
  if (call == NULL) {
    crpc_prepared_call_destroy(&prepared);
    return SALTS_ENOMEM;
  }
  call->on_complete = on_complete;
  call->user = user;
  call->wire_id = options->request_id;
  call->max_json_depth = limits->max_json_depth;
  call->callable = prepared.callable;
  call->has_callable = prepared.has_callable;
  http = (chttp_request_options){
      .connection_uri = options->connection_uri,
      .authority = options->authority,
      .target = options->target,
      .method = CHTTP_METHOD_POST,
      .headers = prepared.headers,
      .header_count = prepared.header_count,
      .body = prepared.encoded.data,
      .body_size = prepared.encoded.size,
      .on_complete = chttp_rpc_direct_complete,
      .user = call,
      .tls = options->tls,
      .protocol = options->protocol};
  status = chttp_async_client_rpc_submit_transport(
      client, &http, options->request_id, deadline_at_ms, out_request);
  crpc_prepared_call_destroy(&prepared);
  if (status != SALTS_OK) {
    free(call);
    *out_request = (chttp_request){0};
  }
  return status;
}
