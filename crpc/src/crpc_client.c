#include "crpc_internal.h"

#include <salts/clock.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct crpc_async_client_impl crpc_async_client_impl;

typedef struct crpc_slot {
  crpc_async_client_impl *client;
  crpc_request public_handle;
  chttp_request http_request;
  uint64_t request_id;
  uint64_t deadline_at_ms;
  crpc_complete_fn on_complete;
  void *user;
  cmeta_callable callable;
  uint32_t generation;
  bool has_callable;
  bool active;
  bool result_delivered;
  bool cancel_requested;
  bool deadline_expired;
} crpc_slot;

struct crpc_async_client_impl {
  chttp_async_client http;
  crpc_slot *slots;
  size_t request_capacity;
  size_t max_method_bytes;
  size_t max_json_depth;
  size_t max_body_bytes;
  size_t max_http_header_count;
  size_t completion_count;
  bool admission_open;
  bool submit_active;
  bool poll_active;
  bool callback_active;
  bool stop_active;
  bool stopped;
};

static crpc_async_client_impl *crpc_async_client_get(crpc_async_client *client) {
  return client != NULL ? (crpc_async_client_impl *)client->impl : NULL;
}

static uint32_t crpc_next_generation(uint32_t generation) {
  ++generation;
  return generation == 0u ? 1u : generation;
}

static crpc_slot *crpc_slot_find_free(crpc_async_client_impl *impl) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index)
    if (!impl->slots[index].active) return &impl->slots[index];
  return NULL;
}

static crpc_slot *crpc_slot_find(crpc_async_client_impl *impl, crpc_request request) {
  crpc_slot *slot;
  if (impl == NULL || request.slot == 0u || request.slot > impl->request_capacity ||
      request.generation == 0u)
    return NULL;
  slot = &impl->slots[request.slot - 1u];
  return slot->active && slot->generation == request.generation ? slot : NULL;
}

static bool crpc_request_id_active(const crpc_async_client_impl *impl, uint64_t request_id) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index)
    if (impl->slots[index].active && impl->slots[index].request_id == request_id) return true;
  return false;
}

static void crpc_slot_release(crpc_slot *slot) {
  crpc_async_client_impl *client;
  uint32_t generation;
  if (slot == NULL) return;
  client = slot->client;
  generation = slot->generation;
  *slot = (crpc_slot){.client = client, .generation = generation};
}

static uint64_t crpc_deadline_after(uint64_t now_ms, uint32_t timeout_ms) {
  return timeout_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + timeout_ms;
}

static void crpc_slot_complete(void *user, chttp_request http_request,
                               const chttp_response_view *http_response,
                               const chttp_error *http_error) {
  crpc_slot *slot = (crpc_slot *)user;
  crpc_async_client_impl *impl;
  crpc_decoded_response decoded = {0};
  crpc_error error;
  const crpc_error *error_view = NULL;
  const crpc_response_view *response_view = NULL;
  const char *stage = NULL;
  int status;

  if (slot == NULL || !slot->active || slot->result_delivered ||
      slot->http_request.slot != http_request.slot ||
      slot->http_request.generation != http_request.generation)
    return;
  impl = slot->client;
  slot->result_delivered = true;
  if (slot->deadline_expired || (!impl->stop_active && slot->deadline_at_ms != 0u &&
                                 salts_monotonic_ms() >= slot->deadline_at_ms)) {
    slot->deadline_expired = true;
    error = (crpc_error){.status = SALTS_ETIMEDOUT, .stage = "rpc-deadline"};
    error_view = &error;
  } else if (http_error != NULL) {
    error = (crpc_error){.status = http_error->status,
                         .native_status = http_error->native_status,
                         .stage = http_error->stage};
    error_view = &error;
  } else if (http_response == NULL) {
    error = (crpc_error){.status = SALTS_EPROTO, .stage = "http-response"};
    error_view = &error;
  } else {
    status = crpc_json_decode_response(
        http_response->body, http_response->body_size, slot->request_id, http_response->status_code,
        impl->max_json_depth, slot->has_callable ? &slot->callable : NULL, &decoded, &stage);
    if (status == SALTS_OK) {
      response_view = &decoded.response;
    } else {
      error =
          (crpc_error){.status = status, .http_status = http_response->status_code, .stage = stage};
      error_view = &error;
    }
  }

  impl->callback_active = true;
  slot->on_complete(slot->user, slot->public_handle, response_view, error_view);
  impl->callback_active = false;
  ++impl->completion_count;
  crpc_decoded_response_destroy(&decoded);
  crpc_slot_release(slot);
}

int crpc_async_client_init(crpc_async_client *client, const crpc_client_config *config) {
  crpc_async_client_impl *impl;
  size_t index;
  int status;
  if (client == NULL || config == NULL) return SALTS_EINVAL;
  if (client->impl != NULL) return SALTS_EALREADY;
  if (!crpc_client_config_valid(config) || config->request_capacity > SIZE_MAX / sizeof(crpc_slot))
    return SALTS_EINVAL;
  impl = (crpc_async_client_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->slots = (crpc_slot *)calloc(config->request_capacity, sizeof(*impl->slots));
  if (impl->slots == NULL) {
    free(impl);
    return SALTS_ENOMEM;
  }
  impl->request_capacity = config->request_capacity;
  impl->max_method_bytes = config->max_method_bytes;
  impl->max_json_depth = config->max_json_depth;
  impl->max_body_bytes = config->http.max_request_body_bytes;
  impl->max_http_header_count = config->http.max_header_count;
  for (index = 0u; index < impl->request_capacity; ++index)
    impl->slots[index].client = impl;
  status = chttp_async_client_init(&impl->http, &config->http);
  if (status != SALTS_OK) {
    free(impl->slots);
    free(impl);
    return status;
  }
  impl->admission_open = true;
  client->impl = impl;
  return SALTS_OK;
}

int crpc_async_client_submit(crpc_async_client *client, const crpc_options *options,
                             crpc_complete_fn on_complete, void *user, crpc_request *out_request) {
  crpc_async_client_impl *impl = crpc_async_client_get(client);
  crpc_prepared_call prepared = {0};
  crpc_slot *slot;
  chttp_request_options http_options;
  size_t slot_index;
  uint64_t started_ms;
  int status;

  if (out_request == NULL) return SALTS_EINVAL;
  *out_request = (crpc_request){0};
  if (impl == NULL || options == NULL || on_complete == NULL) return SALTS_EINVAL;
  if (impl->submit_active || impl->poll_active || impl->callback_active || impl->stop_active)
    return SALTS_EBUSY;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  if (crpc_request_id_active(impl, options->request_id)) return SALTS_EALREADY;
  slot = crpc_slot_find_free(impl);
  if (slot == NULL) return SALTS_ENOBUFS;

  impl->submit_active = true;
  started_ms = salts_monotonic_ms();
  status = crpc_prepare_call(options, impl->max_method_bytes, impl->max_json_depth,
                             impl->max_body_bytes, impl->max_http_header_count, &prepared);
  if (status == SALTS_OK && options->deadline_ms != 0u &&
      salts_monotonic_ms() - started_ms >= options->deadline_ms)
    status = SALTS_ETIMEDOUT;
  if (status != SALTS_OK) goto done;

  slot_index = (size_t)(slot - impl->slots);
  slot->generation = crpc_next_generation(slot->generation);
  slot->public_handle =
      (crpc_request){.slot = (uint32_t)(slot_index + 1u), .generation = slot->generation};
  slot->request_id = options->request_id;
  slot->deadline_at_ms =
      options->deadline_ms == 0u ? 0u : crpc_deadline_after(started_ms, options->deadline_ms);
  slot->on_complete = on_complete;
  slot->user = user;
  slot->callable = prepared.callable;
  slot->has_callable = prepared.has_callable;
  slot->active = true;
  if (options->deadline_ms != 0u && salts_monotonic_ms() >= slot->deadline_at_ms) {
    crpc_slot_release(slot);
    status = SALTS_ETIMEDOUT;
    goto done;
  }
  http_options = (chttp_request_options){.connection_uri = options->connection_uri,
                                         .authority = options->authority,
                                         .target = options->target,
                                         .method = CHTTP_METHOD_POST,
                                         .headers = prepared.headers,
                                         .header_count = prepared.header_count,
                                         .body = prepared.encoded.data,
                                         .body_size = prepared.encoded.size,
                                         .on_complete = crpc_slot_complete,
                                         .user = slot,
                                         .tls = options->tls,
                                         .protocol = options->protocol};
  status = chttp_async_client_submit(&impl->http, &http_options, &slot->http_request);
  if (status != SALTS_OK) {
    crpc_slot_release(slot);
    goto done;
  }
  *out_request = slot->public_handle;

done:
  crpc_prepared_call_destroy(&prepared);
  impl->submit_active = false;
  return status;
}

int crpc_async_request_cancel(crpc_async_client *client, crpc_request request) {
  crpc_async_client_impl *impl = crpc_async_client_get(client);
  crpc_slot *slot;
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if ((impl->poll_active && !impl->callback_active) || impl->submit_active) return SALTS_EBUSY;
  slot = crpc_slot_find(impl, request);
  if (slot == NULL) return SALTS_ENOENT;
  if (slot->result_delivered || slot->cancel_requested || slot->deadline_expired)
    return SALTS_EALREADY;
  status = chttp_async_request_cancel(&impl->http, slot->http_request);
  if (status == SALTS_OK) slot->cancel_requested = true;
  return status;
}

static int crpc_expire_deadlines(crpc_async_client_impl *impl) {
  const uint64_t now_ms = salts_monotonic_ms();
  int first_status = SALTS_OK;
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    crpc_slot *slot = &impl->slots[index];
    int status;
    if (!slot->active || slot->result_delivered || slot->cancel_requested ||
        slot->deadline_at_ms == 0u || slot->deadline_at_ms > now_ms)
      continue;
    slot->deadline_expired = true;
    status = chttp_async_request_cancel(&impl->http, slot->http_request);
    if (status != SALTS_OK && status != SALTS_EALREADY && status != SALTS_ENOENT &&
        first_status == SALTS_OK)
      first_status = status;
  }
  return first_status;
}

static uint32_t crpc_poll_wait(const crpc_async_client_impl *impl, uint32_t timeout_ms) {
  const uint64_t now_ms = salts_monotonic_ms();
  uint64_t wait_ms = timeout_ms;
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    const crpc_slot *slot = &impl->slots[index];
    uint64_t remaining;
    if (!slot->active || slot->result_delivered || slot->cancel_requested ||
        slot->deadline_at_ms == 0u)
      continue;
    remaining = slot->deadline_at_ms > now_ms ? slot->deadline_at_ms - now_ms : 0u;
    if (remaining < wait_ms) wait_ms = remaining;
  }
  return wait_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)wait_ms;
}

int crpc_async_client_poll(crpc_async_client *client, uint32_t timeout_ms,
                           size_t *out_completions) {
  crpc_async_client_impl *impl = crpc_async_client_get(client);
  size_t http_completions = 0u;
  int deadline_status;
  int after_status;
  int status;
  if (impl == NULL || out_completions == NULL) return SALTS_EINVAL;
  *out_completions = 0u;
  if (impl->submit_active || impl->poll_active || impl->callback_active || impl->stop_active)
    return SALTS_EBUSY;
  if (impl->stopped) return SALTS_ESHUTDOWN;
  impl->poll_active = true;
  impl->completion_count = 0u;
  deadline_status = crpc_expire_deadlines(impl);
  status =
      chttp_async_client_poll(&impl->http, crpc_poll_wait(impl, timeout_ms), &http_completions);
  after_status = crpc_expire_deadlines(impl);
  if (deadline_status == SALTS_OK) deadline_status = after_status;
  *out_completions = impl->completion_count;
  impl->poll_active = false;
  if (status != SALTS_OK) return status;
  return deadline_status;
}

static bool crpc_has_active_requests(const crpc_async_client_impl *impl) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index)
    if (impl->slots[index].active) return true;
  return false;
}

int crpc_async_client_stop(crpc_async_client *client, uint32_t timeout_ms) {
  crpc_async_client_impl *impl = crpc_async_client_get(client);
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->submit_active || impl->poll_active || impl->callback_active) return SALTS_EBUSY;
  if (impl->stopped) return SALTS_OK;
  impl->admission_open = false;
  impl->stop_active = true;
  impl->completion_count = 0u;
  status = chttp_async_client_stop(&impl->http, timeout_ms);
  if (status == SALTS_OK) {
    impl->stopped = true;
    if (crpc_has_active_requests(impl)) status = SALTS_EPROTO;
  }
  return status;
}

int crpc_async_client_destroy(crpc_async_client *client) {
  crpc_async_client_impl *impl;
  int status;
  if (client == NULL) return SALTS_EINVAL;
  impl = crpc_async_client_get(client);
  if (impl == NULL) return SALTS_OK;
  if (impl->submit_active || impl->poll_active || impl->callback_active || !impl->stopped)
    return SALTS_EBUSY;
  status = chttp_async_client_destroy(&impl->http);
  if (status != SALTS_OK) return status;
  free(impl->slots);
  free(impl);
  client->impl = NULL;
  return SALTS_OK;
}
