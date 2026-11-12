#include <chttp_app/app.h>
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNTER_METHODS(X,I) X(I,R0,int,value,_)
CMETA_INTERFACE(TestCounter, COUNTER_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(TestCounter);
typedef struct TestController { int initialized; TestCounter counter; } TestController;
cmeta_reflect_data(TestController, "test.application.Controller", cmeta_field(int, initialized));
cmeta_component(TestServiceComponent, cmeta_provides(TestCounter));
cmeta_component(TestControllerComponent, cmeta_requires(TestCounter));
cmeta_component_empty(TestHookA);
cmeta_component_empty(TestHookB);

typedef struct ProviderState { int kind; bool fail; cmeta_object_lifecycle lifecycle; } ProviderState;
static struct {
  chttp_application app;
  chttp_client client;
  chttp_websocket_client websocket;
  chttp_application_config config;
  ProviderState states[4];
  salts_configurator_provider_binding providers[4];
  salts_configurator_deployment deployments[4];
  chttp_controller controller;
  chttp_action actions[4];
  chttp_interceptor hooks[2];
  chttp_web_asset_mount assets;
  chttp_web_security_policy security;
  char *asset_root;
  bool reject_assets;
  size_t response_size;
  int creates, destroys, calls, opened, closed, body_bytes, body_status;
  int destruction[4], trace[32];
  size_t trace_count;
} fixture;

static int counter_value(void *self) { return *(int *)self; }
CMETA_IMPLEMENTS(TestCounter, counter_impl, 0u, .value = counter_value);
static cmeta_status counter_project(void *context, const cmeta_object_ref *object,
    const cmeta_interface_desc *expected, cmeta_interface_projection *out) {
  (void)context;
  if (!cmeta_interface_desc_equal(expected, TestCounter_interface())) return CMETA_TRAIT_MISSING;
  *out = (cmeta_interface_projection){sizeof(*out), TestCounter_interface(), object->object, &counter_impl_vtable};
  return CMETA_OK;
}
static const cmeta_object_interface_provider counter_interfaces = {
    sizeof(counter_interfaces), NULL, counter_project};
static void bean_destroy(void *context, void *object) {
  ProviderState *state = context;
  if (fixture.destroys < 4) fixture.destruction[fixture.destroys] = state->kind;
  ++fixture.destroys;
  free(object);
}
static cmeta_status bean_create(void *context, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *deps, size_t count,
    cmeta_object_ref *out) {
  ProviderState *state = context;
  cmeta_status status;
  (void)data; (void)config;
  ++fixture.creates;
  if (state->kind == 1) {
    const salts_configurator_dependency *dependency = NULL;
    if (salts_configurator_dependency_find(deps, count, TestCounter_interface(), &dependency)
        != SALTS_CONFIGURATOR_OK) return CMETA_CALLBACK_ERROR;
    TestController *controller = calloc(1u, sizeof(*controller));
    if (controller == NULL) return CMETA_OUT_OF_MEMORY;
    status = TestCounter_borrow_from_object(dependency->provider_instance,
        dependency->provider_interfaces, &controller->counter);
    if (status == CMETA_OK) {
      controller->initialized = 1;
      status = cmeta_object_borrow(out, controller, cmeta_reflected_data(TestController), NULL);
    }
    if (status != CMETA_OK) { free(controller); return status; }
  } else {
    int *value = malloc(sizeof(*value));
    if (value == NULL) return CMETA_OUT_OF_MEMORY;
    *value = state->kind == 0 ? 42 : state->kind;
    status = cmeta_object_borrow(out, value, &cmeta_data_int, NULL);
    if (status != CMETA_OK) { free(value); return status; }
  }
  status = cmeta_object_take(out, &state->lifecycle);
  return status == CMETA_OK && state->fail ? CMETA_CALLBACK_ERROR : status;
}

CHTTP_ACTION(hello);
int hello(void *self, const chttp_exchange *exchange) {
  TestController *controller = self;
  char value[32];
  ++fixture.calls;
  const int size = snprintf(value, sizeof(value), "%d", TestCounter_value(&controller->counter));
  return size < 0 || (size_t)size >= sizeof(value) ? SALTS_ERANGE :
      chttp_server_reply(exchange->response, 200u, "text/plain", value, (size_t)size);
}
CHTTP_ACTION(failure);
int failure(void *self, const chttp_exchange *exchange) {
  (void)self; (void)exchange; ++fixture.calls; return SALTS_EINVAL;
}
static int body_write(void *self, const void *bytes, size_t size) {
  TestController *controller = self;
  if (controller->initialized != 1 || TestCounter_value(&controller->counter) != 42 ||
      size > 3u - (size_t)fixture.body_bytes ||
      memcmp(bytes, "abc" + fixture.body_bytes, size) != 0) return SALTS_EPROTO;
  fixture.body_bytes += (int)size;
  return SALTS_OK;
}
static int body_open(void *self, const chttp_server_request_view *request, chttp_body_sink *sink) {
  TestController *controller = self;
  (void)request;
  if (controller->initialized != 1) return SALTS_EINVAL;
  ++fixture.opened;
  *sink = (chttp_body_sink){body_write, self};
  return SALTS_OK;
}
static void body_close(void *self, chttp_body_sink *sink, int status) {
  fixture.body_status = self == sink->user ? status : SALTS_EINVAL;
  ++fixture.closed;
}
static int websocket_open(void *self, chttp_websocket *websocket,
    const chttp_server_request_view *request, chttp_server_response *response) {
  TestController *controller = self;
  (void)request; (void)response;
  if (controller->initialized != 1 || TestCounter_value(&controller->counter) != 42) return SALTS_EPROTO;
  return chttp_websocket_send_text(websocket, "42", 2u);
}
static void websocket_event(void *self, chttp_websocket *websocket, const chttp_websocket_event *event) {
  (void)self; (void)websocket; (void)event;
}
static int scoped_error(chttp_application *app, void *context) {
  (void)app;
  ++*(int *)context;
  return SALTS_EPERM;
}
static void trace(int value) {
  if (fixture.trace_count < sizeof(fixture.trace) / sizeof(fixture.trace[0]))
    fixture.trace[fixture.trace_count++] = value;
}
static cmeta_status before(void *self, const chttp_exchange *exchange, bool *proceed) {
  const int hook = *(int *)self;
  trace(hook == 2 ? 1 : 2);
  if (fixture.config.assets != NULL) {
    if (exchange->operation != NULL) return CMETA_TYPE_MISMATCH;
  } else if (!cmeta_function_desc_valid(exchange->operation)) return CMETA_TYPE_MISMATCH;
  if (hook == 3 && (fixture.reject_assets || strcmp(exchange->request->path, "/reject") == 0)) {
    if (chttp_exchange_reject(exchange, 403u, NULL, NULL, 0u) != SALTS_OK) return CMETA_CALLBACK_ERROR;
    *proceed = false;
  }
  return CMETA_OK;
}
static void after(void *self, const chttp_exchange *exchange, const chttp_exchange_result *result) {
  (void)exchange; (void)result; trace(*(int *)self == 3 ? 3 : 4);
}
static void on_error(void *self, const chttp_exchange *exchange, cmeta_status status) {
  (void)exchange; (void)status; trace(*(int *)self == 3 ? -3 : -4);
}
static int request(chttp_protocol protocol, const char *target, unsigned int *http_status) {
  uint16_t port = 0u;
  char uri[64];
  chttp_response response = {0};
  chttp_error error = {0};
  int status = chttp_application_start(&fixture.app);
  if (status != SALTS_OK) return status;
  status = chttp_application_port(&fixture.app, &port);
  if (status != SALTS_OK) return status;
  snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port);
  const bool upload = strcmp(target, "/upload") == 0;
  const chttp_options options = {.connection_uri = uri, .authority = "application.test",
      .target = target, .protocol = protocol, .timeout_ms = 5000u,
      .body = upload ? "abc" : NULL, .body_size = upload ? 3u : 0u};
  status = upload ? chttp_post(&fixture.client, &options, &response, &error) :
      chttp_get(&fixture.client, &options, &response, &error);
  *http_status = response.status_code;
  fixture.response_size = response.body_size;
  if (status == SALTS_OK && fixture.config.security != NULL) {
    const char *value = chttp_response_header(&response, "X-Content-Type-Options");
    if (value == NULL || strcmp(value, "nosniff") != 0) status = SALTS_EPROTO;
  }
  if (status == SALTS_OK && response.status_code == 200u &&
      (response.body_size != 2u || memcmp(response.body, "42", 2u) != 0)) status = SALTS_EPROTO;
  chttp_response_destroy(&response);
  const int closed = chttp_application_close(&fixture.app, 5000u);
  return closed != SALTS_OK ? closed : status;
}

static void setup_assets(bool reject) {
  fixture.asset_root = tt_make_temp_dir("chttp-application-assets-");
  check_not_null(fixture.asset_root);
  char path[1024];
  const int size = snprintf(path, sizeof(path), "%s/answer.txt", fixture.asset_root);
  check_true(size > 0 && (size_t)size < sizeof(path));
  check_equal(tt_write_file(path, "42", 2u), 0);
  fixture.assets = (chttp_web_asset_mount)CHTTP_WEB_ASSET_MOUNT_INIT;
  fixture.assets.url_prefix = "/assets";
  fixture.assets.filesystem_root = fixture.asset_root;
  fixture.assets.max_relative_path_bytes = 256u;
  fixture.assets.spa_url_prefix = "/app";
  fixture.assets.spa_fallback_relative_path = "answer.txt";
  fixture.security = (chttp_web_security_policy)CHTTP_WEB_SECURITY_POLICY_INIT;
  fixture.security.nosniff = true;
  fixture.config.assets = &fixture.assets; fixture.config.security = &fixture.security;
  fixture.reject_assets = reject;
}

spec("Reflected HTTP Application") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    fixture.config = chttp_application_config_default();
    const cmeta_component_desc *components[] = {cmeta_component_meta(TestServiceComponent),
        cmeta_component_meta(TestControllerComponent), cmeta_component_meta(TestHookA), cmeta_component_meta(TestHookB)};
    for (size_t i = 0u; i < 4u; ++i) {
      fixture.states[i].kind = (int)i;
      fixture.states[i].lifecycle = (cmeta_object_lifecycle){sizeof(cmeta_object_lifecycle),
          &fixture.states[i], NULL, NULL, bean_destroy};
      fixture.providers[i] = (salts_configurator_provider_binding){sizeof(salts_configurator_provider_binding),
          SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, components[i], &fixture.states[i],
          i == 0u ? &counter_interfaces : NULL, bean_create, NULL, NULL};
      fixture.deployments[i] = (salts_configurator_deployment){&fixture.providers[i], NULL, NULL};
    }
    fixture.actions[0] = (chttp_action)CHTTP_GET("/hello", hello);
    fixture.actions[1] = (chttp_action)CHTTP_GET("/reject", hello);
    fixture.actions[2] = (chttp_action)CHTTP_GET("/failure", failure);
    fixture.actions[3] = (chttp_action)CHTTP_POST("/upload", hello);
    fixture.actions[3].body_open = body_open; fixture.actions[3].body_close = body_close;
    fixture.controller = (chttp_controller){.component_id = "TestControllerComponent",
        .receiver = cmeta_reflected_data(TestController), .actions = fixture.actions, .action_count = 4u};
    fixture.hooks[0] = (chttp_interceptor){"TestHookA", &cmeta_data_int, before, after, on_error};
    fixture.hooks[1] = (chttp_interceptor){"TestHookB", &cmeta_data_int, before, after, on_error};
    fixture.config.components = fixture.deployments; fixture.config.component_count = 4u;
    fixture.config.dependency_capacity = 1u;
    fixture.config.controllers = &fixture.controller; fixture.config.controller_count = 1u;
    fixture.config.interceptors = fixture.hooks; fixture.config.interceptor_count = 2u;
    const chttp_client_config client = {.network = fixture.config.http.network,
        .request_capacity = 2u, .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
        .max_response_body_bytes = 8192u, .max_informational_responses = 4u};
    check_equal(chttp_client_init(&fixture.client, &client), SALTS_OK);
  }
  after_each() {
    check_equal(chttp_websocket_client_destroy(&fixture.websocket, 5000u), SALTS_OK);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(chttp_client_destroy(&fixture.client, 5000u), SALTS_OK);
    if (fixture.asset_root != NULL) {
      check_equal(tt_remove_tree(fixture.asset_root), 0);
      free(fixture.asset_root); fixture.asset_root = NULL;
    }
  }
  it("injects a reflected Service into a Controller and serves HTTP/1 through ordered interceptors") {
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_1_1, "/hello", &status), SALTS_OK);
    check_equal(status, 200u); check_equal(fixture.calls, 1);
    const int expected[] = {1,2,3,4};
    check_equal(fixture.trace_count, (size_t)4u);
    check_equal(fixture.trace, expected, sizeof(expected));
    check_equal(fixture.destroys, 4);
    check_equal(fixture.destruction[3], 0); // Service survives its dependent Controller.
  }
  it("runs the same reflected Controller and Interceptor lifecycle over HTTP/2") {
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_2, "/hello", &status), SALTS_OK);
    check_equal(status, 200u); check_equal(fixture.calls, 1); check_equal(fixture.destroys, 4);
  }
  it("keeps an explicit interceptor rejection and unwinds error hooks without running the action") {
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_2, "/reject", &status), SALTS_OK);
    check_equal(status, 403u); check_equal(fixture.calls, 0);
    const int expected[] = {1,2,-3,-4};
    check_equal(fixture.trace_count, (size_t)4u); check_equal(fixture.trace, expected, sizeof(expected));
  }
  it("unwinds entered interceptors on action failure and preserves the HTTP 500 behavior") {
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_1_1, "/failure", &status), SALTS_OK);
    check_equal(status, 500u); check_equal(fixture.calls, 1);
    const int expected[] = {1,2,-3,-4};
    check_equal(fixture.trace_count, (size_t)4u); check_equal(fixture.trace, expected, sizeof(expected));
  }
  it("rolls back all Beans when reflected action ABI admission fails before start") {
    cmeta_function_abi_desc bad = *fixture.actions[0].abi;
    bad.return_carrier = CMETA_ABI_OBJECT_POINTER;
    fixture.actions[0].abi = &bad;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_EINVAL);
    check_null(fixture.app.impl); check_equal(fixture.creates, 4); check_equal(fixture.destroys, 4);
  }
  it("fails a missing Service dependency before invoking any factory") {
    fixture.providers[0].component = cmeta_component_meta(TestHookA);
    fixture.config.component_count = 2u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_EINVAL);
    check_null(fixture.app.impl); check_equal(fixture.creates, 0); check_equal(fixture.destroys, 0);
  }
  it("releases a partially returned owned Bean and rolls back prior dependencies exactly once") {
    fixture.states[3].fail = true;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_EINVAL);
    check_null(fixture.app.impl); check_equal(fixture.creates, 4); check_equal(fixture.destroys, 4);
  }
  it("rolls back a missing or mismatched Controller component instead of guessing an object") {
    fixture.controller.component_id = "TestServiceComponent";
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_EINVAL);
    check_null(fixture.app.impl); check_equal(fixture.destroys, 4);
  }
  it("keeps failed route capacity admission transactional") {
    fixture.config.http.route_capacity = 2u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_ENOBUFS);
    check_null(fixture.app.impl); check_equal(fixture.destroys, 4);
  }
  it("binds streamed body open write and close to the same ACE-owned Controller") {
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_2, "/upload", &status), SALTS_OK);
    check_equal(status, 200u); check_equal(fixture.opened, 1); check_equal(fixture.closed, 1);
    check_equal(fixture.body_bytes, 3); check_equal(fixture.body_status, SALTS_OK);
    check_equal(fixture.destroys, 4);
  }
  it("mounts a WebSocket endpoint on the ACE-owned Controller until handshake and drain complete") {
    const chttp_websocket_endpoint endpoint = {.path = "/socket", .on_open = websocket_open,
        .on_event = websocket_event};
    fixture.controller.websockets = &endpoint; fixture.controller.websocket_count = 1u;
    fixture.config.interceptor_count = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(chttp_application_start(&fixture.app), SALTS_OK);
    uint16_t port = 0u;
    check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
    char uri[80]; snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/socket", (unsigned)port);
    const chttp_websocket_client_config cc = {.size = sizeof(cc), .network = fixture.config.http.network,
        .max_handshake_header_bytes = 4096u, .event_capacity = 8u};
    check_equal(chttp_websocket_client_init(&fixture.websocket, &cc), SALTS_OK);
    const chttp_websocket_connect_options options = {.size = sizeof(options), .uri = uri, .timeout_ms = 5000u};
    unsigned int code = 0u;
    check_equal(chttp_websocket_client_connect(&fixture.websocket, &options, &code), SALTS_OK);
    check_equal(code, 101u);
    chttp_websocket_event event = {0};
    check_equal(chttp_websocket_client_receive(&fixture.websocket, 5000u, &event), SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, (size_t)2u); check_equal(event.data, "42", 2u);
    check_equal(chttp_websocket_client_close(&fixture.websocket, 1000u, NULL, 0u, 5000u), SALTS_OK);
    check_equal(chttp_websocket_client_destroy(&fixture.websocket, 5000u), SALTS_OK);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.destroys, 4);
  }
  it("reports a C11 scoped operation failure after completing owner cleanup") {
    int calls = 0;
    chttp_application_outcome outcome = {0};
    check_equal(chttp_application_with(&fixture.app, &fixture.config, scoped_error, &calls, &outcome), SALTS_EPERM);
    check_equal(calls, 1); check_equal(outcome.operation_status, SALTS_EPERM);
    check_equal(outcome.close_status, SALTS_OK); check_null(fixture.app.impl);
    check_equal(fixture.destroys, 4);
  }
  it("rejects a C11 scoped call on an existing owner without closing it") {
    int calls = 0;
    chttp_application_outcome outcome = {0};
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(chttp_application_with(&fixture.app, &fixture.config, scoped_error, &calls, &outcome), SALTS_EALREADY);
    check_equal(calls, 0); check_equal(outcome.operation_status, SALTS_EALREADY);
    check_not_null(fixture.app.impl); check_equal(fixture.destroys, 0);
  }
  it("enforces Interceptor rejection before serving an H1 static asset") {
    setup_assets(true);
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_1_1, "/assets/answer.txt", &status), SALTS_OK);
    check_equal(status, 403u); check_equal(fixture.response_size, (size_t)0u);
    const int expected[] = {1,2,-3,-4};
    check_equal(fixture.trace_count, (size_t)4u); check_equal(fixture.trace, expected, sizeof(expected));
    check_equal(fixture.calls, 0); check_equal(fixture.destroys, 4);
  }
  it("enforces Interceptor rejection before serving an H2 SPA fallback") {
    setup_assets(true);
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_2, "/app/private", &status), SALTS_OK);
    check_equal(status, 403u); check_equal(fixture.response_size, (size_t)0u);
    const int expected[] = {1,2,-3,-4};
    check_equal(fixture.trace_count, (size_t)4u); check_equal(fixture.trace, expected, sizeof(expected));
  }
  it("unwinds successful Interceptors around an allowed H1 asset response") {
    setup_assets(false);
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_1_1, "/assets/answer.txt", &status), SALTS_OK);
    check_equal(status, 200u);
    const int expected[] = {1,2,3,4};
    check_equal(fixture.trace_count, (size_t)4u); check_equal(fixture.trace, expected, sizeof(expected));
    check_equal(fixture.calls, 0);
  }
  it("unwinds successful Interceptors around an allowed H2 SPA fallback") {
    setup_assets(false);
    unsigned int status = 0u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(request(CHTTP_HTTP_2, "/app/private", &status), SALTS_OK);
    check_equal(status, 200u);
    const int expected[] = {1,2,3,4};
    check_equal(fixture.trace_count, (size_t)4u); check_equal(fixture.trace, expected, sizeof(expected));
  }
}
