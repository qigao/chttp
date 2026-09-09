#include "s3_internal.h"

#include <stdlib.h>

typedef struct s3_async_completion {
  s3_async_client_impl *client;
  s3_complete_fn callback;
  void *user;
} s3_async_completion;

static s3_error s3_async_error(int status, int native_status, const char *stage) {
  return (s3_error){.status = status, .native_status = native_status, .stage = stage};
}

static void s3_async_complete(void *user, chttp_request request,
                              const chttp_response_view *http_response,
                              const chttp_error *http_error) {
  s3_async_completion *completion = (s3_async_completion *)user;
  s3_service_error service_error = {0};
  s3_response_view response = {.http = http_response};
  s3_error error = {0};
  const s3_error *error_pointer = NULL;
  const s3_response_view *response_pointer = NULL;
  int status;
  if (completion == NULL || completion->client == NULL) return;
  completion->client->callback_active = 1;
  if (http_error != NULL) {
    error = s3_async_error(http_error->status, http_error->native_status,
                           http_error->stage != NULL ? http_error->stage : "http");
    error_pointer = &error;
  } else if (http_response == NULL) {
    error = s3_async_error(SALTS_EPROTO, 0, "s3-response");
    error_pointer = &error;
  } else {
    response_pointer = &response;
    if (http_response->status_code < 200u || http_response->status_code >= 300u) {
      status =
          s3_service_error_parse(http_response->body, http_response->body_size,
                                 completion->client->base.config.max_xml_bytes,
                                 completion->client->base.config.max_xml_nodes, &service_error);
      response.service_error =
          (s3_service_error_view){service_error.code, service_error.message,
                                  service_error.request_id, service_error.host_id};
      error = status == SALTS_OK ? s3_async_error(SALTS_EPROTO, 0, "s3-service")
                                 : s3_async_error(status, 0, "s3-service-parse");
      error_pointer = &error;
    }
  }
  completion->callback(completion->user, (s3_request_handle){request.slot, request.generation},
                       response_pointer, error_pointer);
  s3_service_error_destroy(&service_error);
  completion->client->callback_active = 0;
  --completion->client->active_requests;
  free(completion);
}

int s3_async_client_init(s3_async_client *client, chttp_async_client *http_client,
                         const s3_client_config *config) {
  s3_async_client_impl *impl;
  int status;
  if (client == NULL || http_client == NULL || http_client->impl == NULL || config == NULL)
    return SALTS_EINVAL;
  if (client->impl != NULL) return SALTS_EALREADY;
  impl = (s3_async_client_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  status = s3_client_base_init(&impl->base, config);
  if (status != SALTS_OK) {
    free(impl);
    return status;
  }
  impl->http_client = http_client;
  client->impl = impl;
  return SALTS_OK;
}

int s3_async_client_submit(s3_async_client *client, const s3_async_request_options *options,
                           s3_request_handle *out_request) {
  s3_async_client_impl *impl = client != NULL ? (s3_async_client_impl *)client->impl : NULL;
  s3_request_plan plan = {0};
  s3_async_completion *completion = NULL;
  chttp_request http_request = {0};
  chttp_request_options http_options;
  int status;
  if (out_request == NULL) return SALTS_EINVAL;
  *out_request = (s3_request_handle){0};
  if (impl == NULL || options == NULL || options->size != sizeof(*options) ||
      options->request == NULL || options->on_complete == NULL)
    return SALTS_EINVAL;
  if (impl->callback_active) return SALTS_EBUSY;
  status = s3_request_plan_build(&impl->base, options->request, &plan);
  if (status != SALTS_OK) return status;
  completion = (s3_async_completion *)calloc(1u, sizeof(*completion));
  if (completion == NULL) {
    s3_request_plan_destroy(&plan);
    return SALTS_ENOMEM;
  }
  completion->client = impl;
  completion->callback = options->on_complete;
  completion->user = options->user;
  http_options = (chttp_request_options){.connection_uri = impl->base.connection_uri,
                                         .authority = plan.authority,
                                         .target = plan.target,
                                         .method = plan.method,
                                         .headers = plan.headers,
                                         .header_count = plan.header_count,
                                         .body = options->request->body,
                                         .body_size = options->request->body_size,
                                         .body_source = options->request->body_source,
                                         .body_sink = options->request->body_sink,
                                         .on_complete = s3_async_complete,
                                         .user = completion,
                                         .tls = impl->base.config.tls,
                                         .protocol = impl->base.config.protocol};
  status = chttp_async_client_submit(impl->http_client, &http_options, &http_request);
  s3_request_plan_destroy(&plan);
  if (status != SALTS_OK) {
    free(completion);
    return status;
  }
  ++impl->active_requests;
  *out_request = (s3_request_handle){http_request.slot, http_request.generation};
  return SALTS_OK;
}

int s3_async_request_cancel(s3_async_client *client, s3_request_handle request) {
  s3_async_client_impl *impl = client != NULL ? (s3_async_client_impl *)client->impl : NULL;
  if (impl == NULL) return SALTS_EINVAL;
  return chttp_async_request_cancel(
      impl->http_client, (chttp_request){.slot = request.slot, .generation = request.generation});
}

int s3_async_client_poll(s3_async_client *client, uint32_t timeout_ms, size_t *out_completions) {
  s3_async_client_impl *impl = client != NULL ? (s3_async_client_impl *)client->impl : NULL;
  if (impl == NULL) return SALTS_EINVAL;
  return chttp_async_client_poll(impl->http_client, timeout_ms, out_completions);
}

int s3_async_client_destroy(s3_async_client *client) {
  s3_async_client_impl *impl;
  if (client == NULL) return SALTS_EINVAL;
  impl = (s3_async_client_impl *)client->impl;
  if (impl == NULL) return SALTS_OK;
  if (impl->active_requests != 0u || impl->callback_active) return SALTS_EBUSY;
  s3_client_base_destroy(&impl->base);
  free(impl);
  client->impl = NULL;
  return SALTS_OK;
}
