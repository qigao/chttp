#include <chttp/chttp.h>

#include "chttp_client_internal.h"

#include <salts/clock.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_requests_probe {
  chttp_response response;
  chttp_error error;
  int copy_status;
  bool done;
} chttp_requests_probe;

typedef struct chttp_blocking_client_impl {
  chttp_async_client async;
  chttp_client_config config;
  chttp_requests_probe probe;
  bool operation_active;
  bool usable;
} chttp_blocking_client_impl;

static chttp_blocking_client_impl *chttp_client_get_impl(chttp_client *client) {
  return client != NULL ? (chttp_blocking_client_impl *)client->impl : NULL;
}

static char *chttp_requests_copy_text(const char *text) {
  size_t size;
  char *copy;
  if (text == NULL) return NULL;
  size = strlen(text);
  if (size == SIZE_MAX) return NULL;
  copy = (char *)malloc(size + 1u);
  if (copy == NULL) return NULL;
  memcpy(copy, text, size + 1u);
  return copy;
}

void chttp_response_destroy(chttp_response *response) {
  size_t index;
  if (response == NULL) return;
  free(response->reason);
  for (index = 0u; index < response->header_count; ++index) {
    free((void *)response->headers[index].name);
    free((void *)response->headers[index].value);
  }
  free(response->headers);
  free(response->body);
  *response = (chttp_response){0};
}

static int chttp_requests_copy_response(const chttp_response_view *source, chttp_response *out) {
  size_t index;
  if (source == NULL || out == NULL) return SALTS_EINVAL;
  *out = (chttp_response){.http_major = source->http_major,
                          .http_minor = source->http_minor,
                          .status_code = source->status_code,
                          .header_count = source->header_count,
                          .body_size = source->body_size,
                          .protocol_keep_alive = source->protocol_keep_alive};
  if (source->reason != NULL) {
    out->reason = chttp_requests_copy_text(source->reason);
    if (out->reason == NULL) goto no_memory;
  }
  if (source->header_count != 0u) {
    if (source->headers == NULL || source->header_count > SIZE_MAX / sizeof(*out->headers))
      goto protocol_error;
    out->headers = (chttp_header *)calloc(source->header_count, sizeof(*out->headers));
    if (out->headers == NULL) goto no_memory;
    for (index = 0u; index < source->header_count; ++index) {
      char *name = chttp_requests_copy_text(source->headers[index].name);
      char *value = chttp_requests_copy_text(source->headers[index].value);
      if (name == NULL || value == NULL) {
        free(name);
        free(value);
        goto no_memory;
      }
      out->headers[index] = (chttp_header){name, value};
    }
  }
  if (source->body_size != 0u && source->body != NULL) {
    out->body = malloc(source->body_size);
    if (out->body == NULL) goto no_memory;
    memcpy(out->body, source->body, source->body_size);
  }
  return SALTS_OK;

no_memory:
  chttp_response_destroy(out);
  return SALTS_ENOMEM;
protocol_error:
  chttp_response_destroy(out);
  return SALTS_EPROTO;
}

static void chttp_requests_complete(void *user, chttp_request request,
                                    const chttp_response_view *response, const chttp_error *error) {
  chttp_requests_probe *probe = (chttp_requests_probe *)user;
  (void)request;
  if (probe == NULL || probe->done) return;
  if (error != NULL) {
    probe->error = *error;
    probe->copy_status = error->status;
  } else if (response == NULL) {
    probe->error = (chttp_error){.status = SALTS_EPROTO, .stage = "response"};
    probe->copy_status = SALTS_EPROTO;
  } else {
    probe->copy_status = chttp_requests_copy_response(response, &probe->response);
    if (probe->copy_status != SALTS_OK)
      probe->error = (chttp_error){.status = probe->copy_status, .stage = "response-copy"};
  }
  probe->done = true;
}

static uint64_t chttp_requests_deadline_after(uint64_t now_ms, uint32_t timeout_ms) {
  return timeout_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + timeout_ms;
}

static uint32_t chttp_requests_poll_wait(uint64_t deadline_at_ms) {
  const uint64_t now_ms = salts_monotonic_ms();
  const uint64_t remaining = deadline_at_ms > now_ms ? deadline_at_ms - now_ms : 0u;
  return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static int chttp_requests_recover(chttp_blocking_client_impl *impl, uint32_t timeout_ms) {
  int status;
  impl->usable = false;
  status = chttp_async_client_stop(&impl->async, timeout_ms);
  if (status != SALTS_OK) return status;
  status = chttp_async_client_destroy(&impl->async);
  if (status != SALTS_OK) return status;
  chttp_response_destroy(&impl->probe.response);
  impl->probe = (chttp_requests_probe){0};
  status = chttp_async_client_init(&impl->async, &impl->config);
  if (status == SALTS_OK) impl->usable = true;
  return status;
}

static int chttp_requests_wait_recycled(chttp_blocking_client_impl *impl, chttp_request request) {
  size_t completions = 0u;
  int status;
  for (;;) {
    status = chttp_async_request_cancel(&impl->async, request);
    if (status == SALTS_ENOENT) return SALTS_OK;
    if (status != SALTS_EALREADY) return status;
    status = chttp_async_client_poll(&impl->async, UINT32_MAX, &completions);
    if (status != SALTS_OK) return status;
  }
}

int chttp_client_init(chttp_client *client, const chttp_client_config *config) {
  chttp_blocking_client_impl *impl;
  int status;
  if (client == NULL || config == NULL) return SALTS_EINVAL;
  if (client->impl != NULL) return SALTS_EALREADY;
  impl = (chttp_blocking_client_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  status = chttp_async_client_init(&impl->async, config);
  if (status != SALTS_OK) {
    free(impl);
    return status;
  }
  impl->config = *config;
  impl->usable = true;
  client->impl = impl;
  return SALTS_OK;
}

static int chttp_requests_perform(chttp_client *client, chttp_method method,
                                  const chttp_options *options, chttp_response *out_response,
                                  chttp_error *out_error, chttp_file_transfer *file_transfer,
                                  chttp_file_sink_transfer *file_sink_transfer) {
  chttp_blocking_client_impl *impl = chttp_client_get_impl(client);
  chttp_request_options request_options;
  chttp_request request = {0};
  uint64_t deadline_at_ms = 0u;
  size_t completions = 0u;
  int status;
  int recovery_status;
  bool timed_out = false;
  bool admitted = false;
  bool admission_progressed = false;

  if (out_response == NULL || out_error == NULL) return SALTS_EINVAL;
  *out_response = (chttp_response){0};
  *out_error = (chttp_error){0};
  if (impl == NULL || options == NULL) return SALTS_EINVAL;
  if (!impl->usable) return SALTS_ESHUTDOWN;
  if (impl->operation_active) return SALTS_EBUSY;

  impl->operation_active = true;
  chttp_response_destroy(&impl->probe.response);
  impl->probe = (chttp_requests_probe){0};
  request_options = (chttp_request_options){.connection_uri = options->connection_uri,
                                            .authority = options->authority,
                                            .target = options->target,
                                            .method = method,
                                            .headers = options->headers,
                                            .header_count = options->header_count,
                                            .body = options->body,
                                            .body_size = options->body_size,
                                            .body_source = options->body_source,
                                            .body_sink = options->body_sink,
                                            .on_complete = chttp_requests_complete,
                                            .user = &impl->probe,
                                            .tls = options->tls,
                                            .protocol = options->protocol};
  if (options->timeout_ms != 0u)
    deadline_at_ms = chttp_requests_deadline_after(salts_monotonic_ms(), options->timeout_ms);
  for (;;) {
    if (file_transfer != NULL)
      status =
          chttp_async_client_submit_file(&impl->async, &request_options, file_transfer, &request);
    else if (file_sink_transfer != NULL)
      status = chttp_async_client_submit_file_download(&impl->async, &request_options,
                                                       file_sink_transfer, &request);
    else status = chttp_async_client_submit(&impl->async, &request_options, &request);
    if (status != SALTS_ENOBUFS) break;
    admission_progressed = true;
    {
      uint32_t wait_ms = UINT32_MAX;
      if (deadline_at_ms != 0u) {
        wait_ms = chttp_requests_poll_wait(deadline_at_ms);
        if (wait_ms == 0u) {
          timed_out = true;
          break;
        }
      }
      status = chttp_async_client_poll(&impl->async, wait_ms, &completions);
      if (status != SALTS_OK) break;
    }
  }
  admitted = status == SALTS_OK;
  while (status == SALTS_OK && !impl->probe.done) {
    uint32_t wait_ms = UINT32_MAX;
    if (deadline_at_ms != 0u) {
      wait_ms = chttp_requests_poll_wait(deadline_at_ms);
      if (wait_ms == 0u) {
        timed_out = true;
        break;
      }
    }
    status = chttp_async_client_poll(&impl->async, wait_ms, &completions);
  }
  if (status == SALTS_OK && impl->probe.done) status = chttp_requests_wait_recycled(impl, request);

  if (!admitted) {
    if (timed_out) status = SALTS_ETIMEDOUT;
    *out_error = (chttp_error){.status = status,
                               .stage = timed_out              ? "request-deadline"
                                        : admission_progressed ? "request-progress"
                                                               : "request-submit"};
  } else if (timed_out || status != SALTS_OK) {
    const int primary_status = timed_out ? SALTS_ETIMEDOUT : status;
    const char *primary_stage = timed_out ? "request-deadline" : "request-progress";
    if (!impl->probe.done) {
      if (request.slot != 0u) (void)chttp_async_request_cancel(&impl->async, request);
      if (file_transfer != NULL && file_transfer->file.impl != NULL) {
        cflow_io_file_runtime *runtime = NULL;
        recovery_status = chttp_async_client_file_runtime(&impl->async, &runtime);
        if (recovery_status == SALTS_OK)
          recovery_status = chttp_file_transfer_drain_destroy(file_transfer, runtime);
        if (recovery_status != SALTS_OK) {
          status = recovery_status;
          *out_error = (chttp_error){.status = status, .stage = "file-drain"};
          goto finished;
        }
      }
      if (file_sink_transfer != NULL && file_sink_transfer->file.impl != NULL) {
        cflow_io_file_runtime *runtime = NULL;
        recovery_status = chttp_async_client_file_runtime(&impl->async, &runtime);
        if (recovery_status == SALTS_OK)
          recovery_status = chttp_file_sink_transfer_drain_destroy(file_sink_transfer, runtime);
        if (recovery_status != SALTS_OK) {
          status = recovery_status;
          *out_error = (chttp_error){.status = status, .stage = "file-drain"};
          goto finished;
        }
      }
      recovery_status = chttp_requests_recover(impl, UINT32_MAX);
      if (recovery_status != SALTS_OK) {
        status = recovery_status;
        *out_error = (chttp_error){.status = status, .stage = "request-recover"};
      } else {
        status = primary_status;
        *out_error = (chttp_error){.status = status, .stage = primary_stage};
      }
    } else {
      status = primary_status;
      *out_error = (chttp_error){.status = status, .stage = primary_stage};
      chttp_response_destroy(&impl->probe.response);
    }
  } else if (status == SALTS_OK && impl->probe.done) {
    status = impl->probe.copy_status;
    if (status == SALTS_OK) {
      *out_response = impl->probe.response;
      impl->probe.response = (chttp_response){0};
    } else {
      *out_error = impl->probe.error;
    }
  }
finished:
  impl->operation_active = false;
  return status;
}

int chttp_get(chttp_client *client, const chttp_options *options, chttp_response *out_response,
              chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_GET, options, out_response, out_error, NULL,
                                NULL);
}

int chttp_head(chttp_client *client, const chttp_options *options, chttp_response *out_response,
               chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_HEAD, options, out_response, out_error, NULL,
                                NULL);
}

int chttp_post(chttp_client *client, const chttp_options *options, chttp_response *out_response,
               chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_POST, options, out_response, out_error, NULL,
                                NULL);
}

int chttp_put(chttp_client *client, const chttp_options *options, chttp_response *out_response,
              chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_PUT, options, out_response, out_error, NULL,
                                NULL);
}

int chttp_delete(chttp_client *client, const chttp_options *options, chttp_response *out_response,
                 chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_DELETE, options, out_response, out_error, NULL,
                                NULL);
}

int chttp_patch(chttp_client *client, const chttp_options *options, chttp_response *out_response,
                chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_PATCH, options, out_response, out_error, NULL,
                                NULL);
}

int chttp_client_file_runtime(chttp_client *client, cflow_io_file_runtime **out_runtime) {
  chttp_blocking_client_impl *impl = chttp_client_get_impl(client);
  if (impl == NULL || out_runtime == NULL || impl->operation_active) return SALTS_EINVAL;
  return chttp_async_client_file_runtime(&impl->async, out_runtime);
}

int chttp_client_file_sink_capacity(chttp_client *client, size_t *out_capacity) {
  chttp_blocking_client_impl *impl = chttp_client_get_impl(client);
  if (impl == NULL || out_capacity == NULL || impl->operation_active) return SALTS_EINVAL;
  return chttp_async_client_file_sink_capacity(&impl->async, out_capacity);
}

int chttp_client_perform_file(chttp_client *client, chttp_method method,
                              const chttp_options *options, chttp_file_transfer *transfer,
                              chttp_response *out_response, chttp_error *out_error) {
  if (method != CHTTP_METHOD_POST && method != CHTTP_METHOD_PUT) return SALTS_EINVAL;
  return chttp_requests_perform(client, method, options, out_response, out_error, transfer, NULL);
}

int chttp_client_perform_file_download(chttp_client *client, const chttp_options *options,
                                       chttp_file_sink_transfer *transfer,
                                       chttp_response *out_response, chttp_error *out_error) {
  if (transfer == NULL) return SALTS_EINVAL;
  return chttp_requests_perform(client, CHTTP_METHOD_GET, options, out_response, out_error, NULL,
                                transfer);
}

const char *chttp_response_header(const chttp_response *response, const char *name) {
  chttp_response_view view;
  if (response == NULL) return NULL;
  view =
      (chttp_response_view){.headers = response->headers, .header_count = response->header_count};
  return chttp_response_view_header(&view, name);
}

int chttp_client_destroy(chttp_client *client, uint32_t timeout_ms) {
  chttp_blocking_client_impl *impl;
  int status;
  if (client == NULL) return SALTS_EINVAL;
  impl = chttp_client_get_impl(client);
  if (impl == NULL) return SALTS_OK;
  if (impl->operation_active) return SALTS_EBUSY;
  if (impl->async.impl != NULL) {
    status = chttp_async_client_stop(&impl->async, timeout_ms);
    if (status != SALTS_OK) return status;
    status = chttp_async_client_destroy(&impl->async);
    if (status != SALTS_OK) return status;
  }
  chttp_response_destroy(&impl->probe.response);
  free(impl);
  client->impl = NULL;
  return SALTS_OK;
}
