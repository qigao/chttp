#include <chttp_app/app.h>
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include "chttp_service_cflow.http.h"
#include "chttp_service_cflow.service_native.h"
#include "chttp_service_cflow_native.h"
#include <core/clock.h>
#include <threads/threads.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "tinytest.h"

typedef struct BindingController { int marker; } BindingController;
cmeta_reflect_data(BindingController, "test.BindingController", cmeta_field(int, marker));
cmeta_component_empty(BindingComponent);
static struct {
  chttp_application app;
  cflow_executor executor;
  bool executor_live, thread_live;
  sync_thread_t thread;
  _Atomic int entered, release;
  bool block;
  int destroyed, request_status;
  unsigned int response_status;
  char uri[64];
  chttp_client_config client;
  DataBind *contract;
  DataBindHttpMethodPlan *plan;
  DataBindNativeTypeBinding request, response;
  DataBindServiceNativeBinding binding;
  DataBindNativeExecution execution;
  cflow_function_typed_adapter_projection projection;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_http_method method;
  chttp_controller controller;
  chttp_application_config config;
} fixture;

static void destroy(void *context, void *object) {
  (void)context; ++fixture.destroyed; free(object);
}
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, destroy};
static cmeta_status create(void *context, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count != 0) return CMETA_INVALID_ARGUMENT;
  BindingController *self = malloc(sizeof(*self));
  if (!self) return CMETA_OUT_OF_MEMORY;
  self->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, self, cmeta_reflected_data(BindingController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(self); }
  return status;
}
static bool DATA_BIND_NATIVE_CALL invoke(void *context, void *result, void *const *params, size_t count) {
  BindingController *self = context;
  if (!self || self->marker != 42) return false;
  atomic_store_explicit(&fixture.entered, 1, memory_order_release);
  if (fixture.block)
    while (!atomic_load_explicit(&fixture.release, memory_order_acquire)) sync_thread_yield();
  return databind_8_CFlowGen_4_Calc_3_Add__databind_execution()->invoke(NULL, result, params, count);
}
static int request(const char *target, unsigned int *code) {
  chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  int status = chttp_client_init(&client, &fixture.client);
  if (status != SALTS_OK) return status;
  const chttp_options options = {.connection_uri = fixture.uri, .authority = "test",
      .target = target, .timeout_ms = 5000u};
  status = chttp_get(&client, &options, &response, &error);
  *code = response.status_code;
  if (status == SALTS_OK && *code == 201u &&
      (response.body_size != 1u || *(const char *)response.body != '7')) status = SALTS_EPROTO;
  chttp_response_destroy(&response);
  const int closed = chttp_client_destroy(&client, 5000u);
  return closed != SALTS_OK ? closed : status;
}
static void request_thread(void *unused) {
  (void)unused;
  fixture.request_status = request("/generated-cflow/3?right=4", &fixture.response_status);
}
static int start(chttp_dispatch dispatch) {
  fixture.method.dispatch = dispatch;
  fixture.method.executor = dispatch == CHTTP_DISPATCH_INLINE ? NULL : &fixture.executor;
  int status = chttp_application_init(&fixture.app, &fixture.config);
  if (status == SALTS_OK) status = chttp_application_start(&fixture.app);
  uint16_t port = 0;
  if (status == SALTS_OK) status = chttp_application_port(&fixture.app, &port);
  if (status == SALTS_OK) snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
  return status;
}
spec("Application generated MethodPlan bindings") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    atomic_init(&fixture.entered, 0); atomic_init(&fixture.release, 0);
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    check_equal(CFlowGen_codec_create(&fixture.contract, &error), DATA_BIND_OK);
    check_equal(databind_8_CFlowGen_4_Calc_3_Add__databind_native_binding(
        &fixture.request, &fixture.response, &fixture.binding, &error), DATA_BIND_OK);
    const DataBindHttpProjectionConfig *projection = data_bind_http_projection_artifact_find(
        &databind_chttp_service_cflow_http_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(data_bind_http_method_plan_compile_service(fixture.contract, "Calc", "Add",
        projection, &fixture.binding, &fixture.plan, &diagnostic), DATA_BIND_OK);
    fixture.execution = *databind_8_CFlowGen_4_Calc_3_Add__databind_execution();
    fixture.execution.invoke = invoke;
    fixture.provider = (salts_configurator_provider_binding){sizeof(fixture.provider),
        SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(BindingComponent),
        NULL, NULL, create, NULL, NULL};
    fixture.deployment = (salts_configurator_deployment){&fixture.provider, NULL, NULL};
    fixture.method = (chttp_http_method){.plan = fixture.plan, .binding = &fixture.binding,
        .execution = &fixture.execution, .executor = &fixture.executor};
    fixture.controller = (chttp_controller){.component_id = "BindingComponent",
        .receiver = cmeta_reflected_data(BindingController), .http_methods = &fixture.method,
        .http_method_count = 1u};
    fixture.config = chttp_application_config_default();
    fixture.config.bindings.method_capacity = 1u;
    fixture.config.components = &fixture.deployment; fixture.config.component_count = 1;
    fixture.config.controllers = &fixture.controller; fixture.config.controller_count = 1;
    fixture.client = (chttp_client_config){.network = fixture.config.http.network,
        .request_capacity = 1, .max_start_line_bytes = 256, .max_header_count = 16,
        .max_header_bytes = 4096, .max_request_body_bytes = 8192,
        .max_response_body_bytes = 8192, .max_informational_responses = 4};
    fixture.executor_live = cflow_executor_worker_init_with_capacity(&fixture.executor, 1, 2);
    check_true(fixture.executor_live);
  }
  after_each() {
    atomic_store_explicit(&fixture.release, 1, memory_order_release);
    if (fixture.thread_live) { sync_thread_join(&fixture.thread); fixture.thread_live = false; }
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    if (fixture.executor_live) {
      check_true(cflow_executor_wait_idle(&fixture.executor));
      cflow_executor_destroy(&fixture.executor);
    }
    data_bind_http_method_plan_free(fixture.plan); data_bind_free(fixture.contract);
  }
  it("binds canonical native execution to the ACE Controller and retains validation errors") {
    check_equal(start(CHTTP_DISPATCH_INLINE), SALTS_OK);
    unsigned int code = 0;
    check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
    check_equal(request("/generated-cflow/0?right=4", &code), SALTS_OK); check_equal(code, 422u);
    check_equal(request("/generated-cflow/3", &code), SALTS_OK); check_equal(code, 400u);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.destroyed, 1); check_equal(atomic_load(&fixture.entered), 1);
  }
  it("executes deferred bindings and drains native finalizers before Bean destruction") {
    check_equal(start(CHTTP_DISPATCH_DEFERRED), SALTS_OK);
    unsigned int code = 0;
    check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.destroyed, 1);
  }
  it("mounts an immutable producer-owned CFlow projection through the same Controller") {
    check_equal(databind_8_CFlowGen_4_Calc_3_Add__databind_cflow_projection(&fixture.projection),
        CFLOW_FUNCTION_PROJECTION_OK);
    fixture.method.execution = NULL; fixture.method.projection = &fixture.projection;
    check_equal(start(CHTTP_DISPATCH_CFLOW), SALTS_OK);
    unsigned int code = 0;
    check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.destroyed, 1);
  }
  it("retains the owner and Beans when close times out with a real invocation in flight") {
    fixture.block = true;
    check_equal(start(CHTTP_DISPATCH_DEFERRED), SALTS_OK);
    check_equal(sync_thread_create(&fixture.thread, request_thread, NULL), SALTS_OK);
    fixture.thread_live = true;
    const uint64_t deadline = monotonic_ms() + 5000u;
    while (!atomic_load_explicit(&fixture.entered, memory_order_acquire) && monotonic_ms() < deadline)
      sync_thread_yield();
    check_equal(atomic_load(&fixture.entered), 1);
    check_equal(chttp_application_close(&fixture.app, 10u), SALTS_ETIMEDOUT);
    check_not_null(fixture.app.impl); check_equal(fixture.destroyed, 0);
    atomic_store_explicit(&fixture.release, 1, memory_order_release);
    sync_thread_join(&fixture.thread); fixture.thread_live = false;
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_null(fixture.app.impl); check_equal(fixture.destroyed, 1);
  }
}
