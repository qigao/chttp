/*
 * Exact, bounded H2 connection-window pressure with Salts ServiceHost scopes.
 *
 * This complements (not substitutes for) CHttp's real MethodPlan/H2 tests:
 * response bodies here use the canonical CHttp asynchronous Source/Sink API,
 * because the existing scalar Add MethodPlan deliberately returns only 2/3
 * bytes. Each 48 KiB stream stays below the RFC initial per-stream 65,535
 * window, but two simultaneous responses exceed the 65,535 connection
 * receive window. Completing both thus requires connection WINDOW_UPDATE.
 *
 * One advanced H2 client, one physical connection slot, two request slots,
 * immutable typed Provider capability per generation, one H2 listener and
 * one Source cleanup per admitted response. No new Provider loader or Owner.
 */
#include "chttp_service_host_component.h"
#include "chttp_service_plugin_native.h"

#include <http_client/http.h>
#include <http_server/http.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/service_host.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef CHTTP_SERVICE_HOST_FLOW_CANCEL
#define CHTTP_SERVICE_HOST_FLOW_CANCEL 0
#endif

enum {
  FLOW_TEST_TIMEOUT_MS = 12000,
  FLOW_RESPONSE_BYTES = 48 * 1024,
  FLOW_SLOTS = 4
};
_Static_assert(FLOW_RESPONSE_BYTES < 65535, "each H2 stream must stay under default credit");
_Static_assert(2 * FLOW_RESPONSE_BYTES > 65535, "aggregate must exhaust H2 connection credit");

#define REQUIRE(expr) do { if (!(expr)) { \
  fprintf(stderr, "ServiceHost H2 connection-window %s:%d: %s\n", \
          __FILE__, __LINE__, #expr); return 1; \
} } while (0)

typedef struct flow_generation {
  salts_service_host_generation generation;
  salts_configurator_deployment deployments[1];
  salts_configurator_instance instances[1];
  salts_component_dependency dependencies[1];
  size_t activation_order[1];
  salts_service_host_module modules[1];
} flow_generation;

typedef struct flow_binding {
  uint64_t generation_id;
  chttp_service_component_operation operation;
  bool ready;
} flow_binding;

struct flow_server;
typedef struct flow_source {
  struct flow_server *owner;
  salts_service_host_scope scope; /* never moved while live */
  size_t read_bytes;
  size_t total_bytes;
  bool expected_cancel;
  unsigned char marker;
  bool busy;                      /* protected by flow_server.lock */
} flow_source;

typedef struct flow_server {
  cmeta_mutex_t lock;
  salts_service_host_runtime *runtime; /* borrowed, one Salts Owner */
  flow_binding bindings[2];            /* control-plane only, immutable after ready */
  flow_source sources[FLOW_SLOTS];     /* stable address & bounded capacity */
  _Atomic size_t admitted;
  _Atomic size_t source_terminals;
  _Atomic size_t released;
  _Atomic size_t canceled_terminals;
  _Atomic int failure;
} flow_server;

typedef struct flow_result {
  size_t received;
  size_t expected_bytes;
  size_t completions;
  int status;
  unsigned int response_code;
  unsigned int http_major;
  unsigned char expected;
  bool invalid;
  chttp_body_sink sink; /* caller-owned until completion */
} flow_result;

static native_io_backend_kind flow_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config flow_net(size_t connections) {
  const cnet_client_config config = {
    .backend = flow_backend(),
    .connection_capacity = connections,
    .command_capacity = 32u,
    .request_capacity = 8u,
    .completion_batch_capacity = 8u,
    .event_capacity = 32u,
    .max_send_bytes = 64u * 1024u,
    .receive_buffer_bytes = 4096u,
    .connect_timeout_ms = FLOW_TEST_TIMEOUT_MS,
    .read_timeout_ms = FLOW_TEST_TIMEOUT_MS,
    .write_timeout_ms = FLOW_TEST_TIMEOUT_MS
  };
  return config;
}

static chttp_server_config flow_server_config(void) {
  const chttp_server_config config = {
    .host = "127.0.0.1",
    .port = 0u,
    .backlog = 8u,
    .network = flow_net(4u),
    .route_capacity = CHTTP_SERVICE_HOST_FLOW_CANCEL ? 3u : 2u,
    .middleware_capacity = 2u,
    .max_route_middleware_count = 2u,
    .max_route_param_count = 4u,
    .max_route_param_bytes = 64u,
    .max_target_bytes = 256u,
    .max_header_count = 16u,
    .max_header_bytes = 4096u,
    .max_request_body_bytes = 512u,
    .max_response_header_count = 16u,
    .max_response_header_bytes = 4096u,
    .max_response_body_bytes = 64u * 1024u,
    .max_buffered_response_body_bytes = 4096u,
    .poll_slice_ms = 2u,
    .enable_http2 = 1,
    .h2_stream_capacity = 4u,
    .h2_input_buffer_bytes = 64u * 1024u,
    .h2_output_buffer_bytes = 64u * 1024u,
    .h2_hpack_dynamic_table_bytes = 4096u,
    .h2_max_settings_count = 16u
  };
  return config;
}

static chttp_client_config flow_client_config(void) {
  const chttp_client_config config = {
    .network = flow_net(1u),        /* exactly one physical H2 session slot */
    .request_capacity = 2u,        /* two multiplexed request slots */
    .max_start_line_bytes = 256u,
    .max_header_count = 16u,
    .max_header_bytes = 4096u,
    .max_request_body_bytes = 512u,
    .max_response_body_bytes = 64u * 1024u,
    .max_informational_responses = 2u,
    .h2_input_buffer_bytes = 64u * 1024u,
    .h2_hpack_dynamic_table_bytes = 4096u,
    .h2_max_settings_count = 16u
  };
  return config;
}

static int flow_build(
    flow_generation *g, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref plugin) {
  const salts_service_host_generation_storage storage = {
    g->deployments, 1u, g->instances, 1u, g->dependencies, 1u,
    g->activation_order, 1u, g->modules, 1u
  };
  const salts_service_host_source source = {
    .plugin = plugin, .export_id = "component-provider"
  };
  return salts_service_host_generation_build(
      &g->generation, id, registry, &storage, NULL, 0u, &source, 1u,
      NULL, 0u) == SALTS_SERVICE_HOST_OK ? SALTS_OK : SALTS_EINVAL;
}

/* Resolve immutable typed Provider operation on the exclusive control thread.
 * A request does not call CMeta projection, Component lookup or Plugin loader. */
static int flow_prepare(flow_server *state, size_t index, flow_generation *g) {
  salts_configurator_service target = {0};
  chttp_service_operation_provider provider =
      chttp_service_operation_provider_bind(NULL, NULL);
  flow_binding *binding;
  if (state == NULL || g == NULL || index >= 2u ||
      state->bindings[index].ready)
    return SALTS_EINVAL;
  binding = &state->bindings[index];
  binding->operation =
      (chttp_service_component_operation)CHTTP_SERVICE_COMPONENT_OPERATION_INIT;
  if (salts_configurator_context_find_service_from(
          &g->generation.components, "CHttpPlugin_Calc_Add",
          chttp_service_operation_provider_interface(),
          &target) != SALTS_COMPONENT_OK ||
      chttp_service_operation_provider_borrow_from_object(
          target.object, target.interfaces, &provider) != CMETA_OK ||
      !chttp_service_operation_provider_valid(&provider) ||
      !chttp_service_operation_provider_get_operation(
          &provider, &binding->operation) ||
      !chttp_service_component_operation_valid(&binding->operation))
    return SALTS_EINVAL;
  binding->generation_id = g->generation.id;
  binding->ready = true;
  return SALTS_OK;
}

static int flow_source_read(
    void *user, void *buffer, size_t capacity, size_t *out_size) {
  flow_source *source = (flow_source *)user;
  size_t length;
  if (source == NULL || buffer == NULL || out_size == NULL ||
      capacity == 0u || source->read_bytes > source->total_bytes)
    return SALTS_EINVAL;
  length = source->total_bytes - source->read_bytes;
  if (length > capacity) length = capacity;
  if (length > 1024u) length = 1024u; /* strict bounded C11 source callback */
  if (length != 0u) memset(buffer, source->marker, length);
  source->read_bytes += length;
  *out_size = length; /* length=0 means EOF */
  return SALTS_OK;
}

static void flow_source_done(void *user, int terminal) {
  flow_source *source = (flow_source *)user;
  flow_server *state;
  if (source == NULL || source->owner == NULL) return;
  state = source->owner;
  if (source->expected_cancel) {
    if (terminal != SALTS_ECANCELED ||
        source->read_bytes == 0u ||
        source->read_bytes >= source->total_bytes)
      atomic_fetch_add_explicit(&state->failure, 1, memory_order_relaxed);
    else
      atomic_fetch_add_explicit(
          &state->canceled_terminals, 1u, memory_order_release);
  } else if (terminal != SALTS_OK ||
             source->read_bytes != source->total_bytes) {
    atomic_fetch_add_explicit(&state->failure, 1, memory_order_relaxed);
  }
  if (salts_service_host_scope_release(&source->scope) !=
      SALTS_SERVICE_HOST_OK)
    atomic_fetch_add_explicit(&state->failure, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->released, 1u, memory_order_release);
  cmeta_mutex_lock(&state->lock);
  source->busy = false;
  cmeta_mutex_unlock(&state->lock);
  atomic_fetch_add_explicit(
      &state->source_terminals, 1u, memory_order_release);
}

/* The H2 Source cleanup (not HTTP handler return) releases the nonmoving Scope.
 * Generation N+1 can publish while an N Stream still has output in flight. */
static int flow_handler(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  flow_server *state = (flow_server *)user;
  flow_source *source = NULL;
  const flow_binding *binding = NULL;
  AddRequest_t args = {0};
  AddResponse_t result = {0};
  void *params[2];
  int native_status = -1;
  uint64_t id;
  int status;
  chttp_body_source body;
  bool short_body, cancel_body;

  if (state == NULL || request == NULL || request->path == NULL ||
      response == NULL) return SALTS_EINVAL;
  cmeta_mutex_lock(&state->lock);
  for (size_t i = 0u; i < FLOW_SLOTS; ++i) {
    if (!state->sources[i].busy) {
      source = &state->sources[i];
      source->busy = true;
      source->owner = state;
      break;
    }
  }
  cmeta_mutex_unlock(&state->lock);
  if (source == NULL)
    return chttp_server_reply(response, 503u, "text/plain",
                              "Service Unavailable", sizeof("Service Unavailable") - 1u);

  if (salts_service_host_scope_acquire(state->runtime, &source->scope) !=
      SALTS_SERVICE_HOST_OK) {
    cmeta_mutex_lock(&state->lock);
    source->busy = false;
    cmeta_mutex_unlock(&state->lock);
    return chttp_server_reply(response, 503u, "text/plain",
                              "Service Unavailable", sizeof("Service Unavailable") - 1u);
  }

  id = salts_service_host_scope_generation_id(&source->scope);
  cmeta_mutex_lock(&state->lock);
  for (size_t i = 0u; i < 2u; ++i) {
    if (state->bindings[i].ready && state->bindings[i].generation_id == id) {
      binding = &state->bindings[i];
      break;
    }
  }
  cmeta_mutex_unlock(&state->lock);
  if (binding == NULL) goto reject;

  args.left = 3u;
  args.right = 4u;
  args.scale = 2u;
  params[0] = &args;
  params[1] = &result;
  if (!binding->operation.execution.invoke(
          binding->operation.execution.context,
          &native_status, params, 2u) || native_status != 0 ||
      (result.sum != 11u && result.sum != 111u))
    goto reject;

  short_body = strcmp(request->path, "/flow-short") == 0;
  cancel_body = strcmp(request->path, "/flow-cancel") == 0;
  source->marker = result.sum == 11u ? 'a' : 'b';
  source->read_bytes = 0u;
  source->total_bytes = short_body ? 3u : FLOW_RESPONSE_BYTES;
  source->expected_cancel = cancel_body;
  body = (chttp_body_source) {
    .read = flow_source_read,
    .user = source,
    .content_length = source->total_bytes,
    .content_length_known = 1
  };
  status = chttp_server_response_source_with_cleanup(
      response, 200u, "application/octet-stream",
      &body, flow_source_done, source);
  if (status != SALTS_OK) goto reject;
  atomic_fetch_add_explicit(&state->admitted, 1u, memory_order_release);
  return SALTS_OK;

reject:
  if (salts_service_host_scope_release(&source->scope) !=
      SALTS_SERVICE_HOST_OK)
    atomic_fetch_add_explicit(&state->failure, 1, memory_order_relaxed);
  cmeta_mutex_lock(&state->lock);
  source->busy = false;
  cmeta_mutex_unlock(&state->lock);
  return chttp_server_reply(
      response, 500u, "text/plain",
      "Internal Server Error", sizeof("Internal Server Error") - 1u);
}

static int flow_sink_write(void *user, const void *data, size_t size) {
  flow_result *result = (flow_result *)user;
  const unsigned char *bytes = (const unsigned char *)data;
  if (result == NULL || (size != 0u && data == NULL) ||
      result->received > result->expected_bytes ||
      size > result->expected_bytes - result->received)
    return SALTS_EMSGSIZE;
  for (size_t i = 0u; i < size; ++i)
    if (bytes[i] != result->expected)
      result->invalid = true;
  result->received += size;
  return result->invalid ? SALTS_EPROTO : SALTS_OK;
}

static void flow_complete(
    void *user, chttp_request request,
    const chttp_response_view *view, const chttp_error *error) {
  flow_result *result = (flow_result *)user;
  (void)request;
  if (result == NULL) return;
  result->completions++;
  result->status = error == NULL ? SALTS_OK : error->status;
  if (view != NULL) {
    result->http_major = view->http_major;
    result->response_code = view->status_code;
    if (view->body != NULL) result->invalid = true;
  }
}

static int flow_submit_target(
    chttp_async_client *client, const char *uri,
    flow_result *result, unsigned char expected,
    const char *target, size_t expected_bytes,
    chttp_request *out_request) {
  chttp_request_options options;
  if (client == NULL || uri == NULL || result == NULL ||
      target == NULL || expected_bytes == 0u ||
      expected_bytes > FLOW_RESPONSE_BYTES || out_request == NULL)
    return SALTS_EINVAL;
  *result = (flow_result){0};
  result->expected = expected;
  result->expected_bytes = expected_bytes;
  result->sink = (chttp_body_sink){flow_sink_write, result};
  options = (chttp_request_options){
    .connection_uri = uri,
    .authority = "127.0.0.1",
    .target = target,
    .method = CHTTP_METHOD_GET,
    .body_sink = &result->sink,
    .on_complete = flow_complete,
    .user = result,
    .protocol = CHTTP_HTTP_2
  };
  return chttp_async_client_submit(client, &options, out_request);
}

static int flow_submit(
    chttp_async_client *client, const char *uri,
    flow_result *result, unsigned char expected,
    chttp_request *out_request) {
  return flow_submit_target(client, uri, result, expected,
                            "/flow", FLOW_RESPONSE_BYTES, out_request);
}

static bool flow_wait(
    chttp_async_client *client,
    const flow_result *a, const flow_result *b) {
  const uint64_t deadline = cmeta_monotonic_ms() + FLOW_TEST_TIMEOUT_MS;
  while (cmeta_monotonic_ms() < deadline) {
    size_t completions = 0u;
    if (a->completions == 1u && (b == NULL || b->completions == 1u))
      return true;
    if (chttp_async_client_poll(client, 5u, &completions) != SALTS_OK)
      return false;
  }
  return false;
}

static bool flow_wait_source_terminals(
    const flow_server *state, size_t count) {
  const uint64_t deadline = cmeta_monotonic_ms() + FLOW_TEST_TIMEOUT_MS;
  while (cmeta_monotonic_ms() < deadline) {
    if (atomic_load_explicit(
            &state->source_terminals, memory_order_acquire) >= count)
      return true;
    cmeta_thread_yield();
  }
  return false;
}

static bool flow_result_valid(const flow_result *result) {
  return result != NULL && result->completions == 1u &&
      result->status == SALTS_OK && result->http_major == 2u &&
      result->response_code == 200u &&
      result->received == result->expected_bytes && !result->invalid;
}

int main(void) {
  const cmeta_plugin_registry_config registry_config = {.capacity = 2u};
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_ref old_plugin = {0}, new_plugin = {0};
  flow_generation old_generation = {0}, new_generation = {0};
  salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
  salts_service_host_generation *previous = NULL;
  flow_server state = {0};
  chttp_server server = {0};
  chttp_async_client client = {0};
  const chttp_server_config server_config = flow_server_config();
  const chttp_client_config client_config = flow_client_config();
  const chttp_server_execution_options two_owners = {
    sizeof(chttp_server_execution_options),
    CHTTP_SERVER_EXECUTION_OPTIONS_VERSION, 2u
  };
  const chttp_h2_receive_window_policy credit = {
    sizeof(chttp_h2_receive_window_policy), UINT32_C(65535), UINT32_C(65535)
  };
  flow_result warm = {0}, first = {0}, second = {0};
  flow_result third = {0}, fourth = {0};
  chttp_request handle = {0}, a = {0}, b = {0};
  uint16_t port = 0u;
  char uri[64] = {0};
  bool quiescent = false;

  REQUIRE(cmeta_plugin_registry_init(&registry, &registry_config) == CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_load(&registry,
          GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH, &old_plugin) == CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_start(&registry, old_plugin) == CMETA_PLUGIN_OK);
  REQUIRE(flow_build(&old_generation, UINT64_C(1), &registry, old_plugin) == SALTS_OK);
  REQUIRE(salts_service_host_runtime_init(&runtime) == SALTS_SERVICE_HOST_OK);

  state.runtime = &runtime;
  cmeta_mutex_init(&state.lock);
  REQUIRE(state.lock != NULL);
  REQUIRE(flow_prepare(&state, 0u, &old_generation) == SALTS_OK);
  REQUIRE(salts_service_host_runtime_publish(
          &runtime, &old_generation.generation, &previous) == SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == NULL);

  REQUIRE(chttp_server_init(&server, &server_config) == SALTS_OK);
  REQUIRE(chttp_server_set_execution_options(&server, &two_owners) == SALTS_OK);
  REQUIRE(chttp_server_get(&server, "/flow", flow_handler, &state) == SALTS_OK);
#if CHTTP_SERVICE_HOST_FLOW_CANCEL
  REQUIRE(chttp_server_get(&server, "/flow-cancel", flow_handler, &state) == SALTS_OK);
  REQUIRE(chttp_server_get(&server, "/flow-short", flow_handler, &state) == SALTS_OK);
#endif
  REQUIRE(chttp_server_start(&server) == SALTS_OK);
  REQUIRE(chttp_server_port(&server, &port) == SALTS_OK);
  REQUIRE(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                   (unsigned int)port) > 0);

  REQUIRE(chttp_async_client_init(&client, &client_config) == SALTS_OK);
  REQUIRE(chttp_async_client_set_h2_receive_window_policy(
          &client, &credit) == SALTS_OK);

  /* Warm a verified H2 session (SETTINGS exchanged) before multiplexing.
   * This is a bounded, known-length streamed response, not a large buffer. */
  REQUIRE(flow_submit(&client, uri, &warm, 'a', &handle) == SALTS_OK);
  REQUIRE(flow_wait(&client, &warm, NULL));
  REQUIRE(flow_result_valid(&warm));
  REQUIRE(flow_wait_source_terminals(&state, 1u));

  /* Both frames use the same Client, origin, protocol and conn-capacity 1.
   * Two simultaneous 48KiB streams cumulatively exceed 65,535 bytes, but
   * neither stream separately exhausts its initial 65,535 credit. */
  REQUIRE(flow_submit(&client, uri, &first, 'a', &a) == SALTS_OK);
  REQUIRE(flow_submit(&client, uri, &second, 'a', &b) == SALTS_OK);
  REQUIRE(first.completions == 0u && second.completions == 0u);
  REQUIRE(flow_wait(&client, &first, &second));
  REQUIRE(flow_result_valid(&first));
  REQUIRE(flow_result_valid(&second));
  REQUIRE(flow_wait_source_terminals(&state, 3u));
  REQUIRE(atomic_load_explicit(&state.failure, memory_order_acquire) == 0);

  /* Generation cutover remains the original Salts runtime's decision. A
   * pre-admitted new typed binding is immutable before publish, and the
   * retired old generation must release both streaming source Scopes. */
  REQUIRE(cmeta_plugin_registry_load(&registry,
          GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH, &new_plugin) == CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_start(&registry, new_plugin) == CMETA_PLUGIN_OK);
  REQUIRE(flow_build(&new_generation, UINT64_C(2), &registry, new_plugin) == SALTS_OK);
  REQUIRE(flow_prepare(&state, 1u, &new_generation) == SALTS_OK);
  REQUIRE(salts_service_host_runtime_publish(
          &runtime, &new_generation.generation, &previous) == SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == &old_generation.generation);
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &old_generation.generation) == SALTS_SERVICE_HOST_OK);

  REQUIRE(flow_submit(&client, uri, &third, 'b', &a) == SALTS_OK);
  REQUIRE(flow_submit(&client, uri, &fourth, 'b', &b) == SALTS_OK);
  REQUIRE(flow_wait(&client, &third, &fourth));
  REQUIRE(flow_result_valid(&third));
  REQUIRE(flow_result_valid(&fourth));
  REQUIRE(flow_wait_source_terminals(&state, 5u));
  REQUIRE(atomic_load_explicit(&state.failure, memory_order_acquire) == 0);


#if CHTTP_SERVICE_HOST_FLOW_CANCEL
  /*
   * Each Stream starts with only one byte of receive credit; body_sink is
   * called on the caller-driven client poll thread. Do not block either HTTP
   * Owner or the CFlow worker. Cancel a Source only after at least one byte
   * has reached the sink and before its full 48-KiB terminal.
   *
   * Both calls belong to one advanced H2 client with physical connection
   * capacity=1 and request capacity=2. The unaffected sibling has a 3-byte
   * Source so that it can drain under the same tiny stream credit.
   */
  chttp_async_client cancel_client = {0};
  const chttp_client_config cancel_config = flow_client_config();
  const chttp_h2_receive_window_policy tiny_credit = {
      sizeof(chttp_h2_receive_window_policy), UINT32_C(1), UINT32_C(65535)
  };
  flow_result canceled = {0}, sibling = {0};
  chttp_request canceled_req = {0}, sibling_req = {0};
  REQUIRE(chttp_async_client_init(&cancel_client, &cancel_config) == SALTS_OK);
  REQUIRE(chttp_async_client_set_h2_receive_window_policy(
          &cancel_client, &tiny_credit) == SALTS_OK);
  REQUIRE(flow_submit_target(
          &cancel_client, uri, &canceled, 'b',
          "/flow-cancel", FLOW_RESPONSE_BYTES, &canceled_req) == SALTS_OK);

  const uint64_t first_byte_deadline =
      cmeta_monotonic_ms() + FLOW_TEST_TIMEOUT_MS;
  while (canceled.received == 0u && canceled.completions == 0u &&
         cmeta_monotonic_ms() < first_byte_deadline) {
    size_t polled = 0u;
    REQUIRE(chttp_async_client_poll(
            &cancel_client, 5u, &polled) == SALTS_OK);
  }
  REQUIRE(canceled.received > 0u);
  REQUIRE(canceled.received < FLOW_RESPONSE_BYTES);
  REQUIRE(canceled.completions == 0u);
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &new_generation.generation) == SALTS_SERVICE_HOST_BUSY);

  REQUIRE(flow_submit_target(
          &cancel_client, uri, &sibling, 'b',
          "/flow-short", 3u, &sibling_req) == SALTS_OK);
  REQUIRE(chttp_async_request_cancel(&cancel_client, canceled_req) == SALTS_OK);
  REQUIRE(flow_wait(&cancel_client, &canceled, &sibling));
  REQUIRE(canceled.completions == 1u && canceled.status == SALTS_ECANCELED);
  REQUIRE(canceled.received > 0u && canceled.received < FLOW_RESPONSE_BYTES);
  REQUIRE(flow_result_valid(&sibling));
  REQUIRE(chttp_async_request_cancel(&cancel_client, canceled_req) == SALTS_ENOENT);
  REQUIRE(flow_wait_source_terminals(&state, 7u));
  REQUIRE(atomic_load_explicit(
          &state.canceled_terminals, memory_order_acquire) == 1u);
  REQUIRE(atomic_load_explicit(&state.failure, memory_order_acquire) == 0);
  REQUIRE(chttp_async_client_stop(
          &cancel_client, FLOW_TEST_TIMEOUT_MS) == SALTS_OK);
  REQUIRE(chttp_async_client_destroy(&cancel_client) == SALTS_OK);
#endif

  REQUIRE(chttp_async_client_stop(&client, FLOW_TEST_TIMEOUT_MS) == SALTS_OK);
  REQUIRE(chttp_async_client_destroy(&client) == SALTS_OK);
  REQUIRE(salts_service_host_runtime_close(&runtime, &previous) ==
          SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == &new_generation.generation);
  REQUIRE(chttp_server_stop(&server, FLOW_TEST_TIMEOUT_MS) == SALTS_OK);
  REQUIRE(chttp_server_destroy(&server) == SALTS_OK);
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &new_generation.generation) == SALTS_SERVICE_HOST_OK);
  REQUIRE(salts_service_host_runtime_destroy(&runtime) == SALTS_SERVICE_HOST_OK);

  const size_t expected_terminal_count =
      CHTTP_SERVICE_HOST_FLOW_CANCEL ? 7u : 5u;
  REQUIRE(atomic_load_explicit(&state.admitted, memory_order_acquire)
          == expected_terminal_count);
  REQUIRE(atomic_load_explicit(&state.source_terminals, memory_order_acquire)
          == expected_terminal_count);
  REQUIRE(atomic_load_explicit(&state.released, memory_order_acquire)
          == expected_terminal_count);
  cmeta_mutex_destroy(&state.lock);

  REQUIRE(cmeta_plugin_registry_request_stop(&registry, old_plugin) == CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_poll_quiescent(
          &registry, old_plugin, &quiescent) == CMETA_PLUGIN_OK);
  REQUIRE(quiescent);
  REQUIRE(cmeta_plugin_registry_unload(&registry, old_plugin) == CMETA_PLUGIN_OK);
  quiescent = false;
  REQUIRE(cmeta_plugin_registry_request_stop(&registry, new_plugin) == CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_poll_quiescent(
          &registry, new_plugin, &quiescent) == CMETA_PLUGIN_OK);
  REQUIRE(quiescent);
  REQUIRE(cmeta_plugin_registry_unload(&registry, new_plugin) == CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);

  puts(CHTTP_SERVICE_HOST_FLOW_CANCEL
      ? "ServiceHost H2 stream cancel: 48KiB tiny-window RST, sibling survives, 7 Scope terminals PASS"
      : "ServiceHost H2 connection WINDOW_UPDATE: 2 x 48KiB, two generations, 5 Scope terminals PASS");
  return 0;
}
