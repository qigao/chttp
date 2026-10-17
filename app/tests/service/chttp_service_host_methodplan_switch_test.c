/* Candidate-only real MethodPlan/ServiceHost N->N+1 on one HTTP Listener.
 * No custom HTTP execution path: CHttp::App retains DataBind ingress/egress,
 * deferred native execution, Interceptor and per-call finalizer authority. */
#include "chttp_service_host_integration.h"
#include "chttp_service_plugin.http.h"
#include "chttp_service_plugin_native.h"
#include "chttp_service_plugin.service_native.h"
#include <http_client/http.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef CHTTP_SERVICE_HOST_TEST_H2
#define CHTTP_SERVICE_HOST_TEST_H2 0
#endif
#ifndef CHTTP_SERVICE_HOST_TEST_MULTIPLEX
#define CHTTP_SERVICE_HOST_TEST_MULTIPLEX 0
#endif
#ifndef CHTTP_SERVICE_HOST_TEST_WINDOW
#define CHTTP_SERVICE_HOST_TEST_WINDOW 0
#endif
#if CHTTP_SERVICE_HOST_TEST_WINDOW && !CHTTP_SERVICE_HOST_TEST_MULTIPLEX
#error "Stream-window conformance needs the multiplex test"
#endif
#if CHTTP_SERVICE_HOST_TEST_MULTIPLEX && !CHTTP_SERVICE_HOST_TEST_H2
#error "The multiplex conformance requires explicit h2c"
#endif

enum { HOST_TEST_TIMEOUT_MS = 10000 };
#define REQUIRE(x) do { if (!(x)) { \
  fprintf(stderr, "MethodPlan ServiceHost switch: %s:%d: %s\n", \
          __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct host_generation_fixture {
  salts_service_host_generation generation;
  salts_configurator_deployment deployments[1];
  salts_configurator_instance instances[1];
  salts_component_dependency dependencies[1];
  size_t activation_order[1];
  salts_service_host_module modules[1];
} host_generation_fixture;

typedef struct host_worker_gate {
  _Atomic int started;
  _Atomic int release;
} host_worker_gate;

typedef struct host_middleware_counts {
  _Atomic size_t entered;
  _Atomic size_t returned;
} host_middleware_counts;

typedef struct host_client_call {
  chttp_client_config config;
  char uri[64];
  char body[32];
  size_t body_size;
  unsigned int http_status;
  int status;
  _Atomic int completed;
} host_client_call;

static native_io_backend_kind host_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config host_network(size_t capacity) {
  const cnet_client_config config = {
    .backend = host_backend(),
    .connection_capacity = capacity,
    .command_capacity = 16u,
    .request_capacity = 8u,
    .completion_batch_capacity = 8u,
    .event_capacity = 16u,
    .max_send_bytes = CHTTP_SERVICE_HOST_TEST_H2 ? 64u * 1024u : 4096u,
    .receive_buffer_bytes = CHTTP_SERVICE_HOST_TEST_H2 ? 4096u : 256u,
    .connect_timeout_ms = HOST_TEST_TIMEOUT_MS,
    .read_timeout_ms = HOST_TEST_TIMEOUT_MS,
    .write_timeout_ms = HOST_TEST_TIMEOUT_MS
  };
  return config;
}

static chttp_server_config host_server_config(void) {
  const chttp_server_config config = {
    .host = "127.0.0.1",
    .port = 0u,
    .backlog = 8u,
    .network = host_network(4u),
    .route_capacity = 2u,
    .middleware_capacity = 2u,
    .max_route_middleware_count = 2u,
    .max_route_param_count = 4u,
    .max_route_param_bytes = 128u,
    .max_target_bytes = 256u,
    .max_header_count = 16u,
    .max_header_bytes = 1024u,
    .max_request_body_bytes = 512u,
    .max_response_header_count = 16u,
    .max_response_header_bytes = 1024u,
    .max_response_body_bytes = 512u,
    .poll_slice_ms = 2u,
    .enable_http2 = CHTTP_SERVICE_HOST_TEST_H2,
    .h2_stream_capacity = CHTTP_SERVICE_HOST_TEST_H2 ? 8u : 0u,
    .h2_input_buffer_bytes = CHTTP_SERVICE_HOST_TEST_H2 ? 64u * 1024u : 0u,
    .h2_output_buffer_bytes = CHTTP_SERVICE_HOST_TEST_H2 ? 64u * 1024u : 0u,
    .h2_hpack_dynamic_table_bytes = CHTTP_SERVICE_HOST_TEST_H2 ? 4096u : 0u,
    .h2_max_settings_count = CHTTP_SERVICE_HOST_TEST_H2 ? 16u : 0u
  };
  return config;
}

static chttp_client_config host_client_config(void) {
  const chttp_client_config config = {
    .network = host_network(2u),
    .request_capacity = 1u,
    .max_start_line_bytes = 256u,
    .max_header_count = 16u,
    .max_header_bytes = 1024u,
    .max_request_body_bytes = 512u,
    .max_response_body_bytes = 512u,
    .max_informational_responses = 2u,
    .h2_input_buffer_bytes = CHTTP_SERVICE_HOST_TEST_H2 ? 64u * 1024u : 0u,
    .h2_hpack_dynamic_table_bytes = CHTTP_SERVICE_HOST_TEST_H2 ? 4096u : 0u,
    .h2_max_settings_count = CHTTP_SERVICE_HOST_TEST_H2 ? 16u : 0u
  };
  return config;
}

static int host_build(
    host_generation_fixture *fixture, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
  const salts_service_host_generation_storage storage = {
    fixture->deployments, 1u, fixture->instances, 1u,
    fixture->dependencies, 1u, fixture->activation_order, 1u,
    fixture->modules, 1u
  };
  const salts_service_host_source source = {
    .plugin = ref, .export_id = "component-provider"
  };
  return salts_service_host_generation_build(
      &fixture->generation, id, registry, &storage,
      NULL, 0u, &source, 1u, NULL, 0u) == SALTS_SERVICE_HOST_OK
      ? 0 : -1;
}

static void host_gate_task(void *user) {
  host_worker_gate *gate = (host_worker_gate *)user;
  if (gate == NULL) return;
  atomic_store_explicit(&gate->started, 1, memory_order_release);
  /* Bound the failure to avoid an infinite worker hang even if an assertion
   * fails on the control thread. No polling or sleep is used as test ordering. */
  const uint64_t deadline = cmeta_monotonic_ms() + HOST_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(&gate->release, memory_order_acquire) == 0 &&
         cmeta_monotonic_ms() < deadline)
    cmeta_thread_yield();
}

static int host_interceptor(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  host_middleware_counts *counts = (host_middleware_counts *)user;
  (void)request;
  (void)response;
  if (counts == NULL || next == NULL) return SALTS_EINVAL;
  atomic_fetch_add_explicit(&counts->entered, 1u, memory_order_relaxed);
  const int status = chttp_server_next_call(next);
  atomic_fetch_add_explicit(&counts->returned, 1u, memory_order_relaxed);
  return status;
}

static int host_http_get(
    chttp_client *client, const char *uri,
    unsigned int *out_code, char *body, size_t *body_size) {
  const chttp_options options = {
    .connection_uri = uri, .authority = "127.0.0.1",
    .target = "/plugin/3?right=4&scale=2", .timeout_ms = HOST_TEST_TIMEOUT_MS,
    .protocol = CHTTP_SERVICE_HOST_TEST_H2 ? CHTTP_HTTP_2 : CHTTP_HTTP_1_1
  };
  chttp_response response = {0};
  chttp_error error = {0};
  const int status = chttp_get(client, &options, &response, &error);
  if (status == SALTS_OK) {
    /* Explicit H2c must never silently retry as H1; H1 fixture also guards
     * unintended wire-protocol switching. */
    if (response.http_major != (CHTTP_SERVICE_HOST_TEST_H2 ? 2u : 1u)) {
      chttp_response_destroy(&response);
      return SALTS_EPROTO;
    }
    *out_code = response.status_code;
    if (response.body_size >= 32u) {
      chttp_response_destroy(&response);
      return SALTS_EMSGSIZE;
    }
    if (response.body_size != 0u)
      memcpy(body, response.body, response.body_size);
    body[response.body_size] = '\0';
    *body_size = response.body_size;
  }
  chttp_response_destroy(&response);
  return status;
}

static void host_client_thread(void *user) {
  host_client_call *call = (host_client_call *)user;
  chttp_client client = {0};
  if (call == NULL) return;
  call->status = chttp_client_init(&client, &call->config);
  if (call->status == SALTS_OK) {
    call->status = host_http_get(
        &client, call->uri, &call->http_status,
        call->body, &call->body_size);
    if (chttp_client_destroy(&client, HOST_TEST_TIMEOUT_MS) != SALTS_OK &&
        call->status == SALTS_OK)
      call->status = SALTS_EIO;
  }
  atomic_store_explicit(&call->completed, 1, memory_order_release);
}

static bool host_pending_at_least(
    cflow_executor *executor, size_t expected) {
  const uint64_t deadline = cmeta_monotonic_ms() + HOST_TEST_TIMEOUT_MS;
  do {
    cflow_executor_stats stats = {0};
    if (!cflow_executor_get_stats(executor, &stats))
      return false;
    if (stats.pending >= expected)
      return true;
    cmeta_thread_yield();
  } while (cmeta_monotonic_ms() < deadline);
  return false;
}


#if CHTTP_SERVICE_HOST_TEST_MULTIPLEX
/* An advanced H2 owner is the only client of its transport. Hard physical
 * connection bound=1, HTTP request/stream slots=2, fixed origin and protocol.
 * The test verifies two simultaneously admitted streams on that one H2
 * session; a third request must fail ENOBUFS before cancellation recycles one. */
typedef struct host_h2_result {
  size_t completions;
  int status;
  unsigned int http_major;
  unsigned int response_status;
  char body[32];
  size_t body_size;
  chttp_request request;
} host_h2_result;

static void host_h2_complete(
    void *user, chttp_request request,
    const chttp_response_view *view, const chttp_error *error) {
  host_h2_result *result = (host_h2_result *)user;
  if (result == NULL) return;
  result->completions++;
  result->request = request;
  result->status = error != NULL ? error->status : SALTS_OK;
  if (view == NULL) return;
  result->http_major = view->http_major;
  result->response_status = view->status_code;
  if (view->body_size >= sizeof(result->body)) {
    result->status = SALTS_EMSGSIZE;
    return;
  }
  if (view->body_size != 0u && view->body != NULL)
    memcpy(result->body, view->body, view->body_size);
  result->body[view->body_size] = '\0';
  result->body_size = view->body_size;
}

/* Test-only explicit bounded progress; the CHttp client is caller-driven.
 * No background implicit poller or retry into a different connection. */
static bool host_h2_poll_until_pending(
    chttp_async_client *client, cflow_executor *executor,
    size_t expected) {
  const uint64_t deadline = cmeta_monotonic_ms() + HOST_TEST_TIMEOUT_MS;
  while (cmeta_monotonic_ms() < deadline) {
    cflow_executor_stats stats = {0};
    size_t completions = 0u;
    if (!cflow_executor_get_stats(executor, &stats))
      return false;
    if (stats.pending >= expected)
      return true;
    if (chttp_async_client_poll(client, 5u, &completions) != SALTS_OK)
      return false;
  }
  return false;
}

static bool host_h2_poll_until_completions(
    chttp_async_client *client,
    const host_h2_result *a, const host_h2_result *b,
    const host_h2_result *c) {
  const uint64_t deadline = cmeta_monotonic_ms() + HOST_TEST_TIMEOUT_MS;
  while (cmeta_monotonic_ms() < deadline) {
    size_t completions = 0u;
    if ((a == NULL || a->completions == 1u) &&
        (b == NULL || b->completions == 1u) &&
        (c == NULL || c->completions == 1u))
      return true;
    if (chttp_async_client_poll(client, 5u, &completions) != SALTS_OK)
      return false;
  }
  return false;
}
#endif

int main(void) {
  const cmeta_plugin_registry_config registry_config = {2u};
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_ref plugin_n = {0}, plugin_next = {0};
  host_generation_fixture old_generation = {0};
  host_generation_fixture new_generation = {0};
  host_generation_fixture overflow_generation = {0};
#if CHTTP_SERVICE_HOST_TEST_MULTIPLEX
  host_generation_fixture multiplex_generation = {0};
#endif
  salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
  salts_service_host_generation *previous = NULL;
  salts_service_host_generation *last_generation = &overflow_generation.generation;
  cflow_executor executor = {0};
  host_worker_gate gate = {0};
  host_middleware_counts counts = {0};
  cflow_executor_task gate_task = {0};
  chttp_service service = {0};
  chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
  chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
  chttp_server server = {0};
  chttp_client client = {0};
  cmeta_thread_t n_thread = NULL, next_thread = NULL;
  host_client_call n_request = {0}, next_request = {0};
  const chttp_server_execution_options two_owners = {
    sizeof(chttp_server_execution_options),
    CHTTP_SERVER_EXECUTION_OPTIONS_VERSION, 2u
  };
  chttp_server_config server_config = host_server_config();
  const chttp_client_config client_config = host_client_config();
  const chttp_server_middleware route_middleware = {
    host_interceptor, &counts
  };
  DataBind *contract = NULL;
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  DataBindNativeTypeBinding request_native =
      DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
  DataBindNativeTypeBinding response_native =
      DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
  DataBindServiceNativeBinding native =
      DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
  const DataBindHttpProjectionConfig *projection = NULL;
  DataBindHttpMethodPlan *method_plan = NULL;
  uint16_t port = 0u;
  char uri[64] = {0}, body[32] = {0};
  size_t body_size = 0u;
  unsigned int code = 0u;
  bool quiet = false;

  REQUIRE(CHttpPlugin_codec_create(&contract, &bind_error) == DATA_BIND_OK);
  REQUIRE(databind_11_CHttpPlugin_4_Calc_3_Add__databind_native_binding(
          &request_native, &response_native, &native, &bind_error)
          == DATA_BIND_OK);
  projection = data_bind_http_projection_artifact_find(
      &databind_chttp_service_plugin_http_projection, "Calc", "Add");
  REQUIRE(projection != NULL);
  REQUIRE(data_bind_http_method_plan_compile_service(
      contract, "Calc", "Add", projection, &native,
      &method_plan, &diagnostic) == DATA_BIND_OK);
  REQUIRE(method_plan != NULL);

  REQUIRE(cmeta_plugin_registry_init(&registry, &registry_config) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_load(
          &registry, GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH, &plugin_n)
          == CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_start(&registry, plugin_n) == CMETA_PLUGIN_OK);
  REQUIRE(host_build(&old_generation, UINT64_C(1), &registry, plugin_n) == 0);
  REQUIRE(salts_service_host_runtime_init(&runtime) == SALTS_SERVICE_HOST_OK);
  REQUIRE(cflow_executor_worker_init_with_capacity(&executor, 1u, 8u));

  service_config.method_capacity = 1u;
  service_config.max_binding_value_bytes = 64u;
  service_config.max_response_body_bytes = 8u;
  service_config.max_call_frame_bytes = 512u;
  service_config.native_workspace_bytes = 4096u;
  service_config.native_max_depth = 16u;
  service_config.native_max_items = 64u;
  service_config.native_max_owned_bytes = 1024u;
  REQUIRE(chttp_service_init(&service, &service_config) == SALTS_OK);
  REQUIRE(chttp_server_init(&server, &server_config) == SALTS_OK);
  REQUIRE(chttp_server_set_execution_options(
          &server, &two_owners) == SALTS_OK);

  mount.method_plan = method_plan;
  mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
  mount.executor = &executor;
  mount.middleware = &route_middleware;
  mount.middleware_count = 1u;
  REQUIRE(chttp_service_mount_host_switch(
          &service, &server, &mount, &runtime, "CHttpPlugin_Calc_Add")
          == SALTS_OK);
  /* Fail-closed controls: only real BUILT generations are admitted. */
  REQUIRE(chttp_service_host_switch_prepare(&service, NULL) ==
          SALTS_EINVAL);
  REQUIRE(chttp_service_host_switch_prepare(
          &service, &old_generation.generation) == SALTS_OK);
  REQUIRE(chttp_service_host_switch_prepare(
          &service, &old_generation.generation) == SALTS_EALREADY);
  REQUIRE(salts_service_host_runtime_publish(
          &runtime, &old_generation.generation, &previous)
          == SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == NULL);
  REQUIRE(chttp_server_start(&server) == SALTS_OK);
  REQUIRE(chttp_server_port(&server, &port) == SALTS_OK);
  REQUIRE(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                   (unsigned int)port) > 0);
  REQUIRE(chttp_client_init(&client, &client_config) == SALTS_OK);

  gate_task = (cflow_executor_task){.run = host_gate_task, .user = &gate};
  REQUIRE(cflow_executor_try_post_task(&executor, &gate_task) ==
          CFLOW_ADMISSION_ACCEPTED);
  const uint64_t deadline = cmeta_monotonic_ms() + HOST_TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(&gate.started, memory_order_acquire) &&
         cmeta_monotonic_ms() < deadline)
    cmeta_thread_yield();
  REQUIRE(atomic_load_explicit(&gate.started, memory_order_acquire));

  n_request.config = host_client_config();
  REQUIRE(snprintf(n_request.uri, sizeof(n_request.uri), "%s", uri) > 0);
  REQUIRE(cmeta_thread_create(&n_thread, host_client_thread, &n_request)
          == SALTS_OK);
  REQUIRE(host_pending_at_least(&executor, 2u));

  REQUIRE(cmeta_plugin_registry_load(
          &registry, GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH, &plugin_next)
          == CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_start(&registry, plugin_next) == CMETA_PLUGIN_OK);
  REQUIRE(host_build(&new_generation, UINT64_C(2), &registry, plugin_next) == 0);
  REQUIRE(chttp_service_host_switch_prepare(
          &service, &new_generation.generation) == SALTS_OK);
  REQUIRE(salts_service_host_runtime_publish(
          &runtime, &new_generation.generation, &previous)
          == SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == &old_generation.generation);
  /* The candidate route holds two immutable pre-admitted generation slots.
   * A third BUILT graph cannot be silently ranked or spilled to another
   * runtime/method; test rollback returns its extra Provider lease. */
  REQUIRE(host_build(&overflow_generation, UINT64_C(3), &registry, plugin_n) == 0);
  REQUIRE(chttp_service_host_switch_prepare(
          &service, &overflow_generation.generation) == SALTS_ENOBUFS);
  REQUIRE(salts_service_host_generation_discard(
          &overflow_generation.generation) == SALTS_SERVICE_HOST_OK);

  next_request.config = host_client_config();
  REQUIRE(snprintf(next_request.uri, sizeof(next_request.uri), "%s", uri) > 0);
  REQUIRE(cmeta_thread_create(&next_thread, host_client_thread, &next_request)
          == SALTS_OK);
  REQUIRE(host_pending_at_least(&executor, 3u));

  REQUIRE(chttp_service_destroy(&service) == SALTS_EBUSY);
  REQUIRE(chttp_service_host_switch_retire(
          &service, &old_generation.generation) == SALTS_EBUSY);
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &old_generation.generation) == SALTS_SERVICE_HOST_BUSY);
  REQUIRE(cmeta_plugin_registry_unload(&registry, plugin_n) ==
          CMETA_PLUGIN_BUSY);

  atomic_store_explicit(&gate.release, 1, memory_order_release);
  REQUIRE(cmeta_thread_join(&n_thread) == SALTS_OK);
  cmeta_thread_destroy(&n_thread);
  REQUIRE(cmeta_thread_join(&next_thread) == SALTS_OK);
  cmeta_thread_destroy(&next_thread);
  REQUIRE(n_request.status == SALTS_OK);
  REQUIRE(n_request.http_status == 201u);
  REQUIRE(n_request.body_size == 2u && strcmp(n_request.body, "11") == 0);
  REQUIRE(next_request.status == SALTS_OK);
  REQUIRE(next_request.http_status == 201u);
  REQUIRE(next_request.body_size == 3u && strcmp(next_request.body, "111") == 0);
  REQUIRE(cflow_executor_wait_idle(&executor));

  REQUIRE(salts_service_host_generation_drain(
          &runtime, &old_generation.generation) == SALTS_SERVICE_HOST_OK);
  REQUIRE(host_http_get(&client, uri, &code, body, &body_size) == SALTS_OK);
  REQUIRE(code == 201u);
  REQUIRE(body_size == 3u && strcmp(body, "111") == 0);
  REQUIRE(cflow_executor_wait_idle(&executor));

  /* Two candidate slots remained full until the original N truly drained.
   * Explicit retire now reclaims that slot without touching the live N+1. */
  REQUIRE(chttp_service_host_switch_retire(
          &service, &old_generation.generation) == SALTS_OK);
  REQUIRE(chttp_service_host_switch_retire(
          &service, &old_generation.generation) == SALTS_EINVAL);
  REQUIRE(host_build(&overflow_generation, UINT64_C(3), &registry, plugin_n) == 0);
  REQUIRE(chttp_service_host_switch_prepare(
          &service, &overflow_generation.generation) == SALTS_OK);
  REQUIRE(salts_service_host_runtime_publish(
          &runtime, &overflow_generation.generation, &previous)
          == SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == &new_generation.generation);
  REQUIRE(host_http_get(&client, uri, &code, body, &body_size) == SALTS_OK);
  REQUIRE(code == 201u);
  REQUIRE(body_size == 2u && strcmp(body, "11") == 0);
  REQUIRE(cflow_executor_wait_idle(&executor));
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &new_generation.generation) == SALTS_SERVICE_HOST_OK);

#if CHTTP_SERVICE_HOST_TEST_MULTIPLEX
  /* Release the old N+1 slot before preparing N+3. Salts and CHttp both
   * enforce the same two-generation upper bound: no silent third owner. */
  REQUIRE(chttp_service_host_switch_retire(
          &service, &new_generation.generation) == SALTS_OK);
  REQUIRE(host_build(&multiplex_generation, UINT64_C(4),
                     &registry, plugin_next) == 0);

  chttp_async_client async = {0};
  chttp_client_config async_config = host_client_config();
  async_config.network.connection_capacity = 1u; /* physical H2 cap */
  async_config.request_capacity = 2u;            /* stream/admission cap */
  host_h2_result warm = {0}, old = {0}, sibling = {0}, later = {0};
  chttp_request warm_req = {0}, old_req = {0};
  chttp_request sibling_req = {0}, later_req = {0};
  chttp_request rejected_req = {0};
  chttp_request_options opts = {
      .connection_uri = uri, .authority = "127.0.0.1",
      .target = "/plugin/3?right=4&scale=2", .method = CHTTP_METHOD_GET,
      .protocol = CHTTP_HTTP_2, .on_complete = host_h2_complete, .user = &warm
  };
  size_t polled = 0u;
  REQUIRE(chttp_async_client_init(&async, &async_config) == SALTS_OK);
#if CHTTP_SERVICE_HOST_TEST_WINDOW
  /* A stream window of exactly one byte makes every 2/3-byte generated
   * MethodPlan response require peer WINDOW_UPDATE progression. Keep the
   * connection window at the RFC default, isolating stream credit and
   * ensuring a blocked/canceled stream cannot steal sibling credit. */
  chttp_h2_receive_window_policy flow =
      (chttp_h2_receive_window_policy)CHTTP_H2_RECEIVE_WINDOW_POLICY_INIT;
  flow.stream_window = 1u;
  flow.connection_window = 65535u;
  REQUIRE(chttp_async_client_set_h2_receive_window_policy(
          &async, &flow) == SALTS_OK);
#endif
  /* Successful warmup proves the single physical H2 connection is READY
   * and has exchanged SETTINGS before the concurrent stream test begins. */
  REQUIRE(chttp_async_client_submit(&async, &opts, &warm_req) == SALTS_OK);
  REQUIRE(host_h2_poll_until_completions(&async, &warm, NULL, NULL));
  REQUIRE(warm.status == SALTS_OK && warm.completions == 1u);
  REQUIRE(warm.http_major == 2u && warm.response_status == 201u);
  REQUIRE(warm.body_size == 2u && strcmp(warm.body, "11") == 0);

  /* Occupy the existing one-worker executor to hold N+2's native task
   * after real H2 stream admission and before its Provider executes. */
  atomic_store_explicit(&gate.started, 0, memory_order_relaxed);
  atomic_store_explicit(&gate.release, 0, memory_order_relaxed);
  REQUIRE(cflow_executor_try_post_task(&executor, &gate_task)
          == CFLOW_ADMISSION_ACCEPTED);
  const uint64_t gate_deadline = cmeta_monotonic_ms() + HOST_TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(&gate.started, memory_order_acquire) &&
         cmeta_monotonic_ms() < gate_deadline)
    cmeta_thread_yield();
  REQUIRE(atomic_load_explicit(&gate.started, memory_order_acquire));

  opts.user = &old;
  REQUIRE(chttp_async_client_submit(&async, &opts, &old_req) == SALTS_OK);
  REQUIRE(host_h2_poll_until_pending(&async, &executor, 2u));

  REQUIRE(chttp_service_host_switch_prepare(
          &service, &multiplex_generation.generation) == SALTS_OK);
  REQUIRE(salts_service_host_runtime_publish(
          &runtime, &multiplex_generation.generation, &previous)
          == SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == &overflow_generation.generation);
  last_generation = &multiplex_generation.generation;

  opts.user = &sibling;
  REQUIRE(chttp_async_client_submit(&async, &opts, &sibling_req) == SALTS_OK);
  REQUIRE(host_h2_poll_until_pending(&async, &executor, 3u));
  /* A third simultaneous HTTP/2 stream must fail promptly at the bound,
   * rather than opening another connection or falling back to HTTP/1. */
  REQUIRE(chttp_async_client_submit(&async, &opts, &rejected_req)
          == SALTS_ENOBUFS);
  REQUIRE(rejected_req.slot == 0u && rejected_req.generation == 0u);
  REQUIRE(sibling.completions == 0u);
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &overflow_generation.generation)
          == SALTS_SERVICE_HOST_BUSY);
  REQUIRE(chttp_async_request_cancel(&async, old_req) == SALTS_OK);
  REQUIRE(host_h2_poll_until_completions(&async, &old, NULL, NULL));
  REQUIRE(old.status == SALTS_ECANCELED && old.completions == 1u);
  REQUIRE(chttp_async_request_cancel(&async, old_req) == SALTS_ENOENT);
  REQUIRE(sibling.completions == 0u);
  /* The client-owned Stream terminal does not settle the Provider's native
   * work: old DSO/Scope must remain BUSY until CFlow task.finalize. */
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &overflow_generation.generation)
          == SALTS_SERVICE_HOST_BUSY);

  /* The canceled stream's request slot is recyclable while the sibling
   * remains live on the same single-capacity H2 session. */
  opts.user = &later;
  REQUIRE(chttp_async_client_submit(&async, &opts, &later_req) == SALTS_OK);
  REQUIRE(host_h2_poll_until_pending(&async, &executor, 4u));

  atomic_store_explicit(&gate.release, 1, memory_order_release);
  REQUIRE(host_h2_poll_until_completions(&async, &sibling, &later, NULL));
  REQUIRE(sibling.completions == 1u && later.completions == 1u);
  REQUIRE(sibling.status == SALTS_OK && later.status == SALTS_OK);
  REQUIRE(sibling.http_major == 2u && later.http_major == 2u);
  REQUIRE(sibling.response_status == 201u && later.response_status == 201u);
  REQUIRE(strcmp(sibling.body, "111") == 0 && strcmp(later.body, "111") == 0);
  REQUIRE(chttp_async_client_stop(&async, HOST_TEST_TIMEOUT_MS) == SALTS_OK);
  REQUIRE(chttp_async_client_destroy(&async) == SALTS_OK);
  REQUIRE(cflow_executor_wait_idle(&executor));
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &overflow_generation.generation) == SALTS_SERVICE_HOST_OK);
#endif

  /* A malformed request does not invoke the Provider or escape its Scope.
   * Preserve the existing DataBind ingress -> HTTP 400 contract. */
  const chttp_options malformed = {
      .connection_uri = uri, .authority = "127.0.0.1",
      .target = "/plugin/3?right=not-an-integer&scale=2",
      .timeout_ms = HOST_TEST_TIMEOUT_MS,
      .protocol = CHTTP_SERVICE_HOST_TEST_H2 ? CHTTP_HTTP_2 : CHTTP_HTTP_1_1};
  chttp_response bad_response = {0};
  chttp_error bad_error = {0};
  REQUIRE(chttp_get(&client, &malformed, &bad_response, &bad_error) == SALTS_OK);
  REQUIRE(bad_response.status_code == 400u);
  REQUIRE(bad_response.http_major == (CHTTP_SERVICE_HOST_TEST_H2 ? 2u : 1u));
  chttp_response_destroy(&bad_response);

  REQUIRE(salts_service_host_runtime_close(&runtime, &previous) ==
          SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == last_generation);
  REQUIRE(host_http_get(&client, uri, &code, body, &body_size) == SALTS_OK);
  REQUIRE(code == 503u);
  const size_t expected_callbacks =
      CHTTP_SERVICE_HOST_TEST_MULTIPLEX ? 10u : 6u;
  REQUIRE(atomic_load_explicit(&counts.entered, memory_order_acquire)
          == expected_callbacks);
  REQUIRE(atomic_load_explicit(&counts.returned, memory_order_acquire)
          == expected_callbacks);

  REQUIRE(chttp_client_destroy(&client, HOST_TEST_TIMEOUT_MS) == SALTS_OK);
  REQUIRE(chttp_server_stop(&server, HOST_TEST_TIMEOUT_MS) == SALTS_OK);
  /* No accepted workers remain, yet route->user still borrows this Service
   * until the Server has destroyed its route storage. */
  REQUIRE(chttp_service_destroy(&service) == SALTS_EBUSY);
  REQUIRE(chttp_server_destroy(&server) == SALTS_OK);
  REQUIRE(chttp_service_destroy(&service) == SALTS_OK);
  REQUIRE(cflow_executor_wait_idle(&executor));
  cflow_executor_destroy(&executor);
  REQUIRE(salts_service_host_generation_drain(
          &runtime, last_generation) == SALTS_SERVICE_HOST_OK);
  REQUIRE(salts_service_host_runtime_destroy(&runtime) == SALTS_SERVICE_HOST_OK);

  REQUIRE(cmeta_plugin_registry_request_stop(&registry, plugin_n) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_poll_quiescent(
          &registry, plugin_n, &quiet) == CMETA_PLUGIN_OK);
  REQUIRE(quiet);
  REQUIRE(cmeta_plugin_registry_unload(&registry, plugin_n) == CMETA_PLUGIN_OK);
  quiet = false;
  REQUIRE(cmeta_plugin_registry_request_stop(&registry, plugin_next) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_poll_quiescent(
          &registry, plugin_next, &quiet) == CMETA_PLUGIN_OK);
  REQUIRE(quiet);
  REQUIRE(cmeta_plugin_registry_unload(&registry, plugin_next) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);
  data_bind_http_method_plan_free(method_plan);
  data_bind_free(contract);
  puts(CHTTP_SERVICE_HOST_TEST_WINDOW
      ? "H2 stream receive window=1: DATA/WINDOW_UPDATE, CANCEL sibling and bounded FULL PASS"
      : CHTTP_SERVICE_HOST_TEST_MULTIPLEX
        ? "Same H2 connection: N+2 canceled stream, N+3 siblings unaffected, FULL/retire PASS"
        : CHTTP_SERVICE_HOST_TEST_H2
        ? "Same H2c MethodPlan+Listener: N(11), N+1(111), N+2(11), close 503 PASS"
        : "Same H1 MethodPlan+Listener: N(11), N+1(111), N+2(11), close 503 PASS");
  return 0;
}
