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
typedef SOCKET invocation_socket;
#define INVOCATION_INVALID_SOCKET INVALID_SOCKET
#define invocation_socket_close closesocket
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int invocation_socket;
#define INVOCATION_INVALID_SOCKET (-1)
#define invocation_socket_close close
#endif

typedef struct InvocationController { int marker; } InvocationController;
cmeta_reflect_data(InvocationController, "test.InvocationController", cmeta_field(int, marker));
cmeta_component_empty(InvocationComponent);
CHTTP_ACTION(invocation_echo);
FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, invocation_legacy_contract,
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (const chttp_server_request_view *, request, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &chttp_admission_request_pointer, CMETA_ABI_OBJECT_POINTER));
CHTTP_ADMISSION_ACTION(invocation_admission);
typedef struct invocation_call {
  const char *target;
  chttp_protocol protocol;
  int status;
  unsigned int code;
  size_t bytes;
  char body[16];
  char draft[32], stage[32], after[32], type[64];
  bool after_present;
  bool head;
  bool get;
  const chttp_header *headers;
  size_t header_count;
  int repeat;
  char length[24];
  char retry_after[24];
  bool expect_continue;
  char etag[96], modified[40], range[96], allow[24], accept_ranges[24];
} invocation_call;
static struct {
  chttp_application app;
  chttp_application_config config;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  chttp_application_worker_binding binding;
  chttp_invocation_interceptor hooks[2];
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t thread;
  bool thread_live, release, reject, fail, no_reply, overflow, recover, foreign, cross_scope, wait_cancel;
  const chttp_invocation *first_scope;
  size_t workers;
  size_t header_capacity, header_bytes;
  size_t reply_bytes;
  size_t capacity, retained_bytes;
  bool admission_enabled, admission_hold, admission_close;
  const char *param_value;
  unsigned int admission_code;
  uint32_t admission_retry;
  int admission_status, admissions;
  chttp_jwt_bearer_validator jwt;
  char *token;
  char *file_path;
  bool file_removed, file_then_fail, conditional_file, remove_after_plan;
  const char *file_etag;
  const char *reply_type;
  int (*write_headers)(const chttp_invocation *);
  int (*before_headers)(const chttp_invocation *);
  int (*after_headers)(const chttp_invocation *);
  int header_status[20];
  int entered, calls, completions, destroyed, trace[32];
  size_t trace_count;
  chttp_application_work_outcome outcome;
  _Atomic int invalid;
  char uri[64];
  invocation_call background;
} fixture;
static void destroy(void *context, void *object) { (void)context; ++fixture.destroyed; free(object); }
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, destroy};
static cmeta_status create(void *context, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  InvocationController *object = malloc(sizeof(*object));
  if (!object) return CMETA_OUT_OF_MEMORY;
  object->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, object, cmeta_reflected_data(InvocationController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(object); }
  return status;
}
static void trace(int event) {
  sync_mutex_lock(&fixture.mutex);
  if (fixture.trace_count < sizeof(fixture.trace) / sizeof(fixture.trace[0])) fixture.trace[fixture.trace_count++] = event;
  else atomic_fetch_add(&fixture.invalid, 1);
  sync_mutex_unlock(&fixture.mutex);
}
static void header_scope_check(void *self, const chttp_server_request_view *request) {
  const char *value = chttp_server_request_header(request, "X-Admission");
  if (!self || ((InvocationController *)self)->marker != 42 || !chttp_application_is_worker(&fixture.app) ||
      !request || request->session || request->body || request->body_size || request->body_streamed ||
      !request->path || !request->target || !value || strcmp(value, "original") != 0)
    atomic_fetch_add(&fixture.invalid, 1);
  if (fixture.jwt.impl && (!request->jwt_claims || !request->jwt_claims->subject ||
      strcmp(request->jwt_claims->subject, "alice") != 0 || !request->jwt_claims->expires_at))
    atomic_fetch_add(&fixture.invalid, 1);
  if (fixture.param_value) {
    const char *value = chttp_server_request_param(request, "id");
    if (!value || strcmp(value, fixture.param_value) != 0) atomic_fetch_add(&fixture.invalid, 1);
  }
}
int invocation_admission(void *self, const chttp_server_request_view *request, chttp_server_admission_result *result) {
  header_scope_check(self, request); trace(0);
  chttp_application_work_stats stats = {0};
  if (chttp_application_worker_stats(&fixture.app, &stats) != SALTS_OK || stats.accepted <= stats.completed)
    atomic_fetch_add(&fixture.invalid, 1);
  if (fixture.admission_close && chttp_application_close(&fixture.app, 5u) != SALTS_EBUSY)
    atomic_fetch_add(&fixture.invalid, 1);
  sync_mutex_lock(&fixture.mutex); ++fixture.admissions;
  if (fixture.admission_hold) {
    ++fixture.entered; sync_cond_broadcast(&fixture.changed);
    while (!fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
  }
  sync_mutex_unlock(&fixture.mutex);
  header_scope_check(self, request);
  *result = (chttp_server_admission_result){fixture.admission_code, fixture.admission_retry};
  return fixture.admission_status;
}
static void scope_check(void *self, const chttp_invocation *invocation) {
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  if (!self || ((InvocationController *)self)->marker != 42 || !chttp_application_is_worker(&fixture.app) ||
      !request || request->session || chttp_invocation_session(invocation) ||
      !cmeta_function_desc_equal(chttp_invocation_operation(invocation), FunctionAbi(invocation_echo)->function))
    atomic_fetch_add(&fixture.invalid, 1);
}
static void foreign_scope(void *context) {
  const chttp_invocation *invocation = context;
  bool cancelled = true, proceed = true;
  if (chttp_invocation_request(invocation) || chttp_invocation_session(invocation) || chttp_invocation_operation(invocation) ||
      chttp_invocation_cancelled(invocation, &cancelled) != SALTS_EINVAL || !cancelled ||
      chttp_invocation_respond(invocation, 200u, NULL, "bad", 3u) != SALTS_EINVAL ||
      chttp_invocation_set_header(invocation, "X-Draft", "foreign") != SALTS_EINVAL ||
      chttp_invocation_response_file(invocation, 200u, NULL, "unused") != SALTS_EINVAL ||
      chttp_invocation_serve_file(invocation, NULL) != SALTS_EINVAL ||
      chttp_invocation_reject(invocation, &proceed, 403u, NULL, NULL, 0u) != SALTS_EINVAL || !proceed)
    atomic_fetch_add(&fixture.invalid, 1);
}
int invocation_echo(void *self, const chttp_invocation *invocation) {
  scope_check(self, invocation); trace(3);
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  if (!request) return SALTS_EPROTO;
  bool cancelled = true, proceed = true;
  if (chttp_invocation_cancelled(invocation, &cancelled) != SALTS_OK || cancelled ||
      chttp_invocation_reject(invocation, &proceed, 403u, NULL, NULL, 0u) != SALTS_EINVAL || !proceed)
    atomic_fetch_add(&fixture.invalid, 1);
  if (fixture.write_headers) {
    const int status = fixture.write_headers(invocation);
    if (status != SALTS_OK) return status;
  }
  sync_mutex_lock(&fixture.mutex); ++fixture.calls;
  if (fixture.wait_cancel || (fixture.cross_scope && strcmp(request->path, "/first") == 0)) {
    fixture.first_scope = invocation; ++fixture.entered; sync_cond_broadcast(&fixture.changed);
    while (!fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
    fixture.first_scope = NULL;
  }
  const chttp_invocation *foreign = fixture.cross_scope && strcmp(request->path, "/second") == 0 ? fixture.first_scope : NULL;
  sync_mutex_unlock(&fixture.mutex);
  if (fixture.cross_scope && strcmp(request->path, "/second") == 0) {
    if (!foreign) atomic_fetch_add(&fixture.invalid, 1);
    else foreign_scope((void *)foreign);
  }
  if (fixture.foreign) {
    sync_thread_t thread;
    int status = sync_thread_create(&thread, foreign_scope, (void *)invocation);
    if (status != SALTS_OK) return status;
    status = sync_thread_join(&thread);
    if (status != SALTS_OK) return status;
  }
  if (fixture.wait_cancel) {
    const uint64_t began = monotonic_ms();
    do {
      if (chttp_invocation_cancelled(invocation, &cancelled) != SALTS_OK) return SALTS_EPROTO;
      if (!cancelled) sync_thread_yield();
    } while (!cancelled && monotonic_ms() - began < 5000u);
    if (!cancelled) return SALTS_EPROTO;
  }
  if (fixture.fail) return SALTS_EPERM;
  if (fixture.no_reply) return SALTS_OK;
  if (fixture.file_path) {
    char path[512];
    if (strlen(fixture.file_path) >= sizeof(path)) return SALTS_EMSGSIZE;
    snprintf(path, sizeof(path), "%s", fixture.file_path);
    char type[128] = {0}, etag[256] = {0};
    if (fixture.reply_type) snprintf(type, sizeof(type), "%s", fixture.reply_type);
    if (fixture.file_etag) snprintf(etag, sizeof(etag), "%s", fixture.file_etag);
    const chttp_server_file_options options = {path, fixture.reply_type ? type : NULL, fixture.file_etag ? etag : NULL};
    const int status = fixture.conditional_file ? chttp_invocation_serve_file(invocation, &options) :
        chttp_invocation_response_file(invocation, 201u, fixture.reply_type, path);
    fixture.header_status[0] = status;
    memset(path, '!', sizeof(path));
    memset(type, '!', sizeof(type)); memset(etag, '!', sizeof(etag));
    if (status != SALTS_OK && fixture.recover)
      return chttp_invocation_respond(invocation, 201u, NULL, "recovered", 9u);
    if (status == SALTS_OK && fixture.remove_after_plan) {
      if (tt_remove_file(fixture.file_path) != 0) return SALTS_EIO;
      fixture.file_removed = true;
    }
    if (status == SALTS_OK && chttp_invocation_respond(invocation, 200u, NULL, NULL, 0u) != SALTS_EALREADY)
      return SALTS_EPROTO;
    if (status == SALTS_OK && chttp_invocation_serve_file(invocation, &options) != SALTS_EALREADY)
      return SALTS_EPROTO;
    return status == SALTS_OK && fixture.file_then_fail ? SALTS_EPERM : status;
  }
  if (fixture.overflow) {
    char large[256]; memset(large, 'x', sizeof(large));
    const int status = chttp_invocation_respond(invocation, 200u, "text/plain", large, sizeof(large));
    if (!fixture.recover) return status;
    if (status != SALTS_EMSGSIZE) return SALTS_EPROTO;
  }
  char body[3] = {'a', '\0', 'z'};
  if (request->body_size != sizeof(body) || memcmp(request->body, body, sizeof(body)) != 0) return SALTS_EPROTO;
  const int status = chttp_invocation_respond(invocation, 201u, fixture.reply_type, body, sizeof(body));
  memset(body, '!', sizeof(body));
  if (status == SALTS_OK && chttp_invocation_respond(invocation, 202u, NULL, NULL, 0u) != SALTS_EALREADY) return SALTS_EPROTO;
  return status;
}
static cmeta_status before1(void *self, const chttp_invocation *invocation, bool *proceed) {
  (void)proceed; scope_check(self, invocation); trace(1);
  return fixture.before_headers && fixture.before_headers(invocation) != SALTS_OK ? CMETA_CALLBACK_ERROR : CMETA_OK;
}
static cmeta_status before2(void *self, const chttp_invocation *invocation, bool *proceed) {
  scope_check(self, invocation); trace(2);
  return fixture.reject && chttp_invocation_reject(invocation, proceed, 403u, "text/plain", "denied", 6u) != SALTS_OK ?
      CMETA_CALLBACK_ERROR : CMETA_OK;
}
static void after1(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  scope_check(self, invocation); if (result->status != SALTS_OK) atomic_fetch_add(&fixture.invalid, 1); trace(11);
  if (fixture.after_headers && fixture.after_headers(invocation) != SALTS_OK) atomic_fetch_add(&fixture.invalid, 1);
}
static void after2(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  scope_check(self, invocation); if (result->status != SALTS_OK) atomic_fetch_add(&fixture.invalid, 1); trace(12);
}
static void error1(void *self, const chttp_invocation *invocation, cmeta_status status) {
  scope_check(self, invocation); if (status == CMETA_OK) atomic_fetch_add(&fixture.invalid, 1); trace(-1);
}
static void error2(void *self, const chttp_invocation *invocation, cmeta_status status) {
  scope_check(self, invocation); if (status == CMETA_OK) atomic_fetch_add(&fixture.invalid, 1); trace(-2);
}
static void completed(void *self, const chttp_server_request_view *request, const chttp_application_work_outcome *outcome) {
  if (!self || ((InvocationController *)self)->marker != 42 || !chttp_application_is_worker(&fixture.app) ||
      !cmeta_function_desc_equal(request->operation, FunctionAbi(invocation_echo)->function)) atomic_fetch_add(&fixture.invalid, 1);
  trace(200); sync_mutex_lock(&fixture.mutex); ++fixture.completions; fixture.outcome = *outcome;
  sync_mutex_unlock(&fixture.mutex);
}
static void initialize(void) {
      const chttp_application_work_config limits = {fixture.workers, fixture.capacity, fixture.retained_bytes, fixture.reply_bytes,
      fixture.header_capacity, fixture.header_bytes};
  fixture.config.workers = limits;
  check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);

  if (fixture.admission_enabled) {
    const chttp_application_admission_binding binding = {"InvocationComponent", cmeta_reflected_data(InvocationController),
        FunctionAbi(invocation_admission), invocation_admission};
    check_equal(chttp_application_worker_admission(&fixture.app, &binding), SALTS_OK);
  }
}
static void fixture_listen(void) {
  check_equal(chttp_application_start(&fixture.app), SALTS_OK);
  uint16_t port = 0u; check_equal(chttp_application_port(&fixture.app, &port), SALTS_OK);
  snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned)port);
}
static void start(void) {
  initialize(); check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK); fixture_listen();
}
static void perform(void *context) {
  invocation_call *call = context; chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  const chttp_client_config config = {.network = fixture.config.http.network, .request_capacity = 1u,
      .max_start_line_bytes = 256u, .max_header_count = 16u, .max_header_bytes = 4096u,
      .max_request_body_bytes = 8192u, .max_response_body_bytes = 8192u, .max_informational_responses = 4u};
  call->status = chttp_client_init(&client, &config);
  if (call->status != SALTS_OK) return;
  static const char body[] = {'a', '\0', 'z'};
  chttp_header headers[8] = {{"X-Admission", "original"}}; size_t count = 1u;
  char authorization[1024];
  if (fixture.token) {
    snprintf(authorization, sizeof(authorization), "Bearer %s", fixture.token);
    headers[count++] = (chttp_header){"Authorization", authorization};
  }
  if (call->expect_continue) headers[count++] = (chttp_header){"Expect", "100-continue"};
  for (size_t i = 0u; i < call->header_count; ++i) headers[count++] = call->headers[i];
  const chttp_options options = {.connection_uri = fixture.uri, .authority = "invocation.test", .target = call->target,
      .protocol = call->protocol, .timeout_ms = 5000u, .headers = headers, .header_count = count,
      .body = call->head || call->get ? NULL : body, .body_size = call->head || call->get ? 0u : sizeof(body)};
  for (int iteration = 0; iteration < (call->repeat ? call->repeat : 1); ++iteration) {
    call->status = call->head ? chttp_head(&client, &options, &response, &error) : call->get ?
        chttp_get(&client, &options, &response, &error) : chttp_post(&client, &options, &response, &error);
    call->code = response.status_code; call->bytes = response.body_size;
    if (response.body_size <= sizeof(call->body) && response.body_size) memcpy(call->body, response.body, response.body_size);
    const char *value = chttp_response_header(&response, "X-Draft");
    if (value) snprintf(call->draft, sizeof(call->draft), "%s", value);
    value = chttp_response_header(&response, "X-Stage");
    if (value) snprintf(call->stage, sizeof(call->stage), "%s", value);
    value = chttp_response_header(&response, "X-After");
    call->after_present = value != NULL;
    if (value) snprintf(call->after, sizeof(call->after), "%s", value);
    value = chttp_response_header(&response, "Content-Type");
    if (value) snprintf(call->type, sizeof(call->type), "%s", value);
    value = chttp_response_header(&response, "Content-Length");
    if (value) snprintf(call->length, sizeof(call->length), "%s", value);
    value = chttp_response_header(&response, "Retry-After");
    if (value) snprintf(call->retry_after, sizeof(call->retry_after), "%s", value);
#define CAPTURE(name_, member_) do { value = chttp_response_header(&response, name_); \
    snprintf(call->member_, sizeof(call->member_), "%s", value ? value : ""); } while (0)
    CAPTURE("ETag", etag); CAPTURE("Last-Modified", modified); CAPTURE("Content-Range", range);
    CAPTURE("Allow", allow); CAPTURE("Accept-Ranges", accept_ranges);
#undef CAPTURE
    chttp_response_destroy(&response);
    if (call->status != SALTS_OK) break;
  }
  const int closed = chttp_client_destroy(&client, 5000u);
  if (call->status == SALTS_OK) call->status = closed;
}
static void wait_for_entry(void) {
  const uint64_t began = monotonic_ms(); sync_mutex_lock(&fixture.mutex);
  while (!fixture.entered && monotonic_ms() - began < 5000u)
    (void)sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(10000000));
  const int entered = fixture.entered; sync_mutex_unlock(&fixture.mutex); check_equal(entered, 1);
}

/* Send coalesced HTTP bytes without the Client's sequential request policy.
 * The fixture owns the socket through peer EOF and bounds both wire buffers. */
static int raw_exchange(const char *wire, char *response, size_t capacity) {
  uint16_t port = 0u;
  int status = chttp_application_port(&fixture.app, &port);
  if (status != SALTS_OK) return status;
  invocation_socket peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (peer == INVOCATION_INVALID_SOCKET) return SALTS_EIO;
#if defined(_WIN32)
  const DWORD timeout = 5000u;
  status = setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
  const struct timeval timeout = {5, 0};
  status = setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
  const struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port),
      .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
  if (status == 0) status = connect(peer, (const struct sockaddr *)&address, sizeof(address));
  size_t sent = 0u, size = 0u;
  const size_t length = strlen(wire);
  while (status == 0 && sent < length) {
    const int count = send(peer, wire + sent, (int)(length - sent), 0);
    if (count <= 0) status = -1;
    else sent += (size_t)count;
  }
  response[0] = '\0';
  while (status == 0 && size + 1u < capacity) {
    const int count = recv(peer, response + size, (int)(capacity - size - 1u), 0);
    if (count <= 0) { if (count < 0) status = -1; break; }
    size += (size_t)count; response[size] = '\0';
  }
  invocation_socket_close(peer);
  return status != 0 ? SALTS_EIO : size + 1u == capacity ? SALTS_EMSGSIZE : SALTS_OK;
}

static int copied_header(const chttp_invocation *invocation) {
  char name[] = "X-Draft", value[] = "copied";
  const int status = chttp_invocation_set_header(invocation, name, value);
  memset(name, '!', sizeof(name) - 1u); memset(value, '!', sizeof(value) - 1u);
  return status;
}
static int before_header(const chttp_invocation *invocation) {
  char value[] = "before";
  const int status = chttp_invocation_set_header(invocation, "X-Stage", value);
  memset(value, '!', sizeof(value) - 1u); return status;
}
static int after_headers(const chttp_invocation *invocation) {
  int status = chttp_invocation_set_header(invocation, "x-STAGE", "after");
  return status == SALTS_OK ? chttp_invocation_set_header(invocation, "X-After", "") : status;
}
static int content_type(const chttp_invocation *invocation) {
  return chttp_invocation_set_header(invocation, "cOnTeNt-TyPe", "test/override");
}
static int invalid_headers(const chttp_invocation *invocation) {
  static const chttp_header fields[] = {{"bad name", "v"}, {NULL, "v"}, {"X", NULL},
      {"X", "bad\r\nInjected: true"}, {"X", "bad\177"}, {"Content-Length", "0"},
      {"Connection", "close"}, {"Transfer-Encoding", "chunked"}, {"Upgrade", "websocket"},
      {"Sec-WebSocket-Accept", "x"}, {"Sec-WebSocket-Extensions", "x"}, {"Sec-WebSocket-Protocol", "x"}};
  for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
    fixture.header_status[i] = chttp_invocation_set_header(invocation, fields[i].name, fields[i].value);
  fixture.header_status[12] = chttp_invocation_respond(invocation, 200u, "text/plain\r\nX: bad", NULL, 0u);
  fixture.header_status[13] = chttp_invocation_respond(invocation, 204u, NULL, "x", 1u);
  char large[129]; memset(large, 'x', sizeof(large) - 1u); large[sizeof(large) - 1u] = 0;
  fixture.header_status[14] = chttp_invocation_set_header(invocation, "Content-Type", large);
  fixture.header_status[15] = chttp_invocation_response_file(invocation, 200u, NULL, NULL);
  fixture.header_status[16] = chttp_invocation_response_file(invocation, 200u, NULL, "");
  fixture.header_status[17] = chttp_invocation_response_file(invocation, 204u, NULL, "unused");
  fixture.header_status[18] = chttp_invocation_response_file(invocation, 200u, "bad\r\nX: true", "unused");
  fixture.header_status[19] = chttp_invocation_response_file(invocation, 200u, NULL, large);
  return chttp_invocation_set_header(invocation, "X-Draft", "valid");
}
static int bounded_header(const chttp_invocation *invocation) {
  fixture.header_status[0] = chttp_invocation_set_header(invocation, "X-Draft", "1234567");
  return SALTS_OK;
}
static int replace_header(const chttp_invocation *invocation) {
  (void)bounded_header(invocation);
  fixture.header_status[1] = chttp_invocation_set_header(invocation, "X-Other", "");
  fixture.header_status[2] = chttp_invocation_set_header(invocation, "x-draft", "z");
  fixture.header_status[3] = chttp_invocation_set_header(invocation, "X-DRAFT", "q");
  return SALTS_OK;
}
static void prepare_conditional_file(void) {
  fixture.conditional_file = true; fixture.reply_type = NULL; fixture.file_etag = "\"revision,one\"";
  fixture.reply_bytes = 512u; fixture.header_capacity = 4u; fixture.header_bytes = 256u;
  fixture.binding.method = CHTTP_METHOD_GET;
  fixture.file_path = tt_make_temp_file("chttp-worker-conditional-", ".txt"); check_not_null(fixture.file_path);
  check_equal(tt_write_file(fixture.file_path, "0123456789", 10u), 0);
}
static int before_etag(const chttp_invocation *invocation) {
  return chttp_invocation_set_header(invocation, "ETag", "prior");
}
static int after_etag(const chttp_invocation *invocation) {
  return chttp_invocation_set_header(invocation, "eTAG", "after");
}

spec("Application canonical buffered Invocation") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture)); atomic_init(&fixture.invalid, 0);
    sync_mutex_init(&fixture.mutex); sync_cond_init(&fixture.changed); fixture.workers = 1u;
    fixture.reply_type = "application/octet-stream";
    fixture.reply_bytes = 128u;
    fixture.capacity = 4u;
    fixture.retained_bytes = 16384u;
    fixture.provider = (salts_configurator_provider_binding){sizeof(fixture.provider),
        SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(InvocationComponent), NULL, NULL, create, NULL, NULL};
    fixture.deployment = (salts_configurator_deployment){&fixture.provider, NULL, NULL};
    fixture.controller = (chttp_controller){.component_id = "InvocationComponent", .receiver = cmeta_reflected_data(InvocationController)};
    fixture.config = chttp_application_config_default(); fixture.config.components = &fixture.deployment; fixture.config.component_count = 1u;
    fixture.config.controllers = &fixture.controller; fixture.config.controller_count = 1u;
    fixture.hooks[0] = (chttp_invocation_interceptor){"InvocationComponent", cmeta_reflected_data(InvocationController), before1, after1, error1};
    fixture.hooks[1] = (chttp_invocation_interceptor){"InvocationComponent", cmeta_reflected_data(InvocationController), before2, after2, error2};
    fixture.binding = (chttp_application_worker_binding){.component_id = "InvocationComponent", .receiver = cmeta_reflected_data(InvocationController),
        .method = CHTTP_METHOD_POST, .path = "/echo", .abi = FunctionAbi(invocation_echo), .invoke = invocation_echo,
        .after_completion = completed, .hooks = fixture.hooks, .hook_count = 2u};
  }
  after_each() {
    sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    if (fixture.thread_live) check_equal(sync_thread_join(&fixture.thread), SALTS_OK);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_equal(atomic_load(&fixture.invalid), 0);
    chttp_jwt_token_destroy(fixture.token); check_equal(chttp_jwt_bearer_validator_destroy(&fixture.jwt), SALTS_OK);
    if (fixture.file_path) {
      if (!fixture.file_removed) check_equal(tt_remove_file(fixture.file_path), 0);
      free(fixture.file_path);
    }
    sync_cond_destroy(&fixture.changed); sync_mutex_destroy(&fixture.mutex);
  }
  it("rejects zero handles without changing query outputs") {
    const chttp_invocation empty = {0}; bool cancelled = true, proceed = true;
    check_null(chttp_invocation_request(&empty)); check_null(chttp_invocation_session(&empty)); check_null(chttp_invocation_operation(&empty));
    check_equal(chttp_invocation_cancelled(&empty, &cancelled), SALTS_EINVAL); check_true(cancelled);
    check_equal(chttp_invocation_respond(&empty, 200u, NULL, NULL, 0u), SALTS_EINVAL);
    check_equal(chttp_invocation_set_header(&empty, "X-Draft", "bad"), SALTS_EINVAL);
    check_equal(chttp_invocation_response_file(&empty, 200u, NULL, "unused"), SALTS_EINVAL);
    check_equal(chttp_invocation_serve_file(&empty, NULL), SALTS_EINVAL);
    check_equal(chttp_invocation_reject(&empty, &proceed, 403u, NULL, NULL, 0u), SALTS_EINVAL); check_true(proceed);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    static const struct {
      const char *name, *value, *second_name, *second_value, *body, *range;
      unsigned int code;
    } file_cases[] = {
      {NULL, NULL, NULL, NULL, "0123456789", "", 200u},
      {"Range", "bytes=2-5", NULL, NULL, "2345", "bytes 2-5/10", 206u},
      {"Range", "bytes=7-", NULL, NULL, "789", "bytes 7-9/10", 206u},
      {"Range", "bytes=-3", NULL, NULL, "789", "bytes 7-9/10", 206u},
      {"Range", "bytes=10-", NULL, NULL, "", "bytes */10", 416u},
      {"Range", "bytes=0-1,4-5", NULL, NULL, "0123456789", "", 200u},
      {"Range", "bytes=18446744073709551616-", NULL, NULL, "0123456789", "", 200u},
      {"If-None-Match", "W/\"revision,one\"", NULL, NULL, "", "", 304u},
      {"If-None-Match", "bad", NULL, NULL, "", "", 400u},
      {"If-Match", "W/\"revision,one\"", NULL, NULL, "", "", 412u},
      {"If-None-Match", "\"other\"", "if-none-match", "\"revision,one\"", "", "", 304u},
      {"Range", "bytes=0-1", "If-Range", "\"revision,one\"", "01", "bytes 0-1/10", 206u},
      {"Range", "bytes=0-1", "If-Range", "W/\"revision,one\"", "0123456789", "", 200u},
      {"If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:37 GMT", NULL, NULL, "", "", 412u},
      {"If-None-Match", "\"other\"", "If-Modified-Since", "Sun, 06 Nov 2094 08:49:37 GMT", "0123456789", "", 200u}
    };
    for (size_t row = 0u; row < sizeof(file_cases) / sizeof(file_cases[0]); ++row) {
      it("copies conditional file decision %zu through repeated native terminals (protocol=%d)", row, protocol) {
        prepare_conditional_file(); start();
        chttp_header headers[2] = {{file_cases[row].name, file_cases[row].value},
            {file_cases[row].second_name, file_cases[row].second_value}};
        invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true,
            .headers = headers, .header_count = file_cases[row].second_name ? 2u : file_cases[row].name ? 1u : 0u, .repeat = 2};
        perform(&call); check_equal(call.status, SALTS_OK); check_equal(call.code, file_cases[row].code);
        check_equal(call.bytes, strlen(file_cases[row].body)); check_equal(call.body, file_cases[row].body);
        check_equal(call.range, file_cases[row].range); check_equal(call.etag, "\"revision,one\"");
        check_equal(call.accept_ranges, "bytes"); check_equal(strlen(call.modified), 29u);
        if (call.bytes) check_equal(call.type, "text/plain; charset=utf-8");
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(fixture.calls, 2); check_equal(fixture.completions, 2);
        check_equal(fixture.outcome.business_status, SALTS_OK); check_equal(fixture.outcome.publication_status, SALTS_OK);
      }
    }
    it("retains generated ETag/date and HEAD semantics (protocol=%d)", protocol) {
      prepare_conditional_file(); fixture.file_etag = NULL; start();
      const chttp_header range = {"Range", "bytes=2-5"};
      invocation_call head = {.target = "/echo", .protocol = (chttp_protocol)protocol, .head = true, .headers = &range, .header_count = 1u};
      perform(&head); check_equal(head.status, SALTS_OK); check_equal(head.code, 200u);
      check_equal(head.bytes, 0u); check_equal(head.length, "10"); check_equal(head.range, "");
      check_equal(strncmp(head.etag, "W/\"", 3u), 0);
      const chttp_header condition = {"If-None-Match", head.etag};
      invocation_call cached = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true, .headers = &condition, .header_count = 1u};
      perform(&cached); check_equal(cached.status, SALTS_OK); check_equal(cached.code, 304u); check_equal(cached.bytes, 0u);
      const chttp_header modified = {"If-Modified-Since", head.modified};
      cached.headers = &modified; perform(&cached); check_equal(cached.status, SALTS_OK); check_equal(cached.code, 304u);
    }
    it("keeps generated file headers in normal before/action/after order (protocol=%d)", protocol) {
      prepare_conditional_file(); fixture.before_headers = before_etag; fixture.after_headers = after_etag; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 200u); check_equal(call.etag, "after");
    }
    it("uses exact metadata capacity and owns an explicit MIME (protocol=%d)", protocol) {
      prepare_conditional_file(); fixture.before_headers = before_etag; fixture.header_capacity = 3u;
      fixture.header_bytes = 90u; fixture.reply_type = "test/copied"; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 200u);
      check_equal(call.etag, "\"revision,one\""); check_equal(call.type, "test/copied");
    }
    for (int ranged = 0; ranged < 2; ++ranged) {
      it("handles an empty conditional file (protocol=%d ranged=%d)", protocol, ranged) {
        prepare_conditional_file(); check_equal(tt_write_file(fixture.file_path, "", 0u), 0); start();
        const chttp_header range = {"Range", "bytes=0-"};
        invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true,
            .headers = &range, .header_count = ranged ? 1u : 0u}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, ranged ? 416u : 200u); check_equal(call.bytes, 0u);
        check_equal(call.range, ranged ? "bytes */0" : "");
      }
    }
    it("returns 405 before reading metadata for an unsupported method (protocol=%d)", protocol) {
      prepare_conditional_file(); fixture.binding.method = CHTTP_METHOD_POST;
      check_equal(tt_remove_file(fixture.file_path), 0); fixture.file_removed = true; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 405u); check_equal(call.allow, "GET, HEAD");
      check_equal(call.etag, ""); check_equal(call.bytes, 0u);
    }
    for (int invalid = 0; invalid < 2; ++invalid) {
      it("rejects invalid conditional-file input before draft mutation (protocol=%d invalid=%d)", protocol, invalid) {
        prepare_conditional_file(); fixture.before_headers = before_etag; fixture.recover = true;
        if (invalid) fixture.reply_type = "text/plain\r\nInjected: value";
        else fixture.file_etag = "invalid";
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.body, "recovered");
        check_equal(fixture.header_status[0], SALTS_EINVAL); check_equal(call.etag, "prior"); check_equal(call.modified, "");
      }
    }
    it("reports metadata failure as business failure without opening a native source (protocol=%d)", protocol) {
      prepare_conditional_file(); check_equal(tt_remove_file(fixture.file_path), 0); fixture.file_removed = true; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 500u); check_equal(call.etag, "");
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_not_equal(fixture.outcome.business_status, SALTS_OK); check_equal(fixture.outcome.publication_status, SALTS_OK);
    }
    for (int budget = 0; budget < 3; ++budget) {
      it("leaves the draft unchanged on conditional file budget failure %d (protocol=%d)", budget, protocol) {
        prepare_conditional_file(); fixture.before_headers = before_etag; fixture.recover = true;
        if (budget == 0) fixture.header_capacity = 2u;
        if (budget == 1) fixture.header_bytes = 80u;
        if (budget == 2) fixture.reply_bytes = 16u;
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.body, "recovered");
        check_equal(call.etag, "prior"); check_equal(call.modified, ""); check_equal(call.accept_ranges, "");
        check_equal(fixture.header_status[0], budget == 2 ? SALTS_EMSGSIZE : SALTS_ENOBUFS);
      }
    }
    it("does not open a file for a conditional no-body result (protocol=%d)", protocol) {
      prepare_conditional_file(); fixture.remove_after_plan = true; start();
      const chttp_header condition = {"If-None-Match", "\"revision,one\""};
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .get = true, .headers = &condition, .header_count = 1u};
      perform(&call); check_equal(call.status, SALTS_OK); check_equal(call.code, 304u); check_equal(call.bytes, 0u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.outcome.publication_status, SALTS_OK); check_equal(fixture.outcome.transport_status, SALTS_OK);
    }
    it("shares the retained-byte limit with header snapshots (protocol=%d)", protocol) {
      fixture.admission_enabled = true; fixture.retained_bytes = fixture.reply_bytes = 16u; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 503u);
      chttp_application_work_stats stats = {0};
      check_equal(chttp_application_worker_stats(&fixture.app, &stats), SALTS_OK);
      check_equal(stats.pending, 0u); check_equal(stats.retained_bytes, 0u);
      check_equal(stats.accepted, 0u); check_equal(stats.rejected, 1u);
      check_equal(fixture.admissions, 0); check_equal(fixture.calls, 0);
    }
    it("copies route parameters and forbids close from the header worker (protocol=%d)", protocol) {
      fixture.admission_enabled = true; fixture.admission_close = true;
      fixture.binding.path = "/echo/:id"; fixture.param_value = "copied"; start();
      invocation_call call = {.target = "/echo/copied", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.admissions, 1); check_equal(fixture.calls, 1); check_equal(fixture.destroyed, 1);
    }
    it("runs header admission and body action on one worker with one shared slot (protocol=%d)", protocol) {
      fixture.admission_enabled = true; fixture.capacity = 1u; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .expect_continue = protocol == CHTTP_HTTP_1_1}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.admissions, 1); check_equal(fixture.calls, 1); check_equal(fixture.completions, 1);
      const int expected[] = {0,1,2,3,12,11,200}; check_equal(fixture.trace_count, 7u); check_equal(fixture.trace, expected, sizeof(expected));
    }
    it("holds body delivery behind a worker decision and rejects excess header credit (protocol=%d)", protocol) {
      fixture.admission_enabled = true; fixture.admission_hold = true; fixture.capacity = 1u; start();
      fixture.background = (invocation_call){.target = "/echo", .protocol = (chttp_protocol)protocol};
      check_equal(sync_thread_create(&fixture.thread, perform, &fixture.background), SALTS_OK); fixture.thread_live = true; wait_for_entry();
      invocation_call rejected = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&rejected);
      check_equal(rejected.status, SALTS_OK); check_equal(rejected.code, 503u);
      sync_mutex_lock(&fixture.mutex); const int calls = fixture.calls, admissions = fixture.admissions; sync_mutex_unlock(&fixture.mutex);
      check_equal(calls, 0); check_equal(admissions, 1);
      sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
      check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
      check_equal(fixture.background.status, SALTS_OK); check_equal(fixture.background.code, 201u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.calls, 1); check_equal(fixture.admissions, 1);
    }
    for (int failure = 0; failure < 4; ++failure) {
      it("rejects before business for header decision %d (protocol=%d)", failure, protocol) {
        fixture.admission_enabled = true;
        fixture.admission_status = failure == 1 ? SALTS_EIO : SALTS_OK;
        fixture.admission_code = failure == 0 ? 429u : failure == 2 ? 201u : 0u;
        fixture.admission_retry = failure == 0 ? 3u : failure == 3 ? 1u : 0u;
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .expect_continue = protocol == CHTTP_HTTP_1_1}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, failure == 0 ? 429u : 500u);
        check_equal(call.retry_after, failure == 0 ? "3" : "");
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(fixture.admissions, 1); check_equal(fixture.calls, 0); check_equal(fixture.completions, 0);
      }
    }
    for (int authorized = 0; authorized < 2; ++authorized) {
      it("authenticates JWT before worker admission (protocol=%d authorized=%d)", protocol, authorized) {
        static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
        const chttp_jwt_bearer_validator_options options = {.size = sizeof(options), .key = key, .key_size = sizeof(key) - 1u};
        check_equal(chttp_jwt_bearer_validator_init(&fixture.jwt, &options), SALTS_OK);
        fixture.config.jwt = &fixture.jwt; fixture.admission_enabled = true;
        if (authorized) {
          const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
          check_equal(chttp_jwt_hs256_token_create(&claims, key, sizeof(key) - 1u, &fixture.token), SALTS_OK);
        }
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, authorized ? 201u : 401u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(fixture.admissions, authorized); check_equal(fixture.calls, authorized);
      }
    }
    it("includes worker header wait in body deadline and retains finite-close ownership (protocol=%d)", protocol) {
      fixture.admission_enabled = true; fixture.admission_hold = true; fixture.config.deadlines.body_ms = 500u; start();
      fixture.background = (invocation_call){.target = "/echo", .protocol = (chttp_protocol)protocol};
      check_equal(sync_thread_create(&fixture.thread, perform, &fixture.background), SALTS_OK); fixture.thread_live = true; wait_for_entry();
      check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
      check_not_equal(fixture.background.status, SALTS_OK); check_not_equal(fixture.background.status, SALTS_ETIMEDOUT);
      check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT); check_not_null(fixture.app.impl); check_equal(fixture.destroyed, 0);
      sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_equal(fixture.destroyed, 1);
      check_equal(fixture.admissions, 1); check_equal(fixture.calls, 0);
    }
    for (int kind = 0; kind < 3; ++kind) {
      it("owns a copied file decision through native completion (protocol=%d kind=%d)", protocol, kind) {
        fixture.file_path = tt_make_temp_file("chttp-worker-file-", ".bin"); check_not_null(fixture.file_path);
        static const char bytes[] = {'a', '\0', 'z'};
        check_equal(tt_write_file(fixture.file_path, bytes, kind == 1 ? 0u : sizeof(bytes)), 0);
        fixture.reply_bytes = 512u; fixture.header_capacity = 3u; fixture.header_bytes = 128u;
        fixture.write_headers = copied_header; fixture.before_headers = before_header; fixture.after_headers = after_headers;
        if (kind == 2) fixture.binding.method = CHTTP_METHOD_HEAD;
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol, .head = kind == 2, .repeat = 2}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.bytes, kind ? 0u : sizeof(bytes));
        if (!kind) check_equal(call.body, bytes, sizeof(bytes));
        check_equal(call.length, kind == 1 ? "0" : "3"); check_equal(call.draft, "copied"); check_equal(call.stage, "after");
        chttp_server_stats stats = {0}; check_equal(chttp_application_stats(&fixture.app, &stats), SALTS_OK);
        check_equal(stats.accepted_connections, 1u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        check_equal(fixture.calls, 2); check_equal(fixture.completions, 2); check_equal(fixture.outcome.transport_status, SALTS_OK);
      }
    }
    it("preserves an after-hook MIME override on a file response (protocol=%d)", protocol) {
      fixture.file_path = tt_make_temp_file("chttp-worker-type-", ".txt"); check_not_null(fixture.file_path);
      check_equal(tt_write_file(fixture.file_path, "file", 4u), 0); fixture.reply_bytes = 512u;
      fixture.after_headers = content_type; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.bytes, 4u); check_equal(call.type, "test/override");
    }
    it("discards a staged file when the business method fails (protocol=%d)", protocol) {
      fixture.file_path = tt_make_temp_file("chttp-discard-file-", ".txt"); check_not_null(fixture.file_path);
      check_equal(tt_remove_file(fixture.file_path), 0); fixture.file_removed = true; fixture.file_then_fail = true;
      fixture.reply_bytes = 512u; start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 500u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      check_equal(fixture.outcome.business_status, SALTS_EPERM); check_equal(fixture.outcome.publication_status, SALTS_OK);
    }
    it("copies bounded headers through before/action/after hooks on protocol %d", protocol) {
      fixture.header_capacity = 3u; fixture.header_bytes = 128u;
      fixture.write_headers = copied_header; fixture.before_headers = before_header; fixture.after_headers = after_headers;
      start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
      check_equal(call.draft, "copied"); check_equal(call.stage, "after"); check_equal(call.after, "");
      check_true(call.after_present);
      check_equal(call.type, "application/octet-stream");
    }
    for (int order = 0; order < 3; ++order) {
      it("keeps one authoritative Content-Type for order %d on protocol %d", order, protocol) {
        if (order == 0) fixture.before_headers = content_type;
        if (order == 1) fixture.after_headers = content_type;
        if (order == 2) { fixture.before_headers = content_type; fixture.reply_type = NULL; }
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
        check_equal(call.type, order == 0 ? "application/octet-stream" : "test/override");
      }
    }
    it("rejects injection/reserved fields before mutating the draft on protocol %d", protocol) {
      fixture.header_capacity = 1u; fixture.header_bytes = 64u; fixture.write_headers = invalid_headers;
      start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.draft, "valid");
      for (size_t i = 0u; i < 12u; ++i) check_equal(fixture.header_status[i], i < 5u ? SALTS_EINVAL : SALTS_EPERM);
      check_equal(fixture.header_status[12], SALTS_EINVAL); check_equal(fixture.header_status[13], SALTS_EINVAL);
      check_equal(fixture.header_status[14], SALTS_EMSGSIZE);
      for (size_t i = 15u; i < 19u; ++i) check_equal(fixture.header_status[i], SALTS_EINVAL);
      check_equal(fixture.header_status[19], SALTS_EMSGSIZE);
    }
    for (int short_by = 0; short_by < 2; ++short_by) {
      it("enforces exact extra-header wire capacity minus %d on protocol %d", short_by, protocol) {
        fixture.header_capacity = 1u; fixture.header_bytes = 18u - (size_t)short_by; fixture.write_headers = bounded_header;
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
        check_equal(fixture.header_status[0], short_by ? SALTS_ENOBUFS : SALTS_OK);
        check_equal(call.draft, short_by ? "" : "1234567");
      }
    }
    it("preserves the last value when count/append-only storage is exhausted on protocol %d", protocol) {
      fixture.header_capacity = 1u; fixture.header_bytes = 18u; fixture.write_headers = replace_header;
      start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.draft, "z");
      check_equal(fixture.header_status[0], SALTS_OK); check_equal(fixture.header_status[1], SALTS_ENOBUFS);
      check_equal(fixture.header_status[2], SALTS_OK); check_equal(fixture.header_status[3], SALTS_ENOBUFS);
    }
    it("keeps headers disabled when no extra-header budget is configured on protocol %d", protocol) {
      fixture.write_headers = bounded_header;
      start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.draft, "");
      check_equal(fixture.header_status[0], SALTS_ENOBUFS);
    }
    for (int rejected = 0; rejected < 2; ++rejected) {
      it("settles staged headers for rejection %d or business failure on protocol %d", rejected, protocol) {
        fixture.header_capacity = 1u; fixture.header_bytes = 64u; fixture.before_headers = before_header;
        fixture.reject = rejected != 0; fixture.fail = !fixture.reject;
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, rejected ? 403u : 500u);
        check_equal(call.stage, rejected ? "before" : ""); check_equal(call.type, "text/plain");
      }
    }
    it("copies request/response bytes and preserves typed hook order on protocol %d", protocol) {
      start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.bytes, 3u);
      static const char expected_body[] = {'a', '\0', 'z'}; check_equal(call.body, expected_body, sizeof(expected_body));
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      const int expected[] = {1,2,3,12,11,200}; check_equal(fixture.trace_count, 6u); check_equal(fixture.trace, expected, sizeof(expected));
      check_equal(fixture.calls, 1); check_equal(fixture.completions, 1); check_equal(fixture.destroyed, 1);
    }
    it("preserves explicit Invocation rejection and excludes business execution on protocol %d", protocol) {
      fixture.reject = true; start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 403u);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
      const int expected[] = {1,2,-2,-1,200}; check_equal(fixture.trace_count, 5u); check_equal(fixture.trace, expected, sizeof(expected));
      check_equal(fixture.calls, 0); check_true(fixture.outcome.rejected);
    }
    for (int failure = 0; failure <= 2; ++failure) {
      it("unwinds typed error hooks for failure %d on protocol %d", failure, protocol) {
        fixture.fail = failure == 0; fixture.no_reply = failure == 1; fixture.overflow = failure == 2;
        start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 500u);
        check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
        const int expected[] = {1,2,3,-2,-1,200}; check_equal(fixture.trace_count, 6u); check_equal(fixture.trace, expected, sizeof(expected));
        check_equal(fixture.outcome.business_status, failure == 0 ? SALTS_EPERM : failure == 1 ? SALTS_EPROTO : SALTS_EMSGSIZE);
        check_equal(fixture.outcome.transport_status, SALTS_OK);
      }
    }
    it("leaves an oversized draft unsealed and reusable on protocol %d", protocol) {
      fixture.overflow = true; fixture.recover = true; start();
      invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
    }
    it("refuses Invocation access from a foreign thread on protocol %d", protocol) {
      fixture.foreign = true; start(); invocation_call call = {.target = "/echo", .protocol = (chttp_protocol)protocol}; perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
    }
  }
  it("serializes coalesced zero-body requests across header and file-response continuations") {
    static const char wire[] = "GET /echo HTTP/1.1\r\nHost: x\r\nX-Admission: original\r\n\r\n"
        "GET /echo HTTP/1.1\r\nHost: x\r\nX-Admission: original\r\nConnection: close\r\n\r\n";
    fixture.admission_enabled = true; fixture.binding.method = CHTTP_METHOD_GET; fixture.reply_bytes = 512u;
    fixture.file_path = tt_make_temp_file("chttp-worker-pipeline-", ".txt"); check_not_null(fixture.file_path);
    static const char body[] = "pipeline-body";
    check_equal(tt_write_file(fixture.file_path, body, sizeof(body) - 1u), 0); start();
    char response[4096]; check_equal(raw_exchange(wire, response, sizeof(response)), SALTS_OK);
    const char *first = strstr(response, "HTTP/1.1 201"); check_not_null(first);
    const char *second = first ? strstr(first + 1, "HTTP/1.1 201") : NULL; check_not_null(second);
    if (first && second) {
      const char *first_body = strstr(first, body), *second_body = strstr(second, body);
      check_true(first_body && first_body < second); check_not_null(second_body);
      check_null(strstr(second + 1, "HTTP/1.1"));
    }
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.admissions, 2); check_equal(fixture.calls, 2); check_equal(fixture.completions, 2);
  }
  it("rejects an Expect request without sending 100 Continue") {
    fixture.admission_enabled = true; fixture.admission_code = 429u; fixture.admission_retry = 3u; start();
    char response[4096];
    check_equal(raw_exchange("POST /echo HTTP/1.1\r\nHost: x\r\nX-Admission: original\r\n"
        "Content-Length: 4\r\nExpect: 100-continue\r\n\r\n", response, sizeof(response)), SALTS_OK);
    check_not_null(strstr(response, "HTTP/1.1 429")); check_null(strstr(response, "100 Continue"));
    check_not_null(strstr(response, "Retry-After: 3"));
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.admissions, 1); check_equal(fixture.calls, 0);
  }
  it("rejects legacy/missing metadata and missing hook Beans before route publication") {
    initialize(); fixture.binding.abi = FunctionAbi(invocation_legacy_contract);
    check_equal(cmeta_function_param_abi(fixture.binding.abi, 1u), CMETA_ABI_OBJECT_POINTER);
    check_equal(cmeta_function_param_abi(FunctionAbi(invocation_echo), 1u), CMETA_ABI_OBJECT_POINTER);
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_EINVAL);
    fixture.binding.abi = NULL; check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_EINVAL);
    fixture.binding.abi = FunctionAbi(invocation_echo); fixture.hooks[1].component_id = "Missing";
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_ENOENT);
    fixture.hooks[1].component_id = "InvocationComponent";
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK);
    memset(fixture.hooks, 0, sizeof(fixture.hooks)); memset(&fixture.binding, 0, sizeof(fixture.binding)); fixture_listen();
    invocation_call call = {.target = "/echo", .protocol = CHTTP_HTTP_2}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
  }
  it("validates the admission ABI and canonical Bean before publishing its native gate") {
    initialize();
    chttp_application_admission_binding binding = {"InvocationComponent", cmeta_reflected_data(InvocationController),
        FunctionAbi(invocation_echo), invocation_admission};
    check_equal(chttp_application_worker_admission(&fixture.app, &binding), SALTS_EINVAL);
    binding.abi = NULL; check_equal(chttp_application_worker_admission(&fixture.app, &binding), SALTS_EINVAL);
    binding.abi = FunctionAbi(invocation_admission); binding.component_id = "missing";
    check_equal(chttp_application_worker_admission(&fixture.app, &binding), SALTS_ENOENT);
    binding.component_id = "InvocationComponent";
    check_equal(chttp_application_worker_admission(&fixture.app, &binding), SALTS_OK);
    check_equal(chttp_application_worker_admission(&fixture.app, &binding), SALTS_EALREADY);
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK); fixture_listen();
    check_equal(chttp_application_worker_admission(&fixture.app, &binding), SALTS_EBUSY);
    invocation_call call = {.target = "/echo", .protocol = CHTTP_HTTP_2}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
  }
  it("skips a queued header callback whose native body deadline already expired") {
    fixture.admission_enabled = true; fixture.admission_hold = true; fixture.capacity = 2u;
    fixture.config.deadlines.body_ms = 500u; start();
    fixture.background = (invocation_call){.target = "/echo", .protocol = CHTTP_HTTP_2};
    check_equal(sync_thread_create(&fixture.thread, perform, &fixture.background), SALTS_OK); fixture.thread_live = true; wait_for_entry();
    invocation_call queued = {.target = "/echo", .protocol = CHTTP_HTTP_2}; perform(&queued);
    check_not_equal(queued.status, SALTS_OK); check_not_equal(queued.status, SALTS_ETIMEDOUT);
    check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
    check_not_equal(fixture.background.status, SALTS_OK);
    chttp_application_work_stats stats = {0}; check_equal(chttp_application_worker_stats(&fixture.app, &stats), SALTS_OK);
    check_equal(stats.pending, 2u); check_equal(stats.accepted, 2u); check_equal(stats.completed, 0u);
    check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT); check_equal(fixture.destroyed, 0);
    sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.admissions, 1); check_equal(fixture.calls, 0); check_equal(fixture.destroyed, 1);
  }
  it("keeps overlapping Invocations isolated even on the same two-worker executor") {
    fixture.workers = 2u; fixture.cross_scope = true; fixture.binding.path = "/first"; initialize();
    check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK);
    fixture.binding.path = "/second"; check_equal(chttp_application_worker_mount(&fixture.app, &fixture.binding), SALTS_OK); fixture_listen();
    fixture.background = (invocation_call){.target = "/first", .protocol = CHTTP_HTTP_2};
    check_equal(sync_thread_create(&fixture.thread, perform, &fixture.background), SALTS_OK); fixture.thread_live = true; wait_for_entry();
    invocation_call call = {.target = "/second", .protocol = CHTTP_HTTP_2}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
    sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
    check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
    check_equal(fixture.background.status, SALTS_OK); check_equal(fixture.background.code, 201u);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_equal(fixture.calls, 2); check_equal(fixture.completions, 2);
  }
  for (int file = 0; file < 2; ++file) {
    it("queries observed cancellation without releasing a still-running Controller (file=%d)", file) {
      if (file) {
        fixture.file_path = tt_make_temp_file("chttp-cancel-file-", ".txt"); check_not_null(fixture.file_path);
        check_equal(tt_remove_file(fixture.file_path), 0); fixture.file_removed = true; fixture.reply_bytes = 512u;
      }
      fixture.header_capacity = 1u; fixture.header_bytes = 64u; fixture.write_headers = copied_header;
      fixture.wait_cancel = true; fixture.config.deadlines.handler_ms = 500u; start();
      fixture.background = (invocation_call){.target = "/echo", .protocol = CHTTP_HTTP_2};
      check_equal(sync_thread_create(&fixture.thread, perform, &fixture.background), SALTS_OK); fixture.thread_live = true; wait_for_entry();
      check_equal(sync_thread_join(&fixture.thread), SALTS_OK); fixture.thread_live = false;
      check_not_equal(fixture.background.status, SALTS_OK); check_not_equal(fixture.background.status, SALTS_ETIMEDOUT);
      check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT); check_not_null(fixture.app.impl); check_equal(fixture.destroyed, 0);
      sync_mutex_lock(&fixture.mutex); fixture.release = true; sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
      check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_equal(fixture.destroyed, 1);
      check_equal(fixture.outcome.business_status, SALTS_OK); check_not_equal(fixture.outcome.transport_status, SALTS_OK);
      check_not_equal(fixture.outcome.publication_status, SALTS_EIO);
    }
  }
}
