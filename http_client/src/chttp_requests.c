#include <http_client/http.h>

#include "chttp_client_internal.h"
#include "chttp_response_owner.h"
#include "chttp_retry.h"
#include <cnet/recovery_policy.h>
#include <http_client/response_scope.h>

#include <core/clock.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_requests_probe {
  chttp_async_client *async;
  chttp_attempt_facts attempt;
  chttp_response response;
  chttp_error error;
  int copy_status;
  bool done;
} chttp_requests_probe;

typedef struct chttp_blocking_client_impl {
  chttp_async_client async;
  chttp_client_config config;
  cnet_stream_socket_options socket_options;
  chttp_h2_receive_window_policy h2_window_policy;
  chttp_connection_recovery_options recovery_options;
  chttp_requests_probe probe;
  bool socket_options_set;
  bool h2_window_policy_set;
  bool recovery_options_set;
  bool admission_started;
  bool operation_active;
  bool usable;
} chttp_blocking_client_impl;

static chttp_blocking_client_impl *chttp_client_get_impl(chttp_client *client) {
  return client != NULL ? (chttp_blocking_client_impl *)client->impl : NULL;
}

static void chttp_requests_complete(void *user, chttp_request request,
                                    const chttp_response_view *response, const chttp_error *error) {
  chttp_requests_probe *probe = (chttp_requests_probe *)user;
  if (probe == NULL || probe->done) return;
  if (probe->async != NULL) {
    const int status = chttp_async_client_attempt_facts(probe->async, request, &probe->attempt);
    if (status != SALTS_OK) {
      probe->error = (chttp_error){.status = status, .stage = "attempt-facts"};
      probe->copy_status = status;
      probe->done = true;
      return;
    }
  }
  if (error != NULL) {
    probe->error = *error;
    probe->copy_status = error->status;
  } else if (response == NULL) {
    probe->error = (chttp_error){.status = SALTS_EPROTO, .stage = "response"};
    probe->copy_status = SALTS_EPROTO;
  } else {
    probe->copy_status = chttp_response_copy(response, &probe->response);
    if (probe->copy_status != SALTS_OK)
      probe->error = (chttp_error){.status = probe->copy_status, .stage = "response-copy"};
  }
  probe->done = true;
}

static uint64_t chttp_requests_deadline_after(uint64_t now_ms, uint32_t timeout_ms) {
  return timeout_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + timeout_ms;
}

static uint32_t chttp_requests_poll_wait(uint64_t deadline_at_ms) {
  const uint64_t now_ms = monotonic_ms();
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
  if (status == SALTS_OK && impl->h2_window_policy_set)
    status = chttp_async_client_set_h2_receive_window_policy(
        &impl->async, &impl->h2_window_policy);
  if (status == SALTS_OK && impl->socket_options_set)
    status = chttp_async_client_set_socket_options(
        &impl->async, &impl->socket_options);
  if (status == SALTS_OK && impl->recovery_options_set)
    status = chttp_async_client_set_connection_recovery(&impl->async, &impl->recovery_options);
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

int chttp_client_set_h2_receive_window_policy(
    chttp_client *client,
    const chttp_h2_receive_window_policy *policy) {
  chttp_blocking_client_impl *impl = chttp_client_get_impl(client);
  int status;
  if (impl == NULL || policy == NULL || policy->size != sizeof(*policy))
    return SALTS_EINVAL;
  if (!impl->usable) return SALTS_ESHUTDOWN;
  if (impl->operation_active) return SALTS_EBUSY;
  status = chttp_async_client_set_h2_receive_window_policy(
      &impl->async, policy);
  if (status != SALTS_OK) return status;
  impl->h2_window_policy = *policy;
  impl->h2_window_policy_set = true;
  return SALTS_OK;
}

int chttp_client_set_connection_recovery(chttp_client *client,
    const chttp_connection_recovery_options *options) {
  chttp_blocking_client_impl *impl = chttp_client_get_impl(client);
  int status;
  if (impl == NULL || options == NULL) return SALTS_EINVAL;
  if (impl->admission_started || impl->operation_active) return SALTS_EBUSY;
  status = chttp_async_client_set_connection_recovery(&impl->async, options);
  if (status != SALTS_OK) return status;
  impl->recovery_options = *options;
  impl->recovery_options_set = true;
  return SALTS_OK;
}

int chttp_client_set_socket_options(
    chttp_client *client,
    const cnet_stream_socket_options *options) {
  chttp_blocking_client_impl *impl = chttp_client_get_impl(client);
  if (impl == NULL || options == NULL ||
      options->size != sizeof(*options))
    return SALTS_EINVAL;
  if (!impl->usable) return SALTS_ESHUTDOWN;
  if (impl->operation_active) return SALTS_EBUSY;
  {
    const int status =
        chttp_async_client_set_socket_options(&impl->async, options);
    if (status != SALTS_OK) return status;
  }
  impl->socket_options = *options;
  impl->socket_options_set = true;
  return SALTS_OK;
}

static int chttp_requests_perform(chttp_client *client, chttp_method method,
                                  const chttp_options *options, chttp_response *out_response,
                                  chttp_error *out_error, chttp_file_transfer *file_transfer,
                                  chttp_file_sink_transfer *file_sink_transfer,
                                  const chttp_destination_options *destinations,
                                  cnet_destination_result *out_destination,
                                  const chttp_retry_options *retry) {
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
  bool settled = false;
  chttp_retry_request owned = {0};
  uint32_t attempts = 0u;
  size_t byte_budget = retry != NULL ? retry->retry_body_byte_budget : 0u;
  uint64_t exponential_ms = retry != NULL ? retry->initial_backoff_ms : 0u;
  uint64_t random = retry != NULL && retry->jitter_seed != 0u
      ? retry->jitter_seed : UINT64_C(0x9e3779b97f4a7c15);

  if (out_destination != NULL) *out_destination = (cnet_destination_result){.index = SIZE_MAX};
  if (out_response == NULL || out_error == NULL) return SALTS_EINVAL;
  *out_response = (chttp_response){0};
  *out_error = (chttp_error){0};
  if (impl == NULL || options == NULL) return SALTS_EINVAL;
  if (destinations != NULL && out_destination == NULL) return SALTS_EINVAL;
  if (!impl->usable) return SALTS_ESHUTDOWN;
  if (impl->operation_active) return SALTS_EBUSY;

  impl->operation_active = true;
  impl->admission_started = true;
  if (options->timeout_ms != 0u) {
    const uint64_t now = monotonic_ms();
    if (retry != NULL && options->timeout_ms > UINT64_MAX - now) {
      status = SALTS_ERANGE;
      *out_error = (chttp_error){.status = status, .stage = "retry-deadline"};
      goto finished;
    }
    deadline_at_ms = chttp_requests_deadline_after(now, options->timeout_ms);
  }
  if (retry != NULL) {
    status = chttp_retry_request_copy(&owned, options, &impl->config);
    if (status != SALTS_OK) {
      *out_error = (chttp_error){.status = status, .stage = "retry-input"};
      goto finished;
    }
    options = &owned.options;
  }
attempt:
  ++attempts;
  admitted = false;
  admission_progressed = false;
  settled = false;
  timed_out = false;
  request = (chttp_request){0};
  chttp_response_destroy(&impl->probe.response);
  impl->probe = (chttp_requests_probe){.async = &impl->async};
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
  if (destinations != NULL) {
    const char *uri = NULL;
    status = chttp_destination_select(destinations, &request_options, out_destination, &uri);
    if (status != SALTS_OK) {
      *out_error = (chttp_error){.status = status, .stage = "destination-select"};
      goto finished;
    }
    request_options.connection_uri = uri;
  }
  for (;;) {
    if (destinations != NULL || retry != NULL) {
      if (deadline_at_ms != 0u && chttp_requests_poll_wait(deadline_at_ms) == 0u) {
        timed_out = true;
        status = SALTS_ETIMEDOUT;
        break;
      }
    }
    status = chttp_async_client_submit_timed(&impl->async, &request_options,
        file_transfer, file_sink_transfer, destinations != NULL ? out_destination : NULL,
        deadline_at_ms, &request);
    if (status == SALTS_ETIMEDOUT && deadline_at_ms != 0u && monotonic_ms() >= deadline_at_ms)
      timed_out = true;
    if (status != SALTS_ENOBUFS || destinations != NULL) break;
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
  if (status == SALTS_OK && impl->probe.done) {
    status = chttp_requests_wait_recycled(impl, request);
    settled = status == SALTS_OK;
  }
  if (retry != NULL && status == SALTS_OK && deadline_at_ms != 0u &&
      monotonic_ms() >= deadline_at_ms) timed_out = true;

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
      chttp_response_move(out_response, &impl->probe.response);
    } else {
      *out_error = impl->probe.error;
    }
  }
finished:
  if (retry != NULL && timed_out) {
    out_error->stage = "retry-deadline";
    if (impl->probe.done) out_error->native_status = impl->probe.error.native_status;
  }
  if (retry != NULL && retry->max_attempts > 1u && settled && admitted && impl->probe.done && status != SALTS_OK &&
      impl->probe.attempt.retryable_failure && !impl->probe.attempt.response_started) {
    uint64_t delay = 0u, not_before, now = monotonic_ms();
    cnet_retry_input input = {.size = sizeof(input), .version = CNET_RECOVERY_POLICY_VERSION,
        .attempts_used = attempts, .max_attempts = retry->max_attempts,
        .now_ms = now, .deadline_ms = deadline_at_ms,
        .request_body_bytes = options->body_size, .remaining_retry_byte_budget = byte_budget,
        .explicitly_enabled = retry->max_attempts > 1u,
        .cancelled = status == SALTS_ECANCELED || status == SALTS_ESHUTDOWN,
        .security_failure = impl->probe.attempt.security_failure || status == SALTS_EAUTH,
        .owned_replayable_body = true,
        .protocol_proves_not_executed = impl->probe.attempt.protocol_not_executed,
        .application_declares_idempotent = retry->application_declares_idempotent ||
            method == CHTTP_METHOD_GET || method == CHTTP_METHOD_HEAD ||
            method == CHTTP_METHOD_PUT || method == CHTTP_METHOD_DELETE ||
            method == CHTTP_METHOD_OPTIONS};
    cnet_retry_result decision;
    int gate = cnet_retry_evaluate(&input, &decision);
    if (gate != SALTS_OK) { status = gate; goto retry_error; }
    if (!decision.allowed) goto retry_denied;
    random ^= random << 13u; random ^= random >> 7u; random ^= random << 17u;
    {
      const cnet_backoff_input backoff = {.attempts = attempts,
          .initial_ms = retry->initial_backoff_ms, .maximum_ms = retry->maximum_backoff_ms,
          .exponential_ms = exponential_ms, .random_value = random};
      gate = cnet_backoff_exponential_jitter(NULL, &backoff, &delay);
    }
    if (gate != SALTS_OK) { status = gate; goto retry_error; }
    if (delay > UINT64_MAX - now) { status = SALTS_ERANGE; goto retry_error; }
    not_before = now + delay;
    input.backoff_not_before_ms = not_before;
    for (;;) {
      input.now_ms = monotonic_ms();
      gate = cnet_retry_evaluate(&input, &decision);
      if (gate != SALTS_OK) { status = gate; goto retry_error; }
      if (decision.reason != CNET_RETRY_WAIT_BACKOFF) break;
      {
        const uint64_t remaining = not_before - input.now_ms;
        const uint32_t wait = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
        gate = chttp_async_client_poll(&impl->async, wait, &completions);
        if (gate != SALTS_OK) { status = gate; goto retry_error; }
      }
    }
    if (!decision.allowed) goto retry_denied;
    byte_budget -= options->body_size;
    exponential_ms = exponential_ms > retry->maximum_backoff_ms / 2u
        ? retry->maximum_backoff_ms : exponential_ms * 2u;
    *out_error = (chttp_error){0};
    goto attempt;
retry_denied:
    if (decision.reason == CNET_RETRY_DEADLINE) {
      status = SALTS_ETIMEDOUT;
      out_error->status = status;
      out_error->stage = "retry-deadline";
    } else if (decision.reason == CNET_RETRY_BUDGET && attempts < retry->max_attempts) {
      status = SALTS_ENOBUFS;
      out_error->status = status;
      out_error->stage = "retry-byte-budget";
    }
    goto retry_done;
retry_error:
    out_error->status = status;
    out_error->stage = "retry-progress";
  }
retry_done:
  chttp_retry_request_destroy(&owned);
  impl->operation_active = false;
  return status;
}

int chttp_request_retry(chttp_client *client, chttp_method method,
    const chttp_options *options, const chttp_retry_options *retry,
    chttp_response *out_response, chttp_error *out_error) {
  chttp_retry_options policy;
  int status = SALTS_OK;
  if (out_response != NULL) *out_response = (chttp_response){0};
  if (out_error != NULL) *out_error = (chttp_error){0};
  if (out_response == NULL || out_error == NULL || options == NULL || retry == NULL ||
      retry->size != sizeof(*retry) || retry->max_attempts == 0u ||
      retry->initial_backoff_ms > retry->maximum_backoff_ms ||
      (retry->max_attempts > 1u && options->timeout_ms == 0u)) status = SALTS_EINVAL;
  else if (options->body_source != NULL || options->body_sink != NULL) status = SALTS_ENOTSUP;
  if (status != SALTS_OK) {
    if (out_error != NULL) *out_error = (chttp_error){.status = status, .stage = "retry-config"};
    return status;
  }
  policy = *retry;
  return chttp_requests_perform(client, method, options, out_response, out_error,
      NULL, NULL, NULL, NULL, &policy);
}

int chttp_request_to(chttp_client *client, chttp_method method,
                      const chttp_options *options,
                      const chttp_destination_options *destinations,
                      cnet_destination_result *out_destination,
                      chttp_response *out_response, chttp_error *out_error) {
  if (destinations == NULL) {
    if (out_destination != NULL) *out_destination = (cnet_destination_result){.index = SIZE_MAX};
    if (out_response != NULL) *out_response = (chttp_response){0};
    if (out_error != NULL) *out_error = (chttp_error){.status = SALTS_EINVAL,
                                                    .stage = "destination-select"};
    return SALTS_EINVAL;
  }
  return chttp_requests_perform(client, method, options, out_response, out_error, NULL, NULL,
                                 destinations, out_destination, NULL);
}

int chttp_get(chttp_client *client, const chttp_options *options, chttp_response *out_response,
              chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_GET, options, out_response, out_error, NULL,
                                NULL, NULL, NULL, NULL);
}

int chttp_head(chttp_client *client, const chttp_options *options, chttp_response *out_response,
               chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_HEAD, options, out_response, out_error, NULL,
                                NULL, NULL, NULL, NULL);
}

int chttp_post(chttp_client *client, const chttp_options *options, chttp_response *out_response,
               chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_POST, options, out_response, out_error, NULL,
                                NULL, NULL, NULL, NULL);
}

int chttp_put(chttp_client *client, const chttp_options *options, chttp_response *out_response,
              chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_PUT, options, out_response, out_error, NULL,
                                NULL, NULL, NULL, NULL);
}

int chttp_delete(chttp_client *client, const chttp_options *options, chttp_response *out_response,
                 chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_DELETE, options, out_response, out_error, NULL,
                                NULL, NULL, NULL, NULL);
}

int chttp_patch(chttp_client *client, const chttp_options *options, chttp_response *out_response,
                chttp_error *out_error) {
  return chttp_requests_perform(client, CHTTP_METHOD_PATCH, options, out_response, out_error, NULL,
                                NULL, NULL, NULL, NULL);
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
  return chttp_requests_perform(client, method, options, out_response, out_error, transfer, NULL,
                                 NULL, NULL, NULL);
}

int chttp_client_perform_file_download(chttp_client *client, const chttp_options *options,
                                       chttp_file_sink_transfer *transfer,
                                       chttp_response *out_response, chttp_error *out_error) {
  if (transfer == NULL) return SALTS_EINVAL;
  return chttp_requests_perform(client, CHTTP_METHOD_GET, options, out_response, out_error, NULL,
                                transfer, NULL, NULL, NULL);
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
