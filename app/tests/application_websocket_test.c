#include "chttp_application_registration.h"
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include <threads/threads.h>
#include <core/clock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinytest.h"
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ws_socket;
#define WS_INVALID_SOCKET INVALID_SOCKET
#define ws_socket_close closesocket
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int ws_socket;
#define WS_INVALID_SOCKET (-1)
#define ws_socket_close close
#endif

typedef struct WsController { int marker; } WsController;
cmeta_reflect_data(WsController, "test.WsController", cmeta_field(int, marker));
cmeta_component_empty(WsComponent);
CHTTP_ACTION(ws_open);
CHTTP_WEBSOCKET_EVENT_ACTION(ws_event);
CHTTP_WEBSOCKET_EVENT_ACTION(ws_ready);
CHTTP_ADMISSION_ACTION(ws_admit);
CHTTP_ACTION(ws_session);

static struct {
  chttp_application app;
  chttp_application_config config;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  chttp_application_websocket_binding binding;
  chttp_invocation_interceptor hook;
  chttp_application_work_config limits;
  chttp_websocket_client clients[2];
  chttp_websocket_pool pool;
  chttp_jwt_bearer_validator jwt;
  char *token;
  ws_socket socket;
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t thread;
  bool thread_live;
  chttp_protocol connecting_protocol;
  int connecting_status;
  int connecting_destroy_status, connecting_done;
  unsigned int connecting_code;
  bool hold, hold_open, hold_close, release, entered, deny, fail_open, fail_event, gate;
  bool hold_ready, ready_done;
  int opens, afters, events, messages, closes, completions, destroyed, active[2], admissions;
  uint16_t close_code;
  size_t bytes;
  unsigned char received[256];
  chttp_server_websocket_session peer;
  chttp_application_work_outcome outcome;
  _Atomic int invalid;
  char uri[96];
  char cookie[128];
} ws;
static void ws_destroy(void *user, void *object) { (void)user; ++ws.destroyed; free(object); }
static const cmeta_object_lifecycle ws_lifecycle = {sizeof(ws_lifecycle), NULL, NULL, NULL, ws_destroy};
static cmeta_status ws_create(void *user, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)user; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  WsController *object = malloc(sizeof(*object));
  if (!object) return CMETA_OUT_OF_MEMORY;
  object->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, object, cmeta_reflected_data(WsController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &ws_lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(object); }
  return status;
}
static void ws_worker(void *self) {
  if (!chttp_application_is_worker(&ws.app) || !self || ((WsController *)self)->marker != 42)
    atomic_fetch_add(&ws.invalid, 1);
}
int ws_admit(void *self, const chttp_server_request_view *request, chttp_server_admission_result *result) {
  ws_worker(self);
  if (request->session || request->body_size) atomic_fetch_add(&ws.invalid, 1);
  sync_mutex_lock(&ws.mutex); ++ws.admissions; sync_mutex_unlock(&ws.mutex);
  result->status_code = ws.deny ? 429u : 0u;
  return SALTS_OK;
}
int ws_open(void *self, const chttp_invocation *invocation) {
  ws_worker(self);
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  if (!request || request->operation != FunctionAbi(ws_open)->function || request->session != chttp_invocation_session(invocation) ||
      !chttp_server_request_param(request, "id")) return SALTS_EPROTO;
  if (ws.jwt.impl && (!request->jwt_claims || !request->jwt_claims->subject ||
      strcmp(request->jwt_claims->subject, "alice") != 0)) return SALTS_EPROTO;
  sync_mutex_lock(&ws.mutex);
  ++ws.opens;
  if (ws.hold_open) {
    ws.entered = true; sync_cond_broadcast(&ws.changed);
    while (!ws.release) sync_cond_wait(&ws.changed, &ws.mutex);
  }
  sync_mutex_unlock(&ws.mutex);
  if (ws.fail_open) return SALTS_EIO;
  if (ws.deny) return chttp_invocation_respond(invocation, 403u, "text/plain", "denied", 6u);
  int status = chttp_invocation_set_header(invocation, "X-Worker", "accepted");
  chttp_session *session = chttp_invocation_session(invocation);
  if (status == SALTS_OK && session) status = chttp_session_set(session, "ws", "accepted");
  return status == SALTS_OK ? chttp_invocation_websocket_accept(invocation) : status;
}
int ws_session(void *self, const chttp_invocation *invocation) {
  ws_worker(self);
  const char *value = chttp_session_get(chttp_invocation_session(invocation), "ws");
  return chttp_invocation_respond(invocation, value ? 200u : 404u, "text/plain", value, value ? strlen(value) : 0u);
}
static void ws_after(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  (void)invocation; (void)result; ws_worker(self);
  sync_mutex_lock(&ws.mutex); ++ws.afters; sync_mutex_unlock(&ws.mutex);
}
int ws_ready(void *self, const chttp_application_websocket_event *scope) {
  ws_worker(self);
  if (scope->event || scope->request->session || scope->request->operation != FunctionAbi(ws_ready)->function)
    atomic_fetch_add(&ws.invalid, 1);
  sync_mutex_lock(&ws.mutex);
  if (ws.afters != 1 || ws.events || ws.ready_done) atomic_fetch_add(&ws.invalid, 1);
  ws.entered = true; sync_cond_broadcast(&ws.changed);
  while (ws.hold_ready && !ws.release) sync_cond_wait(&ws.changed, &ws.mutex);
  ws.ready_done = true;
  sync_mutex_unlock(&ws.mutex);
  return ws.hold_ready ? SALTS_OK : chttp_server_websocket_send_text(scope->peer, "ready", 5u);
}
int ws_event(void *self, const chttp_application_websocket_event *scope) {
  ws_worker(self);
  const chttp_websocket_event *event = scope->event;
  const char *id = chttp_server_request_param(scope->request, "id");
  if (scope->request->session || scope->request->operation != FunctionAbi(ws_event)->function || !id)
    atomic_fetch_add(&ws.invalid, 1);
  if (ws.jwt.impl && (!scope->request->jwt_claims || !scope->request->jwt_claims->subject ||
      strcmp(scope->request->jwt_claims->subject, "alice") != 0)) atomic_fetch_add(&ws.invalid, 1);
  sync_mutex_lock(&ws.mutex);
  if (ws.binding.ready && !ws.ready_done) atomic_fetch_add(&ws.invalid, 1);
  const size_t slot = id && strcmp(id, "43") == 0 ? 1u : 0u;
  if (ws.active[slot]++) atomic_fetch_add(&ws.invalid, 1);
  if (ws.afters < ws.opens) atomic_fetch_add(&ws.invalid, 1);
  ws.peer = *scope->peer; ++ws.events;
  if (event->kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
    ++ws.messages;
    if (ws.hold && strcmp(id, "42") == 0) {
      ws.entered = true; sync_cond_broadcast(&ws.changed);
      while (!ws.release) sync_cond_wait(&ws.changed, &ws.mutex);
    }
    if (event->size < sizeof(ws.received)) memcpy(ws.received, event->data, event->size);
    ws.bytes += event->size;
  } else if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
    ++ws.closes; ws.close_code = event->close_code;
    if (ws.hold_close) {
      ws.entered = true; sync_cond_broadcast(&ws.changed);
      while (!ws.release) sync_cond_wait(&ws.changed, &ws.mutex);
    }
  }
  --ws.active[slot]; sync_cond_broadcast(&ws.changed);
  sync_mutex_unlock(&ws.mutex);
  if (event->kind != CHTTP_WEBSOCKET_EVENT_MESSAGE) return SALTS_OK;
  if (ws.fail_event && slot == 0u) return SALTS_EIO;
  return event->message_type == CHTTP_WEBSOCKET_MESSAGE_TEXT ?
      chttp_server_websocket_send_text(scope->peer, event->data, event->size) :
      chttp_server_websocket_send_binary(scope->peer, event->data, event->size);
}
static void ws_completed(void *self, const chttp_server_request_view *request, const chttp_application_work_outcome *outcome) {
  ws_worker(self);
  sync_mutex_lock(&ws.mutex);
  const char *id = chttp_server_request_param(request, "id");
  if (ws.active[id && strcmp(id, "43") == 0 ? 1u : 0u]) atomic_fetch_add(&ws.invalid, 1);
  ++ws.completions; ws.outcome = *outcome; sync_cond_broadcast(&ws.changed);
  sync_mutex_unlock(&ws.mutex);
}
static chttp_websocket_client_config ws_client_config(void) {
  return (chttp_websocket_client_config){.size = sizeof(chttp_websocket_client_config),
      .network = ws.config.http.network, .max_frame_bytes = ws.binding.max_frame_bytes, .max_message_bytes = ws.binding.max_message_bytes,
      .max_buffered_input_bytes = ws.binding.max_buffered_input_bytes, .max_handshake_header_bytes = 4096u, .event_capacity = 16u};
}
static void ws_start(void) {
  ws.config.workers = ws.limits;
  check_equal(chttp_application_init(&ws.app, &ws.config), SALTS_OK);

  if (ws.gate) {
    const chttp_application_admission_binding gate = {"WsComponent", cmeta_reflected_data(WsController), FunctionAbi(ws_admit), ws_admit};
    check_equal(chttp_application_worker_admission(&ws.app, &gate), SALTS_OK);
  }
  check_equal(chttp_application_worker_websocket(&ws.app, &ws.binding), SALTS_OK);
  chttp_application_worker_binding session = ws.binding.controller;
  session.path = "/session"; session.abi = FunctionAbi(ws_session); session.invoke = ws_session;
  session.after_completion = NULL; session.hooks = NULL; session.hook_count = 0u;
  check_equal(chttp_application_worker_mount(&ws.app, &session), SALTS_OK);
  check_equal(chttp_application_start(&ws.app), SALTS_OK);
  uint16_t port = 0u; check_equal(chttp_application_port(&ws.app, &port), SALTS_OK);
  snprintf(ws.uri, sizeof(ws.uri), "ws://127.0.0.1:%u/ws/42", (unsigned)port);
}
static int ws_connect(int index, chttp_protocol protocol, unsigned int *code) {
  const chttp_websocket_client_config config = ws_client_config();
  int status = chttp_websocket_client_init(&ws.clients[index], &config);
  if (status != SALTS_OK) return status;
  char authorization[1024];
  if (ws.token) snprintf(authorization, sizeof(authorization), "Bearer %s", ws.token);
  const chttp_header header = {"Authorization", authorization};
  const chttp_websocket_connect_options options = {.size = sizeof(options), .uri = ws.uri,
      .timeout_ms = 5000u, .protocol = protocol, .headers = ws.token ? &header : NULL, .header_count = ws.token ? 1u : 0u};
  return chttp_websocket_client_connect(&ws.clients[index], &options, code);
}
static void ws_wait(int *value, int target) {
  const uint64_t deadline = monotonic_ms() + 5000u;
  sync_mutex_lock(&ws.mutex);
  while (*value < target && monotonic_ms() < deadline) sync_cond_timedwait(&ws.changed, &ws.mutex, UINT64_C(10000000));
  const int actual = *value; sync_mutex_unlock(&ws.mutex); check_equal(actual, target);
}
static void ws_connect_background(void *user) {
  (void)user; ws.connecting_status = ws_connect(0, ws.connecting_protocol, &ws.connecting_code);
  /* This thread owns this client, including terminal drain/destruction. */
  ws.connecting_destroy_status = chttp_websocket_client_destroy(&ws.clients[0], 5000u);
  sync_mutex_lock(&ws.mutex); ws.connecting_done = 1; sync_cond_broadcast(&ws.changed); sync_mutex_unlock(&ws.mutex);
}
static void ws_wait_entered(void) {
  const uint64_t deadline = monotonic_ms() + 5000u;
  sync_mutex_lock(&ws.mutex);
  while (!ws.entered && monotonic_ms() < deadline) sync_cond_timedwait(&ws.changed, &ws.mutex, UINT64_C(10000000));
  const bool entered = ws.entered; sync_mutex_unlock(&ws.mutex); check_true(entered);
}
static void ws_release(void) {
  sync_mutex_lock(&ws.mutex); ws.release = true; sync_cond_broadcast(&ws.changed); sync_mutex_unlock(&ws.mutex);
}
static void ws_wait_released(void) {
  const uint64_t deadline = monotonic_ms() + 5000u;
  chttp_application_work_stats stats = {0};
  do {
    check_equal(chttp_application_worker_stats(&ws.app, &stats), SALTS_OK);
    if (!stats.pending) break;
    sync_mutex_lock(&ws.mutex); sync_cond_timedwait(&ws.changed, &ws.mutex, UINT64_C(1000000)); sync_mutex_unlock(&ws.mutex);
  } while (monotonic_ms() < deadline);
  check_equal(stats.pending, 0u); check_equal(stats.retained_bytes, 0u);
}
static void ws_settled(int count) {
  ws_wait(&ws.completions, count);
  check_equal(chttp_application_close(&ws.app, 5000u), SALTS_OK);
  check_equal(ws.destroyed, 1);
}
static void ws_raw_connect(void) {
  uint16_t port = 0u; check_equal(chttp_application_port(&ws.app, &port), SALTS_OK);
  ws.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); check_not_equal(ws.socket, WS_INVALID_SOCKET);
  struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_port = htons(port);
  unsigned char *ip = (unsigned char *)&address.sin_addr; ip[0] = 127u; ip[3] = 1u;
  check_equal(connect(ws.socket, (const struct sockaddr *)&address, sizeof(address)), 0);
#if defined(_WIN32)
  DWORD timeout = 5000u;
#else
  struct timeval timeout = {5, 0};
#endif
  check_equal(setsockopt(ws.socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)), 0);
}
static void ws_raw_headers(unsigned char *out, size_t *used) {
  static const char request[] = "GET /ws/42 HTTP/1.1\r\nHost: test\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
  memcpy(out, request, sizeof(request) - 1u); *used = sizeof(request) - 1u;
}
static size_t ws_raw_frame(unsigned char *out, const char *text) {
  const size_t size = strlen(text);
  out[0] = 0x81u; out[1] = (unsigned char)(0x80u | size);
  for (size_t i = 0; i < 4u; ++i) out[2u + i] = (unsigned char)(i + 1u);
  for (size_t i = 0; i < size; ++i) out[6u + i] = (unsigned char)text[i] ^ out[2u + i % 4u];
  return size + 6u;
}
static void ws_raw_handshake(void) {
  unsigned char bytes[512]; size_t size = 0u; ws_raw_headers(bytes, &size);
  check_equal(send(ws.socket, (const char *)bytes, (int)size, 0), (int)size);
  size_t used = 0u;
  do {
    const int received = recv(ws.socket, (char *)bytes + used, (int)(sizeof(bytes) - 1u - used), 0);
    check_greater(received, 0); used += (size_t)received; bytes[used] = 0u;
  } while (!strstr((const char *)bytes, "\r\n\r\n"));
  check_not_equal(strstr((const char *)bytes, "101 Switching Protocols"), NULL);
  const char *cookie = strstr((const char *)bytes, "Set-Cookie: ");
  if (cookie) {
    cookie += strlen("Set-Cookie: "); const size_t size = strcspn(cookie, ";\r\n");
    check_less(size, sizeof(ws.cookie)); memcpy(ws.cookie, cookie, size); ws.cookie[size] = '\0';
  }
}
spec("Application worker WebSocket") {
  before_each() {
    memset(&ws, 0, sizeof(ws)); atomic_init(&ws.invalid, 0);
    ws.socket = WS_INVALID_SOCKET;
    sync_mutex_init(&ws.mutex); sync_cond_init(&ws.changed);
    ws.provider = (salts_configurator_provider_binding){sizeof(ws.provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(WsComponent), NULL, NULL, ws_create, NULL, NULL};
    ws.deployment = (salts_configurator_deployment){&ws.provider, NULL, NULL};
    ws.controller = (chttp_controller){.component_id = "WsComponent", .receiver = cmeta_reflected_data(WsController)};
    ws.config = chttp_application_config_default(); ws.config.components = &ws.deployment; ws.config.component_count = 1u;
    ws.config.controllers = &ws.controller; ws.config.controller_count = 1u;
    ws.limits = (chttp_application_work_config){2u, 4u, 131072u, 256u, 4u, 256u};
    ws.hook = (chttp_invocation_interceptor){"WsComponent", cmeta_reflected_data(WsController), NULL, ws_after, NULL};
    ws.binding = (chttp_application_websocket_binding){.controller = {.component_id = "WsComponent", .receiver = cmeta_reflected_data(WsController),
        .method = CHTTP_METHOD_GET, .path = "/ws/:id", .abi = FunctionAbi(ws_open), .invoke = ws_open,
        .after_completion = ws_completed, .hooks = &ws.hook, .hook_count = 1u},
        .event_abi = FunctionAbi(ws_event), .event = ws_event, .event_capacity = 4u,
        .max_frame_bytes = 4096u, .max_message_bytes = 8192u, .max_buffered_input_bytes = 16384u};
  }
  after_each() {
    ws_release();
    if (ws.thread_live) { check_equal(sync_thread_join(&ws.thread), SALTS_OK); ws.thread_live = false; }
    if (ws.socket != WS_INVALID_SOCKET) { ws_socket_close(ws.socket); ws.socket = WS_INVALID_SOCKET; }
    for (size_t i = 0; i < 2u; ++i) if (ws.clients[i].impl) check_equal(chttp_websocket_client_destroy(&ws.clients[i], 5000u), SALTS_OK);
    if (ws.pool.impl) check_equal(chttp_websocket_pool_destroy(&ws.pool, 5000u), SALTS_OK);
    check_equal(chttp_application_close(&ws.app, 5000u), SALTS_OK);
    chttp_jwt_token_destroy(ws.token); check_equal(chttp_jwt_bearer_validator_destroy(&ws.jwt), SALTS_OK);
    check_equal(atomic_load(&ws.invalid), 0);
    sync_cond_destroy(&ws.changed); sync_mutex_destroy(&ws.mutex);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    it("sends the ready greeting before serial events (protocol=%d)", protocol) {
      ws.binding.ready = ws_ready; ws.binding.ready_abi = FunctionAbi(ws_ready);
      ws_start(); unsigned int code = 0u;
      check_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK);
      chttp_websocket_event event = {0};
      check_equal(chttp_websocket_client_receive(&ws.clients[0], 5000u, &event), SALTS_OK);
      check_equal(event.size, 5u); check_equal(event.data, "ready", 5u);
      check_equal(chttp_websocket_client_send_text(&ws.clients[0], "42", 2u, 5000u), SALTS_OK);
      check_equal(chttp_websocket_client_receive(&ws.clients[0], 5000u, &event), SALTS_OK);
      check_equal(event.size, 2u); check_equal(event.data, "42", 2u);
      check_equal(chttp_websocket_client_close(&ws.clients[0], 1000u, NULL, 0u, 5000u), SALTS_OK);
      check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      ws_settled(1); check_true(ws.ready_done); check_equal(ws.closes, 1);
    }
    it("retains a blocked ready callback through finite close (protocol=%d)", protocol) {
      ws.binding.ready = ws_ready; ws.binding.ready_abi = FunctionAbi(ws_ready); ws.hold_ready = true;
      ws_start(); unsigned int code = 0u;
      check_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK); ws_wait_entered();
      check_equal(chttp_application_close(&ws.app, 5u), SALTS_ETIMEDOUT);
      check_not_null(ws.app.impl); check_equal(ws.destroyed, 0);
      ws_release();
      check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      ws_settled(1); check_true(ws.ready_done); check_equal(ws.closes, 1);
    }
  }
  it("commits the opening Session and releases its gate while the WS peer remains open") {
    ws.config.http.session_capacity = 4u; ws.config.http.session_entry_capacity = 4u;
    ws.config.http.max_session_key_bytes = 32u; ws.config.http.max_session_value_bytes = 64u;
    ws.config.http.session_idle_timeout_ms = 60000u; ws.config.http.session_cookie_name = "ws_sid";
    ws_start(); ws_raw_connect(); ws_raw_handshake(); check_not_equal(ws.cookie[0], '\0');
    uint16_t port = 0u; check_equal(chttp_application_port(&ws.app, &port), SALTS_OK);
    char uri[64]; snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port);
    const chttp_client_config config = {ws.config.http.network, 1u, 256u, 16u, 4096u, 8192u, 8192u, 4u};
    chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    const chttp_header cookie = {"Cookie", ws.cookie};
    const chttp_options options = {.connection_uri = uri, .authority = "test", .target = "/session",
        .headers = &cookie, .header_count = 1u, .timeout_ms = 5000u};
    const int status = chttp_get(&client, &options, &response, &error);
    const unsigned int code = response.status_code;
    const bool committed = response.body_size == 8u && memcmp(response.body, "accepted", 8u) == 0;
    chttp_response_destroy(&response); check_equal(chttp_client_destroy(&client, 5000u), SALTS_OK);
    check_equal(status, SALTS_OK); check_equal(code, 200u); check_true(committed);
    ws_socket_close(ws.socket); ws.socket = WS_INVALID_SOCKET; ws_settled(1);
  }
  it("rejects invalid event ABI/capacity transactionally before mounting a valid endpoint") {
    ws.config.workers = ws.limits;
    check_equal(chttp_application_init(&ws.app, &ws.config), SALTS_OK);

    chttp_application_websocket_binding binding = ws.binding;
    binding.event_abi = FunctionAbi(ws_open);
    check_equal(chttp_application_worker_websocket(&ws.app, &binding), SALTS_EINVAL);
    binding = ws.binding; binding.event_capacity = 0u;
    check_equal(chttp_application_worker_websocket(&ws.app, &binding), SALTS_EINVAL);
    binding = ws.binding; binding.ready = ws_ready;
    check_equal(chttp_application_worker_websocket(&ws.app, &binding), SALTS_EINVAL);
    binding.ready_abi = FunctionAbi(ws_open);
    check_equal(chttp_application_worker_websocket(&ws.app, &binding), SALTS_EINVAL);
    binding.ready = NULL; binding.ready_abi = FunctionAbi(ws_ready);
    check_equal(chttp_application_worker_websocket(&ws.app, &binding), SALTS_EINVAL);
    check_equal(chttp_application_worker_websocket(&ws.app, &ws.binding), SALTS_OK);
  }
  for (int dedicated = 0; dedicated < 2; ++dedicated) {
    it("settles native Close while the worker Close method is still blocked (dedicated=%d)", dedicated) {
      const chttp_server_websocket_transport_options transport = {sizeof(transport), CHTTP_SERVER_WEBSOCKET_TRANSPORT_OPTIONS_VERSION, dedicated};
      ws.config.websocket_transport = &transport; ws.hold_close = true; ws_start(); ws_raw_connect(); ws_raw_handshake();
      const unsigned char close_frame[] = {0x88u, 0x82u, 1u, 2u, 3u, 4u, 0x02u, 0xeau};
      check_equal(send(ws.socket, (const char *)close_frame, sizeof(close_frame), 0), (int)sizeof(close_frame));
      ws_wait_entered();
      char bytes[32]; int received;
      do { received = recv(ws.socket, bytes, sizeof(bytes), 0); check_greater_equal(received, 0); } while (received);
      sync_mutex_lock(&ws.mutex); const int completed = ws.completions; sync_mutex_unlock(&ws.mutex); check_equal(completed, 0);
      ws_release(); ws_socket_close(ws.socket); ws.socket = WS_INVALID_SOCKET; ws_settled(1); check_equal(ws.closes, 1);
      check_equal(ws.outcome.transport_status, SALTS_OK);
    }
    it("reports abnormal peer retirement after a CNet protocol failure (dedicated=%d)", dedicated) {
      const chttp_server_websocket_transport_options transport = {sizeof(transport), CHTTP_SERVER_WEBSOCKET_TRANSPORT_OPTIONS_VERSION, dedicated};
      ws.config.websocket_transport = &transport; ws_start(); ws_raw_connect(); ws_raw_handshake();
      const unsigned char unmasked[] = {0x81u, 1u, 'x'};
      check_equal(send(ws.socket, (const char *)unmasked, sizeof(unmasked), 0), (int)sizeof(unmasked));
      ws_wait(&ws.completions, 1);
      ws_socket_close(ws.socket); ws.socket = WS_INVALID_SOCKET;
      ws_settled(1); check_equal(ws.messages, 0); check_equal(ws.closes, 1); check_equal(ws.close_code, 1006u);
      check_not_equal(ws.outcome.transport_status, SALTS_OK);
    }
    it("rejects event overflow and drains admitted data before reserved Close (dedicated=%d)", dedicated) {
      const chttp_server_websocket_transport_options transport = {sizeof(transport), CHTTP_SERVER_WEBSOCKET_TRANSPORT_OPTIONS_VERSION, dedicated};
      ws.config.websocket_transport = &transport;
      ws.binding.event_capacity = 1u; ws.hold = true; ws_start(); ws_raw_connect(); ws_raw_handshake();
      unsigned char batch[64]; size_t size = ws_raw_frame(batch, "retained"); size += ws_raw_frame(batch + size, "overflow");
      check_equal(send(ws.socket, (const char *)batch, (int)size, 0), (int)size);
      ws_wait_entered();
      /* Keep the first slot occupied until native overflow closes the peer.
       * Releasing at callback entry could race the second event's admission. */
      char bytes[32]; int received;
      do { received = recv(ws.socket, bytes, sizeof(bytes), 0); check_greater_equal(received, 0); } while (received);
      ws_release();
      ws_socket_close(ws.socket); ws.socket = WS_INVALID_SOCKET;
      ws_settled(1); check_equal(ws.messages, 1); check_equal(ws.closes, 1);
      check_equal(ws.outcome.business_status, SALTS_ENOBUFS); check_equal(ws.received, "retained", 8u);
    }
    it("processes a coalesced upgrade and early frame after worker header admission (dedicated=%d)", dedicated) {
      const chttp_server_websocket_transport_options transport = {sizeof(transport), CHTTP_SERVER_WEBSOCKET_TRANSPORT_OPTIONS_VERSION, dedicated};
      ws.config.websocket_transport = &transport; ws.gate = true; ws_start(); ws_raw_connect();
      unsigned char batch[512]; size_t size = 0u; ws_raw_headers(batch, &size); size += ws_raw_frame(batch + size, "early");
      check_equal(send(ws.socket, (const char *)batch, (int)size, 0), (int)size);
      ws_wait(&ws.messages, 1); ws_socket_close(ws.socket); ws.socket = WS_INVALID_SOCKET;
      ws_settled(1); check_equal(ws.received, "early", 5u); check_equal(ws.closes, 1);
    }
  }
  for (int failing = 0; failing < 2; ++failing) {
    it("keeps an H2 sibling alive while one peer is blocked or fails (failure=%d)", failing) {
      ws.hold = true; ws.fail_event = failing != 0; ws_start();
      const chttp_websocket_pool_config config = {sizeof(config), ws_client_config(), 2u};
      check_equal(chttp_websocket_pool_init(&ws.pool, &config), SALTS_OK);
      chttp_websocket_connect_options options = {.size = sizeof(options), .uri = ws.uri, .timeout_ms = 5000u, .protocol = CHTTP_HTTP_2};
      chttp_websocket_session first = {0}, second = {0}; unsigned int code = 0u;
      check_equal(chttp_websocket_pool_open(&ws.pool, &options, &first, &code), SALTS_OK);
      char other[96]; snprintf(other, sizeof(other), "%.*s43", (int)strlen(ws.uri) - 2, ws.uri); options.uri = other;
      check_equal(chttp_websocket_pool_open(&ws.pool, &options, &second, &code), SALTS_OK);
      check_equal(chttp_websocket_pool_send_text(&ws.pool, first, "blocked", 7u, 5000u), SALTS_OK); ws_wait_entered();
      check_equal(chttp_websocket_pool_send_text(&ws.pool, second, "sibling", 7u, 5000u), SALTS_OK);
      chttp_websocket_event event = {0};
      check_equal(chttp_websocket_pool_receive(&ws.pool, second, 5000u, &event), SALTS_OK);
      check_equal(event.size, 7u); check_equal(event.data, "sibling", 7u);
      chttp_server_stats stats = {0}; check_equal(chttp_application_stats(&ws.app, &stats), SALTS_OK); check_equal(stats.accepted_connections, 1u);
      ws_release();
      if (!failing) check_equal(chttp_websocket_pool_receive(&ws.pool, first, 5000u, &event), SALTS_OK);
      else { const int status = chttp_websocket_pool_receive(&ws.pool, first, 5000u, &event); check_not_equal(status, SALTS_ETIMEDOUT); }
      check_equal(chttp_websocket_pool_send_text(&ws.pool, second, "still-live", 10u, 5000u), SALTS_OK);
      check_equal(chttp_websocket_pool_receive(&ws.pool, second, 5000u, &event), SALTS_OK); check_equal(event.size, 10u);
      check_equal(chttp_websocket_pool_destroy(&ws.pool, 5000u), SALTS_OK); ws_settled(2); check_equal(ws.closes, 2);
    }
  }
  it("assembles an H2 message larger than one DATA window without releasing pending-event credit early") {
    ws.binding.max_frame_bytes = ws.binding.max_message_bytes = 128u * 1024u;
    ws.binding.max_buffered_input_bytes = 256u * 1024u; ws.binding.event_capacity = 2u;
    ws.config.http.network.max_send_bytes = 256u * 1024u; ws.limits.retained_bytes = 1024u * 1024u;
    ws_start(); unsigned int code = 0u; check_equal(ws_connect(0, CHTTP_HTTP_2, &code), SALTS_OK);
    unsigned char *data = malloc(128u * 1024u); check_true(data != NULL); memset(data, 'x', 128u * 1024u);
    const int sent = chttp_websocket_client_send_binary(&ws.clients[0], data, 128u * 1024u, 5000u); free(data); check_equal(sent, SALTS_OK);
    chttp_websocket_event event = {0}; check_equal(chttp_websocket_client_receive(&ws.clients[0], 5000u, &event), SALTS_OK);
    check_equal(event.size, 128u * 1024u); check_equal(event.data[0], 'x'); check_equal(event.data[event.size - 1u], 'x');
    check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK); ws_settled(1); check_equal(ws.bytes, 128u * 1024u);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    it("dispatches peers on two owners through the same pool while one event is blocked (protocol=%d)", protocol) {
      chttp_server_execution_options execution = CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
      execution.owner_count = 2u; ws.config.execution = &execution; ws.hold = true; ws_start();
      unsigned int code = 0u; check_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK);
      check_equal(chttp_websocket_client_send_text(&ws.clients[0], "blocked", 7u, 5000u), SALTS_OK); ws_wait_entered();
      const size_t end = strlen(ws.uri); ws.uri[end - 1u] = '3';
      check_equal(ws_connect(1, (chttp_protocol)protocol, &code), SALTS_OK);
      check_equal(chttp_websocket_client_send_text(&ws.clients[1], "other-owner", 11u, 5000u), SALTS_OK);
      chttp_websocket_event event = {0}; check_equal(chttp_websocket_client_receive(&ws.clients[1], 5000u, &event), SALTS_OK);
      check_equal(event.size, 11u); check_equal(event.data, "other-owner", 11u);
      ws_release(); check_equal(chttp_websocket_client_receive(&ws.clients[0], 5000u, &event), SALTS_OK);
      for (size_t i = 0; i < 2u; ++i) check_equal(chttp_websocket_client_destroy(&ws.clients[i], 5000u), SALTS_OK);
      ws_settled(2); check_equal(ws.closes, 2);
    }
    it("reuses one charged record only after producer/event retirement and rejects stale peer delivery (protocol=%d)", protocol) {
      ws.limits.capacity = 1u; ws.gate = true; ws_start(); chttp_server_websocket_session stale = {0};
      for (int round = 0; round < 16; ++round) {
        unsigned int code = 0u; check_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK);
        /* Command admission is not peer validation/receipt. The owner must
         * discard this stale generation before it can reach the new peer. */
        if (round) check_equal(chttp_server_websocket_send_text(&stale, "stale", 5u), SALTS_OK);
        check_equal(chttp_websocket_client_send_text(&ws.clients[0], "current", 7u, 5000u), SALTS_OK);
        chttp_websocket_event event = {0}; check_equal(chttp_websocket_client_receive(&ws.clients[0], 5000u, &event), SALTS_OK);
        check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE); check_equal(event.size, 7u); check_equal(event.data, "current", 7u);
        sync_mutex_lock(&ws.mutex); stale = ws.peer; sync_mutex_unlock(&ws.mutex);
        check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
        ws_wait(&ws.completions, round + 1); ws_wait_released();
      }
      check_equal(ws.messages, 16); check_equal(ws.closes, 16); check_equal(ws.completions, 16);
    }
    it("retains a running opening across native cancellation and failed finite close (protocol=%d)", protocol) {
      ws.hold_open = true; ws.config.deadlines.handler_ms = 30u; ws_start(); ws.connecting_protocol = (chttp_protocol)protocol;
      check_equal(sync_thread_create(&ws.thread, ws_connect_background, NULL), SALTS_OK); ws.thread_live = true;
      ws_wait_entered(); check_not_equal(chttp_application_close(&ws.app, 20u), SALTS_OK); check_true(ws.app.impl != NULL);
      ws_wait(&ws.connecting_done, 1);
      sync_mutex_lock(&ws.mutex); const int completed = ws.completions, destroyed = ws.destroyed; sync_mutex_unlock(&ws.mutex);
      check_equal(completed, 0); check_equal(destroyed, 0);
      ws_release(); check_equal(sync_thread_join(&ws.thread), SALTS_OK); ws.thread_live = false;
      check_not_equal(ws.connecting_status, SALTS_OK); check_not_equal(ws.connecting_status, SALTS_ETIMEDOUT);
      check_equal(ws.connecting_destroy_status, SALTS_OK);
      ws_settled(1); check_equal(ws.events, 0);
    }
    for (int authenticated = 0; authenticated < 2; ++authenticated) {
      it("keeps JWT admission ahead of worker opening and copies claims into event scope (protocol=%d auth=%d)", protocol, authenticated) {
        static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
        const chttp_jwt_bearer_validator_options options = {.size = sizeof(options), .key = key, .key_size = sizeof(key) - 1u};
        check_equal(chttp_jwt_bearer_validator_init(&ws.jwt, &options), SALTS_OK); ws.config.jwt = &ws.jwt;
        if (authenticated) {
          const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
          check_equal(chttp_jwt_hs256_token_create(&claims, key, sizeof(key) - 1u, &ws.token), SALTS_OK);
        }
        ws_start(); unsigned int code = 0u; const int status = ws_connect(0, (chttp_protocol)protocol, &code);
        if (authenticated) {
          check_equal(status, SALTS_OK);
          check_equal(chttp_websocket_client_send_text(&ws.clients[0], "jwt", 3u, 5000u), SALTS_OK);
          chttp_websocket_event event = {0}; check_equal(chttp_websocket_client_receive(&ws.clients[0], 5000u, &event), SALTS_OK);
        } else { check_not_equal(status, SALTS_OK); check_equal(code, 401u); }
        check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
        if (authenticated) ws_settled(1);
        else { check_equal(chttp_application_close(&ws.app, 5000u), SALTS_OK); check_equal(ws.opens, 0); }
      }
    }
    it("preserves 404 after asynchronous header admission of an unknown upgrade (protocol=%d)", protocol) {
      ws.gate = true; ws_start(); char *path = strstr(ws.uri, "/ws/"); check_true(path != NULL); strcpy(path, "/unknown");
      unsigned int code = 0u; check_not_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK); check_equal(code, 404u);
      check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      check_equal(chttp_application_close(&ws.app, 5000u), SALTS_OK); check_equal(ws.opens, 0);
    }
    it("runs opening, reflected ordered events and completion on workers (protocol=%d)", protocol) {
      ws.gate = true; ws_start(); unsigned int code = 0u;
      check_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK);
      check_equal(code, protocol == CHTTP_HTTP_2 ? 200u : 101u);
      for (int i = 0; i < 12; ++i) {
        char payload[32]; const int bytes = snprintf(payload, sizeof(payload), "message-%d", i);
        check_equal(chttp_websocket_client_send_text(&ws.clients[0], payload, (size_t)bytes, 5000u), SALTS_OK);
        chttp_websocket_event event = {0};
        check_equal(chttp_websocket_client_receive(&ws.clients[0], 5000u, &event), SALTS_OK);
        check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE); check_equal(event.size, (size_t)bytes);
        check_equal(event.data, payload, (size_t)bytes);
      }
      check_equal(chttp_websocket_client_close(&ws.clients[0], 1000u, "done", 4u, 5000u), SALTS_OK);
      check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      ws_settled(1); check_equal(ws.messages, 12); check_equal(ws.closes, 1); check_equal(ws.admissions, 1);
      check_equal(ws.outcome.business_status, SALTS_OK);
      /* H2 END_STREAM receipt can precede the Server's local send terminal;
       * the Client's subsequent TCP close can still win that observer race. */
      if (protocol == CHTTP_HTTP_1_1) check_equal(ws.outcome.transport_status, SALTS_OK);
    }
    it("denies an opening without creating accepted-peer event callbacks (protocol=%d)", protocol) {
      ws.deny = true; ws_start(); unsigned int code = 0u;
      check_not_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK); check_equal(code, 403u);
      check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      ws_settled(1); check_equal(ws.opens, 1); check_equal(ws.events, 0);
    }
    it("preserves header admission before the opening Invocation (protocol=%d)", protocol) {
      ws.deny = true; ws.gate = true; ws_start(); unsigned int code = 0u;
      check_not_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK); check_equal(code, 429u);
      check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      check_equal(chttp_application_close(&ws.app, 5000u), SALTS_OK); check_equal(ws.opens, 0); check_equal(ws.admissions, 1);
    }
    it("fails a peer on an event error and joins its Close before completion (protocol=%d)", protocol) {
      ws.fail_event = true; ws_start(); unsigned int code = 0u;
      check_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK);
      check_equal(chttp_websocket_client_send_text(&ws.clients[0], "fail", 4u, 5000u), SALTS_OK);
      chttp_websocket_event event = {0};
      const int status = chttp_websocket_client_receive(&ws.clients[0], 5000u, &event);
      check_not_equal(status, SALTS_ETIMEDOUT);
      check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      ws_settled(1); check_equal(ws.outcome.business_status, SALTS_EIO); check_equal(ws.messages, 1); check_equal(ws.closes, 1);
    }
    it("retains a blocked event and Bean across a failed finite close (protocol=%d)", protocol) {
      ws.hold = true; ws_start(); unsigned int code = 0u;
      check_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK);
      check_equal(chttp_websocket_client_send_text(&ws.clients[0], "retained", 8u, 5000u), SALTS_OK); ws_wait_entered();
      check_not_equal(chttp_application_close(&ws.app, 20u), SALTS_OK); check_not_equal(ws.app.impl, NULL);
      sync_mutex_lock(&ws.mutex); const int completed = ws.completions, destroyed = ws.destroyed; sync_mutex_unlock(&ws.mutex);
      check_equal(completed, 0); check_equal(destroyed, 0);
      ws_release(); check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      ws_settled(1); check_equal(ws.closes, 1); check_equal(ws.received, "retained", 8u);
    }
    it("rejects byte exhaustion before running opening code (protocol=%d)", protocol) {
      ws.limits.retained_bytes = 2048u; ws_start(); unsigned int code = 0u;
      check_not_equal(ws_connect(0, (chttp_protocol)protocol, &code), SALTS_OK); check_equal(code, 503u);
      check_equal(chttp_websocket_client_destroy(&ws.clients[0], 5000u), SALTS_OK);
      check_equal(chttp_application_close(&ws.app, 5000u), SALTS_OK); check_equal(ws.opens, 0); check_equal(ws.events, 0);
    }
  }
}
