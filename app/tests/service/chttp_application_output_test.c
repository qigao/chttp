#include "chttp_application_worker.h"
#include "chttp_service_plugin_probe.h"
#include "chttp_worker_output.service_native.h"
#include "chttp_worker_output_native.h"
#include "chttp_worker_output_support.h"
#include <data_bind_plugin_execution.h>
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include <json_parser.h>
#include <json_cserde_reader.h>
#include <threads/threads.h>
#include <core/clock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "tinytest.h"

typedef struct OutputController { int marker; } OutputController;
cmeta_reflect_data(OutputController, "test.OutputController", cmeta_field(int, marker));
cmeta_component_empty(OutputComponent);
static const char EXPECTED_JSON[] =
    "{\"id\":18446744073709551615,\"child\":{\"number\":1},\"note\":\"a\\\"\\\\\\n\\u0000z\"}";
typedef struct observed_completion {
  int hook, kind, native_status, adapter_status;
  DataBindStatus status;
  cmeta_status interceptor_status;
  bool invoked, rejected, notification;
  uint64_t rpc_id;
  char rpc_method[32], typed_error[32], path[32];
  chttp_response_outcome response;
} observed_completion;
static struct {
  chttp_application app;
  chttp_application_config config;
  chttp_application_work_config work_config;
  chttp_client_config client;
  chttp_protocol protocol;
  DataBind *codec;
  DataBindHttpMethodPlan *http_plans[2];
  DataBindRpcMethodPlan *rpc_plans[2];
  DataBindNativeTypeBinding input[2], output[2];
  DataBindServiceNativeBinding native[2];
  DataBindNativeExecution executions[2];
  cflow_function_typed_adapter_projection projection;
  chttp_http_method http[2];
  chttp_rpc_method rpc[2];
  chttp_application_method_interceptor hooks[3];
  size_t hook_count;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  cmeta_plugin_registry registry;
  cmeta_plugin_ref ref;
  cmeta_plugin_lease discovery;
  bool stopped, block, release, thread_live, rpc_call;
  bool hold_completion, release_completion, reject_middle, fail_middle, native_fail;
  bool flow_control, sink_entered, release_sink;
  size_t received;
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t thread;
  char uri[64];
  int calls, destroyed, request_status;
  size_t completion_count;
  observed_completion completions[64];
  _Atomic int invalid, after, errors, last_kind, last_status;
} fixture;
static void destroy(void *context, void *object) { (void)context; ++fixture.destroyed; free(object); }
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, destroy};
static cmeta_status create(void *context, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  OutputController *self = malloc(sizeof(*self));
  if (!self) return CMETA_OUT_OF_MEMORY;
  self->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, self, cmeta_reflected_data(OutputController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(self); }
  return status;
}
static bool observe(void *context) {
  if (context != &fixture || !chttp_application_worker_current(&fixture.app)) atomic_fetch_add(&fixture.invalid, 1);
  sync_mutex_lock(&fixture.mutex); ++fixture.calls; sync_cond_broadcast(&fixture.changed);
  while (fixture.block && !fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
  sync_mutex_unlock(&fixture.mutex);
  return !fixture.native_fail;
}
static cmeta_status before(void *self, const chttp_method_call *call, bool *proceed) {
  (void)proceed;
  if (!self || ((OutputController *)self)->marker != 42 || !chttp_application_worker_current(&fixture.app) ||
      !call->input || !call->http || call->http->session) {
    atomic_fetch_add(&fixture.invalid, 1); return CMETA_TYPE_MISMATCH;
  }
  return CMETA_OK;
}
static void after(void *self, const chttp_method_call *call, const chttp_method_result *result) {
  (void)self; (void)call;
  const Envelope_t *output = result->output ? result->output->params[1] : NULL;
  if (!chttp_application_worker_current(&fixture.app) || !output || output->payload.id != UINT64_MAX ||
      !output->payload.note || result->outcome.kind != DATA_BIND_BINDING_OUTCOME_SUCCESS)
    atomic_fetch_add(&fixture.invalid, 1);
  atomic_fetch_add(&fixture.after, 1);
}
static void on_error(void *self, const chttp_method_call *call, cmeta_status status) {
  (void)self;
  if (!chttp_application_worker_current(&fixture.app) || status == CMETA_OK ||
      (!call->result->invoked && !call->result->rejected && !fixture.fail_middle))
    atomic_fetch_add(&fixture.invalid, 1);
  if (call->result->outcome.kind == DATA_BIND_BINDING_OUTCOME_TYPED_ERROR) {
    const databind_6_Output_5_Probe_4_Fail__error *error = call->input->params[2];
    if (error->kind != databind_6_Output_5_Probe_4_Fail__ERROR_1 || error->payload.error_1.code != 77u ||
        !error->payload.error_1.reason) atomic_fetch_add(&fixture.invalid, 1);
  }
  atomic_store(&fixture.last_kind, call->result->outcome.kind);
  atomic_store(&fixture.last_status, call->result->status);
  atomic_fetch_add(&fixture.errors, 1);
}
static void completion_observe(int hook, void *self, const chttp_method_completion *completion) {
  if (!self || ((OutputController *)self)->marker != 42 || !chttp_application_worker_current(&fixture.app) ||
      !completion->http || completion->http->session || !completion->http->target || !completion->http->path ||
      !cmeta_function_desc_valid(completion->method.operation)) atomic_fetch_add(&fixture.invalid, 1);
  const chttp_method_terminal *method = &completion->method;
  observed_completion observed = {.hook = hook, .kind = method->outcome.kind, .native_status = method->native_status,
      .adapter_status = method->response_status, .status = method->status, .interceptor_status = method->interceptor_status,
      .invoked = method->invoked, .rejected = method->rejected, .notification = method->notification,
      .rpc_id = method->rpc_id, .response = completion->response};
  if (method->rpc_method) {
    if (strcmp(method->rpc_method, "Probe.Get") && strcmp(method->rpc_method, "Probe.Fail")) atomic_fetch_add(&fixture.invalid, 1);
    if (strcmp(completion->http->path, "/rpc") || !completion->http->body_size) atomic_fetch_add(&fixture.invalid, 1);
    snprintf(observed.rpc_method, sizeof(observed.rpc_method), "%s", method->rpc_method);
  }
  if (method->outcome.typed_error) snprintf(observed.typed_error, sizeof(observed.typed_error), "%s", method->outcome.typed_error);
  snprintf(observed.path, sizeof(observed.path), "%s", completion->http->path);
  sync_mutex_lock(&fixture.mutex);
  if (fixture.completion_count == sizeof(fixture.completions) / sizeof(fixture.completions[0])) atomic_fetch_add(&fixture.invalid, 1);
  else fixture.completions[fixture.completion_count++] = observed;
  sync_cond_broadcast(&fixture.changed);
  while (fixture.hold_completion && !fixture.release_completion) sync_cond_wait(&fixture.changed, &fixture.mutex);
  sync_mutex_unlock(&fixture.mutex);
}
static void completion0(void *self, const chttp_method_completion *completion) { completion_observe(0, self, completion); }
static void completion1(void *self, const chttp_method_completion *completion) { completion_observe(1, self, completion); }
static void completion2(void *self, const chttp_method_completion *completion) { completion_observe(2, self, completion); }
static cmeta_status middle(void *self, const chttp_method_call *call, bool *proceed) {
  (void)self;
  if (fixture.fail_middle) return CMETA_TYPE_MISMATCH;
  return fixture.reject_middle ? chttp_method_call_reject(call, proceed) : CMETA_OK;
}
static int request_with_sink(const char *target, const char *rpc_body, chttp_response *response, const chttp_body_sink *sink) {
  chttp_client client = {0}; chttp_error error = {0};
  int status = chttp_client_init(&client, &fixture.client);
  if (status != SALTS_OK) return status;
  const chttp_options options = {.connection_uri = fixture.uri, .authority = "test", .target = target,
      .protocol = fixture.protocol, .timeout_ms = sink ? 20000u : 5000u, .body = rpc_body, .body_sink = sink,
      .body_size = rpc_body ? strlen(rpc_body) : 0u};
  status = rpc_body ? chttp_post(&client, &options, response, &error) : chttp_get(&client, &options, response, &error);
  const int closed = chttp_client_destroy(&client, 5000u);
  return status == SALTS_OK ? closed : status;
}
static int request(const char *target, const char *rpc_body, chttp_response *response) {
  return request_with_sink(target, rpc_body, response, NULL);
}
static int receive_body(void *user, const void *data, size_t size) {
  (void)user; (void)data;
  sync_mutex_lock(&fixture.mutex); fixture.sink_entered = true; sync_cond_broadcast(&fixture.changed);
  while (!fixture.release_sink) sync_cond_wait(&fixture.changed, &fixture.mutex);
  fixture.received += size; sync_mutex_unlock(&fixture.mutex);
  return SALTS_OK;
}
static void request_thread(void *unused) {
  (void)unused; chttp_response response = {0};
  const chttp_body_sink sink = {receive_body, NULL};
  fixture.request_status = request_with_sink(fixture.rpc_call ? "/rpc" : fixture.flow_control ? "/value/4" : "/value/1",
      fixture.rpc_call ? "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[1],\"id\":1}" : NULL,
      &response, fixture.flow_control ? &sink : NULL);
  chttp_response_destroy(&response);
}
static void wait_for_entry(void) {
  const uint64_t began = monotonic_ms();
  sync_mutex_lock(&fixture.mutex);
  while (!fixture.calls && monotonic_ms() - began < 5000u)
    (void)sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(10000000));
  const int calls = fixture.calls;
  sync_mutex_unlock(&fixture.mutex);
  check_equal(calls, 1);
}
static void wait_for_completion(void) {
  const uint64_t began = monotonic_ms();
  sync_mutex_lock(&fixture.mutex);
  while (!fixture.completion_count && monotonic_ms() - began < 5000u)
    (void)sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(10000000));
  const size_t count = fixture.completion_count;
  sync_mutex_unlock(&fixture.mutex);
  check_equal(count, 1u);
}
static size_t leases(void) {
  cmeta_plugin_lifecycle_info info = {0};
  check_equal(cmeta_plugin_registry_get_lifecycle(&fixture.registry, fixture.ref, &info), CMETA_PLUGIN_OK);
  return info.active_leases;
}
static void configure_plugin(void) {
  const cmeta_plugin_registry_config config = {.capacity = 1u};
  check_equal(cmeta_plugin_registry_init(&fixture.registry, &config), CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_load(&fixture.registry, WORKER_OUTPUT_PLUGIN_PATH, &fixture.ref), CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_start(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK);
  const cmeta_plugin_manifest *manifest = NULL;
  const cmeta_plugin_export *catalog = NULL;
  check_equal(cmeta_plugin_registry_acquire(&fixture.registry, fixture.ref, &fixture.discovery, &manifest), CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_manifest_find_export(manifest, DATA_BIND_PLUGIN_CATALOG_EXPORT_ID, &catalog), CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_export_require_interface(catalog, DATA_BIND_PLUGIN_CATALOG_CONTRACT_ID,
      DATA_BIND_PLUGIN_CATALOG_CONTRACT_VERSION, 0u, data_bind_plugin_catalog_interface()), CMETA_PLUGIN_OK);
  for (size_t i = 0u; i < 2u; ++i) {
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindPluginOperationBinding operation = DATA_BIND_PLUGIN_OPERATION_BINDING_INIT;
    check_equal(data_bind_plugin_catalog_operation_at(catalog->value.interface.value, i, &operation, &error), DATA_BIND_OK);
    const size_t index = strcmp(operation.operation_name, "Get") == 0 ? 0u : 1u;
    check_equal(operation.operation_name, index ? "Fail" : "Get");
    const cmeta_plugin_export *function = NULL;
    check_equal(cmeta_plugin_manifest_find_export(manifest, operation.export_id, &function), CMETA_PLUGIN_OK);
    check_true(data_bind_plugin_operation_execution_admit(&operation, function, &fixture.executions[index]));
    fixture.http[index].dispatch = CHTTP_DISPATCH_PLUGIN;
    fixture.http[index].binding = NULL; fixture.http[index].execution = NULL;
    fixture.http[index].plugin_registry = &fixture.registry; fixture.http[index].plugin_ref = fixture.ref;
    fixture.http[index].plugin_export_id = operation.export_id;
    fixture.rpc[index].plugin_registry = &fixture.registry; fixture.rpc[index].plugin_ref = fixture.ref;
  }
}
static void initialize(int mode) {
  if (mode == CHTTP_DISPATCH_PLUGIN) configure_plugin();
  if (mode == CHTTP_DISPATCH_CFLOW) {
    check_equal(databind_6_Output_5_Probe_3_Get__databind_cflow_projection(&fixture.projection), CFLOW_FUNCTION_PROJECTION_OK);
    fixture.http[0].dispatch = CHTTP_DISPATCH_CFLOW;
    fixture.http[0].execution = NULL; fixture.http[0].projection = &fixture.projection;
  }
  check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
  check_equal(chttp_application_worker_prepare(&fixture.app, &fixture.work_config), SALTS_OK);
}
static int mount_http(size_t i, DataBind *codec) {
  return chttp_application_worker_mount_http(&fixture.app, "OutputComponent",
      cmeta_reflected_data(OutputController), &fixture.http[i], codec, fixture.hooks, fixture.hook_count);
}
static void listen(void) {
  /* Method/FormatPlans promise copied schema facts, not a runtime codec borrow. */
  data_bind_free(fixture.codec); fixture.codec = NULL;
  check_equal(chttp_application_start(&fixture.app), SALTS_OK);
  uint16_t port = 0u; check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
  snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
}
static void start(int mode) {
  initialize(mode);
  const size_t count = mode == CHTTP_DISPATCH_CFLOW ? 1u : 2u;
  for (size_t i = 0u; i < count; ++i) {
    check_equal(mount_http(i, fixture.codec), SALTS_OK);
    if (mode != CHTTP_DISPATCH_CFLOW)
      check_equal(chttp_application_worker_mount_rpc(&fixture.app, "OutputComponent",
          cmeta_reflected_data(OutputController), &fixture.rpc[i], fixture.codec, fixture.hooks, fixture.hook_count), SALTS_OK);
  }
  if (fixture.registry.impl) {
    check_equal(cmeta_plugin_registry_release(&fixture.registry, &fixture.discovery), CMETA_PLUGIN_OK);
    check_equal(leases(), 4u);
  }
  listen();
}
static json_value_t *parse(const chttp_response *response) {
  json_value_t *root = json_parse(response->body, response->body_size); check_not_null(root); return root;
}
static void check_payload(json_value_t *payload, uint32_t value) {
  static const char escaped[] = {'a', '"', '\\', '\n', '\0', 'z'};
  check_not_null(payload);
  const json_value_t *note = json_object_get(payload, "note");
  check_equal(json_string_len(note), sizeof(escaped));
  check_equal(json_string(note), escaped, sizeof(escaped));
  check_equal(json_number(json_object_get(json_object_get(payload, "child"), "number")), (double)value);
  cserde_reader *reader = json_cserde_reader_create(payload, 8u); check_not_null(reader);
  cserde_token token; bool id = false, found = false; cserde_status status;
  while ((status = cserde_reader_next(reader, &token)) == CSERDE_OK) {
    if (id) { check_equal(token.kind, CSERDE_UINT); check_equal(token.value.uint, UINT64_MAX); found = true; id = false; }
    else if (token.kind == CSERDE_STRING && token.value.slice.size == 2u &&
        memcmp(token.value.slice.data, "id", 2u) == 0) id = true;
  }
  json_cserde_reader_destroy(reader); check_equal(status, CSERDE_DONE); check_true(found);
}

spec("Application bounded structured output") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture)); sync_mutex_init(&fixture.mutex); sync_cond_init(&fixture.changed);
    atomic_init(&fixture.invalid, 0); atomic_init(&fixture.after, 0); atomic_init(&fixture.errors, 0);
    atomic_init(&fixture.last_kind, 0); atomic_init(&fixture.last_status, 0);
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    check_equal(Output_codec_create(&fixture.codec, &error), DATA_BIND_OK);
    check_equal(databind_6_Output_5_Probe_3_Get__databind_native_binding(
        &fixture.input[0], &fixture.output[0], &fixture.native[0], &error), DATA_BIND_OK);
    check_equal(databind_6_Output_5_Probe_4_Fail__databind_native_binding(
        &fixture.input[1], &fixture.output[1], &fixture.native[1], &error), DATA_BIND_OK);
    fixture.executions[0] = *databind_6_Output_5_Probe_3_Get__databind_execution();
    fixture.executions[1] = *databind_6_Output_5_Probe_4_Fail__databind_execution();
    static const DataBindHttpFieldProjection fields[] = {
        {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS, "value", DATA_BIND_HTTP_PATH, "value", 0u},
        {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_EGRESS, "payload", DATA_BIND_HTTP_RESPONSE_BODY, "payload", 0u}};
    static const DataBindHttpErrorMapping http_error = {sizeof(DataBindHttpErrorMapping), "Fault", 409};
    static const DataBindRpcFieldProjection rpc_fields[] = {
        {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_INGRESS, "value", "value", 0u},
        {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_EGRESS, "payload", "payload", 0u}};
    static const DataBindRpcErrorMapping rpc_error = {sizeof(DataBindRpcErrorMapping), "Fault", -32077};
    for (size_t i = 0u; i < 2u; ++i) {
      const char *operation = i ? "Fail" : "Get";
      DataBindHttpProjectionConfig http = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
      http.method = "GET"; http.route = i ? "/fault/{value}" : "/value/{value}";
      http.fields = fields; http.field_count = 2u; http.errors = i ? &http_error : NULL; http.error_count = i;
      check_equal(data_bind_http_method_plan_compile_service(fixture.codec, "Probe", operation,
          &http, &fixture.native[i], &fixture.http_plans[i], &diagnostic), DATA_BIND_OK);
      DataBindRpcProjectionConfig rpc = DATA_BIND_RPC_PROJECTION_CONFIG_INIT;
      rpc.wire_method = i ? "Probe.Fail" : "Probe.Get"; rpc.fields = rpc_fields; rpc.field_count = 2u;
      rpc.errors = i ? &rpc_error : NULL; rpc.error_count = i;
      check_equal(data_bind_rpc_method_plan_compile_service(fixture.codec, "Probe", operation,
          &rpc, &fixture.native[i], &fixture.rpc_plans[i], &diagnostic), DATA_BIND_OK);
      fixture.http[i] = (chttp_http_method){.plan = fixture.http_plans[i], .binding = &fixture.native[i],
          .execution = &fixture.executions[i]};
      fixture.rpc[i] = (chttp_rpc_method){.path = "/rpc", .plan = fixture.rpc_plans[i], .binding = &fixture.native[i],
          .execution = &fixture.executions[i]};
    }
    fixture.provider = (salts_configurator_provider_binding){sizeof(fixture.provider),
        SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(OutputComponent),
        NULL, NULL, create, NULL, NULL};
    fixture.deployment = (salts_configurator_deployment){&fixture.provider, NULL, NULL};
    fixture.controller = (chttp_controller){.component_id = "OutputComponent", .receiver = cmeta_reflected_data(OutputController)};
    fixture.config = chttp_application_config_default(); fixture.config.bindings.method_capacity = 2u;
    fixture.config.components = &fixture.deployment; fixture.config.component_count = 1u;
    fixture.config.controllers = &fixture.controller; fixture.config.controller_count = 1u;
    fixture.config.rpc.method_capacity = 2u;
    fixture.work_config = (chttp_application_work_config){1u, 4u, 65536u, 8192u};
    fixture.client = (chttp_client_config){.network = fixture.config.http.network,
        .request_capacity = 1u, .max_start_line_bytes = 256u, .max_header_count = 16u, .max_header_bytes = 4096u,
        .max_request_body_bytes = 8192u, .max_response_body_bytes = 8192u, .max_informational_responses = 4u};
    fixture.hooks[0] = (chttp_application_method_interceptor){"OutputComponent", cmeta_reflected_data(OutputController),
        before, after, on_error, completion0};
    fixture.hooks[1] = (chttp_application_method_interceptor){"OutputComponent", cmeta_reflected_data(OutputController),
        middle, NULL, NULL, completion1};
    fixture.hooks[2] = (chttp_application_method_interceptor){"OutputComponent", cmeta_reflected_data(OutputController),
        NULL, NULL, NULL, completion2};
    fixture.hook_count = 1u;
    chttp_service_plugin_probe_set(observe, &fixture);
  }
  after_each() {
    sync_mutex_lock(&fixture.mutex); fixture.release = true; fixture.release_completion = true; fixture.release_sink = true;
    sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    if (fixture.thread_live) { sync_thread_join(&fixture.thread); fixture.thread_live = false; }
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    chttp_service_plugin_probe_set(NULL, NULL); check_equal(atomic_load(&fixture.invalid), 0);
    for (size_t i = 0u; i < 2u; ++i) { data_bind_http_method_plan_free(fixture.http_plans[i]); data_bind_rpc_method_plan_free(fixture.rpc_plans[i]); }
    data_bind_free(fixture.codec);
    if (fixture.registry.impl) {
      if (cmeta_plugin_lease_valid(fixture.discovery))
        check_equal(cmeta_plugin_registry_release(&fixture.registry, &fixture.discovery), CMETA_PLUGIN_OK);
      if (!fixture.stopped) check_equal(cmeta_plugin_registry_request_stop(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK);
      check_equal(leases(), 0u);
      bool quiet = false;
      check_equal(cmeta_plugin_registry_poll_quiescent(&fixture.registry, fixture.ref, &quiet), CMETA_PLUGIN_OK); check_true(quiet);
      check_equal(cmeta_plugin_registry_unload(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_destroy(&fixture.registry), CMETA_PLUGIN_OK);
    }
    sync_cond_destroy(&fixture.changed); sync_mutex_destroy(&fixture.mutex);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    static const int modes[] = {CHTTP_DISPATCH_INLINE, CHTTP_DISPATCH_CFLOW, CHTTP_DISPATCH_PLUGIN};
    for (size_t m = 0u; m < sizeof(modes) / sizeof(modes[0]); ++m) {
      const int mode = modes[m];
      it("publishes owned record JSON with exact uint64 and escaped text (protocol=%d provider=%d)", protocol, mode) {
        fixture.protocol = (chttp_protocol)protocol; start(mode);
        chttp_response response = {0}; check_equal(request("/value/1", NULL, &response), SALTS_OK);
        check_equal(response.status_code, 200u); check_equal(chttp_response_header(&response, "Content-Type"), "application/json");
        check_equal(response.body_size, sizeof(EXPECTED_JSON) - 1u);
        check_equal(response.body, EXPECTED_JSON, sizeof(EXPECTED_JSON) - 1u);
        json_value_t *root = parse(&response); check_payload(root, 1u); json_free(root); chttp_response_destroy(&response);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_equal(atomic_load(&fixture.after), 1);
        check_equal(fixture.completion_count, 1u);
        check_equal(fixture.completions[0].kind, (int)DATA_BIND_BINDING_OUTCOME_SUCCESS);
        check_true(fixture.completions[0].invoked); check_false(fixture.completions[0].rejected);
        check_equal(fixture.completions[0].response.transport_status, SALTS_OK);
        check_equal(fixture.completions[0].adapter_status, SALTS_OK);
      }
    }
    for (int plugin = 0; plugin <= 1; ++plugin) {
      it("maps typed error names and preserves structured RPC values (protocol=%d Plugin=%d)", protocol, plugin) {
        fixture.protocol = (chttp_protocol)protocol; start(plugin ? CHTTP_DISPATCH_PLUGIN : CHTTP_DISPATCH_INLINE);
        chttp_response response = {0}; check_equal(request("/fault/1", NULL, &response), SALTS_OK); check_equal(response.status_code, 409u);
        check_equal(chttp_response_header(&response, "Content-Type"), "application/json");
        json_value_t *root = parse(&response); check_equal(json_number(json_object_get(root, "fault_code")), 77.0);
        check_null(json_object_get(root, "code")); json_free(root); chttp_response_destroy(&response);
        check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[1],\"id\":1}", &response), SALTS_OK);
        root = parse(&response); check_payload(json_object_get(root, "result"), 1u); json_free(root); chttp_response_destroy(&response);
        check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Fail\",\"params\":[1],\"id\":2}", &response), SALTS_OK);
        root = parse(&response); json_value_t *error = json_object_get(root, "error");
        check_equal(json_number(json_object_get(error, "code")), -32077.0);
        check_equal(json_number(json_object_get(json_object_get(error, "data"), "fault_code")), 77.0);
        json_free(root); chttp_response_destroy(&response);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.after), 1); check_equal(atomic_load(&fixture.errors), 2);
        check_equal(atomic_load(&fixture.last_kind), (int)DATA_BIND_BINDING_OUTCOME_TYPED_ERROR);
      }
    }
    it("aborts overflowing JSON and reuses the transaction on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol;
      fixture.config.bindings.max_response_body_bytes = 128u; fixture.config.rpc.max_output_value_bytes = 128u;
      start(CHTTP_DISPATCH_INLINE); chttp_response response = {0};
      check_equal(request("/value/2", NULL, &response), SALTS_OK); check_equal(response.status_code, 500u); chttp_response_destroy(&response);
      check_equal(request("/value/1", NULL, &response), SALTS_OK); check_equal(response.status_code, 200u); chttp_response_destroy(&response);
      check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[2],\"id\":1}", &response), SALTS_OK);
      json_value_t *root = parse(&response); check_equal(json_number(json_object_get(json_object_get(root, "error"), "code")), -32603.0);
      json_free(root); chttp_response_destroy(&response);
      check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[1],\"id\":2}", &response), SALTS_OK);
      root = parse(&response); check_payload(json_object_get(root, "result"), 1u); json_free(root); chttp_response_destroy(&response);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(atomic_load(&fixture.errors), 2); check_equal(atomic_load(&fixture.after), 2);
      check_equal(atomic_load(&fixture.last_status), (int)DATA_BIND_ERR_LIMIT);
      check_equal(fixture.completion_count, 4u);
      int failures = 0;
      for (size_t i = 0u; i < fixture.completion_count; ++i) {
        if (fixture.completions[i].status == DATA_BIND_ERR_LIMIT) {
          ++failures; check_equal(fixture.completions[i].kind, (int)DATA_BIND_BINDING_OUTCOME_NONE);
        }
        check_equal(fixture.completions[i].response.transport_status, SALTS_OK);
      }
      check_equal(failures, 2);
    }
    for (size_t short_by = 0u; short_by <= 1u; ++short_by) {
      it("enforces the exact JSON byte boundary (protocol=%d short=%zu)", protocol, short_by) {
        fixture.protocol = (chttp_protocol)protocol;
        fixture.config.bindings.max_response_body_bytes = sizeof(EXPECTED_JSON) - 1u - short_by;
        fixture.config.rpc.max_output_value_bytes = fixture.config.bindings.max_response_body_bytes;
        start(CHTTP_DISPATCH_INLINE); chttp_response response = {0};
        check_equal(request("/value/1", NULL, &response), SALTS_OK);
        check_equal(response.status_code, short_by ? 500u : 200u); chttp_response_destroy(&response);
        check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[1],\"id\":1}", &response), SALTS_OK);
        json_value_t *root = parse(&response);
        if (short_by) check_equal(json_number(json_object_get(json_object_get(root, "error"), "code")), -32603.0);
        else check_payload(json_object_get(root, "result"), 1u);
        json_free(root); chttp_response_destroy(&response);
        check_equal(request("/value/0", NULL, &response), SALTS_OK); check_equal(response.status_code, 200u);
        root = parse(&response); check_equal(json_string_len(json_object_get(root, "note")), 0u);
        json_free(root); chttp_response_destroy(&response);
        check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[0],\"id\":2}", &response), SALTS_OK);
        root = parse(&response); check_equal(json_string_len(json_object_get(json_object_get(root, "result"), "note")), 0u);
        json_free(root); chttp_response_destroy(&response);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.errors), short_by ? 2 : 0);
        check_equal(atomic_load(&fixture.after), short_by ? 2 : 4);
      }
    }
    it("rolls back typed-error overflow without publishing a partial payload on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol;
      fixture.config.bindings.max_response_body_bytes = 128u; fixture.config.rpc.max_output_value_bytes = 128u;
      start(CHTTP_DISPATCH_PLUGIN); chttp_response response = {0};
      check_equal(request("/fault/2", NULL, &response), SALTS_OK); check_equal(response.status_code, 500u); chttp_response_destroy(&response);
      check_equal(request("/fault/1", NULL, &response), SALTS_OK); check_equal(response.status_code, 409u); chttp_response_destroy(&response);
      check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Fail\",\"params\":[2],\"id\":1}", &response), SALTS_OK);
      json_value_t *root = parse(&response); check_equal(json_number(json_object_get(json_object_get(root, "error"), "code")), -32603.0);
      json_free(root); chttp_response_destroy(&response);
      check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Fail\",\"params\":[1],\"id\":2}", &response), SALTS_OK);
      root = parse(&response); check_equal(json_number(json_object_get(json_object_get(root, "error"), "code")), -32077.0);
      json_free(root); chttp_response_destroy(&response);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(atomic_load(&fixture.errors), 4); check_equal(atomic_load(&fixture.after), 0);
      check_equal(atomic_load(&fixture.last_kind), (int)DATA_BIND_BINDING_OUTCOME_TYPED_ERROR);
    }
    it("keeps structured mixed batches sequential and notifications silent on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; start(CHTTP_DISPATCH_INLINE);
      chttp_response response = {0};
      check_equal(request("/rpc", "[{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[1],\"id\":7},"
          "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Fail\",\"params\":[1],\"id\":8},"
          "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[0]}]", &response), SALTS_OK);
      json_value_t *root = parse(&response); check_equal(json_array_size(root), 2u);
      check_equal(json_number(json_object_get(json_array_get(root, 0u), "id")), 7.0);
      check_payload(json_object_get(json_array_get(root, 0u), "result"), 1u);
      check_equal(json_number(json_object_get(json_array_get(root, 1u), "id")), 8.0);
      check_equal(json_number(json_object_get(json_object_get(json_array_get(root, 1u), "error"), "code")), -32077.0);
      json_free(root); chttp_response_destroy(&response);
      check_equal(request("/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Fail\",\"params\":[1]}", &response), SALTS_OK);
      check_equal(response.status_code, 204u); check_equal(response.body_size, 0u);
      check_null(chttp_response_header(&response, "Content-Type")); chttp_response_destroy(&response);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(atomic_load(&fixture.after), 2); check_equal(atomic_load(&fixture.errors), 2);
      check_equal(fixture.completion_count, 4u);
      size_t first = SIZE_MAX, second = SIZE_MAX, notification = SIZE_MAX;
      for (size_t i = 0u; i < fixture.completion_count; ++i) {
        const observed_completion *seen = &fixture.completions[i];
        if (seen->rpc_id == 7u) { first = i; check_equal(seen->rpc_method, "Probe.Get"); }
        if (seen->rpc_id == 8u) {
          second = i; check_equal(seen->rpc_method, "Probe.Fail"); check_equal(seen->typed_error, "Fault");
          check_equal(seen->kind, (int)DATA_BIND_BINDING_OUTCOME_TYPED_ERROR);
        }
        if (seen->notification && strcmp(seen->rpc_method, "Probe.Get") == 0) notification = i;
      }
      check_less(first, second); check_less(second, notification); check_less(notification, SIZE_MAX);
    }
    for (int rpc = 0; rpc <= 1; ++rpc) for (int disposition = 0; disposition <= 2; ++disposition) {
      it("notifies only entered hooks in reverse order (protocol=%d RPC=%d disposition=%d)", protocol, rpc, disposition) {
        fixture.protocol = (chttp_protocol)protocol; fixture.hook_count = 3u;
        fixture.reject_middle = disposition == 1; fixture.fail_middle = disposition == 2;
        start(CHTTP_DISPATCH_INLINE); chttp_response response = {0};
        check_equal(request(rpc ? "/rpc" : "/value/1", rpc ?
            "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[1],\"id\":11}" : NULL, &response), SALTS_OK);
        check_equal(response.status_code, rpc ? 200u : disposition == 1 ? 403u : disposition == 2 ? 500u : 200u);
        if (rpc && disposition) {
          json_value_t *root = parse(&response);
          check_equal(json_number(json_object_get(json_object_get(root, "error"), "code")), disposition == 1 ? -32003.0 : -32603.0);
          json_free(root);
        }
        chttp_response_destroy(&response);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(fixture.completion_count, disposition ? 2u : 3u);
        for (size_t i = 0u; i < fixture.completion_count; ++i) {
          const observed_completion *seen = &fixture.completions[i];
          check_equal(seen->hook, (int)(fixture.completion_count - i - 1u));
          check_equal(seen->invoked, disposition == 0); check_equal(seen->rejected, disposition == 1);
          check_equal(seen->interceptor_status, disposition == 1 ? CMETA_CALLBACK_ERROR :
              disposition == 2 ? CMETA_TYPE_MISMATCH : CMETA_OK);
          check_equal(seen->response.transport_status, SALTS_OK);
          if (rpc) { check_equal(seen->rpc_id, 11u); check_equal(seen->rpc_method, "Probe.Get"); }
        }
        check_equal(fixture.calls, disposition ? 0 : 1);
      }
    }
    it("does not notify methods that failed ingress or never resolved on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; start(CHTTP_DISPATCH_INLINE);
      chttp_response response = {0};
      check_equal(request("/value/bad", NULL, &response), SALTS_OK); check_equal(response.status_code, 400u); chttp_response_destroy(&response);
      check_equal(request("/rpc", "[{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[],\"id\":1},"
          "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Missing\",\"id\":2}]", &response), SALTS_OK);
      chttp_response_destroy(&response);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.completion_count, 0u); check_equal(fixture.calls, 0);
    }
    it("preserves native failure separately from successful error delivery on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; fixture.native_fail = true; start(CHTTP_DISPATCH_PLUGIN);
      chttp_response response = {0}; check_equal(request("/value/1", NULL, &response), SALTS_OK);
      check_equal(response.status_code, 500u); chttp_response_destroy(&response);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.completion_count, 1u); const observed_completion *seen = &fixture.completions[0];
      check_equal(seen->native_status, -1); check_equal(seen->kind, (int)DATA_BIND_BINDING_OUTCOME_NATIVE_STATUS);
      check_equal(seen->status, DATA_BIND_OK); check_equal(seen->adapter_status, SALTS_OK);
      check_equal(seen->response.transport_status, SALTS_OK);
    }
    for (int rpc = 0; rpc <= 1; ++rpc) {
      it("retains copied results and stopped DSO during a blocked completion (protocol=%d RPC=%d)", protocol, rpc) {
        fixture.protocol = (chttp_protocol)protocol; fixture.hold_completion = true;
        start(CHTTP_DISPATCH_PLUGIN); chttp_response response = {0};
        check_equal(request(rpc ? "/rpc" : "/value/1", rpc ?
            "{\"jsonrpc\":\"2.0\",\"method\":\"Probe.Get\",\"params\":[1],\"id\":9}" : NULL, &response), SALTS_OK);
        chttp_response_destroy(&response); wait_for_completion();
        check_equal(cmeta_plugin_registry_request_stop(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK); fixture.stopped = true;
        check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
        check_not_null(fixture.app.impl); check_equal(fixture.destroyed, 0); check_equal(leases(), 4u);
        chttp_application_work_stats stats = {0}; check_equal(chttp_application_worker_stats(&fixture.app, &stats), SALTS_OK);
        check_equal(stats.pending, 1u); check_greater(stats.retained_bytes, 0u);
        check_equal(cmeta_plugin_registry_unload(&fixture.registry, fixture.ref), CMETA_PLUGIN_BUSY);
        sync_mutex_lock(&fixture.mutex); fixture.release_completion = true;
        sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(fixture.destroyed, 1); check_equal(leases(), 0u); check_equal(fixture.completion_count, 1u);
        check_equal(fixture.completions[0].response.transport_status, SALTS_OK);
        check_equal(fixture.completions[0].kind, (int)DATA_BIND_BINDING_OUTCOME_SUCCESS);
      }
    }
  }
  it("waits for real H2 flow-control terminal while the sole worker serves another method") {
    const size_t bytes = CHTTP_WORKER_OUTPUT_LARGE_NOTE_BYTES + 256u;
    fixture.protocol = CHTTP_HTTP_2; fixture.flow_control = true;
    fixture.config.bindings.max_response_body_bytes = bytes; fixture.config.bindings.native_max_owned_bytes = bytes;
    fixture.config.http.max_response_body_bytes = bytes; fixture.config.http.max_buffered_response_body_bytes = bytes;
    fixture.config.http.network.max_send_bytes = bytes * 2u; fixture.config.http.buffer_capacity_bytes = bytes * 16u;
    fixture.client.max_response_body_bytes = bytes;
    fixture.work_config.reply_bytes = bytes + 32u; fixture.work_config.retained_bytes = bytes * 4u;
    start(CHTTP_DISPATCH_PLUGIN);
    check_equal(sync_thread_create(&fixture.thread, request_thread, NULL), SALTS_OK); fixture.thread_live = true;
    sync_mutex_lock(&fixture.mutex);
    const uint64_t began = monotonic_ms();
    while (!fixture.sink_entered && monotonic_ms() - began < 5000u)
      (void)sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(10000000));
    const bool entered = fixture.sink_entered;
    sync_mutex_unlock(&fixture.mutex); check_true(entered);
    /* More than one H2 window remains unsent; another response proves the
     * sole worker has completed business without reporting the first terminal. */
    chttp_response response = {0}; check_equal(request("/value/1", NULL, &response), SALTS_OK);
    check_equal(response.status_code, 200u); chttp_response_destroy(&response); wait_for_completion();
    sync_mutex_lock(&fixture.mutex);
    const size_t count = fixture.completion_count;
    const bool small = count == 1u && strcmp(fixture.completions[0].path, "/value/1") == 0;
    fixture.release_sink = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    check_equal(count, 1u); check_true(small); check_equal(atomic_load(&fixture.after), 2);
    check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
    check_equal(fixture.request_status, SALTS_OK); check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.completion_count, 2u); check_equal(fixture.completions[1].path, "/value/4");
    check_equal(fixture.completions[1].response.transport_status, SALTS_OK);
    static const char empty[] = "{\"id\":18446744073709551615,\"child\":{\"number\":4},\"note\":\"\"}";
    check_equal(fixture.received, sizeof(empty) - 1u + CHTTP_WORKER_OUTPUT_LARGE_NOTE_BYTES);
  }
  it("rejects a missing/mismatched codec before publishing a route or consuming a Plugin lease") {
    initialize(CHTTP_DISPATCH_PLUGIN);
    check_equal(mount_http(0u, NULL), SALTS_EINVAL); check_equal(leases(), 1u);
    static const char schema[] = "schema Output [version(1)]; message Detail { uint64 id; } message Envelope { Detail payload; }";
    DataBind *wrong = NULL; DataBindError error = DATA_BIND_ERROR_INIT;
    check_equal(data_bind_create_from_text(schema, sizeof(schema) - 1u, &wrong, &error), DATA_BIND_OK);
    check_equal(mount_http(0u, wrong), SALTS_ENOTSUP); check_equal(leases(), 1u);
    check_equal(chttp_application_worker_mount_rpc(&fixture.app, "OutputComponent", cmeta_reflected_data(OutputController),
        &fixture.rpc[0], wrong, fixture.hooks, fixture.hook_count), SALTS_ENOTSUP); check_equal(leases(), 1u);
    data_bind_free(wrong);
    check_equal(mount_http(0u, fixture.codec), SALTS_OK);
    check_equal(chttp_application_worker_mount_rpc(&fixture.app, "OutputComponent", cmeta_reflected_data(OutputController),
        &fixture.rpc[0], fixture.codec, fixture.hooks, fixture.hook_count), SALTS_OK);
    check_equal(cmeta_plugin_registry_release(&fixture.registry, &fixture.discovery), CMETA_PLUGIN_OK);
    check_equal(leases(), 2u); listen();
    chttp_response response = {0}; check_equal(request("/value/1", NULL, &response), SALTS_OK);
    check_equal(response.status_code, 200u); chttp_response_destroy(&response);
  }
  for (int nullable = 0; nullable <= 1; ++nullable) {
    it("rejects undeclared native presence/null state before mounting (nullable=%d)", nullable) {
      initialize(CHTTP_DISPATCH_INLINE);
      char schema[256];
      snprintf(schema, sizeof(schema), "schema Output [version(1)]; message Child { uint32 number; } "
          "message Detail { uint64 id; Child child; %s string note; } message Envelope { Detail payload; }",
          nullable ? "nullable" : "optional");
      DataBind *wrong = NULL; DataBindError error = DATA_BIND_ERROR_INIT;
      check_equal(data_bind_create_from_text(schema, strlen(schema), &wrong, &error), DATA_BIND_OK);
      check_equal(mount_http(0u, wrong), SALTS_ENOTSUP);
      check_equal(chttp_application_worker_mount_rpc(&fixture.app, "OutputComponent", cmeta_reflected_data(OutputController),
          &fixture.rpc[0], wrong, fixture.hooks, fixture.hook_count), SALTS_ENOTSUP);
      data_bind_free(wrong);
      check_equal(mount_http(0u, fixture.codec), SALTS_OK); listen();
      chttp_response response = {0}; check_equal(request("/value/1", NULL, &response), SALTS_OK);
      check_equal(response.status_code, 200u); chttp_response_destroy(&response);
    }
  }
  for (int rpc = 0; rpc <= 1; ++rpc) {
    it("pins structured plans and stopped DSO until canceled user code returns (RPC=%d)", rpc) {
      fixture.protocol = CHTTP_HTTP_2; fixture.rpc_call = rpc != 0; fixture.block = true;
      fixture.config.deadlines.handler_ms = 100u; start(CHTTP_DISPATCH_PLUGIN);
      check_equal(sync_thread_create(&fixture.thread, request_thread, NULL), SALTS_OK); fixture.thread_live = true;
      wait_for_entry();
      check_equal(cmeta_plugin_registry_request_stop(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK); fixture.stopped = true;
      check_equal(cmeta_plugin_registry_unload(&fixture.registry, fixture.ref), CMETA_PLUGIN_BUSY);
      check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
      check_not_equal(fixture.request_status, SALTS_OK); check_not_equal(fixture.request_status, SALTS_ETIMEDOUT);
      check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
      check_not_null(fixture.app.impl); check_equal(fixture.destroyed, 0); check_equal(leases(), 4u);
      check_equal(atomic_load(&fixture.after), 0); check_equal(atomic_load(&fixture.errors), 0);
      check_equal(fixture.completion_count, 0u);
      sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.destroyed, 1); check_equal(leases(), 0u);
      check_equal(atomic_load(&fixture.after), 1); check_equal(atomic_load(&fixture.errors), 0);
      check_equal(fixture.completion_count, 1u); check_equal(fixture.completions[0].kind, (int)DATA_BIND_BINDING_OUTCOME_SUCCESS);
      check_equal(fixture.completions[0].response.business_status, SALTS_OK);
      check_not_equal(fixture.completions[0].response.transport_status, SALTS_OK);
    }
  }
}
