#include <chttp_app/app.h>
#include <threads/threads.h>
#include <core/clock.h>
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
  chttp_invocation_interceptor hooks[2];
  chttp_web_asset_mount assets;
  chttp_assets asset_binding;
  chttp_upload upload;
  chttp_web_security_policy security;
  chttp_server_cors_options cors;
  chttp_header request_headers[1];
  size_t request_header_count;
  const char *expected_origin;
  char *asset_root;
  bool reject_assets;
  bool browser_auth, htmx;
  char redirect[128];
  size_t response_size;
  int creates, destroys, calls, opened, closed, body_bytes, body_status;
  int worker_method, worker_service, worker_completed, worker_close_status, destroyed_during_work;
  sync_mutex_t worker_mutex;
  sync_cond_t worker_changed;
  int hold_worker_completion, worker_completion_entered, release_worker_completion;
  int hold_worker_method, worker_method_entered;
  int destruction[4], trace[32];
  size_t trace_count;
} fixture;

static int counter_value(void *self) {
  if (chttp_application_is_worker(&fixture.app)) ++fixture.worker_service;
  return *(int *)self;
}
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

CHTTP_ACTION(worker_hello);
int worker_hello(void *self, const chttp_invocation *invocation) {
  if (!cmeta_function_desc_valid(chttp_invocation_operation(invocation))) return SALTS_EPROTO;
  TestController *controller = self;
  fixture.worker_method = chttp_application_is_worker(&fixture.app);
  fixture.worker_close_status = chttp_application_close(&fixture.app, 1u);
  fixture.destroyed_during_work = fixture.destroys;
  sync_mutex_lock(&fixture.worker_mutex);
  fixture.worker_method_entered = 1;
  sync_cond_broadcast(&fixture.worker_changed);
  while (fixture.hold_worker_method && !fixture.release_worker_completion)
    sync_cond_wait(&fixture.worker_changed, &fixture.worker_mutex);
  sync_mutex_unlock(&fixture.worker_mutex);
  char value[32];
  const int size = snprintf(value, sizeof(value), "%d", TestCounter_value(&controller->counter));
  return size < 0 || (size_t)size >= sizeof(value) ? SALTS_ERANGE :
      chttp_invocation_respond(invocation, 200u, "text/plain", value, (size_t)size);
}
static void worker_completed(void *self, const chttp_server_request_view *request,
    const chttp_response_outcome *outcome) {
  (void)self; (void)request;
  if (chttp_application_is_worker(&fixture.app) && fixture.destroys == 0 &&
      outcome->business_status == SALTS_OK && outcome->transport_status == SALTS_OK)
    ++fixture.worker_completed;
  sync_mutex_lock(&fixture.worker_mutex);
  fixture.worker_completion_entered = 1;
  sync_cond_broadcast(&fixture.worker_changed);
  while (fixture.hold_worker_completion && !fixture.release_worker_completion)
    sync_cond_wait(&fixture.worker_changed, &fixture.worker_mutex);
  sync_mutex_unlock(&fixture.worker_mutex);
}
CHTTP_ACTION(hello);
int hello(void *self, const chttp_invocation *exchange) {
  TestController *controller = self;
  char value[32];
  ++fixture.calls;
  const int size = snprintf(value, sizeof(value), "%d", TestCounter_value(&controller->counter));
  return size < 0 || (size_t)size >= sizeof(value) ? SALTS_ERANGE :
      chttp_invocation_respond(exchange, 200u, "text/plain", value, (size_t)size);
}
CHTTP_ACTION(failure);
int failure(void *self, const chttp_invocation *exchange) {
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
CHTTP_UPLOAD_OPEN_ACTION(body_open);
int body_open(void *self, const chttp_server_request_view *request, chttp_body_sink *sink) {
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
static void body_cleanup(void *self, chttp_body_sink *sink, const chttp_response_outcome *outcome) {
  (void)self; (void)sink; (void)outcome;
}
CHTTP_ACTION(websocket_open);
int websocket_open(void *self, const chttp_invocation *invocation) {
  TestController *controller = self;
  if (controller->initialized != 1 || TestCounter_value(&controller->counter) != 42) return SALTS_EPROTO;
  return chttp_invocation_websocket_accept(invocation);
}
CHTTP_WEBSOCKET_EVENT_ACTION(websocket_event);
CHTTP_WEBSOCKET_EVENT_ACTION(websocket_ready);
int websocket_ready(void *self, const chttp_websocket_event_view *scope) {
  TestController *controller = self;
  if (!chttp_application_is_worker(&fixture.app) || scope->event || scope->request->session ||
      !cmeta_function_desc_equal(scope->request->operation, FunctionAbi(websocket_ready)->function) ||
      TestCounter_value(&controller->counter) != 42) return SALTS_EPROTO;
  return chttp_server_websocket_send_text(scope->peer, "42", 2u);
}
int websocket_event(void *self, const chttp_websocket_event_view *scope) {
  (void)self;
  return scope->event->kind == CHTTP_WEBSOCKET_EVENT_MESSAGE ?
      chttp_server_websocket_send_text(scope->peer, "42", 2u) : SALTS_OK;
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
static cmeta_status before(void *self, const chttp_invocation *exchange, bool *proceed) {
  const int hook = *(int *)self;
  trace(hook == 2 ? 1 : 2);
  if (!chttp_application_is_worker(&fixture.app)) return CMETA_CALLBACK_ERROR;
  if (!cmeta_function_desc_valid(chttp_invocation_operation(exchange))) return CMETA_TYPE_MISMATCH;
  if (hook == 3 && fixture.browser_auth) {
    const chttp_web_auth_policy policy = {sizeof(policy), "/login", true, 256u, NULL, NULL, NULL, NULL};
    return chttp_web_authorize(exchange, &policy, proceed) == SALTS_OK ? CMETA_OK : CMETA_CALLBACK_ERROR;
  }
  if (hook == 3 && (fixture.reject_assets || strcmp(chttp_invocation_request(exchange)->path, "/reject") == 0)) {
    if (chttp_invocation_reject(exchange, proceed, 403u, NULL, NULL, 0u) != SALTS_OK) return CMETA_CALLBACK_ERROR;
    *proceed = false;
  }
  return CMETA_OK;
}
static void after(void *self, const chttp_invocation *exchange, const chttp_invocation_result *result) {
  (void)exchange; (void)result; trace(*(int *)self == 3 ? 3 : 4);
}
static void on_error(void *self, const chttp_invocation *exchange, cmeta_status status) {
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
      .headers = fixture.request_headers, .header_count = fixture.request_header_count,
      .body = upload ? "abc" : NULL, .body_size = upload ? 3u : 0u};
  status = upload ? chttp_post(&fixture.client, &options, &response, &error) :
      chttp_get(&fixture.client, &options, &response, &error);
  if (status != SALTS_OK)
    fprintf(stderr, "Application request failed: protocol=%d target=%s status=%d stage=%s native=%d http=%u bytes=%zu\n",
        (int)protocol, target, status, error.stage ? error.stage : "unset",
        error.native_status, response.status_code, response.body_size);
  *http_status = response.status_code;
  fixture.response_size = response.body_size;
  if (fixture.browser_auth) {
    const char *redirect = chttp_response_header(&response, fixture.htmx ? "HX-Redirect" : "Location");
    snprintf(fixture.redirect, sizeof(fixture.redirect), "%s", redirect ? redirect : "");
  }
  if (status == SALTS_OK && fixture.config.security != NULL) {
    const char *value = chttp_response_header(&response, "X-Content-Type-Options");
    if (value == NULL || strcmp(value, "nosniff") != 0) status = SALTS_EPROTO;
  }
  if (status == SALTS_OK && fixture.expected_origin != NULL) {
    const char *origin = chttp_response_header(&response, "Access-Control-Allow-Origin");
    const char *credentials = chttp_response_header(&response, "Access-Control-Allow-Credentials");
    if (origin == NULL || strcmp(origin, fixture.expected_origin) != 0 ||
        credentials == NULL || strcmp(credentials, "true") != 0) status = SALTS_EPROTO;
  }
  if (status == SALTS_OK && response.status_code == 200u && !fixture.browser_auth &&
      (response.body_size != 2u || memcmp(response.body, "42", 2u) != 0)) status = SALTS_EPROTO;
  chttp_response_destroy(&response);
  chttp_server_stats before_close = {0};
  const int stats_status = chttp_application_stats(&fixture.app, &before_close);
  const int closed = chttp_application_close(&fixture.app, 5000u);
  if (closed != SALTS_OK) {
    fprintf(stderr, "Application close failed: protocol=%d target=%s close=%d request=%d stats=%d active=%zu requests=%llu responses=%llu protocol_errors=%llu handler_errors=%llu terminal=%d\n",
        (int)protocol, target, closed, status, stats_status, before_close.active_connections,
        (unsigned long long)before_close.requests, (unsigned long long)before_close.responses,
        (unsigned long long)before_close.protocol_errors, (unsigned long long)before_close.handler_errors,
        before_close.terminal_status);
    chttp_server_stats after_close = {0};
    const int after_status = chttp_application_stats(&fixture.app, &after_close);
    fprintf(stderr, "Application after close: owned=%d stats=%d running=%d stopping=%d active=%zu terminal=%d\n",
        fixture.app.impl != NULL, after_status, after_close.running, after_close.stopping,
        after_close.active_connections, after_close.terminal_status);
  }
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
  fixture.asset_binding = (chttp_assets){.mount = &fixture.assets};
  fixture.config.assets = &fixture.asset_binding; fixture.config.security = &fixture.security;
  fixture.reject_assets = reject;
}

static void setup_worker_controller(void) {
  fixture.config.interceptors = NULL; fixture.config.interceptor_count = 0u;
  fixture.config.workers = (chttp_worker_config){1u, 2u, 8192u, 256u};
  fixture.actions[0] = (chttp_action)CHTTP_GET("/hello", worker_hello);
  fixture.actions[0].after_completion = worker_completed;
  fixture.controller.action_count = 1u;
}

spec("Reflected HTTP Application") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    sync_mutex_init(&fixture.worker_mutex); sync_cond_init(&fixture.worker_changed);
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
    fixture.upload = (chttp_upload){FunctionAbi(body_open), body_open, body_close, body_cleanup};
    fixture.actions[3].upload = &fixture.upload;
    fixture.controller = (chttp_controller){.component_id = "TestControllerComponent",
        .receiver = cmeta_reflected_data(TestController), .actions = fixture.actions, .action_count = 4u};
    fixture.hooks[0] = (chttp_invocation_interceptor){"TestHookA", &cmeta_data_int, before, after, on_error};
    fixture.hooks[1] = (chttp_invocation_interceptor){"TestHookB", &cmeta_data_int, before, after, on_error};
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
    sync_mutex_lock(&fixture.worker_mutex); fixture.release_worker_completion = 1;
    sync_cond_broadcast(&fixture.worker_changed); sync_mutex_unlock(&fixture.worker_mutex);
    check_equal(chttp_websocket_client_destroy(&fixture.websocket, 5000u), SALTS_OK);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(chttp_client_destroy(&fixture.client, 5000u), SALTS_OK);
    sync_cond_destroy(&fixture.worker_changed); sync_mutex_destroy(&fixture.worker_mutex);
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
    const chttp_websocket_endpoint endpoint = {.opening = CHTTP_GET("/socket", websocket_open),
        .event_abi = FunctionAbi(websocket_event), .event = websocket_event, .event_capacity = 2u,
        .max_frame_bytes = 4096u, .max_message_bytes = 4096u, .max_buffered_input_bytes = 8192u,
        .ready_abi = FunctionAbi(websocket_ready), .ready = websocket_ready};
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
  it("snapshots Security policy values before the caller reuses its configuration") {
    fixture.security = (chttp_web_security_policy)CHTTP_WEB_SECURITY_POLICY_INIT;
    fixture.security.nosniff = true;
    fixture.config.security = &fixture.security;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    fixture.security.nosniff = false;
    unsigned int status = 0u;
    check_equal(request(CHTTP_HTTP_1_1, "/hello", &status), SALTS_OK);
    check_equal(status, 200u); check_equal(fixture.destroys, 4);
  }
  it("snapshots an H2 asset mount without retaining the caller's mount structure") {
    setup_assets(false);
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    fixture.assets.url_prefix = "/changed";
    unsigned int status = 0u;
    check_equal(request(CHTTP_HTTP_2, "/assets/answer.txt", &status), SALTS_OK);
    check_equal(status, 200u); check_equal(fixture.destroys, 4);
  }
  it("snapshots CORS values while borrowing the original immutable origin array") {
    static const char *const origins[] = {"https://application.test"};
    static const char *const replacement[] = {"https://changed.test"};
    fixture.cors = (chttp_server_cors_options){.origins = origins, .origin_count = 1u,
        .methods = "GET", .allow_credentials = 1};
    fixture.config.cors = &fixture.cors;
    fixture.expected_origin = origins[0];
    fixture.request_headers[0] = (chttp_header){"Origin", origins[0]};
    fixture.request_header_count = 1u;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    fixture.cors.origins = replacement; fixture.cors.allow_credentials = 0;
    unsigned int status = 0u;
    check_equal(request(CHTTP_HTTP_2, "/hello", &status), SALTS_OK);
    check_equal(status, 200u); check_equal(fixture.destroys, 4);
  }
  it("rejects an undersized Security policy before creating any Bean") {
    fixture.security = (chttp_web_security_policy)CHTTP_WEB_SECURITY_POLICY_INIT;
    fixture.security.size = sizeof(fixture.security) - 1u;
    fixture.config.security = &fixture.security;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_EINVAL);
    check_null(fixture.app.impl); check_equal(fixture.creates, 0); check_equal(fixture.destroys, 0);
  }
  it("rejects an undersized asset policy before reading or activating the graph") {
    fixture.assets = (chttp_web_asset_mount)CHTTP_WEB_ASSET_MOUNT_INIT;
    fixture.assets.size = sizeof(fixture.assets) - 1u;
    fixture.asset_binding = (chttp_assets){.mount = &fixture.assets};
    fixture.config.assets = &fixture.asset_binding;
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_EINVAL);
    check_null(fixture.app.impl); check_equal(fixture.creates, 0); check_equal(fixture.destroys, 0);
  }
  group("public worker Controllers") {
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      for (int htmx = 0; htmx <= 1; ++htmx) {
        it("stages browser authentication denial from a worker before hook (protocol=%d HTMX=%d)", protocol, htmx) {
          fixture.browser_auth = true; fixture.htmx = htmx != 0;
          fixture.request_headers[0] = (chttp_header){"HX-Request", htmx ? "true" : "false"};
          fixture.request_header_count = 1u;
          check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
          unsigned int code = 0u;
          check_equal(request((chttp_protocol)protocol, "/hello", &code), SALTS_OK);
          check_equal(code, htmx ? 200u : 303u); check_equal(fixture.calls, 0);
          check_equal(fixture.redirect, "/login?return_to=%2Fhello");
          const int expected[] = {1, 2, -3, -4};
          check_equal(fixture.trace_count, 4u); check_equal(fixture.trace, expected, sizeof(expected));
        }
      }
    }
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      it("keeps the injected Service and Controller alive through protocol %d completion", protocol) {
        setup_worker_controller();
        check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
        unsigned int status = 0u;
        check_equal(request((chttp_protocol)protocol, "/hello", &status), SALTS_OK);
        check_equal(status, 200u); check_equal(fixture.worker_method, 1);
        check_equal(fixture.worker_service, 1); check_equal(fixture.worker_completed, 1);
        check_equal(fixture.worker_close_status, SALTS_EBUSY);
        check_equal(fixture.destroyed_during_work, 0); check_equal(fixture.destroys, 4);
      }
    }
    it("rejects an Application without worker capacity") {
      fixture.config.workers.workers = 0u;
      check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_EINVAL);
      check_null(fixture.app.impl);
    }
  }

  it("keeps Server storage alive while a timed-out worker still owns its deferred handle") {
    fixture.config.deadlines.handler_ms = 100u;
    fixture.hold_worker_method = 1;
    setup_worker_controller();
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(chttp_application_start(&fixture.app), SALTS_OK);
    uint16_t port = 0u; check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
    char uri[64]; snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port);
    const chttp_options options = {.connection_uri = uri, .authority = "worker.test",
        .target = "/hello", .protocol = CHTTP_HTTP_2, .timeout_ms = 5000u};
    chttp_response response = {0}; chttp_error error = {0};
    const int status = chttp_get(&fixture.client, &options, &response, &error);
    chttp_response_destroy(&response);
    check_not_equal(status, SALTS_OK); check_not_equal(status, SALTS_ETIMEDOUT);
    sync_mutex_lock(&fixture.worker_mutex);
    const int entered = fixture.worker_method_entered;
    sync_mutex_unlock(&fixture.worker_mutex);
    check_equal(entered, 1);
    check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
    check_not_equal(fixture.app.impl, NULL); check_equal(fixture.destroys, 0);
    sync_mutex_lock(&fixture.worker_mutex); fixture.release_worker_completion = 1;
    sync_cond_broadcast(&fixture.worker_changed); sync_mutex_unlock(&fixture.worker_mutex);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_null(fixture.app.impl); check_equal(fixture.destroys, 4);
    check_equal(fixture.worker_service, 1);
  }
  it("retains the ACE graph when worker notification makes close exceed its budget") {
    fixture.hold_worker_completion = 1;
    setup_worker_controller();
    check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
    check_equal(chttp_application_start(&fixture.app), SALTS_OK);
    uint16_t port = 0u; check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
    char uri[64]; snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port);
    const chttp_options options = {.connection_uri = uri, .authority = "worker.test",
        .target = "/hello", .protocol = CHTTP_HTTP_2, .timeout_ms = 5000u};
    chttp_response response = {0}; chttp_error error = {0};
    const int status = chttp_get(&fixture.client, &options, &response, &error);
    const unsigned int code = response.status_code;
    chttp_response_destroy(&response);
    check_equal(status, SALTS_OK); check_equal(code, 200u);
    sync_mutex_lock(&fixture.worker_mutex);
    const uint64_t started = monotonic_ms();
    while (!fixture.worker_completion_entered && monotonic_ms() - started < 5000u)
      sync_cond_timedwait(&fixture.worker_changed, &fixture.worker_mutex, UINT64_C(100000000));
    const int entered = fixture.worker_completion_entered;
    sync_mutex_unlock(&fixture.worker_mutex);
    check_equal(entered, 1);
    check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
    check_not_equal(fixture.app.impl, NULL); check_equal(fixture.destroys, 0);
    sync_mutex_lock(&fixture.worker_mutex); fixture.release_worker_completion = 1;
    sync_cond_broadcast(&fixture.worker_changed); sync_mutex_unlock(&fixture.worker_mutex);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_null(fixture.app.impl); check_equal(fixture.destroys, 4);
  }

}
