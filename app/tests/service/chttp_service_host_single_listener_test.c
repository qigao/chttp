/*
 * Candidate-only single-listener HTTP N->N+1 conformance.
 *
 * The stable HTTP route is registered once. Salts ServiceHost alone publishes
 * generations and owns Scope/Plugin lifetime. This fixture owns two immutable
 * pre-admitted typed bindings and four bounded request slots; it is NOT an
 * alternate service registry, provider loader, or published CHttp runtime API.
 *
 * An N request is held on a CFlow worker while N+1 is published; a second
 * request uses the SAME listener and N+1. Old N remains BUSY until its worker
 * has finished native execution and released its original address-stable Scope.
 */
#include "chttp_service_host_component.h"
#include "chttp_service_plugin_native.h"

#include <http_server/http.h>
#include <http_client/http.h>
#include <salts/service_host.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <cflow/executor.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { HOST_TEST_TIMEOUT_MS = 10000, HOST_CALL_SLOTS = 4 };
#define REQUIRE(x) do { if (!(x)) { \
  fprintf(stderr, "host single-listener: %s:%d: %s\n", \
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

typedef struct host_binding {
  uint64_t id;
  chttp_service_component_operation operation;
  bool ready; /* guarded by host_router.lock */
} host_binding;

struct host_router;
typedef struct host_call {
  struct host_router *owner;
  salts_service_host_scope scope; /* never copied or moved */
  const host_binding *binding;
  chttp_server_deferred deferred;
  bool occupied; /* guarded by host_router.lock */
} host_call;

typedef struct host_router {
  cmeta_mutex_t lock;
  salts_service_host_runtime *runtime; /* borrowed */
  cflow_executor executor;            /* single existing CFlow authority */
  host_binding generations[2];        /* immutable once published */
  host_call calls[HOST_CALL_SLOTS];    /* bounded stable slots */
  _Atomic int old_started;
  _Atomic int old_continue;
  _Atomic int scope_errors;
  _Atomic size_t completed;
  _Atomic size_t middleware_entered;
  _Atomic size_t middleware_returned;
} host_router;

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
    .max_send_bytes = 4096u,
    .receive_buffer_bytes = 256u,
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
    .max_route_middleware_count = 1u,
    .max_route_param_count = 2u,
    .max_route_param_bytes = 64u,
    .max_target_bytes = 256u,
    .max_header_count = 12u,
    .max_header_bytes = 1024u,
    .max_request_body_bytes = 256u,
    .max_response_header_count = 12u,
    .max_response_header_bytes = 1024u,
    .max_response_body_bytes = 256u,
    .poll_slice_ms = 2u
  };
  return config;
}

static chttp_client_config host_client_config(void) {
  const chttp_client_config config = {
    .network = host_network(2u),
    .request_capacity = 1u,
    .max_start_line_bytes = 256u,
    .max_header_count = 12u,
    .max_header_bytes = 1024u,
    .max_request_body_bytes = 256u,
    .max_response_body_bytes = 256u,
    .max_informational_responses = 2u
  };
  return config;
}

static int host_build(
    host_generation_fixture *fixture, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
  const salts_service_host_generation_storage storage = {
    fixture->deployments, 1u,
    fixture->instances, 1u,
    fixture->dependencies, 1u,
    fixture->activation_order, 1u,
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

/* The control thread prepares the final address-stable binding *before*
 * publication. It is the only writer for this not-yet-ready slot.
 *
 * Provider callbacks and CMeta projections run OUTSIDE the HTTP admission
 * mutex. The mutex publishes ready=true after all pointer-bearing native
 * binding fields have been constructed; readers may only inspect operation
 * bytes after seeing ready under the same mutex. No hot-path lookup. */
static int host_prepare(
    host_router *router, size_t slot, host_generation_fixture *fixture) {
  salts_configurator_service service = {0};
  chttp_service_operation_provider provider =
      chttp_service_operation_provider_bind(NULL, NULL);
  host_binding *binding;
  if (router == NULL || slot >= 2u || fixture == NULL)
    return -1;
  cmeta_mutex_lock(&router->lock);
  binding = &router->generations[slot];
  const bool already_ready = binding->ready;
  cmeta_mutex_unlock(&router->lock);
  if (already_ready)
    return -1;

  /* These provider/reflective calls may invoke arbitrary DSO code. They must
   * not run while the domain admission mutex is held. Unpublished storage is
   * exclusive to the control thread until ready is set. */
  if (salts_configurator_context_find_service_from(
        &fixture->generation.components, "CHttpPlugin_Calc_Add",
        chttp_service_operation_provider_interface(),
        &service) != SALTS_COMPONENT_OK ||
      chttp_service_operation_provider_borrow_from_object(
        service.object, service.interfaces, &provider) != CMETA_OK ||
      !chttp_service_operation_provider_valid(&provider))
    return -1;
  binding->operation =
      (chttp_service_component_operation)CHTTP_SERVICE_COMPONENT_OPERATION_INIT;
  if (!chttp_service_operation_provider_get_operation(
          &provider, &binding->operation) ||
      !chttp_service_component_operation_valid(&binding->operation))
    return -1;

  cmeta_mutex_lock(&router->lock);
  binding->id = fixture->generation.id;
  binding->ready = true;
  cmeta_mutex_unlock(&router->lock);
  return 0;
}

static void host_cancel_task(void *user) {
  host_call *call = (host_call *)user;
  if (call != NULL && call->deferred.impl != NULL)
    (void)chttp_server_deferred_cancel(&call->deferred);
}

static void host_finalize_task(void *user) {
  host_call *call = (host_call *)user;
  host_router *router;
  if (call == NULL || call->owner == NULL) return;
  router = call->owner;
  host_cancel_task(call);
  if (salts_service_host_scope_release(&call->scope) !=
      SALTS_SERVICE_HOST_OK)
    atomic_fetch_add_explicit(&router->scope_errors, 1, memory_order_relaxed);
  cmeta_mutex_lock(&router->lock);
  call->binding = NULL;
  call->occupied = false;
  cmeta_mutex_unlock(&router->lock);
  atomic_fetch_add_explicit(&router->completed, 1u, memory_order_release);
}

static void host_run_task(void *user) {
  host_call *call = (host_call *)user;
  host_router *router;
  AddRequest_t request = {0};
  AddResponse_t response = {0};
  void *params[2];
  int native_status = -1;
  char text[32];
  int length, status;
  chttp_server_deferred_response reply;

  if (call == NULL || call->owner == NULL || call->binding == NULL)
    return;
  router = call->owner;
  if (salts_service_host_scope_generation_id(&call->scope) == UINT64_C(1)) {
    atomic_store_explicit(&router->old_started, 1, memory_order_release);
    const uint64_t deadline = cmeta_monotonic_ms() + HOST_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(&router->old_continue, memory_order_acquire) == 0 &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    if (atomic_load_explicit(&router->old_continue, memory_order_acquire) == 0) {
      host_cancel_task(call);
      return;
    }
  }

  request.left = 3u;
  request.right = 4u;
  request.scale = 2u;
  params[0] = &request;
  params[1] = &response;
  if (!call->binding->operation.execution.invoke(
          call->binding->operation.execution.context,
          &native_status, params, 2u) || native_status != 0) {
    host_cancel_task(call);
    return;
  }

  length = snprintf(text, sizeof(text), "%u", (unsigned int)response.sum);
  if (length <= 0 || (size_t)length >= sizeof(text)) {
    host_cancel_task(call);
    return;
  }
  reply = (chttp_server_deferred_response){
    sizeof(reply), 200u, "text/plain", NULL, 0u, text, (size_t)length
  };
  status = chttp_server_deferred_reply(&call->deferred, &reply);
  if (status != SALTS_OK) host_cancel_task(call);
}

static int host_route(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  host_router *router = (host_router *)user;
  host_call *call = NULL;
  const host_binding *binding = NULL;
  uint64_t id;
  int defer_status;
  cflow_executor_task task;
  cflow_admission_status admission;
  (void)request;

  if (router == NULL || response == NULL) return SALTS_EINVAL;
  cmeta_mutex_lock(&router->lock);
  for (size_t i = 0u; i < HOST_CALL_SLOTS; ++i) {
    if (!router->calls[i].occupied) {
      call = &router->calls[i];
      call->occupied = true;
      call->owner = router;
      break;
    }
  }
  if (call == NULL) {
    cmeta_mutex_unlock(&router->lock);
    return chttp_server_reply(response, 503u, "text/plain",
                              "Service Unavailable", 19u);
  }

  if (salts_service_host_scope_acquire(router->runtime, &call->scope) !=
      SALTS_SERVICE_HOST_OK) {
    call->occupied = false;
    cmeta_mutex_unlock(&router->lock);
    return chttp_server_reply(response, 503u, "text/plain",
                              "Service Unavailable", 19u);
  }

  id = salts_service_host_scope_generation_id(&call->scope);
  for (size_t i = 0u; i < 2u; ++i) {
    if (router->generations[i].ready &&
        router->generations[i].id == id) {
      binding = &router->generations[i];
      break;
    }
  }
  if (binding == NULL) {
    (void)salts_service_host_scope_release(&call->scope);
    call->occupied = false;
    cmeta_mutex_unlock(&router->lock);
    return chttp_server_reply(response, 500u, "text/plain",
                              "Internal Server Error", 21u);
  }
  call->binding = binding;
  defer_status = chttp_server_response_defer(response, &call->deferred);
  if (defer_status != SALTS_OK) {
    (void)salts_service_host_scope_release(&call->scope);
    call->binding = NULL;
    call->occupied = false;
    cmeta_mutex_unlock(&router->lock);
    return defer_status;
  }
  cmeta_mutex_unlock(&router->lock);

  task = (cflow_executor_task){
    .run = host_run_task,
    .cancel = host_cancel_task,
    .finalize = host_finalize_task,
    .user = call
  };
  admission = cflow_executor_try_post_task(&router->executor, &task);
  if (admission != CFLOW_ADMISSION_ACCEPTED) {
    const chttp_server_deferred_response overload = {
      sizeof(overload), 503u, "text/plain", NULL, 0u,
      "Service Unavailable", 19u
    };
    (void)chttp_server_deferred_reply(&call->deferred, &overload);
    host_finalize_task(call);
  }
  return SALTS_OK;
}

static int host_interceptor(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  host_router *router = (host_router *)user;
  if (router == NULL || next == NULL) return SALTS_EINVAL;
  atomic_fetch_add_explicit(
      &router->middleware_entered, 1u, memory_order_relaxed);
  const int status = chttp_server_next_call(next);
  atomic_fetch_add_explicit(
      &router->middleware_returned, 1u, memory_order_relaxed);
  (void)request;
  (void)response;
  return status;
}

typedef struct host_client_call {
  chttp_client_config config;
  char uri[64];
  char body[32];
  size_t body_size;
  unsigned int http_status;
  int status;
  _Atomic int completed;
} host_client_call;

static int host_http_get(
    chttp_client *client, const char *uri,
    unsigned int *out_code, char *body, size_t *body_size) {
  const chttp_options options = {
    .connection_uri = uri, .authority = "127.0.0.1",
    .target = "/same-listener", .timeout_ms = HOST_TEST_TIMEOUT_MS
  };
  chttp_response response = {0};
  chttp_error error = {0};
  const int status = chttp_get(client, &options, &response, &error);
  if (status == SALTS_OK) {
    *out_code = response.status_code;
    if (response.body_size >= 32u) {
      chttp_response_destroy(&response);
      return SALTS_EMSGSIZE;
    }
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

int main(void) {
  const cmeta_plugin_registry_config registry_config = {2u};
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_ref plugin_n = {0}, plugin_next = {0};
  host_generation_fixture old_generation = {0};
  host_generation_fixture new_generation = {0};
  salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
  salts_service_host_generation *previous = NULL;
  host_router router = {0};
  chttp_server server = {0};
  chttp_client client = {0};
  cmeta_thread_t client_thread = NULL;
  host_client_call old_request = {0};
  const chttp_server_execution_options two_owners = {
    sizeof(chttp_server_execution_options),
    CHTTP_SERVER_EXECUTION_OPTIONS_VERSION, 2u
  };
  chttp_server_config server_config = host_server_config();
  const chttp_client_config client_config = host_client_config();
  uint16_t port = 0u;
  char uri[64];
  char body[32];
  size_t body_size = 0u;
  unsigned int code = 0u;
  bool quiet = false;
  salts_service_host_scope rejected_scope = SALTS_SERVICE_HOST_SCOPE_INIT;
  uint64_t deadline;

  REQUIRE(cmeta_plugin_registry_init(&registry, &registry_config) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_load(&registry,
          GENERATED_CHTTP_SERVICE_HOST_PROVIDER_PATH, &plugin_n) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_start(&registry, plugin_n) ==
          CMETA_PLUGIN_OK);
  REQUIRE(host_build(&old_generation, UINT64_C(1), &registry, plugin_n) == 0);
  REQUIRE(salts_service_host_runtime_init(&runtime) == SALTS_SERVICE_HOST_OK);

  router.runtime = &runtime;
  cmeta_mutex_init(&router.lock);
  REQUIRE(router.lock != NULL);
  REQUIRE(cflow_executor_worker_init_with_capacity(
          &router.executor, 2u, HOST_CALL_SLOTS));
  REQUIRE(host_prepare(&router, 0u, &old_generation) == 0);
  REQUIRE(salts_service_host_runtime_publish(
          &runtime, &old_generation.generation, &previous) ==
          SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == NULL);

  REQUIRE(chttp_server_init(&server, &server_config) == SALTS_OK);
  REQUIRE(chttp_server_set_execution_options(
          &server, &two_owners) == SALTS_OK);
  REQUIRE(chttp_server_use(&server, host_interceptor, &router) == SALTS_OK);
  REQUIRE(chttp_server_get(
          &server, "/same-listener", host_route, &router) == SALTS_OK);
  REQUIRE(chttp_server_start(&server) == SALTS_OK);
  REQUIRE(chttp_server_port(&server, &port) == SALTS_OK);
  REQUIRE(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port)
          > 0);
  REQUIRE(chttp_client_init(&client, &client_config) == SALTS_OK);

  old_request.config = host_client_config();
  REQUIRE(snprintf(old_request.uri, sizeof(old_request.uri), "%s", uri) > 0);
  REQUIRE(cmeta_thread_create(
          &client_thread, host_client_thread, &old_request) == SALTS_OK);
  deadline = cmeta_monotonic_ms() + HOST_TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(&router.old_started, memory_order_acquire) &&
         cmeta_monotonic_ms() < deadline)
    cmeta_thread_yield();
  REQUIRE(atomic_load_explicit(&router.old_started, memory_order_acquire));

  REQUIRE(cmeta_plugin_registry_load(&registry,
          GENERATED_CHTTP_SERVICE_HOST_SECOND_PATH, &plugin_next) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_start(&registry, plugin_next) ==
          CMETA_PLUGIN_OK);
  REQUIRE(host_build(&new_generation, UINT64_C(2), &registry, plugin_next) == 0);

  /* Domain admission lock serializes binding publication with HTTP scopes.
   * The ServiceHost runtime mutex serializes the N/N+1 generation cutover. */
  REQUIRE(host_prepare(&router, 1u, &new_generation) == 0);
  cmeta_mutex_lock(&router.lock);
  const salts_service_host_status publish_status =
      salts_service_host_runtime_publish(
          &runtime, &new_generation.generation, &previous);
  cmeta_mutex_unlock(&router.lock);
  REQUIRE(publish_status == SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == &old_generation.generation);

  REQUIRE(salts_service_host_generation_drain(
          &runtime, &old_generation.generation) == SALTS_SERVICE_HOST_BUSY);
  REQUIRE(cmeta_plugin_registry_unload(&registry, plugin_n) ==
          CMETA_PLUGIN_BUSY);

  /* This is a SECOND HTTP request on the SAME port and same registered route.
   * N remains in flight; the new request binds exclusively to N+1. */
  REQUIRE(host_http_get(&client, uri, &code, body, &body_size) == SALTS_OK);
  REQUIRE(code == 200u);
  REQUIRE(body_size == 3u && strcmp(body, "111") == 0);

  REQUIRE(salts_service_host_runtime_close(&runtime, &previous) ==
          SALTS_SERVICE_HOST_OK);
  REQUIRE(previous == &new_generation.generation);
  REQUIRE(salts_service_host_scope_acquire(&runtime, &rejected_scope) ==
          SALTS_SERVICE_HOST_INVALID_STATE);
  REQUIRE(!rejected_scope.live);

  /* The route still exists, but closed admission refuses further work. */
  REQUIRE(host_http_get(&client, uri, &code, body, &body_size) == SALTS_OK);
  REQUIRE(code == 503u);

  atomic_store_explicit(&router.old_continue, 1, memory_order_release);
  REQUIRE(cmeta_thread_join(&client_thread) == SALTS_OK);
  cmeta_thread_destroy(&client_thread);
  REQUIRE(old_request.status == SALTS_OK);
  REQUIRE(old_request.http_status == 200u);
  REQUIRE(old_request.body_size == 2u && strcmp(old_request.body, "11") == 0);
  REQUIRE(cflow_executor_wait_idle(&router.executor));
  REQUIRE(atomic_load_explicit(&router.scope_errors, memory_order_acquire) == 0);
  REQUIRE(atomic_load_explicit(&router.completed, memory_order_acquire) == 2u);
  REQUIRE(atomic_load_explicit(
          &router.middleware_entered, memory_order_acquire) == 3u);
  REQUIRE(atomic_load_explicit(
          &router.middleware_returned, memory_order_acquire) == 3u);

  REQUIRE(chttp_client_destroy(&client, HOST_TEST_TIMEOUT_MS) == SALTS_OK);
  REQUIRE(chttp_server_stop(&server, HOST_TEST_TIMEOUT_MS) == SALTS_OK);
  REQUIRE(chttp_server_destroy(&server) == SALTS_OK);
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &old_generation.generation) == SALTS_SERVICE_HOST_OK);
  REQUIRE(salts_service_host_generation_drain(
          &runtime, &new_generation.generation) == SALTS_SERVICE_HOST_OK);
  REQUIRE(salts_service_host_runtime_destroy(&runtime) == SALTS_SERVICE_HOST_OK);
  REQUIRE(cflow_executor_wait_idle(&router.executor));
  cflow_executor_destroy(&router.executor);
  cmeta_mutex_destroy(&router.lock);

  REQUIRE(cmeta_plugin_registry_request_stop(&registry, plugin_n) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_poll_quiescent(
          &registry, plugin_n, &quiet) == CMETA_PLUGIN_OK);
  REQUIRE(quiet);
  REQUIRE(cmeta_plugin_registry_unload(&registry, plugin_n) ==
          CMETA_PLUGIN_OK);
  quiet = false;
  REQUIRE(cmeta_plugin_registry_request_stop(&registry, plugin_next) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_poll_quiescent(
          &registry, plugin_next, &quiet) == CMETA_PLUGIN_OK);
  REQUIRE(quiet);
  REQUIRE(cmeta_plugin_registry_unload(&registry, plugin_next) ==
          CMETA_PLUGIN_OK);
  REQUIRE(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);
  puts("Single HTTP listener: N(11) deferred + N+1(111), close=503, scopes/drain/unload PASS");
  return 0;
}
