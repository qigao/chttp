#include "chttp_application_worker.h"
#include "chttp_service_plugin_probe.h"
#include "chttp_service_plugin.http.h"
#include "chttp_service_plugin.service_native.h"
#include "chttp_service_plugin_native.h"
#include <http_client/http.h>
#include <data_bind_plugin_execution.h>
#include <cmeta/data_reflect.h>
#include <threads/threads.h>
#include <core/clock.h>
#include <json_parser.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "tinytest.h"

typedef struct PluginController { int marker; } PluginController;
cmeta_reflect_data(PluginController, "test.PluginController", cmeta_field(int, marker));
cmeta_component_empty(PluginComponent);
static struct {
  chttp_application app;
  cmeta_plugin_registry registry;
  cmeta_plugin_ref ref;
  cmeta_plugin_lease discovery;
  bool stopped;
  DataBind *contract;
  DataBindHttpMethodPlan *http_plan;
  DataBindRpcMethodPlan *rpc_plan;
  DataBindNativeTypeBinding input, output;
  DataBindServiceNativeBinding binding;
  DataBindNativeExecution provider_execution, rpc_execution;
  chttp_http_method http;
  chttp_rpc_method rpc;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  chttp_application_config config;
  chttp_client_config client;
  chttp_protocol protocol;
  char uri[64];
  sync_mutex_t mutex;
  sync_cond_t changed;
  bool block, release, fail, thread_live, rpc_call;
  sync_thread_t thread;
  int entries, wrong_thread, wrong_context, destroyed, request_status;
  unsigned int response_status;
  chttp_application_method_interceptor hook;
  _Atomic int hook_after, hook_errors, hook_invalid;
} fixture;

static void destroy(void *user, void *object) {
  (void)user; ++fixture.destroyed; free(object);
}
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, destroy};
static cmeta_status create(void *user, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)user; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  PluginController *self = malloc(sizeof(*self));
  if (!self) return CMETA_OUT_OF_MEMORY;
  self->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, self, cmeta_reflected_data(PluginController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(self); }
  return status;
}
/* Called from the actual generated Plugin business function in the loaded DSO. */
static bool observe(void *user) {
  const bool worker = user == &fixture && chttp_application_worker_current(&fixture.app);
  sync_mutex_lock(&fixture.mutex);
  ++fixture.entries;
  if (!worker) ++fixture.wrong_thread;
  sync_cond_broadcast(&fixture.changed);
  while (fixture.block && !fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
  const bool good = !fixture.fail;
  sync_mutex_unlock(&fixture.mutex);
  return good;
}
/* RPC permits a producer-owned execution context. Keep that context, then call
 * real provider code; substituting the ACE Controller here is a contract error. */
static bool DATA_BIND_NATIVE_CALL rpc_invoke(void *context, void *result,
    void *const *params, size_t count) {
  if (context != &fixture.rpc_execution) {
    sync_mutex_lock(&fixture.mutex); ++fixture.wrong_context; sync_mutex_unlock(&fixture.mutex);
    return false;
  }
  return fixture.provider_execution.invoke(fixture.provider_execution.context, result, params, count);
}
static cmeta_status method_before(void *self, const chttp_method_call *call, bool *proceed) {
  (void)proceed;
  if (!self || ((PluginController *)self)->marker != 42 || !chttp_application_worker_current(&fixture.app) ||
      !call->http || call->http->session || !call->input || !cmeta_function_desc_valid(call->operation)) {
    atomic_fetch_add(&fixture.hook_invalid, 1); return CMETA_TYPE_MISMATCH;
  }
  return CMETA_OK;
}
static void method_after(void *self, const chttp_method_call *call, const chttp_method_result *result) {
  (void)self; (void)call;
  if (!chttp_application_worker_current(&fixture.app) || !result->output || !result->invoked ||
      result->outcome.kind != DATA_BIND_BINDING_OUTCOME_SUCCESS) atomic_fetch_add(&fixture.hook_invalid, 1);
  atomic_fetch_add(&fixture.hook_after, 1);
}
static void method_error(void *self, const chttp_method_call *call, cmeta_status status) {
  (void)self;
  if (!chttp_application_worker_current(&fixture.app) || status == CMETA_OK || !call->result->invoked)
    atomic_fetch_add(&fixture.hook_invalid, 1);
  atomic_fetch_add(&fixture.hook_errors, 1);
}
static size_t leases(void) {
  cmeta_plugin_lifecycle_info info = {0};
  check_equal(cmeta_plugin_registry_get_lifecycle(&fixture.registry, fixture.ref, &info), CMETA_PLUGIN_OK);
  return info.active_leases;
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
  return closed == SALTS_OK ? status : closed;
}
static int rpc_request(void) {
  chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  int status = chttp_client_init(&client, &fixture.client);
  if (status != SALTS_OK) return status;
  const char body[] = "{\"jsonrpc\":\"2.0\",\"method\":\"Calc.Add\",\"params\":[3,4],\"id\":1}";
  const chttp_options options = {.connection_uri = fixture.uri, .authority = "test", .target = "/rpc",
      .protocol = fixture.protocol, .body = body, .body_size = sizeof(body) - 1u, .timeout_ms = 5000u};
  status = chttp_post(&client, &options, &response, &error);
  if (status == SALTS_OK) {
    json_value_t *root = json_parse(response.body, response.body_size);
    const json_value_t *value = root ? json_object_get(root, "result") : NULL;
    if (response.status_code != 200u || !value || json_number(value) != 7.0) status = SALTS_EPROTO;
    json_free(root);
  }
  chttp_response_destroy(&response);
  const int closed = chttp_client_destroy(&client, 5000u);
  return closed == SALTS_OK ? status : closed;
}
static void request_thread(void *unused) {
  (void)unused;
  fixture.request_status = fixture.rpc_call ? rpc_request() :
      request("/plugin/3?right=4", &fixture.response_status);
}
static void prepare(void) {
  check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
  const chttp_application_work_config limits = {1u, 2u, 32768u, 8192u};
  check_equal(chttp_application_worker_prepare(&fixture.app, &limits), SALTS_OK);
}
static int mount_http(void) {
  return chttp_application_worker_mount_http(&fixture.app, "PluginComponent",
      cmeta_reflected_data(PluginController), &fixture.http, fixture.contract, &fixture.hook, 1u);
}
static void start(void) {
  check_equal(mount_http(), SALTS_OK);
  check_equal(chttp_application_worker_mount_rpc(&fixture.app, "PluginComponent",
      cmeta_reflected_data(PluginController), &fixture.rpc, fixture.contract, &fixture.hook, 1u), SALTS_OK);
  check_equal(cmeta_plugin_registry_release(&fixture.registry, &fixture.discovery), CMETA_PLUGIN_OK);
  check_equal(leases(), 2u);
  check_equal(chttp_application_start(&fixture.app), SALTS_OK);
  uint16_t port = 0u;
  check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
  snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
}
static void stop_provider(void) {
  check_equal(cmeta_plugin_registry_request_stop(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK);
  fixture.stopped = true;
  check_equal(cmeta_plugin_registry_unload(&fixture.registry, fixture.ref), CMETA_PLUGIN_BUSY);
}
static void wait_for_entry(void) {
  const uint64_t began = monotonic_ms();
  sync_mutex_lock(&fixture.mutex);
  while (!fixture.entries && monotonic_ms() - began < 5000u)
    (void)sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(10000000));
  const int entries = fixture.entries;
  sync_mutex_unlock(&fixture.mutex);
  check_equal(entries, 1);
}

spec("Application worker Plugin lifetime") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    atomic_init(&fixture.hook_after, 0); atomic_init(&fixture.hook_errors, 0); atomic_init(&fixture.hook_invalid, 0);
    sync_mutex_init(&fixture.mutex); sync_cond_init(&fixture.changed);
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    check_equal(CHttpPlugin_codec_create(&fixture.contract, &error), DATA_BIND_OK);
    check_equal(databind_11_CHttpPlugin_4_Calc_3_Add__databind_native_binding(
        &fixture.input, &fixture.output, &fixture.binding, &error), DATA_BIND_OK);
    const DataBindHttpProjectionConfig *http = data_bind_http_projection_artifact_find(
        &databind_chttp_service_plugin_http_projection, "Calc", "Add");
    check_equal(data_bind_http_method_plan_compile_service(fixture.contract, "Calc", "Add",
        http, &fixture.binding, &fixture.http_plan, &diagnostic), DATA_BIND_OK);
    static const DataBindRpcFieldProjection fields[] = {
        {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_INGRESS, "left", "left", 0u},
        {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_INGRESS, "right", "right", 1u},
        {sizeof(DataBindRpcFieldProjection), DATA_BIND_BINDING_EGRESS, "sum", "sum", 0u}};
    DataBindRpcProjectionConfig rpc = DATA_BIND_RPC_PROJECTION_CONFIG_INIT;
    rpc.wire_method = "Calc.Add"; rpc.fields = fields; rpc.field_count = 3u;
    check_equal(data_bind_rpc_method_plan_compile_service(fixture.contract, "Calc", "Add",
        &rpc, &fixture.binding, &fixture.rpc_plan, &diagnostic), DATA_BIND_OK);
    const cmeta_plugin_registry_config registry = {.capacity = 1u};
    check_equal(cmeta_plugin_registry_init(&fixture.registry, &registry), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(&fixture.registry, GENERATED_CHTTP_SERVICE_PLUGIN_PATH,
        &fixture.ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK);
    const cmeta_plugin_manifest *manifest = NULL;
    const cmeta_plugin_export *catalog_export = NULL, *function = NULL;
    check_equal(cmeta_plugin_registry_acquire(&fixture.registry, fixture.ref,
        &fixture.discovery, &manifest), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_manifest_find_export(manifest, DATA_BIND_PLUGIN_CATALOG_EXPORT_ID,
        &catalog_export), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_export_require_interface(catalog_export, DATA_BIND_PLUGIN_CATALOG_CONTRACT_ID,
        DATA_BIND_PLUGIN_CATALOG_CONTRACT_VERSION, 0u, data_bind_plugin_catalog_interface()), CMETA_PLUGIN_OK);
    DataBindPluginOperationBinding operation = DATA_BIND_PLUGIN_OPERATION_BINDING_INIT;
    check_equal(data_bind_plugin_catalog_operation_at(catalog_export->value.interface.value,
        0u, &operation, &error), DATA_BIND_OK);
    check_equal(cmeta_plugin_manifest_find_export(manifest, operation.export_id, &function), CMETA_PLUGIN_OK);
    check_true(data_bind_plugin_operation_execution_admit(&operation, function, &fixture.provider_execution));
    fixture.rpc_execution = fixture.provider_execution;
    fixture.rpc_execution.context = &fixture.rpc_execution; fixture.rpc_execution.invoke = rpc_invoke;
    fixture.http = (chttp_http_method){.plan = fixture.http_plan, .dispatch = CHTTP_DISPATCH_PLUGIN,
        .plugin_registry = &fixture.registry, .plugin_ref = fixture.ref, .plugin_export_id = "CHttpPlugin.Calc.Add"};
    fixture.rpc = (chttp_rpc_method){.path = "/rpc", .plan = fixture.rpc_plan, .binding = &fixture.binding,
        .execution = &fixture.rpc_execution, .plugin_registry = &fixture.registry, .plugin_ref = fixture.ref};
    fixture.provider = (salts_configurator_provider_binding){sizeof(fixture.provider),
        SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(PluginComponent),
        NULL, NULL, create, NULL, NULL};
    fixture.deployment = (salts_configurator_deployment){&fixture.provider, NULL, NULL};
    fixture.controller = (chttp_controller){.component_id = "PluginComponent",
        .receiver = cmeta_reflected_data(PluginController)};
    fixture.hook = (chttp_application_method_interceptor){"PluginComponent",
        cmeta_reflected_data(PluginController), method_before, method_after, method_error};
    fixture.config = chttp_application_config_default();
    fixture.config.bindings.method_capacity = 1u; fixture.config.rpc.method_capacity = 1u;
    fixture.config.components = &fixture.deployment; fixture.config.component_count = 1u;
    fixture.config.controllers = &fixture.controller; fixture.config.controller_count = 1u;
    fixture.client = (chttp_client_config){.network = fixture.config.http.network,
        .request_capacity = 1u, .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
        .max_response_body_bytes = 8192u, .max_informational_responses = 4u};
    chttp_service_plugin_probe_set(observe, &fixture);
  }
  after_each() {
    sync_mutex_lock(&fixture.mutex); fixture.release = true;
    sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    if (fixture.thread_live) { sync_thread_join(&fixture.thread); fixture.thread_live = false; }
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    chttp_service_plugin_probe_set(NULL, NULL);
    check_equal(fixture.wrong_thread, 0); check_equal(fixture.wrong_context, 0);
    check_equal(atomic_load(&fixture.hook_invalid), 0);
    if (cmeta_plugin_lease_valid(fixture.discovery))
      check_equal(cmeta_plugin_registry_release(&fixture.registry, &fixture.discovery), CMETA_PLUGIN_OK);
    data_bind_rpc_method_plan_free(fixture.rpc_plan);
    data_bind_http_method_plan_free(fixture.http_plan); data_bind_free(fixture.contract);
    if (cmeta_plugin_ref_valid(fixture.ref)) {
      if (!fixture.stopped)
        check_equal(cmeta_plugin_registry_request_stop(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK);
      check_equal(leases(), 0u);
      bool quiet = false;
      check_equal(cmeta_plugin_registry_poll_quiescent(&fixture.registry, fixture.ref, &quiet), CMETA_PLUGIN_OK);
      check_true(quiet);
      check_equal(cmeta_plugin_registry_unload(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK);
    }
    check_equal(cmeta_plugin_registry_destroy(&fixture.registry), CMETA_PLUGIN_OK);
    sync_cond_destroy(&fixture.changed); sync_mutex_destroy(&fixture.mutex);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    it("executes real leased HTTP/RPC provider code on the owned worker for protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol;
      prepare(); start(); stop_provider();
      unsigned int code = 0u;
      check_equal(request("/plugin/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
      check_equal(request("/plugin/0?right=4", &code), SALTS_OK); check_equal(code, 422u);
      check_equal(request("/plugin/3", &code), SALTS_OK); check_equal(code, 400u);
      check_equal(rpc_request(), SALTS_OK);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.entries, 2); check_equal(fixture.destroyed, 1); check_equal(leases(), 0u);
      check_equal(atomic_load(&fixture.hook_after), 2); check_equal(atomic_load(&fixture.hook_errors), 0);
    }
    it("releases Plugin output after serialization failure for protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; fixture.config.bindings.max_response_body_bytes = 1u;
      prepare(); start();
      unsigned int code = 0u;
      check_equal(request("/plugin/3?right=44", &code), SALTS_OK); check_equal(code, 500u);
      check_equal(request("/plugin/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.entries, 2); check_equal(leases(), 0u);
    }
    it("keeps native provider errors explicit for protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; fixture.fail = true;
      prepare(); start();
      unsigned int code = 0u;
      check_equal(request("/plugin/3?right=4", &code), SALTS_OK); check_equal(code, 500u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.entries, 1); check_equal(leases(), 0u);
      check_equal(atomic_load(&fixture.hook_after), 0); check_equal(atomic_load(&fixture.hook_errors), 1);
    }
  }
  it("rolls back a failed catalog resolution before publishing the route") {
    prepare();
    fixture.http.plugin_export_id = "CHttpPlugin.Calc.Missing";
    check_equal(mount_http(), SALTS_EINVAL); check_equal(leases(), 1u);
    fixture.http.plugin_export_id = "CHttpPlugin.Calc.Add";
    start();
    unsigned int code = 0u;
    check_equal(request("/plugin/3?right=4", &code), SALTS_OK); check_equal(code, 201u);
  }
  it("rejects new HTTP/RPC mounts after provider stop without acquiring another lease") {
    prepare(); stop_provider();
    check_equal(mount_http(), SALTS_EINVAL);
    check_equal(chttp_application_worker_mount_rpc(&fixture.app, "PluginComponent",
        cmeta_reflected_data(PluginController), &fixture.rpc, fixture.contract, NULL, 0u), SALTS_EBUSY);
    check_equal(leases(), 1u); check_equal(fixture.entries, 0);
  }
  for (int rpc = 0; rpc <= 1; ++rpc) {
    it("pins stopped DSO and Beans until a canceled running method exits (RPC=%d)", rpc) {
      fixture.rpc_call = rpc != 0;
      fixture.protocol = CHTTP_HTTP_2; fixture.block = true; fixture.config.deadlines.handler_ms = 100u;
      prepare(); start();
      check_equal(sync_thread_create(&fixture.thread, request_thread, NULL), SALTS_OK);
      fixture.thread_live = true;
      wait_for_entry(); stop_provider();
      check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
      check_not_equal(fixture.request_status, SALTS_OK); check_not_equal(fixture.request_status, SALTS_ETIMEDOUT);
      check_equal(atomic_load(&fixture.hook_after), 0); check_equal(atomic_load(&fixture.hook_errors), 0);
      check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
      check_not_null(fixture.app.impl); check_equal(fixture.destroyed, 0); check_equal(leases(), 2u);
      chttp_server_stats stopped = {0};
      check_equal(chttp_application_stats(&fixture.app, &stopped), SALTS_OK);
      check_equal(stopped.terminal_status, SALTS_OK);
      check_equal(cmeta_plugin_registry_unload(&fixture.registry, fixture.ref), CMETA_PLUGIN_BUSY);
      sync_mutex_lock(&fixture.mutex); fixture.release = true;
      sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.entries, 1); check_equal(fixture.destroyed, 1); check_equal(leases(), 0u);
      check_equal(atomic_load(&fixture.hook_after), 1); check_equal(atomic_load(&fixture.hook_errors), 0);
    }
  }
}
