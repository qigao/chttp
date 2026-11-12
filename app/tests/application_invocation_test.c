#include "chttp_application_worker.h"
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include <threads/threads.h>
#include <core/clock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinytest.h"

typedef struct InvocationController { int marker; } InvocationController;
cmeta_reflect_data(InvocationController, "test.InvocationController", cmeta_field(int, marker));
cmeta_component_empty(InvocationComponent);
CHTTP_INVOCATION_ACTION(invocation_echo);
CHTTP_ACTION(invocation_legacy_contract);
typedef struct invocation_call {
  const char *target;
  chttp_protocol protocol;
  int status;
  unsigned int code;
  size_t bytes;
  char body[16];
} invocation_call;
static struct {
  chttp_application app;
  chttp_application_config config;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  chttp_application_worker_binding binding;
  chttp_invocation_interceptor hooks[2];
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t thread;
  bool thread_live, release, reject, fail, no_reply, overflow, recover, foreign, cross_scope, wait_cancel;
  const chttp_invocation *first_scope;
  size_t workers;
  int entered, calls, completions, destroyed, trace[32];
  size_t trace_count;
  chttp_application_work_outcome outcome;
  _Atomic int invalid;
  char uri[64];
  invocation_call background;
} fixture;
static void destroy(void *context, void *object) { (void)context; ++fixture.destroyed; free(object); }
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, destroy};
static cmeta_status create(void *context, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  InvocationController *object = malloc(sizeof(*object));
  if (!object) return CMETA_OUT_OF_MEMORY;
  object->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, object, cmeta_reflected_data(InvocationController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(object); }
  return status;
}
static void trace(int event) {
  sync_mutex_lock(&fixture.mutex);
  if (fixture.trace_count < sizeof(fixture.trace) / sizeof(fixture.trace[0])) fixture.trace[fixture.trace_count++] = event;
  else atomic_fetch_add(&fixture.invalid, 1);
  sync_mutex_unlock(&fixture.mutex);
}
static void scope_check(void *self, const chttp_invocation *invocation) {
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  if (!self || ((InvocationController *)self)->marker != 42 || !chttp_application_worker_current(&fixture.app) ||
      !request || request->session || chttp_invocation_session(invocation) ||
      !cmeta_function_desc_equal(chttp_invocation_operation(invocation), FunctionAbi(invocation_echo)->function))
    atomic_fetch_add(&fixture.invalid, 1);
}
static void foreign_scope(void *context) {
  const chttp_invocation *invocation = context;
  bool cancelled = true, proceed = true;
  if (chttp_invocation_request(invocation) || chttp_invocation_session(invocation) || chttp_invocation_operation(invocation) ||
      chttp_invocation_cancelled(invocation, &cancelled) != SALTS_EINVAL || !cancelled ||
      chttp_invocation_respond(invocation, 200u, NULL, "bad", 3u) != SALTS_EINVAL ||
      chttp_invocation_reject(invocation, &proceed, 403u, NULL, NULL, 0u) != SALTS_EINVAL || !proceed)
    atomic_fetch_add(&fixture.invalid, 1);
}
int invocation_echo(void *self, const chttp_invocation *invocation) {
  scope_check(self, invocation); trace(3);
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  if (!request) return SALTS_EPROTO;
  bool cancelled = true, proceed = true;
  if (chttp_invocation_cancelled(invocation, &cancelled) != SALTS_OK || cancelled ||
      chttp_invocation_reject(invocation, &proceed, 403u, NULL, NULL, 0u) != SALTS_EINVAL || !proceed)
    atomic_fetch_add(&fixture.invalid, 1);
  sync_mutex_lock(&fixture.mutex); ++fixture.calls;
  if (fixture.wait_cancel || (fixture.cross_scope && strcmp(request->path, "/first") == 0)) {
    fixture.first_scope = invocation; ++fixture.entered; sync_cond_broadcast(&fixture.changed);
    while (!fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
    fixture.first_scope = NULL;
  }
  const chttp_invocation *foreign = fixture.cross_scope && strcmp(request->path, "/second") == 0 ? fixture.first_scope : NULL;
  sync_mutex_unlock(&fixture.mutex);
  if (fixture.cross_scope && strcmp(request->path, "/second") == 0) {
    if (!foreign) atomic_fetch_add(&fixture.invalid, 1);
    else foreign_scope((void *)foreign);
  }
  if (fixture.foreign) {
    sync_thread_t thread;
    int status = sync_thread_create(&thread, foreign_scope, (void *)invocation);
    if (status != SALTS_OK) return status;
    status = sync_thread_join(&thread);
    if (status != SALTS_OK) return status;
  }
  if (fixture.wait_cancel) {
    const uint64_t began = monotonic_ms();
    do {
      if (chttp_invocation_cancelled(invocation, &cancelled) != SALTS_OK) return SALTS_EPROTO;
      if (!cancelled) sync_thread_yield();
    } while (!cancelled && monotonic_ms() - began < 5000u);
    if (!cancelled) return SALTS_EPROTO;
  }
  if (fixture.fail) return SALTS_EPERM;
  if (fixture.no_reply) return SALTS_OK;
  if (fixture.overflow) {
    char large[256]; memset(large, 'x', sizeof(large));
    const int status = chttp_invocation_respond(invocation, 200u, "text/plain", large, sizeof(large));
    if (!fixture.recover) return status;
    if (status != SALTS_EMSGSIZE) return SALTS_EPROTO;
  }
  char body[3] = {'a', '\0', 'z'};
  if (request->body_size != sizeof(body) || memcmp(request->body, body, sizeof(body)) != 0) return SALTS_EPROTO;
  const int status = chttp_invocation_respond(invocation, 201u, "application/octet-stream", body, sizeof(body));
  memset(body, '!', sizeof(body));
  if (status == SALTS_OK && chttp_invocation_respond(invocation, 202u, NULL, NULL, 0u) != SALTS_EALREADY) return SALTS_EPROTO;
  return status;
}
static cmeta_status before1(void *self, const chttp_invocation *invocation, bool *proceed) {
  (void)proceed; scope_check(self, invocation); trace(1); return CMETA_OK;
}
static cmeta_status before2(void *self, const chttp_invocation *invocation, bool *proceed) {
  scope_check(self, invocation); trace(2);
  return fixture.reject && chttp_invocation_reject(invocation, proceed, 403u, "text/plain", "denied", 6u) != SALTS_OK ?
      CMETA_CALLBACK_ERROR : CMETA_OK;
}
static void after1(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  scope_check(self, invocation); if (result->status != SALTS_OK) atomic_fetch_add(&fixture.invalid, 1); trace(11);
}
static void after2(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  scope_check(self, invocation); if (result->status != SALTS_OK) atomic_fetch_add(&fixture.invalid, 1); trace(12);
}
static void error1(void *self, const chttp_invocation *invocation, cmeta_status status) {
  scope_check(self, invocation); if (status == CMETA_OK) atomic_fetch_add(&fixture.invalid, 1); trace(-1);
}
static void error2(void *self, const chttp_invocation *invocation, cmeta_status status) {
  scope_check(self, invocation); if (status == CMETA_OK) atomic_fetch_add(&fixture.invalid, 1); trace(-2);
}
static void completed(void *self, const chttp_server_request_view *request, const chttp_application_work_outcome *outcome) {
  if (!self || ((InvocationController *)self)->marker != 42 || !chttp_application_worker_current(&fixture.app) ||
      !cmeta_function_desc_equal(request->operation, FunctionAbi(invocation_echo)->function)) atomic_fetch_add(&fixture.invalid, 1);
  trace(200); sync_mutex_lock(&fixture.mutex); ++fixture.completions; fixture.outcome = *outcome;
  sync_mutex_unlock(&fixture.mutex);
}
static void initialize(void) {
  check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
  const chttp_application_work_config limits = {fixture.workers, 4u, 16384u, 128u};
  check_equal(chttp_application_worker_prepare(&fixture.app, &limits), SALTS_OK);
}
static void listen(void) {
  check_equal(chttp_application_start(&fixture.app), SALTS_OK);
  uint16_t port = 0u; check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
  snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
}
static void start(void) {
  initialize(); check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK); listen();
}
static void perform(void *context) {
  invocation_call *call = context; chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  const chttp_client_config config = {.network = fixture.config.http.network, .request_capacity = 1u,
      .max_start_line_bytes = 256u, .max_header_count = 16u, .max_header_bytes = 4096u,
      .max_request_body_bytes = 8192u, .max_response_body_bytes = 8192u, .max_informational_responses = 4u};
  call->status = chttp_client_init(&client, &config);
  if (call->status != SALTS_OK) return;
  static const char body[] = {'a', '\0', 'z'};
  const chttp_options options = {.connection_uri = fixture.uri, .authority = "invocation.test", .target = call->target,
      .protocol = call->protocol, .timeout_ms = 5000u, .body = body, .body_size = sizeof(body)};
  call->status = chttp_post(&client, &options, &response, &error); call->code = response.status_code; call->bytes = response.body_size;
  if (response.body_size <= sizeof(call->body) && response.body_size) memcpy(call->body, response.body, response.body_size);
  chttp_response_destroy(&response); const int closed = chttp_client_destroy(&client, 5000u);
  if (call->status == SALTS_OK) call->status = closed;
}
static void wait_for_entry(void) {
  const uint64_t began = monotonic_ms(); sync_mutex_lock(&fixture.mutex);
  while (!fixture.entered && monotonic_ms() - began < 5000u)
    (void)sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(10000000));
  const int entered = fixture.entered; sync_mutex_unlock(&fixture.mutex); check_equal(entered, 1);
}

spec("Application canonical buffered Invocation") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture)); atomic_init(&fixture.invalid, 0);
    sync_mutex_init(&fixture.mutex); sync_cond_init(&fixture.changed); fixture.workers = 1u;
    fixture.provider = (salts_configurator_provider_binding){sizeof(fixture.provider),
        SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(InvocationComponent), NULL, NULL, create, NULL, NULL};
    fixture.deployment = (salts_configurator_deployment){&fixture.provider, NULL, NULL};
    fixture.controller = (chttp_controller){.component_id = "InvocationComponent", .receiver = cmeta_reflected_data(InvocationController)};
    fixture.config = chttp_application_config_default(); fixture.config.components = &fixture.deployment; fixture.config.component_count = 1u;
    fixture.config.controllers = &fixture.controller; fixture.config.controller_count = 1u;
    fixture.hooks[0] = (chttp_invocation_interceptor){"InvocationComponent", cmeta_reflected_data(InvocationController), before1, after1, error1};
    fixture.hooks[1] = (chttp_invocation_interceptor){"InvocationComponent", cmeta_reflected_data(InvocationController), before2, after2, error2};
    fixture.binding = (chttp_application_worker_binding){.component_id = "InvocationComponent", .receiver = cmeta_reflected_data(InvocationController),
        .method = CHTTP_METHOD_POST, .path = "/echo", .abi = FunctionAbi(invocation_echo), .invoke = invocation_echo,
        .after_completion = completed, .hooks = fixture.hooks, .hook_count = 2u};
  }
  after_each() {
    sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    if (fixture.thread_live) check_equal(sync_thread_join(&fixture.thread), SALTS_OK);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_equal(atomic_load(&fixture.invalid), 0);
    sync_cond_destroy(&fixture.changed); sync_mutex_destroy(&fixture.mutex);
  }
  it("rejects zero handles without changing query outputs") {
    const chttp_invocation empty = {0}; bool cancelled = true, proceed = true;
    check_null(chttp_invocation_request(&empty)); check_null(chttp_invocation_session(&empty)); check_null(chttp_invocation_operation(&empty));
    check_equal(chttp_invocation_cancelled(&empty, &cancelled), SALTS_EINVAL); check_true(cancelled);
    check_equal(chttp_invocation_respond(&empty, 200u, NULL, NULL, 0u), SALTS_EINVAL);
    check_equal(chttp_invocation_reject(&empty, &proceed, 403u, NULL, NULL, 0u), SALTS_EINVAL); check_true(proceed);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    it("copies request/response bytes and preserves typed hook order on protocol %d", protocol) {
      start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.bytes, 3u);
      static const char expected_body[] = {'a', '\0', 'z'}; check_equal(call.body, expected_body, sizeof(expected_body));
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      const int expected[] = {1,2,3,12,11,200}; check_equal(fixture.trace_count, 6u); check_equal(fixture.trace, expected, sizeof(expected));
      check_equal(fixture.calls, 1); check_equal(fixture.completions, 1); check_equal(fixture.destroyed, 1);
    }
    it("preserves explicit Invocation rejection and excludes business execution on protocol %d", protocol) {
      fixture.reject = true; start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 403u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      const int expected[] = {1,2,-2,-1,200}; check_equal(fixture.trace_count, 5u); check_equal(fixture.trace, expected, sizeof(expected));
      check_equal(fixture.calls, 0); check_true(fixture.outcome.rejected);
    }
    for (int failure = 0; failure <= 2; ++failure) {
      it("unwinds typed error hooks for failure %d on protocol %d", failure, protocol) {
        fixture.fail = failure == 0; fixture.no_reply = failure == 1; fixture.overflow = failure == 2;
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 500u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        const int expected[] = {1,2,3,-2,-1,200}; check_equal(fixture.trace_count, 6u); check_equal(fixture.trace, expected, sizeof(expected));
        check_equal(fixture.outcome.business_status, failure == 0 ? SALTS_EPERM : failure == 1 ? SALTS_EPROTO : SALTS_EMSGSIZE);
        check_equal(fixture.outcome.transport_status, SALTS_OK);
      }
    }
    it("leaves an oversized draft unsealed and reusable on protocol %d", protocol) {
      fixture.overflow = true; fixture.recover = true; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
    }
    it("refuses Invocation access from a foreign thread on protocol %d", protocol) {
      fixture.foreign = true; start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
    }
  }
  it("rejects legacy/missing metadata and missing hook Beans before route publication") {
    initialize(); fixture.binding.abi = FunctionAbi(invocation_legacy_contract);
    check_equal(cmeta_function_param_abi(fixture.binding.abi, 1u), CMETA_ABI_OBJECT_POINTER);
    check_equal(cmeta_function_param_abi(FunctionAbi(invocation_echo), 1u), CMETA_ABI_OBJECT_POINTER);
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_EINVAL);
    fixture.binding.abi = NULL; check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_EINVAL);
    fixture.binding.abi = FunctionAbi(invocation_echo); fixture.hooks[1].component_id = "Missing";
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_ENOENT);
    fixture.hooks[1].component_id = "InvocationComponent";
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK);
    memset(fixture.hooks, 0, sizeof(fixture.hooks)); memset(&fixture.binding, 0, sizeof(fixture.binding)); listen();
    invocation_call call = {.target = "/echo", .protocol = CHTTP_HTTP_2}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
  }
  it("keeps overlapping Invocations isolated even on the same two-worker executor") {
    fixture.workers = 2u; fixture.cross_scope = true; fixture.binding.path = "/first"; initialize();
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK);
    fixture.binding.path = "/second"; check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK); listen();
    fixture.background = (invocation_call){.target = "/first", .protocol = CHTTP_HTTP_2};
    check_equal(sync_thread_create(&fixture.thread, perform, &fixture.background), SALTS_OK); fixture.thread_live = true; wait_for_entry();
    invocation_call call = {.target = "/second", .protocol = CHTTP_HTTP_2}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
    sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
    check_equal(fixture.background.status, SALTS_OK); check_equal(fixture.background.code, 201u);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_equal(fixture.calls, 2); check_equal(fixture.completions, 2);
  }
  it("queries observed cancellation without releasing a still-running Controller") {
    fixture.wait_cancel = true; fixture.config.deadlines.handler_ms = 500u; start();
    fixture.background = (invocation_call){.target = "/echo", .protocol = CHTTP_HTTP_2};
    check_equal(sync_thread_create(&fixture.thread, perform, &fixture.background), SALTS_OK); fixture.thread_live = true; wait_for_entry();
    check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
    check_not_equal(fixture.background.status, SALTS_OK); check_not_equal(fixture.background.status, SALTS_ETIMEDOUT);
    check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT); check_not_null(fixture.app.impl); check_equal(fixture.destroyed, 0);
    sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_equal(fixture.destroyed, 1);
    check_equal(fixture.outcome.business_status, SALTS_OK); check_not_equal(fixture.outcome.transport_status, SALTS_OK);
  }
}
