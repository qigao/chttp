#include "chttp_application_registration.h"
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
static const char SEQUENCE_BODY[] = "{\"records\":[{\"id\":18446744073709551615,\"child\":{\"number\":1},\"note\":\"a\\\"\\\\\\n\\u0000z\"}],"
    "\"rows\":[[],[0,18446744073709551615]],\"labels\":[\"\",\"a\\\"\\\\\\n\\u0000z\"]}";
static const char EMPTY_SEQUENCE[] = "{\"records\":[],\"rows\":[],\"labels\":[]}";
static const char SHORT_SEQUENCE[] = "{\"records\":[],\"rows\":[[]],\"labels\":[\"x\"]}";
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
  sync_thread_t thread, second_thread;
  bool block, release, thread_live, second_thread_live, stopped, sequence;
  int calls, destroyed;
  _Atomic int invalid, before, after, request_status;
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
  if (context != &fixture || !chttp_application_is_worker(&fixture.app)) atomic_fetch_add(&fixture.invalid, 1);
  sync_mutex_lock(&fixture.mutex); ++fixture.calls; sync_cond_broadcast(&fixture.changed);
  while (fixture.block && !fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
  sync_mutex_unlock(&fixture.mutex);
  return true;
}
static bool batch_records(const Batch_t *batch, cmeta_data_collection_view *view) {
  const cmeta_data_desc *data = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  if (Batch_cmeta_data(&data, &error) != DATA_BIND_OK) return false;
  const cmeta_data_struct_shape *shape = data->shape;
  return cmeta_data_collection_read(shape->fields[0].value, &batch->records, view) == CMETA_OK;
}
static cmeta_status before(void *self, const chttp_method_call *call, bool *proceed) {
  (void)proceed;
  if (!self || ((BodyController *)self)->marker != 42 || !chttp_application_is_worker(&fixture.app) ||
      !call->http || call->http->session || !call->input || !call->input->params[0]) {
    atomic_fetch_add(&fixture.invalid, 1); return CMETA_TYPE_MISMATCH;
  }
  const Detail_t *detail;
  if (fixture.sequence) {
    const BatchRequest_t *input = call->input->params[0];
    cmeta_data_collection_view records = {0};
    if (input->tenant != 42u || !batch_records(&input->payload, &records)) {
      atomic_fetch_add(&fixture.invalid, 1); return CMETA_TYPE_MISMATCH;
    }
    detail = records.count ? records.data : NULL;
  } else {
    const Request_t *input = call->input->params[0];
    if (input->tenant != 42u) { atomic_fetch_add(&fixture.invalid, 1); return CMETA_TYPE_MISMATCH; }
    detail = &input->payload;
  }
  if (detail && detail->child.number != 1u) atomic_fetch_add(&fixture.invalid, 1);
  if (detail && detail->id == UINT64_MAX) {
    static const char text[] = {'a', '"', '\\', '\n', '\0', 'z'};
    if (tstr_len(detail->note) != sizeof(text) || memcmp(detail->note, text, sizeof(text)))
      atomic_fetch_add(&fixture.invalid, 1);
  }
  atomic_fetch_add(&fixture.before, 1);
  return CMETA_OK;
}
static void after(void *self, const chttp_method_call *call, const chttp_method_result *result) {
  (void)self;
  if (fixture.sequence) {
    const BatchRequest_t *input = call->input->params[0];
    const BatchResponse_t *output = result->output ? result->output->params[1] : NULL;
    cmeta_data_collection_view source = {0}, copy = {0};
    if (!chttp_application_is_worker(&fixture.app) || !output ||
        !batch_records(&input->payload, &source) || !batch_records(&output->payload, &copy) || source.count != copy.count)
      atomic_fetch_add(&fixture.invalid, 1);
    else if (source.count) {
      const Detail_t *left = source.data, *right = copy.data;
      const size_t size = tstr_len(left->note);
      if (source.data == copy.data || left->id != right->id || size != tstr_len(right->note) ||
          (size && (left->note == right->note || memcmp(left->note, right->note, size))))
        atomic_fetch_add(&fixture.invalid, 1);
    }
    atomic_fetch_add(&fixture.after, 1);
    return;
  }
  const Request_t *input = call->input->params[0];
  const Response_t *output = result->output ? result->output->params[1] : NULL;
  if (!chttp_application_is_worker(&fixture.app) || !output || output->payload.id != input->payload.id ||
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
  /* Client receipt/destruction does not join the Server's native terminal or
   * worker finalizer. This fixture has one record: settle it before testing
   * the next conversion, and prove the rollback released its reservation. */
  const uint64_t began = monotonic_ms();
  chttp_worker_stats stats = {0};
  for (;;) {
    check_equal(chttp_application_worker_stats(&fixture.app, &stats), SALTS_OK);
    check_equal(stats.publication_status, SALTS_OK);
    if (!stats.pending || monotonic_ms() - began >= 5000u) break;
    sleep_ms(1u);
  }
  check_equal(stats.pending, 0u);
  check_equal(stats.retained_bytes, 0u);
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
  const DataBindStatus status = data_bind_http_method_plan_compile_service(fixture.codec, "Probe", fixture.sequence ? "Batch" : "Echo", &projection,
      &fixture.native, &fixture.plan, &diagnostic);
  check_equal(status, DATA_BIND_OK);
  fixture.method.plan = fixture.plan;
}
static void select_sequence(void) {
  fixture.sequence = true;
  DataBindError error = DATA_BIND_ERROR_INIT;
  check_equal(databind_11_BodyIngress_5_Probe_5_Batch__databind_native_binding(
      &fixture.input, &fixture.output, &fixture.native, &error), DATA_BIND_OK);
  fixture.execution = *databind_11_BodyIngress_5_Probe_5_Batch__databind_execution();
  compile(DATA_BIND_FORMAT_JSON);
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
    const char *name = fixture.sequence ? "Batch" : "Echo";
    bool found = false;
    const size_t count = data_bind_plugin_catalog_operation_count(catalog->value.interface.value);
    for (size_t i = 0u; i < count; ++i) {
      check_equal(data_bind_plugin_catalog_operation_at(catalog->value.interface.value, i, &operation, &error), DATA_BIND_OK);
      if (strcmp(operation.operation_name, name) == 0) { found = true; break; }
    }
    check_true(found);
    fixture.method.dispatch = CHTTP_DISPATCH_PLUGIN;
    fixture.method.binding = NULL; fixture.method.execution = NULL;
    fixture.method.plugin_registry = &fixture.registry; fixture.method.plugin_ref = fixture.ref;
    fixture.method.plugin_export_id = operation.export_id;
  } else if (mode == CHTTP_DISPATCH_CFLOW) {
    check_equal(fixture.sequence ? databind_11_BodyIngress_5_Probe_5_Batch__databind_cflow_projection(&fixture.projection) :
        databind_11_BodyIngress_5_Probe_4_Echo__databind_cflow_projection(&fixture.projection), CFLOW_FUNCTION_PROJECTION_OK);
    fixture.method.dispatch = CHTTP_DISPATCH_CFLOW;
    fixture.method.execution = NULL; fixture.method.projection = &fixture.projection;
  }
  fixture.config.workers = fixture.work;
  check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);

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
  fixture.request_status = request("/echo/42", fixture.sequence ? SEQUENCE_BODY : BODY, "application/json", &response);
  chttp_response_destroy(&response);
}
spec("Application owned JSON body ingress") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture)); sync_mutex_init(&fixture.mutex); sync_cond_init(&fixture.changed);
    atomic_init(&fixture.invalid, 0); atomic_init(&fixture.before, 0); atomic_init(&fixture.after, 0);
    atomic_init(&fixture.request_status, 0);
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
    if (fixture.second_thread_live) sync_thread_join(&fixture.second_thread);
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
    static const int modes[] = {CHTTP_DISPATCH_DIRECT, CHTTP_DISPATCH_CFLOW, CHTTP_DISPATCH_PLUGIN};
    for (size_t m = 0u; m < sizeof(modes) / sizeof(modes[0]); ++m) {
      const int mode = modes[m];
      it("decodes owned uint64, nested record and embedded NUL before hooks (protocol=%d provider=%d)", protocol, mode) {
        fixture.protocol = (chttp_protocol)protocol; start(mode);
        expect("/echo/42", BODY, "Application/JSON ; charset=utf-8", 200u);
        expect("/echo/42", EMPTY_NOTE, "application/json", 200u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.before), 2); check_equal(atomic_load(&fixture.after), 2); check_equal(fixture.calls, 2);
      }
      it("owns nested sequence/record/string values across decoding and execution (protocol=%d provider=%d)", protocol, mode) {
        fixture.protocol = (chttp_protocol)protocol; select_sequence(); start(mode);
        expect("/echo/42", SEQUENCE_BODY, "application/json", 200u);
        expect("/echo/42", EMPTY_SEQUENCE, "application/json", 200u);
        expect("/echo/42", SHORT_SEQUENCE, "application/json", 200u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.before), 3); check_equal(atomic_load(&fixture.after), 3); check_equal(fixture.calls, 3);
      }
      it("rolls back partially decoded sequences and validates record elements (protocol=%d provider=%d)", protocol, mode) {
        fixture.protocol = (chttp_protocol)protocol; select_sequence(); start(mode);
        static const char *bad[] = {
            "{\"records\":[],\"rows\":[],\"labels\":null}",
            "{\"records\":[],\"rows\":[],\"labels\":[\"owned\",0]}",
            "{\"records\":[],\"rows\":[[1],[-1]],\"labels\":[]}",
            "{\"records\":[],\"rows\":[[1],[18446744073709551616]],\"labels\":[]}",
            "{\"records\":[{}],\"rows\":[],\"labels\":[]}",
            "{\"records\":[{\"id\":1,\"child\":{\"number\":1},\"note\":\"owned\"},{}],\"rows\":[],\"labels\":[]}",
            "{\"records\":[],\"rows\":[],\"labels\":[\"owned\"],\"labels\":[]}",
            "{\"records\":[],\"rows\":[],\"labels\":[\"owned\"],\"extra\":0}"};
        for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
          expect("/echo/42", bad[i], "application/json", 400u);
          check_equal(atomic_load(&fixture.before), (int)i);
          expect("/echo/42", SEQUENCE_BODY, "application/json", 200u);
        }
        expect("/echo/42", "{\"records\":[{\"id\":1,\"child\":{\"number\":0},\"note\":\"owned\"}],\"rows\":[],\"labels\":[]}",
            "application/json", 422u);
        check_equal(atomic_load(&fixture.before), (int)(sizeof(bad) / sizeof(bad[0])));
        expect("/echo/invalid", SEQUENCE_BODY, "application/json", 400u);
        expect("/echo/42", EMPTY_SEQUENCE, "application/json", 200u);
      }
      it("isolates concurrent sequence frames and drains both workers (protocol=%d provider=%d)", protocol, mode) {
        fixture.protocol = (chttp_protocol)protocol; select_sequence(); fixture.block = true;
        fixture.work.workers = 2u; fixture.work.capacity = 2u; start(mode);
        check_equal(sync_thread_create(&fixture.thread, request_thread, NULL), SALTS_OK); fixture.thread_live = true;
        check_equal(sync_thread_create(&fixture.second_thread, request_thread, NULL), SALTS_OK); fixture.second_thread_live = true;
        const uint64_t began = monotonic_ms();
        sync_mutex_lock(&fixture.mutex);
        while (fixture.calls < 2 && monotonic_ms() - began < 5000u)
          (void)sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(10000000));
        const int calls = fixture.calls; sync_mutex_unlock(&fixture.mutex); check_equal(calls, 2);
        check_equal(chttp_application_close(&fixture.app, 1u), SALTS_ETIMEDOUT); check_equal(fixture.destroyed, 0);
        sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
        sync_thread_join(&fixture.thread); fixture.thread_live = false;
        sync_thread_join(&fixture.second_thread); fixture.second_thread_live = false;
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(atomic_load(&fixture.before), 2); check_equal(atomic_load(&fixture.after), 2); check_equal(fixture.destroyed, 1);
      }
    }
    it("rejects undeclared aliases and then accepts canonical names on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; start(CHTTP_DISPATCH_DIRECT);
      chttp_response response = {0};
      check_equal(request("/echo/42", "{\"legacyId\":1,\"child\":{\"number\":1},\"note\":\"\"}", "application/json", &response), SALTS_OK);
      check_equal(response.status_code, 400u);
      chttp_response_destroy(&response);
      expect("/echo/42", EMPTY_NOTE, "application/json", 200u);
    }
    it("rejects non-JSON media before business and reuses its only record on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; start(CHTTP_DISPATCH_DIRECT);
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
        start(CHTTP_DISPATCH_DIRECT);
        if (limit == 0 && protocol == CHTTP_HTTP_2) {
          chttp_response response = {0};
          check_equal(request("/echo/42", BODY, "application/json", &response), SALTS_EPROTO);
          chttp_response_destroy(&response);
        } else expect("/echo/42", BODY, "application/json", 413u);
        check_equal(atomic_load(&fixture.before), 0);
        if (limit < 2) expect("/echo/42", EMPTY_NOTE, "application/json", 200u);
      }
    }
    for (int limit = 0; limit < 2; ++limit) {
      it("bounds aggregate sequence buffers/items and reuses the slot (protocol=%d limit=%d)", protocol, limit) {
        fixture.protocol = (chttp_protocol)protocol; select_sequence();
        if (limit == 0) fixture.config.bindings.native_max_owned_bytes = 8u;
        else fixture.config.bindings.native_max_items = 32u;
        start(CHTTP_DISPATCH_PLUGIN);
        const char *body = SEQUENCE_BODY;
        if (limit == 1) body = "{\"records\":[],\"rows\":[],\"labels\":["
            "\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\","
            "\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\","
            "\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\","
            "\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\",\"x\"]}";
        expect("/echo/42", body, "application/json", 413u);
        check_equal(atomic_load(&fixture.before), 0);
        expect("/echo/42", SHORT_SEQUENCE, "application/json", 200u);
        expect("/echo/42", EMPTY_SEQUENCE, "application/json", 200u);
      }
    }
    it("aborts oversized sequence output without retaining its native containers on protocol %d", protocol) {
      fixture.protocol = (chttp_protocol)protocol; select_sequence(); fixture.config.bindings.max_response_body_bytes = 128u;
      start(CHTTP_DISPATCH_PLUGIN);
      expect("/echo/42", SEQUENCE_BODY, "application/json", 500u);
      expect("/echo/42", EMPTY_SEQUENCE, "application/json", 200u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.calls, 2); check_equal(atomic_load(&fixture.after), 1);
    }
    for (int sequence = 0; sequence < 2; ++sequence) {
      it("retains the DTO and Plugin lease during finite close (protocol=%d sequence=%d)", protocol, sequence) {
        fixture.protocol = (chttp_protocol)protocol; fixture.block = true;
        if (sequence) select_sequence();
        start(CHTTP_DISPATCH_PLUGIN);
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
  }
  it("rejects non-JSON at mount and rolls back a Plugin lease and the route slot") {
    compile(DATA_BIND_FORMAT_BINARY); initialize(CHTTP_DISPATCH_PLUGIN);
    check_equal(mount(), SALTS_ENOTSUP); check_equal(leases(), 1u);
    compile(DATA_BIND_FORMAT_JSON); check_equal(mount(), SALTS_OK); check_equal(leases(), 2u);
    check_equal(cmeta_plugin_registry_release(&fixture.registry, &fixture.discovery), CMETA_PLUGIN_OK);
    listen(); expect("/echo/42", BODY, "application/json", 200u);
  }
  for (int sequence = 0; sequence < 2; ++sequence) {
    it("rejects nested native bytes before publishing its route (sequence=%d)", sequence) {
      DataBindNativeTypeBinding input = DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
      DataBindNativeTypeBinding output = DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
      DataBindServiceNativeBinding native = DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
      DataBindError error = DATA_BIND_ERROR_INIT;
      check_equal(sequence ? databind_11_BodyIngress_8_RawProbe_12_SequenceSize__databind_native_binding(&input, &output, &native, &error) :
          databind_11_BodyIngress_8_RawProbe_4_Size__databind_native_binding(&input, &output, &native, &error), DATA_BIND_OK);
      static const DataBindHttpFieldProjection fields[] = {
          {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS, "payload", DATA_BIND_HTTP_BODY, "payload", 0u},
          {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_EGRESS, "value", DATA_BIND_HTTP_RESPONSE_BODY, "value", 0u}};
      DataBindHttpProjectionConfig projection = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
      projection.method = "POST"; projection.route = "/raw"; projection.fields = fields; projection.field_count = 2u;
      DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
      DataBindHttpMethodPlan *plan = NULL;
      check_equal(data_bind_http_method_plan_compile_service(fixture.codec, "RawProbe", sequence ? "SequenceSize" : "Size",
          &projection, &native, &plan, &diagnostic), DATA_BIND_OK);
      initialize(CHTTP_DISPATCH_DIRECT);
      const chttp_http_method method = {.plan = plan, .binding = &native,
          .execution = sequence ? databind_11_BodyIngress_8_RawProbe_12_SequenceSize__databind_execution() :
              databind_11_BodyIngress_8_RawProbe_4_Size__databind_execution()};
      const int status = chttp_application_worker_mount_http(&fixture.app, "BodyComponent", cmeta_reflected_data(BodyController),
          &method, fixture.codec, &fixture.hook, 1u);
      /* Keep local native metadata live even if a regression publishes the route. */
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      data_bind_http_method_plan_free(plan);
      check_equal(status, SALTS_ENOTSUP);
    }
  }
}
