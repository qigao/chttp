#include <chttp_app/app.h>
#include "chttp_application_worker.h"
#include <http_client/http.h>
#include <http_client/rpc_service.h>
#include <cmeta/data_reflect.h>
#include "chttp_service_cflow.http.h"
#include "chttp_service_cflow.service_native.h"
#include "chttp_service_cflow_native.h"
#include <core/clock.h>
#include <threads/threads.h>
#include <stdatomic.h>
#include <json_parser.h>
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
  _Atomic int worker_calls;
  chttp_protocol protocol;
  bool block;
  bool flow_failure;
  int destroyed, request_status;
  unsigned int response_status;
  char uri[64];
  chttp_client_config client;
  DataBind *contract;
  DataBindHttpMethodPlan *plan;
  DataBindRpcMethodPlan *rpc_plan;
  DataBindNativeTypeBinding request, response;
  DataBindServiceNativeBinding binding;
  DataBindNativeExecution execution;
  cflow_function_typed_adapter_projection projection;
  cmeta_callable flow_adapter;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_http_method method;
  chttp_rpc_method rpc_method;
  chttp_interceptor hook;
  _Atomic int before_calls, after_calls;
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
  if (chttp_application_worker_current(&fixture.app))
    atomic_fetch_add_explicit(&fixture.worker_calls, 1, memory_order_relaxed);
  atomic_store_explicit(&fixture.entered, 1, memory_order_release);
  if (fixture.block)
    while (!atomic_load_explicit(&fixture.release, memory_order_acquire)) sync_thread_yield();
  return databind_8_CFlowGen_4_Calc_3_Add__databind_execution()->invoke(NULL, result, params, count);
}
static bool flow_invoke(const cmeta_callable *self, void *result, const void *const *args) {
  (void)self;
  if (!chttp_application_worker_current(&fixture.app) ||
      cflow_executor_is_current(&fixture.executor)) return false;
  atomic_fetch_add_explicit(&fixture.worker_calls, 1, memory_order_relaxed);
  atomic_store_explicit(&fixture.entered, 1, memory_order_release);
  if (fixture.block)
    while (!atomic_load_explicit(&fixture.release, memory_order_acquire)) sync_thread_yield();
  return !fixture.flow_failure && fixture.flow_adapter.invoke(&fixture.flow_adapter, result, args);
}
static void setup_worker_flow(void) {
  check_equal(databind_8_CFlowGen_4_Calc_3_Add__databind_cflow_projection(&fixture.projection),
      CFLOW_FUNCTION_PROJECTION_OK);
  fixture.flow_adapter = fixture.projection.callable;
  fixture.projection.callable.invoke = flow_invoke;
  check_true(cflow_function_typed_adapter_projection_valid(&fixture.projection));
  fixture.method.dispatch = CHTTP_DISPATCH_CFLOW;
  fixture.method.execution = NULL; fixture.method.projection = &fixture.projection;
}
static int request(const char *target, unsigned int *code) {
  chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  int status = chttp_client_init(&client, &fixture.client);
  if (status != SALTS_OK) return status;
  const chttp_options options = {.connection_uri = fixture.uri, .authority = "test",
      .target = target, .protocol = fixture.protocol, .timeout_ms = 5000u};
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

static int start_worker(void) {
  fixture.controller.http_methods = NULL; fixture.controller.http_method_count = 0u;
  fixture.method.executor = NULL;
  int status = chttp_application_init(&fixture.app, &fixture.config);
  const chttp_application_work_config limits = {1u, 2u, 32768u, 8192u};
  if (status == SALTS_OK) status = chttp_application_worker_prepare(&fixture.app, &limits);
  if (status == SALTS_OK) status = chttp_application_worker_mount_http(&fixture.app,
      "BindingComponent", cmeta_reflected_data(BindingController), &fixture.method);
  if (status == SALTS_OK && fixture.rpc_plan) status = chttp_application_worker_mount_rpc(&fixture.app,
      "BindingComponent", cmeta_reflected_data(BindingController), &fixture.rpc_method);
  if (status == SALTS_OK) status = chttp_application_start(&fixture.app);
  uint16_t port = 0u;
  if (status == SALTS_OK) status = chttp_application_port(&fixture.app, &port);
  if (status == SALTS_OK) snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
  return status;
}

static cmeta_status rpc_before(void *self, const chttp_exchange *exchange, bool *proceed) {
  BindingController *controller = self;
  (void)proceed;
  if (controller->marker != 42) return CMETA_TYPE_MISMATCH;
  if (strcmp(exchange->request->path, "/rpc") == 0) {
    if (exchange->operation != NULL) return CMETA_TYPE_MISMATCH;
  } else if (!cmeta_function_desc_valid(exchange->operation)) return CMETA_TYPE_MISMATCH;
  atomic_fetch_add(&fixture.before_calls, 1);
  return CMETA_OK;
}
static void rpc_after(void *self, const chttp_exchange *exchange, const chttp_exchange_result *result) {
  (void)self; (void)exchange; (void)result;
  atomic_fetch_add(&fixture.after_calls, 1);
}
static void setup_rpc(void) {
  static const DataBindRpcFieldProjection fields[] = {
      {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_INGRESS, "left", "left", 0u},
      {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_INGRESS, "right", "right", 1u},
      {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_EGRESS, "sum", "sum", 0u}};
  DataBindRpcProjectionConfig projection = DATA_BIND_RPC_PROJECTION_CONFIG_INIT;
  DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  projection.wire_method = "Calc.Add";
  projection.fields = fields; projection.field_count = sizeof(fields) / sizeof(fields[0]);
  check_equal(data_bind_rpc_method_plan_compile_service(fixture.contract, "Calc", "Add",
      &projection, &fixture.binding, &fixture.rpc_plan, &diagnostic), DATA_BIND_OK);
  fixture.rpc_method = (chttp_rpc_method){.path = "/rpc", .plan = fixture.rpc_plan,
      .binding = &fixture.binding, .execution = &fixture.execution};
  fixture.controller.rpc_methods = &fixture.rpc_method; fixture.controller.rpc_method_count = 1u;
  fixture.config.rpc.method_capacity = 1u;
  fixture.hook = (chttp_interceptor){"BindingComponent", cmeta_reflected_data(BindingController),
      rpc_before, rpc_after, NULL};
  fixture.config.interceptors = &fixture.hook; fixture.config.interceptor_count = 1u;
}
static int rpc_request(chttp_protocol protocol, chttp_client_rpc_outcome_kind *kind, uint32_t *sum) {
  chttp_client client = {0};
  int status = chttp_client_init(&client, &fixture.client);
  if (status != SALTS_OK) return status;
  AddRequest_t input = {.left = 3u, .right = 4u};
  AddResponse_t output = {0};
  unsigned char workspace[4096];
  DataBindNativeOptions native = DATA_BIND_NATIVE_OPTIONS_INIT;
  native.workspace = workspace; native.workspace_bytes = sizeof(workspace);
  native.max_depth = 16u; native.max_items = 64u; native.max_owned_bytes = 1024u;
  chttp_client_rpc_call_options options = CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
  options.connection_uri = fixture.uri; options.authority = "test"; options.target = "/rpc";
  options.protocol = protocol; options.request_id = 1u; options.deadline_ms = 5000u;
  options.method_plan = fixture.rpc_plan; options.native_binding = &fixture.binding;
  options.native_options = &native;
  options.request = &input; options.request_bytes = sizeof(input);
  options.response = &output; options.response_bytes = sizeof(output);
  chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
  crpc_error error = {0};
  status = chttp_client_rpc_call(&client, &options, &outcome, &error);
  *kind = outcome.kind; *sum = output.sum;
  const int closed = chttp_client_destroy(&client, 5000u);
  return closed != SALTS_OK ? closed : status;
}
static int raw_rpc(const char *body, chttp_response *response) {
  chttp_client client = {0}; chttp_error error = {0};
  int status = chttp_client_init(&client, &fixture.client);
  if (status != SALTS_OK) return status;
  const chttp_options options = {.connection_uri = fixture.uri, .authority = "test", .target = "/rpc",
      .protocol = fixture.protocol, .body = body, .body_size = strlen(body), .timeout_ms = 5000u};
  status = chttp_post(&client, &options, response, &error);
  const int closed = chttp_client_destroy(&client, 5000u);
  return closed == SALTS_OK ? status : closed;
}
static void rpc_request_thread(void *unused) {
  (void)unused;
  chttp_client_rpc_outcome_kind kind = CHTTP_CLIENT_RPC_NONE; uint32_t sum = 0u;
  fixture.request_status = rpc_request(fixture.protocol, &kind, &sum);
}
static void exercise_rpc(chttp_protocol protocol) {
  setup_rpc();
  check_equal(start(CHTTP_DISPATCH_INLINE), SALTS_OK);
  chttp_client_rpc_outcome_kind kind = CHTTP_CLIENT_RPC_NONE;
  uint32_t sum = 0u;
  check_equal(rpc_request(protocol, &kind, &sum), SALTS_OK);
  check_equal(kind, CHTTP_CLIENT_RPC_SUCCESS); check_equal(sum, (uint32_t)7u);
  unsigned int code = 0u;
  check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
  check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
  check_equal(fixture.destroyed, 1); check_equal(atomic_load(&fixture.entered), 1);
  check_equal(atomic_load(&fixture.before_calls), 2); check_equal(atomic_load(&fixture.after_calls), 2);
}
spec("Application generated MethodPlan bindings") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    atomic_init(&fixture.entered, 0); atomic_init(&fixture.release, 0);
    atomic_init(&fixture.worker_calls, 0);
    atomic_init(&fixture.before_calls, 0); atomic_init(&fixture.after_calls, 0);
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
    data_bind_rpc_method_plan_free(fixture.rpc_plan);
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
  group("private Application-owned worker generated HTTP") {
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      it("retains native binding behavior on the owned worker for protocol %d", protocol) {
        fixture.protocol = (chttp_protocol)protocol;
        check_equal(start_worker(), SALTS_OK);
        unsigned int code = 0u;
        check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
        check_equal(request("/generated-cflow/0?right=4", &code), SALTS_OK); check_equal(code, 422u);
        check_equal(request("/generated-cflow/3", &code), SALTS_OK); check_equal(code, 400u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
      }
    }
    for (int flow = 0; flow <= 1; ++flow) {
      it("holds the generated receiver until canceled direct/CFlow method exits (CFlow=%d)", flow) {
        if (flow) setup_worker_flow();
        fixture.protocol = CHTTP_HTTP_2; fixture.block = true;
        fixture.config.deadlines.handler_ms = 100u;
        check_equal(start_worker(), SALTS_OK);
        check_equal(sync_thread_create(&fixture.thread, request_thread, NULL), SALTS_OK);
        fixture.thread_live = true;
        const uint64_t started = monotonic_ms();
        while (!atomic_load_explicit(&fixture.entered, memory_order_acquire) &&
            monotonic_ms() - started < 5000u) sync_thread_yield();
        check_equal(atomic_load(&fixture.entered), 1);
        check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
        check_not_equal(fixture.request_status, SALTS_OK);
        check_not_equal(fixture.request_status, SALTS_ETIMEDOUT);
        check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
        check_not_equal(fixture.app.impl, NULL); check_equal(fixture.destroyed, 0);
        atomic_store_explicit(&fixture.release, 1, memory_order_release);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
      }
      }
  }
  group("private Application-owned synchronous CFlow provider") {
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      it("binds/evaluates/serializes on one owned worker for protocol %d", protocol) {
        setup_worker_flow(); fixture.protocol = (chttp_protocol)protocol;
        check_equal(start_worker(), SALTS_OK);
        unsigned int code = 0u;
        check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
        check_equal(request("/generated-cflow/0?right=4", &code), SALTS_OK); check_equal(code, 422u);
        check_equal(request("/generated-cflow/3", &code), SALTS_OK); check_equal(code, 400u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
      }
      it("returns 500 on failed CFlow evaluation and drains for protocol %d", protocol) {
        setup_worker_flow(); fixture.protocol = (chttp_protocol)protocol; fixture.flow_failure = true;
        check_equal(start_worker(), SALTS_OK);
        unsigned int code = 0u;
        check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 500u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
      }
      it("cleans failed output serialization before the next CFlow call for protocol %d", protocol) {
        setup_worker_flow(); fixture.protocol = (chttp_protocol)protocol;
        fixture.config.bindings.max_response_body_bytes = 1u;
        check_equal(start_worker(), SALTS_OK);
        unsigned int code = 0u;
        check_equal(request("/generated-cflow/3?right=44", &code), SALTS_OK); check_equal(code, 500u);
        check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 2); check_equal(fixture.destroyed, 1);
      }
    }
    it("rejects an invalid projection before route publication without consuming the slot") {
      setup_worker_flow();
      fixture.controller.http_methods = NULL; fixture.controller.http_method_count = 0u;
      fixture.method.executor = NULL;
      check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
      const chttp_application_work_config limits = {1u, 2u, 32768u, 8192u};
      check_equal(chttp_application_worker_prepare(&fixture.app, &limits), SALTS_OK);
      fixture.projection.output_type = &cmeta_type_int;
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method), SALTS_EINVAL);
      fixture.projection.output_type = fixture.response.data->storage_type;
      fixture.method.executor = &fixture.executor;
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method), SALTS_ENOTSUP);
      fixture.method.executor = NULL;
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method), SALTS_OK);
      check_equal(chttp_application_start(&fixture.app), SALTS_OK);
      uint16_t port = 0u;
      check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
      snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
      unsigned int code = 0u;
      check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
    }
  }
  group("private worker RPC envelope") {
    it("retains the RPC registry and receiver while a canceled native method still runs") {
      setup_rpc(); fixture.protocol = CHTTP_HTTP_2; fixture.block = true;
      fixture.controller.rpc_methods = NULL; fixture.controller.rpc_method_count = 0u;
      fixture.config.interceptors = NULL; fixture.config.interceptor_count = 0u;
      fixture.config.deadlines.handler_ms = 100u;
      check_equal(start_worker(), SALTS_OK);
      check_equal(sync_thread_create(&fixture.thread, rpc_request_thread, NULL), SALTS_OK);
      fixture.thread_live = true;
      const uint64_t started = monotonic_ms();
      while (!atomic_load_explicit(&fixture.entered, memory_order_acquire) &&
          monotonic_ms() - started < 5000u) sync_thread_yield();
      check_equal(atomic_load(&fixture.entered), 1);
      check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
      check_not_equal(fixture.request_status, SALTS_OK);
      check_not_equal(fixture.request_status, SALTS_ETIMEDOUT);
      check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
      check_not_equal(fixture.app.impl, NULL); check_equal(fixture.destroyed, 0);
      atomic_store_explicit(&fixture.release, 1, memory_order_release);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
    }
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      it("executes typed RPC and generated HTTP on the same owned pool for protocol %d", protocol) {
        setup_rpc(); fixture.protocol = (chttp_protocol)protocol;
        fixture.controller.rpc_methods = NULL; fixture.controller.rpc_method_count = 0u;
        fixture.config.interceptors = NULL; fixture.config.interceptor_count = 0u;
        check_equal(start_worker(), SALTS_OK);
        chttp_client_rpc_outcome_kind kind = CHTTP_CLIENT_RPC_NONE; uint32_t sum = 0u;
        check_equal(rpc_request(fixture.protocol, &kind, &sum), SALTS_OK);
        check_equal(kind, CHTTP_CLIENT_RPC_SUCCESS); check_equal(sum, (uint32_t)7u);
        unsigned int code = 0u;
        check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 2); check_equal(fixture.destroyed, 1);
      }
      it("preserves batch results, notifications and protocol errors for protocol %d", protocol) {
        setup_rpc(); fixture.protocol = (chttp_protocol)protocol;
        fixture.controller.rpc_methods = NULL; fixture.controller.rpc_method_count = 0u;
        fixture.config.interceptors = NULL; fixture.config.interceptor_count = 0u;
        check_equal(start_worker(), SALTS_OK);
        const char batch[] = "[{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[3,4],\"id\":1},"
            "{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":{\"left\":5,\"right\":6}},"
            "{\"jsonrpc\":\"2.0\",\"method\":\"Unknown\",\"id\":2}]";
        chttp_response response = {0};
        check_equal(raw_rpc(batch, &response), SALTS_OK); check_equal(response.status_code, 200u);
        json_value_t *root = json_parse(response.body, response.body_size);
        check_not_null(root); check_equal(json_type(root), JSON_ARRAY);
        check_equal(json_array_size(root), 2u);
        check_equal(json_number(json_object_get(json_array_get(root, 0u), "result")), 7.0);
        check_equal(json_number(json_object_get(json_array_get(root, 0u), "id")), 1.0);
        check_equal(json_number(json_object_get(json_object_get(json_array_get(root, 1u), "error"), "code")), -32601.0);
        json_free(root); chttp_response_destroy(&response);
        check_equal(raw_rpc("{", &response), SALTS_OK); check_equal(response.status_code, 200u);
        root = json_parse(response.body, response.body_size);
        check_not_null(root);
        check_equal(json_number(json_object_get(json_object_get(root, "error"), "code")), -32700.0);
        json_free(root); chttp_response_destroy(&response);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 2); check_equal(fixture.destroyed, 1);
      }
      it("emits an empty 204 for a notification-only batch on protocol %d", protocol) {
        setup_rpc(); fixture.protocol = (chttp_protocol)protocol;
        fixture.controller.rpc_methods = NULL; fixture.controller.rpc_method_count = 0u;
        fixture.config.interceptors = NULL; fixture.config.interceptor_count = 0u;
        check_equal(start_worker(), SALTS_OK);
        const char batch[] = "[{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[3,4]},"
            "{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[5,6]}]";
        chttp_response response = {0};
        check_equal(raw_rpc(batch, &response), SALTS_OK); check_equal(response.status_code, 204u);
        check_equal(response.body_size, 0u); check_null(chttp_response_header(&response, "Content-Type"));
        chttp_response_destroy(&response);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 2); check_equal(fixture.destroyed, 1);
      }
    }
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
  it("shares an ACE Controller and HTTP Interceptors between typed H1 RPC and HTTP bindings") {
    exercise_rpc(CHTTP_HTTP_1_1);
  }
  it("binds typed H2 RPC to the ACE Controller and drains it with the same Application") {
    exercise_rpc(CHTTP_HTTP_2);
  }
}
