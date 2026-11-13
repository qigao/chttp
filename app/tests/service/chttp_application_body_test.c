#include "chttp_application_worker.h"
#include "chttp_service_plugin_probe.h"
#include "chttp_worker_body.service_native.h"
#include "chttp_worker_body_native.h"
#include <data_bind_plugin_execution.h>
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include <threads/threads.h>
#include <core/clock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "tinytest.h"

typedef struct BodyController { int marker; } BodyController;
cmeta_reflect_data(BodyController, "test.BodyController", cmeta_field(int, marker));
cmeta_component_empty(BodyComponent);
static const char BODY[] = "{\"id\":18446744073709551615,\"child\":{\"number\":1},\"note\":\"a\\\"\\\\\\n\\u0000z\"}";
static const char EMPTY_NOTE[] = "{\"id\":1,\"child\":{\"number\":1},\"note\":\"\"}";
static struct {
  chttp_application app;
  chttp_application_config config;
  chttp_application_work_config work;
  chttp_client_config client;
  chttp_protocol protocol;
  DataBind *codec;
  DataBindHttpMethodPlan *plan;
  DataBindNativeTypeBinding input, output;
  DataBindServiceNativeBinding native;
  DataBindNativeExecution execution;
  cflow_function_typed_adapter_projection projection;
  chttp_http_method method;
  chttp_application_method_interceptor hook;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  cmeta_plugin_registry registry;
  cmeta_plugin_ref ref;
  cmeta_plugin_lease discovery;
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t thread;
  bool block, release, thread_live, stopped;
  int calls, destroyed, request_status;
  _Atomic int invalid, before, after;
  char uri[64];
} fixture;
static void destroy(void *context, void *object) { (void)context; ++fixture.destroyed; free(object); }
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, destroy};
static cmeta_status create(void *context, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  BodyController *self = malloc(sizeof(*self));
  if (!self) return CMETA_OUT_OF_MEMORY;
  self->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, self, cmeta_reflected_data(BodyController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(self); }
  return status;
}
static bool observe(void *context) {
  if (context != &fixture || !chttp_application_worker_current(&fixture.app)) atomic_fetch_add(&fixture.invalid, 1);
  sync_mutex_lock(&fixture.mutex); ++fixture.calls; sync_cond_broadcast(&fixture.changed);
  while (fixture.block && !fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
  sync_mutex_unlock(&fixture.mutex);
  return true;
}
static cmeta_status before(void *self, const chttp_method_call *call, bool *proceed) {
  (void)proceed;
  const Request_t *input = call->input ? call->input->params[0] : NULL;
  if (!self || ((BodyController *)self)->marker != 42 || !chttp_application_worker_current(&fixture.app) ||
      !call->http || call->http->session || !input || input->tenant != 42u ||
      input->payload.child.number != 1u) {
    atomic_fetch_add(&fixture.invalid, 1); return CMETA_TYPE_MISMATCH;
  }
  if (input->payload.id == UINT64_MAX) {
    static const char text[] = {'a', '"', '\\', '\n', '\0', 'z'};
    if (tstr_len(input->payload.note) != sizeof(text) || memcmp(input->payload.note, text, sizeof(text)))
      atomic_fetch_add(&fixture.invalid, 1);
  }
  atomic_fetch_add(&fixture.before, 1);
  return CMETA_OK;
}
static void after(void *self, const chttp_method_call *call, const chttp_method_result *result) {
  (void)self;
  const Request_t *input = call->input->params[0];
  const Response_t *output = result->output ? result->output->params[1] : NULL;
  if (!chttp_application_worker_current(&fixture.app) || !output || output->payload.id != input->payload.id ||
      tstr_len(output->payload.note) != tstr_len(input->payload.note) ||
      (tstr_len(input->payload.note) && memcmp(output->payload.note, input->payload.note, tstr_len(input->payload.note))))
    atomic_fetch_add(&fixture.invalid, 1);
  atomic_fetch_add(&fixture.after, 1);
}
static int request(const char *target, const char *body, const char *media, chttp_response *response) {
  chttp_client client = {0}; chttp_error error = {0};
  int status = chttp_client_init(&client, &fixture.client);
  if (status != SALTS_OK) return status;
  const chttp_header header = {"Content-Type", media};
  const chttp_options options = {.connection_uri = fixture.uri, .authority = "test", .target = target,
      .protocol = fixture.protocol, .timeout_ms = 5000u, .body = body, .body_size = body ? strlen(body) : 0u,
      .headers = media ? &header : NULL, .header_count = media ? 1u : 0u};
  status = chttp_post(&client, &options, response, &error);
  const int closed = chttp_client_destroy(&client, 5000u);
  return status == SALTS_OK ? closed : status;
}
static void expect(const char *target, const char *body, const char *media, unsigned int status) {
  chttp_response response = {0};
  check_equal(request(target, body, media, &response), SALTS_OK);
  check_equal(response.status_code, status);
  if (status == 200u) {
    check_equal(chttp_response_header(&response, "Content-Type"), "application/json");
    check_equal(response.body_size, strlen(body));
    check_equal(response.body, body, response.body_size);
  }
  chttp_response_destroy(&response);
}
static size_t leases(void) {
  cmeta_plugin_lifecycle_info info = {0};
  check_equal(cmeta_plugin_registry_get_lifecycle(&fixture.registry, fixture.ref, &info), CMETA_PLUGIN_OK);
  return info.active_leases;
}
static void compile(DataBindFormat format) {
  data_bind_http_method_plan_free(fixture.plan); fixture.plan = NULL;
  static const DataBindHttpFieldProjection fields[] = {
      {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS, "payload", DATA_BIND_HTTP_BODY, "payload", 0u},
      {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS, "tenant", DATA_BIND_HTTP_PATH, "tenant", 1u},
      {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_EGRESS, "payload", DATA_BIND_HTTP_RESPONSE_BODY, "payload", 0u}};
  DataBindHttpProjectionConfig projection = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
  projection.method = "POST"; projection.route = "/echo/{tenant}";
  projection.fields = fields; projection.field_count = 3u; projection.ingress_format = format;
  DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  const DataBindStatus status = data_bind_http_method_plan_compile_service(fixture.codec, "Probe", "Echo", &projection,
      &fixture.native, &fixture.plan, &diagnostic);
  check_equal(status, DATA_BIND_OK);
  fixture.method.plan = fixture.plan;
}
static void initialize(int mode) {
  if (mode == CHTTP_DISPATCH_PLUGIN) {
    const cmeta_plugin_registry_config config = {.capacity = 1u};
    check_equal(cmeta_plugin_registry_init(&fixture.registry, &config), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(&fixture.registry, WORKER_BODY_PLUGIN_PATH, &fixture.ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK);
    const cmeta_plugin_manifest *manifest = NULL;
    const cmeta_plugin_export *catalog = NULL;
    check_equal(cmeta_plugin_registry_acquire(&fixture.registry, fixture.ref, &fixture.discovery, &manifest), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_manifest_find_export(manifest, DATA_BIND_PLUGIN_CATALOG_EXPORT_ID, &catalog), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_export_require_interface(catalog, DATA_BIND_PLUGIN_CATALOG_CONTRACT_ID,
        DATA_BIND_PLUGIN_CATALOG_CONTRACT_VERSION, 0u, data_bind_plugin_catalog_interface()), CMETA_PLUGIN_OK);
    DataBindPluginOperationBinding operation = DATA_BIND_PLUGIN_OPERATION_BINDING_INIT;
    DataBindError error = DATA_BIND_ERROR_INIT;
    check_equal(data_bind_plugin_catalog_operation_at(catalog->value.interface.value, 0u, &operation, &error), DATA_BIND_OK);
    check_equal(operation.operation_name, "Echo");
    fixture.method.dispatch = CHTTP_DISPATCH_PLUGIN;
    fixture.method.binding = NULL; fixture.method.execution = NULL;
    fixture.method.plugin_registry = &fixture.registry; fixture.method.plugin_ref = fixture.ref;
    fixture.method.plugin_export_id = operation.export_id;
  } else if (mode == CHTTP_DISPATCH_CFLOW) {
    check_equal(databind_11_BodyIngress_5_Probe_4_Echo__databind_cflow_projection(&fixture.projection), CFLOW_FUNCTION_PROJECTION_OK);
    fixture.method.dispatch = CHTTP_DISPATCH_CFLOW;
    fixture.method.execution = NULL; fixture.method.projection = &fixture.projection;
  }
  check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
  check_equal(chttp_application_worker_prepare(&fixture.app, &fixture.work), SALTS_OK);
}
static int mount(void) {
  return chttp_application_worker_mount_http(&fixture.app, "BodyComponent", cmeta_reflected_data(BodyController),
      &fixture.method, fixture.codec, &fixture.hook, 1u);
}
static void listen(void) {
  data_bind_free(fixture.codec); fixture.codec = NULL;
  check_equal(chttp_application_start(&fixture.app), SALTS_OK);
  uint16_t port = 0u; check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
  snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
}
static void start(int mode) {
  initialize(mode); check_equal(mount(), SALTS_OK);
  if (fixture.registry.impl) {
    check_equal(cmeta_plugin_registry_release(&fixture.registry, &fixture.discovery), CMETA_PLUGIN_OK);
    check_equal(leases(), 1u);
  }
  listen();
}
static void request_thread(void *unused) {
  (void)unused; chttp_response response = {0};
  fixture.request_status = request("/echo/42", BODY, "application/json", &response);
  chttp_response_destroy(&response);
}
spec("Application owned JSON body ingress") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture)); sync_mutex_init(&fixture.mutex); sync_cond_init(&fixture.changed);
    atomic_init(&fixture.invalid, 0); atomic_init(&fixture.before, 0); atomic_init(&fixture.after, 0);
    DataBindError error = DATA_BIND_ERROR_INIT;
    check_equal(BodyIngress_codec_create(&fixture.codec, &error), DATA_BIND_OK);
    check_equal(databind_11_BodyIngress_5_Probe_4_Echo__databind_native_binding(
        &fixture.input, &fixture.output, &fixture.native, &error), DATA_BIND_OK);
    fixture.execution = *databind_11_BodyIngress_5_Probe_4_Echo__databind_execution();
    fixture.method = (chttp_http_method){.binding = &fixture.native, .execution = &fixture.execution};
    compile(DATA_BIND_FORMAT_JSON);
    fixture.provider = (salts_configurator_provider_binding){sizeof(fixture.provider),
        SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(BodyComponent), NULL, NULL, create, NULL, NULL};
    fixture.deployment = (salts_configurator_deployment){&fixture.provider, NULL, NULL};
    fixture.controller = (chttp_controller){.component_id = "BodyComponent", .receiver = cmeta_reflected_data(BodyController)};
    fixture.config = chttp_application_config_default(); fixture.config.bindings.method_capacity = 1u;
    fixture.config.components = &fixture.deployment; fixture.config.component_count = 1u;
    fixture.config.controllers = &fixture.controller; fixture.config.controller_count = 1u;
    fixture.work = (chttp_application_work_config){1u, 1u, 65536u, 8192u};
    fixture.client = (chttp_client_config){.network = fixture.config.http.network, .request_capacity = 1u,
        .max_start_line_bytes = 256u, .max_header_count = 16u, .max_header_bytes = 4096u,
        .max_request_body_bytes = 8192u, .max_response_body_bytes = 8192u, .max_informational_responses = 4u};
    fixture.hook = (chttp_application_method_interceptor){"BodyComponent", cmeta_reflected_data(BodyController), before, after, NULL, NULL};
    chttp_service_plugin_probe_set(observe, &fixture);
  }
  after_each() {
    sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    if (fixture.thread_live) sync_thread_join(&fixture.thread);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    chttp_service_plugin_probe_set(NULL, NULL); check_equal(atomic_load(&fixture.invalid), 0);
    data_bind_http_method_plan_free(fixture.plan); data_bind_free(fixture.codec);
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
      it("decodes owned uint64, nested record and embedded NUL before hooks (protocol=%d provider=%d)", protocol, mode) {
        fixture.protocol = (chttp_protocol)protocol; start(mode);
        expect("/echo/42", BODY, "Application/JSON ; charset=utf-8", 200u);
        expect("/echo/42", EMPTY_NOTE, "application/json", 200u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.before), 2); check_equal(atomic_load(&fixture.after), 2); check_equal(fixture.calls, 2);
      }
    }
    it("rejects undeclared aliases and then accepts canonical names on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; start(CHTTP_DISPATCH_INLINE);
      chttp_response response = {0};
      check_equal(request("/echo/42", "{\"legacyId\":1,\"child\":{\"number\":1},\"note\":\"\"}", "application/json", &response), SALTS_OK);
      check_equal(response.status_code, 400u);
      chttp_response_destroy(&response);
      expect("/echo/42", EMPTY_NOTE, "application/json", 200u);
    }
    it("rejects non-JSON media before business and reuses its only record on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; start(CHTTP_DISPATCH_INLINE);
      static const char *media[] = {NULL, "text/plain", "application/jsonp", "application/problem+json"};
      for (size_t i = 0u; i < sizeof(media) / sizeof(media[0]); ++i) expect("/echo/42", BODY, media[i], 415u);
      check_equal(atomic_load(&fixture.before), 0); expect("/echo/42", BODY, "application/json", 200u);
    }
    for (size_t m = 0u; m < sizeof(modes) / sizeof(modes[0]); ++m) {
      const int mode = modes[m];
      it("rejects invalid records without hooks and rolls back owned strings (protocol=%d provider=%d)", protocol, mode) {
        fixture.protocol = (chttp_protocol)protocol; start(mode);
        static const char *bad[] = {NULL, "", "{", "null", "[]", "{}",
            "{\"id\":1,\"child\":{\"number\":1}}",
            "{\"id\":1,\"child\":{\"number\":1},\"note\":null}",
            "{\"id\":1,\"child\":{\"number\":1},\"note\":\"owned\",\"extra\":0}",
            "{\"id\":1,\"child\":{\"number\":1},\"note\":\"one\",\"note\":\"two\"}",
            "{\"id\":1,\"legacyId\":2,\"child\":{\"number\":1},\"note\":\"owned\"}",
            "{\"id\":-1,\"child\":{\"number\":1},\"note\":\"owned\"}",
            "{\"id\":18446744073709551616,\"child\":{\"number\":1},\"note\":\"owned\"}"};
        for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
          expect("/echo/42", bad[i], "application/json", 400u);
          check_equal(atomic_load(&fixture.before), (int)i);
          expect("/echo/42", BODY, "application/json", 200u);
        }
        expect("/echo/invalid", BODY, "application/json", 400u);
        expect("/echo/0", BODY, "application/json", 422u);
        expect("/echo/42", "{\"id\":1,\"child\":{\"number\":0},\"note\":\"owned\"}", "application/json", 422u);
        expect("/echo/42", BODY, "application/json", 200u);
      }
    }
    for (int limit = 0; limit < 4; ++limit) {
      it("enforces body/native budgets and recovers the record (protocol=%d limit=%d)", protocol, limit) {
        fixture.protocol = (chttp_protocol)protocol;
        if (limit == 0) fixture.config.http.max_request_body_bytes = sizeof(EMPTY_NOTE) - 1u;
        if (limit == 1) fixture.config.bindings.native_max_owned_bytes = 1u;
        if (limit == 2) fixture.config.bindings.native_max_items = 4u;
        if (limit == 3) fixture.config.bindings.native_max_depth = 2u;
        start(CHTTP_DISPATCH_INLINE);
        if (limit == 0 && protocol == CHTTP_HTTP_2) {
          chttp_response response = {0};
          check_equal(request("/echo/42", BODY, "application/json", &response), SALTS_EPROTO);
          chttp_response_destroy(&response);
        } else expect("/echo/42", BODY, "application/json", 413u);
        check_equal(atomic_load(&fixture.before), 0);
        if (limit < 2) expect("/echo/42", EMPTY_NOTE, "application/json", 200u);
      }
    }
    it("retains the DTO and Plugin lease while finite close meets a blocked method on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; fixture.block = true; start(CHTTP_DISPATCH_PLUGIN);
      check_equal(cmeta_plugin_registry_request_stop(&fixture.registry, fixture.ref), CMETA_PLUGIN_OK); fixture.stopped = true;
      check_equal(sync_thread_create(&fixture.thread, request_thread, NULL), SALTS_OK); fixture.thread_live = true;
      const uint64_t began = monotonic_ms();
      sync_mutex_lock(&fixture.mutex);
      while (!fixture.calls && monotonic_ms() - began < 5000u)
        (void)sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(10000000));
      const int calls = fixture.calls; sync_mutex_unlock(&fixture.mutex); check_equal(calls, 1);
      check_equal(chttp_application_close(&fixture.app, 1u), SALTS_ETIMEDOUT);
      check_equal(fixture.destroyed, 0); check_equal(leases(), 1u);
      check_equal(cmeta_plugin_registry_unload(&fixture.registry, fixture.ref), CMETA_PLUGIN_BUSY);
      sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
      sync_thread_join(&fixture.thread); fixture.thread_live = false;
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.destroyed, 1); check_equal(leases(), 0u); check_equal(atomic_load(&fixture.after), 1);
    }
  }
  it("rejects non-JSON at mount and rolls back a Plugin lease and the route slot") {
    compile(DATA_BIND_FORMAT_BINARY); initialize(CHTTP_DISPATCH_PLUGIN);
    check_equal(mount(), SALTS_ENOTSUP); check_equal(leases(), 1u);
    compile(DATA_BIND_FORMAT_JSON); check_equal(mount(), SALTS_OK); check_equal(leases(), 2u);
    check_equal(cmeta_plugin_registry_release(&fixture.registry, &fixture.discovery), CMETA_PLUGIN_OK);
    listen(); expect("/echo/42", BODY, "application/json", 200u);
  }
  it("rejects a nested native bytes representation before publishing its route") {
    DataBindNativeTypeBinding input = DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindNativeTypeBinding output = DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindServiceNativeBinding native = DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
    DataBindError error = DATA_BIND_ERROR_INIT;
    check_equal(databind_11_BodyIngress_8_RawProbe_4_Size__databind_native_binding(&input, &output, &native, &error), DATA_BIND_OK);
    static const DataBindHttpFieldProjection fields[] = {
        {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS, "payload", DATA_BIND_HTTP_BODY, "payload", 0u},
        {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_EGRESS, "value", DATA_BIND_HTTP_RESPONSE_BODY, "value", 0u}};
    DataBindHttpProjectionConfig projection = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
    projection.method = "POST"; projection.route = "/raw"; projection.fields = fields; projection.field_count = 2u;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindHttpMethodPlan *plan = NULL;
    check_equal(data_bind_http_method_plan_compile_service(fixture.codec, "RawProbe", "Size", &projection, &native, &plan, &diagnostic), DATA_BIND_OK);
    initialize(CHTTP_DISPATCH_INLINE);
    const chttp_http_method method = {.plan = plan, .binding = &native,
        .execution = databind_11_BodyIngress_8_RawProbe_4_Size__databind_execution()};
    const int status = chttp_application_worker_mount_http(&fixture.app, "BodyComponent", cmeta_reflected_data(BodyController),
        &method, fixture.codec, &fixture.hook, 1u);
    /* Keep local native metadata live even if a regression publishes the route. */
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    data_bind_http_method_plan_free(plan);
    check_equal(status, SALTS_ENOTSUP);
  }
}
