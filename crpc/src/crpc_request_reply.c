#include "crpc_internal.h"

#include <salts/clock.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct crpc_blocking_client_impl {
  chttp_client http;
  crpc_client_config config;
  bool operation_active;
} crpc_blocking_client_impl;

static crpc_blocking_client_impl *crpc_client_get(crpc_client *client) {
  return client != NULL ? (crpc_blocking_client_impl *)client->impl : NULL;
}

static int crpc_deadline_remaining(uint64_t started_ms, uint32_t deadline_ms,
                                   uint32_t *out_remaining_ms) {
  const uint64_t now_ms = salts_monotonic_ms();
  const uint64_t elapsed_ms = now_ms >= started_ms ? now_ms - started_ms : 0u;
  if (out_remaining_ms == NULL) return SALTS_EINVAL;
  if (deadline_ms == 0u) {
    *out_remaining_ms = 0u;
    return SALTS_OK;
  }
  if (elapsed_ms >= deadline_ms) {
    *out_remaining_ms = 0u;
    return SALTS_ETIMEDOUT;
  }
  *out_remaining_ms = deadline_ms - (uint32_t)elapsed_ms;
  return SALTS_OK;
}

static void crpc_response_take(crpc_decoded_response *decoded, crpc_response *out) {
  const crpc_response_view *view = &decoded->response;
  *out = (crpc_response){.request_id = view->request_id,
                         .http_status = view->http_status,
                         .kind = view->kind,
                         .callable = view->callable,
                         .impl = decoded};
  if (view->kind == CRPC_RESPONSE_RESULT) out->value.result = view->value.result;
  else out->value.remote_error = view->value.remote_error;
}

int crpc_client_init(crpc_client *client, const crpc_client_config *config) {
  crpc_blocking_client_impl *impl;
  int status;
  if (client == NULL || config == NULL) return SALTS_EINVAL;
  if (client->impl != NULL) return SALTS_EALREADY;
  if (!crpc_client_config_valid(config)) return SALTS_EINVAL;
  impl = (crpc_blocking_client_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  status = chttp_client_init(&impl->http, &config->http);
  if (status != SALTS_OK) {
    free(impl);
    return status;
  }
  impl->config = *config;
  client->impl = impl;
  return SALTS_OK;
}

void crpc_response_destroy(crpc_response *response) {
  crpc_decoded_response *decoded;
  if (response == NULL) return;
  decoded = (crpc_decoded_response *)response->impl;
  if (decoded != NULL) {
    crpc_decoded_response_destroy(decoded);
    free(decoded);
  }
  *response = (crpc_response){0};
}

int crpc_request_reply(crpc_client *client, const crpc_options *options,
                       crpc_response *out_response, crpc_error *out_error) {
  crpc_blocking_client_impl *impl = crpc_client_get(client);
  crpc_prepared_call prepared = {0};
  chttp_response http_response = {0};
  chttp_error http_error = {0};
  crpc_decoded_response *decoded = NULL;
  chttp_options http_options;
  const char *stage = NULL;
  uint64_t started_ms;
  uint32_t remaining_ms = 0u;
  int status;

  if (out_response == NULL || out_error == NULL) return SALTS_EINVAL;
  *out_response = (crpc_response){0};
  *out_error = (crpc_error){0};
  if (impl == NULL || options == NULL) return SALTS_EINVAL;
  if (impl->operation_active) return SALTS_EBUSY;

  impl->operation_active = true;
  started_ms = salts_monotonic_ms();
  status = crpc_prepare_call(options, impl->config.max_method_bytes, impl->config.max_json_depth,
                             impl->config.http.max_request_body_bytes,
                             impl->config.http.max_header_count, &prepared);
  if (status != SALTS_OK) {
    *out_error = (crpc_error){.status = status, .stage = "rpc-prepare"};
    goto done;
  }
  status = crpc_deadline_remaining(started_ms, options->deadline_ms, &remaining_ms);
  if (status != SALTS_OK) {
    *out_error = (crpc_error){.status = status, .stage = "rpc-deadline"};
    goto done;
  }

  http_options = (chttp_options){.connection_uri = options->connection_uri,
                                 .authority = options->authority,
                                 .target = options->target,
                                 .headers = prepared.headers,
                                 .header_count = prepared.header_count,
                                 .body = prepared.encoded.data,
                                 .body_size = prepared.encoded.size,
                                 .timeout_ms = remaining_ms,
                                 .tls = options->tls,
                                 .protocol = options->protocol};
  status = chttp_post(&impl->http, &http_options, &http_response, &http_error);
  if (status != SALTS_OK) {
    *out_error = (crpc_error){
        .status = status, .native_status = http_error.native_status, .stage = http_error.stage};
    goto done;
  }
  status = crpc_deadline_remaining(started_ms, options->deadline_ms, &remaining_ms);
  if (status != SALTS_OK) {
    *out_error = (crpc_error){
        .status = status, .http_status = http_response.status_code, .stage = "rpc-deadline"};
    goto done;
  }

  decoded = (crpc_decoded_response *)calloc(1u, sizeof(*decoded));
  if (decoded == NULL) {
    status = SALTS_ENOMEM;
    *out_error = (crpc_error){
        .status = status, .http_status = http_response.status_code, .stage = "rpc-response"};
    goto done;
  }
  status =
      crpc_json_decode_response(http_response.body, http_response.body_size, options->request_id,
                                http_response.status_code, impl->config.max_json_depth,
                                prepared.has_callable ? &prepared.callable : NULL, decoded, &stage);
  if (status != SALTS_OK) {
    *out_error =
        (crpc_error){.status = status, .http_status = http_response.status_code, .stage = stage};
    goto done;
  }
  status = crpc_deadline_remaining(started_ms, options->deadline_ms, &remaining_ms);
  if (status != SALTS_OK) {
    *out_error = (crpc_error){
        .status = status, .http_status = http_response.status_code, .stage = "rpc-deadline"};
    goto done;
  }

  crpc_response_take(decoded, out_response);
  decoded = NULL;

done:
  if (decoded != NULL) {
    crpc_decoded_response_destroy(decoded);
    free(decoded);
  }
  chttp_response_destroy(&http_response);
  crpc_prepared_call_destroy(&prepared);
  impl->operation_active = false;
  return status;
}

int crpc_client_destroy(crpc_client *client, uint32_t timeout_ms) {
  crpc_blocking_client_impl *impl;
  int status;
  if (client == NULL) return SALTS_EINVAL;
  impl = crpc_client_get(client);
  if (impl == NULL) return SALTS_OK;
  if (impl->operation_active) return SALTS_EBUSY;
  status = chttp_client_destroy(&impl->http, timeout_ms);
  if (status != SALTS_OK) return status;
  free(impl);
  client->impl = NULL;
  return SALTS_OK;
}
