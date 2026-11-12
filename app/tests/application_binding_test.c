#include <chttp_app/app.h>
#include "chttp_application_worker.h"
#include <http_client/http.h>
#include <http_client/rpc_service.h>
#include <cmeta/data_reflect.h>
#include "chttp_service_cflow.http.h"
#include "chttp_service_cflow.service_native.h"
#include "chttp_service_cflow_native.h"
#include "chttp_service_cflow_unsupported.service_native.h"
#include "chttp_service_cflow_unsupported_native.h"
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
  bool invoke_failure, hook_failure, typed_method;
  int native_status;
  uint32_t reject_left;
  chttp_application_method_interceptor method_hooks[2];
  size_t method_hook_count;
  _Atomic int method_before, method_after, method_errors, method_invalid, notifications;
  int trace[64];
  size_t trace_count;
  int last_native, last_kind, last_status, last_invoked, last_rejected;
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
  if (fixture.invoke_failure) return false;
  if (fixture.native_status) { *(int *)result = fixture.native_status; return true; }
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
static void method_trace(int event) {
  if (fixture.trace_count < sizeof(fixture.trace) / sizeof(fixture.trace[0]))
    fixture.trace[fixture.trace_count++] = event;
  else atomic_fetch_add(&fixture.method_invalid, 1);
}
static bool method_valid(void *self, const chttp_method_call *call) {
  return self && ((BindingController *)self)->marker == 42 &&
      chttp_application_worker_current(&fixture.app) && call && call->http && !call->http->session &&
      cmeta_function_desc_equal(call->operation, fixture.binding.function) && call->input &&
      call->input->param_count == (fixture.typed_method ? 3u : 2u) &&
      call->input->params[0] && call->input->params[1] &&
      (!call->rpc_method || strcmp(call->rpc_method, "Calc.Add") == 0) &&
      (!fixture.config.http.session_capacity || (call->session && !chttp_session_get(call->session, "n")));
}
static cmeta_status method_before(void *self, const chttp_method_call *call, bool *proceed, int index) {
  method_trace(index);
  atomic_fetch_add(&fixture.method_before, 1);
  if (!method_valid(self, call)) {
    atomic_fetch_add(&fixture.method_invalid, 1); return CMETA_TYPE_MISMATCH;
  }
  if (index == 1 && call->notification) atomic_fetch_add(&fixture.notifications, 1);
  if (index == 2) {
    if (fixture.hook_failure) return CMETA_TYPE_MISMATCH;
    const AddRequest_t *input = call->input->params[0];
    if (fixture.reject_left && input->left == fixture.reject_left)
      return chttp_method_call_reject(call, proceed);
  }
  return CMETA_OK;
}
static cmeta_status method_before0(void *self, const chttp_method_call *call, bool *proceed) {
  return method_before(self, call, proceed, 1);
}
static cmeta_status method_before1(void *self, const chttp_method_call *call, bool *proceed) {
  return method_before(self, call, proceed, 2);
}
static void method_after(void *self, const chttp_method_call *call, const chttp_method_result *result, int event) {
  method_trace(event); atomic_fetch_add(&fixture.method_after, 1);
  if (!method_valid(self, call) || !result->invoked || result->rejected || result->status != DATA_BIND_OK ||
      result->outcome.kind != DATA_BIND_BINDING_OUTCOME_SUCCESS || !result->output) {
    atomic_fetch_add(&fixture.method_invalid, 1); return;
  }
  if (fixture.typed_method) { atomic_fetch_add(&fixture.method_invalid, 1); return; }
  const AddRequest_t *input = call->input->params[0];
  const AddResponse_t *output = result->output->params[1];
  if (output->sum != input->left + input->right) atomic_fetch_add(&fixture.method_invalid, 1);
}
static void method_after0(void *self, const chttp_method_call *call, const chttp_method_result *result) {
  method_after(self, call, result, 4);
}
static void method_after1(void *self, const chttp_method_call *call, const chttp_method_result *result) {
  method_after(self, call, result, 3);
}
static void method_error(void *self, const chttp_method_call *call, cmeta_status status, int event) {
  method_trace(event); atomic_fetch_add(&fixture.method_errors, 1);
  if (!method_valid(self, call) || status == CMETA_OK) atomic_fetch_add(&fixture.method_invalid, 1);
  fixture.last_native = call->result->native_status;
  fixture.last_kind = (int)call->result->outcome.kind;
  fixture.last_status = (int)call->result->status;
  fixture.last_invoked = call->result->invoked;
  fixture.last_rejected = call->result->rejected;
  if (fixture.typed_method) {
    const databind_7_FlowBad_4_Calc_7_Failing__error *error = call->input->params[2];
    if (!error || error->kind != databind_7_FlowBad_4_Calc_7_Failing__ERROR_1 ||
        error->payload.error_1.code != 77u) atomic_fetch_add(&fixture.method_invalid, 1);
  }
}
static void method_error0(void *self, const chttp_method_call *call, cmeta_status status) {
  method_error(self, call, status, 6);
}
static void method_error1(void *self, const chttp_method_call *call, cmeta_status status) {
  method_error(self, call, status, 5);
}
static void setup_method_hooks(void) {
  fixture.method_hooks[0] = (chttp_application_method_interceptor){"BindingComponent",
      cmeta_reflected_data(BindingController), method_before0, method_after0, method_error0};
  fixture.method_hooks[1] = (chttp_application_method_interceptor){"BindingComponent",
      cmeta_reflected_data(BindingController), method_before1, method_after1, method_error1};
  fixture.method_hook_count = 2u;
}
static bool DATA_BIND_NATIVE_CALL typed_invoke(void *context, void *result,
    void *const *params, size_t count) {
  if (!context || ((BindingController *)context)->marker != 42 || count != 3u ||
      !chttp_application_worker_current(&fixture.app)) return false;
  atomic_fetch_add(&fixture.worker_calls, 1);
  databind_7_FlowBad_4_Calc_7_Failing__error *error = params[2];
  if (databind_7_FlowBad_4_Calc_7_Failing__error_select(error,
          databind_7_FlowBad_4_Calc_7_Failing__ERROR_1) != DATA_BIND_OK) return false;
  error->payload.error_1.code = 77u;
  *(int *)result = 0;
  return true;
}
static void setup_typed_method(void) {
  fixture.typed_method = true;
  data_bind_http_method_plan_free(fixture.plan); fixture.plan = NULL;
  data_bind_free(fixture.contract); fixture.contract = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  check_equal(FlowBad_codec_create(&fixture.contract, &error), DATA_BIND_OK);
  check_equal(databind_7_FlowBad_4_Calc_7_Failing__databind_native_binding(
      &fixture.request, &fixture.response, &fixture.binding, &error), DATA_BIND_OK);
  static const DataBindHttpFieldProjection fields[] = {
      {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS, "value", DATA_BIND_HTTP_PATH, "value", 0u},
      {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_EGRESS, "value", DATA_BIND_HTTP_RESPONSE_BODY, "value", 0u}};
  static const DataBindHttpErrorMapping errors[] = {{sizeof(DataBindHttpErrorMapping), "Fault", 409}};
  DataBindHttpProjectionConfig http = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
  http.method = "GET"; http.route = "/typed/{value}";
  http.fields = fields; http.field_count = 2u; http.errors = errors; http.error_count = 1u;
  check_equal(data_bind_http_method_plan_compile_service(fixture.contract, "Calc", "Failing", &http,
      &fixture.binding, &fixture.plan, &diagnostic), DATA_BIND_OK);
  static const DataBindRpcFieldProjection rpc_fields[] = {
      {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_INGRESS, "value", "value", 0u},
      {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_EGRESS, "value", "value", 0u}};
  static const DataBindRpcErrorMapping rpc_errors[] = {{sizeof(DataBindRpcErrorMapping), "Fault", -32077}};
  DataBindRpcProjectionConfig rpc = DATA_BIND_RPC_PROJECTION_CONFIG_INIT;
  rpc.wire_method = "Calc.Add"; rpc.fields = rpc_fields; rpc.field_count = 2u;
  rpc.errors = rpc_errors; rpc.error_count = 1u;
  check_equal(data_bind_rpc_method_plan_compile_service(fixture.contract, "Calc", "Failing", &rpc,
      &fixture.binding, &fixture.rpc_plan, &diagnostic), DATA_BIND_OK);
  fixture.execution = *databind_7_FlowBad_4_Calc_7_Failing__databind_execution();
  fixture.execution.invoke = typed_invoke;
  fixture.method.plan = fixture.plan;
  fixture.rpc_method = (chttp_rpc_method){.path = "/rpc", .plan = fixture.rpc_plan,
      .binding = &fixture.binding, .execution = &fixture.execution};
  fixture.config.rpc.method_capacity = 1u;
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
      "BindingComponent", cmeta_reflected_data(BindingController), &fixture.method, fixture.contract,
      fixture.method_hooks, fixture.method_hook_count);
  if (status == SALTS_OK && fixture.rpc_plan) status = chttp_application_worker_mount_rpc(&fixture.app,
      "BindingComponent", cmeta_reflected_data(BindingController), &fixture.rpc_method, fixture.contract,
      fixture.method_hooks, fixture.method_hook_count);
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
static void check_trace(const int *expected, size_t count) {
  check_equal(fixture.trace_count, count);
  check_equal(fixture.trace, expected, count * sizeof(*expected));
}
spec("Application generated MethodPlan bindings") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    atomic_init(&fixture.entered, 0); atomic_init(&fixture.release, 0);
    atomic_init(&fixture.worker_calls, 0);
    atomic_init(&fixture.method_before, 0); atomic_init(&fixture.method_after, 0);
    atomic_init(&fixture.method_errors, 0); atomic_init(&fixture.method_invalid, 0);
    atomic_init(&fixture.notifications, 0);
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
    check_equal(atomic_load(&fixture.method_invalid), 0);
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
      it("unwinds structured typed errors with mapped HTTP/RPC responses before cleanup on protocol %d", protocol) {
        setup_method_hooks(); setup_typed_method(); fixture.protocol = (chttp_protocol)protocol;
        check_equal(start_worker(), SALTS_OK);
        unsigned int code = 0u;
        check_equal(request("/typed/7", &code), SALTS_OK); check_equal(code, 409u);
        chttp_response response = {0};
        check_equal(raw_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[7],\"id\":1}", &response), SALTS_OK);
        json_value_t *root = json_parse(response.body, response.body_size);
        check_not_null(root);
        check_equal(json_number(json_object_get(json_object_get(root, "error"), "code")), -32077.0);
        json_free(root); chttp_response_destroy(&response);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.method_errors), 4); check_equal(atomic_load(&fixture.method_after), 0);
        check_equal(fixture.last_kind, (int)DATA_BIND_BINDING_OUTCOME_TYPED_ERROR);
        check_equal(fixture.last_native, 0); check_equal(fixture.last_status, (int)DATA_BIND_OK);
        check_equal(atomic_load(&fixture.worker_calls), 2);
      }
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
        setup_method_hooks();
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
        check_equal(atomic_load(&fixture.method_before), 2);
        check_equal(atomic_load(&fixture.method_after), 0); check_equal(atomic_load(&fixture.method_errors), 0);
        check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
        check_not_equal(fixture.app.impl, NULL); check_equal(fixture.destroyed, 0);
        atomic_store_explicit(&fixture.release, 1, memory_order_release);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
        check_equal(atomic_load(&fixture.method_after), 2); check_equal(atomic_load(&fixture.method_errors), 0);
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
        setup_method_hooks();
        setup_worker_flow(); fixture.protocol = (chttp_protocol)protocol; fixture.flow_failure = true;
        check_equal(start_worker(), SALTS_OK);
        unsigned int code = 0u;
        check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 500u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
        check_equal(atomic_load(&fixture.method_errors), 2); check_equal(atomic_load(&fixture.method_after), 0);
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
          cmeta_reflected_data(BindingController), &fixture.method, fixture.contract, NULL, 0u), SALTS_EINVAL);
      fixture.projection.output_type = fixture.response.data->storage_type;
      fixture.method.executor = &fixture.executor;
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method, fixture.contract, NULL, 0u), SALTS_ENOTSUP);
      fixture.method.executor = NULL;
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method, fixture.contract, NULL, 0u), SALTS_OK);
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
  group("private generated method Interceptors") {
    it("exposes a const detached Session without handing method hooks its mutable request handle") {
      setup_method_hooks();
      fixture.config.http.session_capacity = 2u; fixture.config.http.session_entry_capacity = 2u;
      fixture.config.http.max_session_key_bytes = 8u; fixture.config.http.max_session_value_bytes = 8u;
      fixture.config.http.session_idle_timeout_ms = 60000u;
      fixture.config.http.session_cookie_name = "method_sid";
      check_equal(start_worker(), SALTS_OK);
      unsigned int code = 0u;
      check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(atomic_load(&fixture.method_after), 2); check_equal(atomic_load(&fixture.method_invalid), 0);
    }
    it("prevalidates every canonical Bean hook and copies descriptors before publishing") {
      setup_method_hooks();
      fixture.controller.http_methods = NULL; fixture.controller.http_method_count = 0u;
      fixture.method.executor = NULL;
      check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
      const chttp_application_work_config limits = {1u, 2u, 32768u, 8192u};
      check_equal(chttp_application_worker_prepare(&fixture.app, &limits), SALTS_OK);
      fixture.method_hooks[1].component_id = "missing";
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method, fixture.contract, fixture.method_hooks, 2u), SALTS_ENOENT);
      setup_method_hooks();
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method, fixture.contract, fixture.method_hooks, 17u), SALTS_EINVAL);
      fixture.method_hooks[1].before = NULL; fixture.method_hooks[1].after = NULL;
      fixture.method_hooks[1].on_error = NULL;
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method, fixture.contract, fixture.method_hooks, 2u), SALTS_EINVAL);
      setup_method_hooks();
      check_equal(chttp_application_worker_mount_http(&fixture.app, "BindingComponent",
          cmeta_reflected_data(BindingController), &fixture.method, fixture.contract, fixture.method_hooks, 2u), SALTS_OK);
      memset(fixture.method_hooks, 0, sizeof(fixture.method_hooks));
      check_equal(chttp_application_start(&fixture.app), SALTS_OK);
      uint16_t port = 0u;
      check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
      snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
      unsigned int code = 0u;
      check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      const int trace[] = {1,2,3,4}; check_trace(trace, 4u);
    }
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      for (int flow = 0; flow <= 1; ++flow) {
        it("unwinds live direct/CFlow output on workers (protocol=%d CFlow=%d)", protocol, flow) {
          setup_method_hooks(); if (flow) setup_worker_flow();
          fixture.protocol = (chttp_protocol)protocol;
          check_equal(start_worker(), SALTS_OK);
          unsigned int code = 0u;
          check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
          check_equal(request("/generated-cflow/0?right=4", &code), SALTS_OK); check_equal(code, 422u);
          check_equal(request("/generated-cflow/3", &code), SALTS_OK); check_equal(code, 400u);
          check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
          const int trace[] = {1, 2, 3, 4}; check_trace(trace, 4u);
          check_equal(atomic_load(&fixture.method_before), 2); check_equal(atomic_load(&fixture.method_after), 2);
          check_equal(atomic_load(&fixture.method_errors), 0); check_equal(atomic_load(&fixture.worker_calls), 1);
        }
      }
      it("rejects a bound method before native effects and unwinds entered errors on protocol %d", protocol) {
        setup_method_hooks(); fixture.reject_left = 9u; fixture.protocol = (chttp_protocol)protocol;
        check_equal(start_worker(), SALTS_OK);
        unsigned int code = 0u;
        check_equal(request("/generated-cflow/9?right=4", &code), SALTS_OK); check_equal(code, 403u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        const int trace[] = {1, 2, 5, 6}; check_trace(trace, 4u);
        check_equal(atomic_load(&fixture.worker_calls), 0); check_equal(fixture.last_invoked, 0);
        check_equal(fixture.last_rejected, 1); check_equal(atomic_load(&fixture.method_after), 0);
      }
      for (int cause = 0; cause < 3; ++cause) {
        it("distinguishes native/bridge/hook failure (protocol=%d cause=%d)", protocol, cause) {
          setup_method_hooks(); fixture.protocol = (chttp_protocol)protocol;
          fixture.native_status = cause == 0 ? 17 : 0;
          fixture.invoke_failure = cause == 1; fixture.hook_failure = cause == 2;
          check_equal(start_worker(), SALTS_OK);
          unsigned int code = 0u;
          check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 500u);
          check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
          const int trace[] = {1, 2, 5, 6}; check_trace(trace, 4u);
          check_equal(atomic_load(&fixture.method_errors), 2); check_equal(atomic_load(&fixture.method_after), 0);
          check_equal(fixture.last_invoked, cause == 2 ? 0 : 1);
          check_equal(fixture.last_native, cause == 0 ? 17 : 0);
          check_equal(fixture.last_kind, cause == 0 ? (int)DATA_BIND_BINDING_OUTCOME_NATIVE_STATUS : 0);
          if (cause == 1) check_equal(fixture.last_status, (int)DATA_BIND_ERR_RUNTIME);
        }
      }
      it("reports output staging failure before a later successful call on protocol %d", protocol) {
        setup_method_hooks(); fixture.protocol = (chttp_protocol)protocol;
        fixture.config.bindings.max_response_body_bytes = 1u;
        check_equal(start_worker(), SALTS_OK);
        unsigned int code = 0u;
        check_equal(request("/generated-cflow/3?right=44", &code), SALTS_OK); check_equal(code, 500u);
        check_equal(request("/generated-cflow/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        const int trace[] = {1, 2, 5, 6, 1, 2, 3, 4}; check_trace(trace, 8u);
        check_equal(atomic_load(&fixture.method_errors), 2); check_equal(atomic_load(&fixture.method_after), 2);
      }
      it("isolates each RPC batch method, rejected notification and invalid params on protocol %d", protocol) {
        setup_rpc(); setup_method_hooks(); fixture.reject_left = 9u; fixture.protocol = (chttp_protocol)protocol;
        fixture.config.rpc.max_batch_items = 5u;
        fixture.controller.rpc_methods = NULL; fixture.controller.rpc_method_count = 0u;
        fixture.config.interceptors = NULL; fixture.config.interceptor_count = 0u;
        check_equal(start_worker(), SALTS_OK);
        const char batch[] = "[{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[3,4],\"id\":1},"
            "{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[5,6]},"
            "{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[9,4],\"id\":2},"
            "{\"jsonrpc\":\"2.0\",\"method\":\"Unknown\",\"id\":3},"
            "{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[0,4],\"id\":4}]";
        chttp_response response = {0};
        check_equal(raw_rpc(batch, &response), SALTS_OK); check_equal(response.status_code, 200u);
        json_value_t *root = json_parse(response.body, response.body_size);
        check_not_null(root); check_equal(json_array_size(root), 4u);
        check_equal(json_number(json_object_get(json_array_get(root, 0u), "result")), 7.0);
        check_equal(json_number(json_object_get(json_object_get(json_array_get(root, 1u), "error"), "code")), -32003.0);
        check_equal(json_number(json_object_get(json_object_get(json_array_get(root, 2u), "error"), "code")), -32601.0);
        check_equal(json_number(json_object_get(json_object_get(json_array_get(root, 3u), "error"), "code")), -32602.0);
        json_free(root); chttp_response_destroy(&response);
        const char notifications[] = "[{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[9,4]},"
            "{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[3,4]}]";
        check_equal(raw_rpc(notifications, &response), SALTS_OK); check_equal(response.status_code, 204u);
        check_equal(response.body_size, 0u); check_null(chttp_response_header(&response, "Content-Type"));
        chttp_response_destroy(&response);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        const int trace[] = {1,2,3,4, 1,2,3,4, 1,2,5,6, 1,2,5,6, 1,2,3,4}; check_trace(trace, 20u);
        check_equal(atomic_load(&fixture.method_before), 10); check_equal(atomic_load(&fixture.method_after), 6);
        check_equal(atomic_load(&fixture.method_errors), 4); check_equal(atomic_load(&fixture.notifications), 3);
        check_equal(atomic_load(&fixture.worker_calls), 3);
      }
    }
  }
  group("private worker RPC envelope") {
    it("retains the RPC registry and receiver while a canceled native method still runs") {
      setup_method_hooks();
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
      check_equal(atomic_load(&fixture.method_after), 0); check_equal(atomic_load(&fixture.method_errors), 0);
      check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
      check_not_equal(fixture.app.impl, NULL); check_equal(fixture.destroyed, 0);
      atomic_store_explicit(&fixture.release, 1, memory_order_release);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(atomic_load(&fixture.worker_calls), 1); check_equal(fixture.destroyed, 1);
      check_equal(atomic_load(&fixture.method_after), 2); check_equal(atomic_load(&fixture.method_errors), 0);
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
