#include "chttp_application_worker.h"
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
typedef SOCKET upload_socket;
#define UPLOAD_INVALID_SOCKET INVALID_SOCKET
#define upload_socket_close closesocket
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int upload_socket;
#define UPLOAD_INVALID_SOCKET (-1)
#define upload_socket_close close
#endif

typedef struct UploadBean { int marker; } UploadBean;
cmeta_reflect_data(UploadBean, "test.UploadBean", cmeta_field(int, marker));
cmeta_component_empty(UploadComponent);
CHTTP_UPLOAD_OPEN_ACTION(upload_open);
CHTTP_INVOCATION_ACTION(upload_action);
CHTTP_INVOCATION_ACTION(upload_session);
CHTTP_ADMISSION_ACTION(upload_admit);
typedef struct upload_resource {
  chttp_web_upload_request multipart; /* First member shares the sink-user address. */
  chttp_body_sink decoder;
  UploadBean *bean;
  size_t bytes, part_bytes;
  bool closed;
  int terminal;
} upload_resource;
typedef struct upload_call {
  chttp_protocol protocol;
  const char *path, *cookie, *csrf, *body, *origin, *authorization;
  size_t size, sent, input_chunk;
  bool known;
  uint32_t timeout_ms;
  int status;
  unsigned int code;
  char output[128], next_cookie[256];
} upload_call;
static struct {
  chttp_application app;
  chttp_application_config config;
  chttp_application_work_config limits;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  chttp_application_upload_binding binding;
  chttp_invocation_interceptor hook;
  chttp_jwt_bearer_validator jwt;
  char *token;
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t thread;
  sync_thread_t thread2;
  bool thread_live, thread2_live, hold, entered, release, multipart, fail_action, admission, hold_admission;
  bool expect_trailer;
  bool hold_close, release_close, session_entered, release_session;
  upload_socket peer;
  int admissions;
  int open_status, write_status, opens, writes, closes, cleanups, actions, commits, aborts, resources, destroyed;
  int close_status, completions;
  _Atomic int invalid;
  size_t received, part_bytes;
  chttp_application_work_outcome outcome;
  char uri[64];
  upload_call background;
  upload_call background2;
} uploads;
static void upload_worker(void);
int upload_admit(void *self, const chttp_server_request_view *request, chttp_server_admission_result *out) {
  upload_worker();
  if (!self || !request || request->session || request->body || request->body_size) return SALTS_EPROTO;
  sync_mutex_lock(&uploads.mutex); ++uploads.admissions;
  if (uploads.hold_admission && request->path && strcmp(request->path, "/upload") == 0) {
    uploads.entered = true; sync_cond_broadcast(&uploads.changed);
    while (!uploads.release) sync_cond_wait(&uploads.changed, &uploads.mutex);
  }
  sync_mutex_unlock(&uploads.mutex);
  *out = (chttp_server_admission_result){0}; return SALTS_OK;
}
static void upload_worker(void) { if (!chttp_application_worker_current(&uploads.app)) atomic_fetch_add(&uploads.invalid, 1); }
static void upload_destroy(void *user, void *object) { (void)user; ++uploads.destroyed; free(object); }
static const cmeta_object_lifecycle upload_lifecycle = {sizeof(upload_lifecycle), NULL, NULL, NULL, upload_destroy};
static cmeta_status upload_create(void *user, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)user; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  UploadBean *bean = malloc(sizeof(*bean)); if (!bean) return CMETA_OUT_OF_MEMORY; bean->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, bean, cmeta_reflected_data(UploadBean), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &upload_lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(bean); }
  return status;
}
static int upload_begin(void *user, const chttp_server_request_view *request) {
  upload_resource *resource = user; upload_worker();
  if (uploads.expect_trailer && chttp_server_request_header(request, "X-Upload-Proof")) return SALTS_EPROTO;
  if (uploads.jwt.impl && (!request->jwt_claims || !request->jwt_claims->subject || strcmp(request->jwt_claims->subject, "alice") != 0)) return SALTS_EPROTO;
  return resource->bean->marker == 42 && request && !request->session && !request->body && !request->body_size &&
      !request->body_sink_user && cmeta_function_desc_equal(request->operation, FunctionAbi(upload_action)->function) ? SALTS_OK : SALTS_EPROTO;
}
static int upload_part_begin(void *user, const chttp_web_multipart_part *part) {
  (void)user; upload_worker(); return part && part->name.data ? SALTS_OK : SALTS_EINVAL;
}
static int upload_part_data(void *user, const void *data, size_t size) {
  upload_resource *resource = user; (void)data; upload_worker(); resource->part_bytes += size; return SALTS_OK;
}
static int upload_part_end(void *user) { (void)user; upload_worker(); return SALTS_OK; }
static int upload_commit(void *user) {
  upload_resource *resource = user; upload_worker();
  resource->terminal = 1;
  sync_mutex_lock(&uploads.mutex); ++uploads.commits; uploads.part_bytes = resource->part_bytes; sync_mutex_unlock(&uploads.mutex);
  return SALTS_OK;
}
static void upload_abort(void *user, chttp_web_status status, int native_status) {
  ((upload_resource *)user)->terminal = -1; (void)status; (void)native_status; upload_worker();
  sync_mutex_lock(&uploads.mutex); ++uploads.aborts; sync_mutex_unlock(&uploads.mutex);
}
static int upload_write(void *user, const void *data, size_t size) {
  upload_resource *resource = user; upload_worker();
  sync_mutex_lock(&uploads.mutex); ++uploads.writes;
  if (uploads.hold) {
    uploads.entered = true; sync_cond_broadcast(&uploads.changed);
    while (!uploads.release) sync_cond_wait(&uploads.changed, &uploads.mutex);
  }
  sync_mutex_unlock(&uploads.mutex);
  if (uploads.write_status) return uploads.write_status;
  if (resource->closed || !resource->bean || resource->bean->marker != 42) return SALTS_EPROTO;
  int status = SALTS_OK;
  if (uploads.multipart) status = resource->decoder.write(resource->decoder.user, data, size);
  else for (size_t i = 0u; i < size; ++i) if (((const unsigned char *)data)[i] != 'x') return SALTS_EPROTO;
  if (status == SALTS_OK) resource->bytes += size;
  return status;
}
int upload_open(void *self, const chttp_server_request_view *request, chttp_body_sink *sink) {
  upload_worker();
  if (chttp_application_close(&uploads.app, 1u) != SALTS_EBUSY) atomic_fetch_add(&uploads.invalid, 1);
  sync_mutex_lock(&uploads.mutex); ++uploads.opens; sync_mutex_unlock(&uploads.mutex);
  if (uploads.open_status) return uploads.open_status;
  upload_resource *resource = calloc(1u, sizeof(*resource)); if (!resource) return SALTS_ENOMEM;
  resource->bean = self;
  int status = upload_begin(resource, request);
  if (status == SALTS_OK && uploads.multipart) {
    const chttp_web_upload_callbacks callbacks = {sizeof(callbacks), upload_begin, upload_part_begin, upload_part_data,
        upload_part_end, upload_commit, upload_abort};
    chttp_web_multipart_limits limits = CHTTP_WEB_MULTIPART_LIMITS_INIT;
    limits.max_parts = 8u; limits.max_header_count = 8u; limits.max_header_bytes = 1024u;
    limits.max_name_bytes = 64u; limits.max_filename_bytes = 128u; limits.max_content_type_bytes = 64u;
    limits.max_field_bytes = 128u; limits.max_total_bytes = 256u * 1024u;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    if (chttp_web_upload_open(&resource->multipart, request, &limits, &callbacks, resource, &resource->decoder, &error) != CHTTP_WEB_OK)
      status = error.native_status ? error.native_status : SALTS_EPROTO;
  }
  if (status != SALTS_OK) { free(resource); return status; }
  sync_mutex_lock(&uploads.mutex); ++uploads.resources; sync_mutex_unlock(&uploads.mutex);
  *sink = (chttp_body_sink){upload_write, resource};
  return SALTS_OK;
}
static void upload_close(void *self, chttp_body_sink *sink, int status) {
  upload_resource *resource = sink->user; upload_worker();
  sync_mutex_lock(&uploads.mutex);
  if (uploads.hold_close) {
    uploads.entered = true; sync_cond_broadcast(&uploads.changed);
    while (!uploads.release_close) sync_cond_wait(&uploads.changed, &uploads.mutex);
  }
  sync_mutex_unlock(&uploads.mutex);
  if (self != resource->bean || resource->closed) atomic_fetch_add(&uploads.invalid, 1);
  resource->closed = true;
  if (uploads.multipart) chttp_web_upload_close(&resource->multipart, status);
  sync_mutex_lock(&uploads.mutex); ++uploads.closes; uploads.close_status = status; sync_mutex_unlock(&uploads.mutex);
}
static void upload_cleanup(void *self, chttp_body_sink *sink, const chttp_application_work_outcome *outcome) {
  upload_resource *resource = sink->user; upload_worker(); (void)outcome;
  if (self != resource->bean || !resource->closed) atomic_fetch_add(&uploads.invalid, 1);
  if (uploads.multipart && !resource->terminal)
    if (chttp_web_upload_abort(&resource->multipart, CHTTP_WEB_UPLOAD, SALTS_ECANCELED, NULL) != CHTTP_WEB_OK) atomic_fetch_add(&uploads.invalid, 1);
  sync_mutex_lock(&uploads.mutex); ++uploads.cleanups; --uploads.resources; uploads.received = resource->bytes;
  sync_cond_broadcast(&uploads.changed); sync_mutex_unlock(&uploads.mutex);
  free(resource);
}
static void upload_completed(void *self, const chttp_server_request_view *request, const chttp_application_work_outcome *outcome) {
  (void)self; (void)request; upload_worker(); sync_mutex_lock(&uploads.mutex); ++uploads.completions; uploads.outcome = *outcome; sync_mutex_unlock(&uploads.mutex);
}
static cmeta_status upload_before(void *self, const chttp_invocation *invocation, bool *proceed) {
  (void)proceed; upload_worker();
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  const upload_resource *resource = request ? request->body_sink_user : NULL;
  return resource && resource->bean == self && resource->closed && request->body_streamed ? CMETA_OK : CMETA_CALLBACK_ERROR;
}
static void upload_after(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  (void)self; (void)result; upload_worker();
  if (chttp_invocation_set_header(invocation, "X-Upload", "worker") != SALTS_OK) atomic_fetch_add(&uploads.invalid, 1);
}
int upload_action(void *self, const chttp_invocation *invocation) {
  upload_worker(); const chttp_server_request_view *request = chttp_invocation_request(invocation);
  upload_resource *resource = request ? request->body_sink_user : NULL;
  if (!resource || resource->bean != self || !resource->closed || !request->body_streamed || request->body || request->session || request->body_size != resource->bytes)
    return SALTS_EPROTO;
  if (uploads.expect_trailer) {
    const char *proof = chttp_server_request_header(request, "X-Upload-Proof");
    if (!proof || strcmp(proof, "final") != 0) return SALTS_EPROTO;
  }
  sync_mutex_lock(&uploads.mutex); ++uploads.actions; sync_mutex_unlock(&uploads.mutex);
  if (uploads.fail_action) return SALTS_EPERM;
  unsigned int code = 201u;
  if (uploads.multipart) {
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    const chttp_web_status status = chttp_invocation_upload_finalize(invocation, &resource->multipart, NULL, &error);
    code = status == CHTTP_WEB_OK ? 201u : status == CHTTP_WEB_CSRF ? 403u : 400u;
  }
  if (chttp_session_set(chttp_invocation_session(invocation), "uploaded", "yes") != SALTS_OK) return SALTS_EPROTO;
  return chttp_invocation_respond(invocation, code, "text/plain", "done", 4u);
}
int upload_session(void *self, const chttp_invocation *invocation) {
  (void)self; const char *token = NULL;
  if (strcmp(chttp_invocation_request(invocation)->path, "/hold-session") == 0) {
    sync_mutex_lock(&uploads.mutex); uploads.session_entered = true; sync_cond_broadcast(&uploads.changed);
    while (!uploads.release_session) sync_cond_wait(&uploads.changed, &uploads.mutex);
    sync_mutex_unlock(&uploads.mutex);
  }
  if (chttp_web_csrf_ensure(chttp_invocation_session(invocation), &token, NULL) != CHTTP_WEB_OK) return SALTS_EIO;
  return chttp_invocation_respond(invocation, 200u, "text/plain", token, strlen(token));
}
static void upload_start(void) {
  check_equal(chttp_application_init(&uploads.app, &uploads.config), SALTS_OK);
  check_equal(chttp_application_worker_prepare(&uploads.app, &uploads.limits), SALTS_OK);
  if (uploads.admission) {
    const chttp_application_admission_binding binding = {"UploadComponent", cmeta_reflected_data(UploadBean), FunctionAbi(upload_admit), upload_admit};
    check_equal(chttp_application_worker_admission(&uploads.app, &binding), SALTS_OK);
  }
  check_equal(chttp_application_worker_upload(&uploads.app, &uploads.binding), SALTS_OK);
  chttp_application_worker_binding session = uploads.binding.controller;
  session.method = CHTTP_METHOD_GET; session.path = "/session"; session.abi = FunctionAbi(upload_session); session.invoke = upload_session;
  session.after_completion = NULL;
  session.hooks = NULL; session.hook_count = 0u;
  check_equal(chttp_application_worker_mount(&uploads.app, &session), SALTS_OK);
  session.path = "/hold-session"; check_equal(chttp_application_worker_mount(&uploads.app, &session), SALTS_OK);
  check_equal(chttp_application_start(&uploads.app), SALTS_OK);
  uint16_t port = 0u; check_equal(chttp_application_port(&uploads.app, &port), SALTS_OK);
  snprintf(uploads.uri, sizeof(uploads.uri), "tcp://127.0.0.1:%u", (unsigned)port);
}
static int upload_input(void *user, void *buffer, size_t capacity, size_t *out) {
  upload_call *call = user; size_t bytes = call->size - call->sent;
  if (bytes > capacity) bytes = capacity;
  if (call->input_chunk && bytes > call->input_chunk) bytes = call->input_chunk;
  if (call->body) memcpy(buffer, call->body + call->sent, bytes); else memset(buffer, 'x', bytes);
  call->sent += bytes; *out = bytes; return SALTS_OK;
}
static void upload_perform(void *user) {
  upload_call *call = user; chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  const chttp_client_config config = {uploads.config.http.network, 1u, 256u, 16u, 4096u, 256u * 1024u, 8192u, 4u};
  call->status = chttp_client_init(&client, &config); if (call->status != SALTS_OK) return;
  chttp_header headers[6]; size_t count = 0u;
  if (uploads.multipart) { headers[count++] = (chttp_header){"Content-Type", "multipart/form-data; boundary=worker"}; headers[count++] = (chttp_header){"HX-Request", "true"}; }
  if (call->cookie) headers[count++] = (chttp_header){"Cookie", call->cookie};
  if (call->csrf) headers[count++] = (chttp_header){"X-CSRF-Token", call->csrf};
  if (call->origin) headers[count++] = (chttp_header){"Origin", call->origin};
  if (call->authorization) headers[count++] = (chttp_header){"Authorization", call->authorization};
  const chttp_body_source source = {upload_input, call, call->size, call->known};
  chttp_options options = {.connection_uri = uploads.uri, .authority = "upload.test", .target = call->path ? call->path : "/upload",
      .headers = headers, .header_count = count, .protocol = call->protocol, .timeout_ms = call->timeout_ms ? call->timeout_ms : 5000u};
  if (!call->path) options.body_source = &source;
  call->status = call->path ? chttp_get(&client, &options, &response, &error) : chttp_post(&client, &options, &response, &error);
  call->code = response.status_code;
  if (response.body_size < sizeof(call->output) && response.body_size) memcpy(call->output, response.body, response.body_size);
  const char *cookie = chttp_response_header(&response, "Set-Cookie"); snprintf(call->next_cookie, sizeof(call->next_cookie), "%s", cookie ? cookie : "");
  char *end = strchr(call->next_cookie, ';'); if (end) *end = '\0';
  chttp_response_destroy(&response); const int closed = chttp_client_destroy(&client, 5000u); if (call->status == SALTS_OK) call->status = closed;
}
static void upload_wait_entered(void) {
  const uint64_t deadline = monotonic_ms() + 5000u; sync_mutex_lock(&uploads.mutex);
  while (!uploads.entered && monotonic_ms() < deadline) sync_cond_timedwait(&uploads.changed, &uploads.mutex, UINT64_C(10000000));
  const bool entered = uploads.entered; sync_mutex_unlock(&uploads.mutex); check_true(entered);
}
spec("Application worker uploads") {
  before_each() {
    memset(&uploads, 0, sizeof(uploads)); atomic_init(&uploads.invalid, 0); sync_mutex_init(&uploads.mutex); sync_cond_init(&uploads.changed);
    uploads.peer = UPLOAD_INVALID_SOCKET;
    uploads.provider = (salts_configurator_provider_binding){sizeof(uploads.provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(UploadComponent), NULL, NULL, upload_create, NULL, NULL};
    uploads.deployment = (salts_configurator_deployment){&uploads.provider, NULL, NULL};
    uploads.controller = (chttp_controller){.component_id = "UploadComponent", .receiver = cmeta_reflected_data(UploadBean)};
    uploads.config = chttp_application_config_default(); uploads.config.components = &uploads.deployment; uploads.config.component_count = 1u;
    uploads.config.controllers = &uploads.controller; uploads.config.controller_count = 1u; uploads.config.http.max_request_body_bytes = 256u * 1024u;
    uploads.config.http.session_capacity = 4u; uploads.config.http.session_entry_capacity = 8u;
    uploads.config.http.max_session_key_bytes = 32u; uploads.config.http.max_session_value_bytes = 128u;
    uploads.config.http.session_idle_timeout_ms = 60000u; uploads.config.http.session_cookie_name = "upload_sid";
    uploads.limits = (chttp_application_work_config){1u, 4u, 512u * 1024u, 256u, 2u, 128u};
    uploads.hook = (chttp_invocation_interceptor){"UploadComponent", cmeta_reflected_data(UploadBean), upload_before, upload_after, NULL};
    uploads.binding = (chttp_application_upload_binding){.controller = {.component_id = "UploadComponent", .receiver = cmeta_reflected_data(UploadBean),
        .method = CHTTP_METHOD_POST, .path = "/upload", .abi = FunctionAbi(upload_action), .invoke = upload_action, .after_completion = upload_completed,
        .hooks = &uploads.hook, .hook_count = 1u},
        .open_abi = FunctionAbi(upload_open), .open = upload_open, .close = upload_close, .cleanup = upload_cleanup};
  }
  after_each() {
    if (uploads.peer != UPLOAD_INVALID_SOCKET) { upload_socket_close(uploads.peer); uploads.peer = UPLOAD_INVALID_SOCKET; }
    sync_mutex_lock(&uploads.mutex); uploads.release = true; uploads.release_close = true; uploads.release_session = true;
    sync_cond_broadcast(&uploads.changed); sync_mutex_unlock(&uploads.mutex);
    if (uploads.thread_live) check_equal(sync_thread_join(&uploads.thread), SALTS_OK);
    if (uploads.thread2_live) check_equal(sync_thread_join(&uploads.thread2), SALTS_OK);
    check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK); check_equal(atomic_load(&uploads.invalid), 0); check_equal(uploads.resources, 0);
    chttp_jwt_token_destroy(uploads.token); check_equal(chttp_jwt_bearer_validator_destroy(&uploads.jwt), SALTS_OK);
    sync_cond_destroy(&uploads.changed); sync_mutex_destroy(&uploads.mutex);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    it("settles canceled final business without waiting for an unrelated Session holder (protocol=%d)", protocol) {
      uploads.limits.workers = 2u; uploads.hold_close = true; uploads.config.deadlines.handler_ms = 200u; upload_start();
      upload_call session = {.protocol = (chttp_protocol)protocol, .path = "/session"}; upload_perform(&session);
      check_equal(session.status, SALTS_OK); check_not_equal(session.next_cookie, "");
      uploads.background = (upload_call){.protocol = (chttp_protocol)protocol, .path = "/hold-session", .cookie = session.next_cookie};
      check_equal(sync_thread_create(&uploads.thread, upload_perform, &uploads.background), SALTS_OK); uploads.thread_live = true;
      const uint64_t holder_deadline = monotonic_ms() + 5000u; sync_mutex_lock(&uploads.mutex);
      while (!uploads.session_entered && monotonic_ms() < holder_deadline) sync_cond_timedwait(&uploads.changed, &uploads.mutex, UINT64_C(10000000));
      const bool held = uploads.session_entered; sync_mutex_unlock(&uploads.mutex); check_true(held);
      uploads.background2 = (upload_call){.protocol = (chttp_protocol)protocol, .size = 4096u, .cookie = session.next_cookie};
      check_equal(sync_thread_create(&uploads.thread2, upload_perform, &uploads.background2), SALTS_OK); uploads.thread2_live = true; upload_wait_entered();
      check_equal(sync_thread_join(&uploads.thread2), SALTS_OK); uploads.thread2_live = false;
      check_not_equal(uploads.background2.status, SALTS_OK); check_not_equal(uploads.background2.status, SALTS_ETIMEDOUT);
      const uint64_t cleanup_deadline = monotonic_ms() + 5000u; sync_mutex_lock(&uploads.mutex);
      uploads.release_close = true; sync_cond_broadcast(&uploads.changed);
      while (!uploads.cleanups && monotonic_ms() < cleanup_deadline) sync_cond_timedwait(&uploads.changed, &uploads.mutex, UINT64_C(10000000));
      const int cleaned = uploads.cleanups; uploads.release_session = true; sync_cond_broadcast(&uploads.changed); sync_mutex_unlock(&uploads.mutex);
      check_equal(cleaned, 1); check_equal(sync_thread_join(&uploads.thread), SALTS_OK); uploads.thread_live = false;
      check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK); check_equal(uploads.actions, 0);
    }
    for (int authenticated = 0; authenticated < 2; ++authenticated) {
      it("authenticates JWT before allocating any upload user resource (protocol=%d authenticated=%d)", protocol, authenticated) {
        static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
        const chttp_jwt_bearer_validator_options options = {.size = sizeof(options), .key = key, .key_size = sizeof(key) - 1u};
        check_equal(chttp_jwt_bearer_validator_init(&uploads.jwt, &options), SALTS_OK); uploads.config.jwt = &uploads.jwt;
        char authorization[1024];
        if (authenticated) {
          const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
          check_equal(chttp_jwt_hs256_token_create(&claims, key, sizeof(key) - 1u, &uploads.token), SALTS_OK);
          snprintf(authorization, sizeof(authorization), "Bearer %s", uploads.token);
        }
        upload_start(); upload_call call = {.protocol = (chttp_protocol)protocol, .size = 4096u, .authorization = authenticated ? authorization : NULL}; upload_perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, authenticated ? 201u : 401u);
        check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK); check_equal(uploads.opens, authenticated); check_equal(uploads.cleanups, authenticated);
      }
    }
    it("isolates two upload resources while serializing their final same-Session mutations (protocol=%d)", protocol) {
      chttp_server_execution_options execution = CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
      execution.owner_count = 2u; uploads.config.execution = &execution; uploads.limits.workers = 2u; uploads.hold = true; upload_start();
      upload_call session = {.protocol = (chttp_protocol)protocol, .path = "/session"}; upload_perform(&session);
      check_equal(session.status, SALTS_OK); check_not_equal(session.next_cookie, "");
      uploads.background = (upload_call){.protocol = (chttp_protocol)protocol, .size = 128u * 1024u, .cookie = session.next_cookie};
      uploads.background2 = uploads.background;
      check_equal(sync_thread_create(&uploads.thread, upload_perform, &uploads.background), SALTS_OK); uploads.thread_live = true;
      check_equal(sync_thread_create(&uploads.thread2, upload_perform, &uploads.background2), SALTS_OK); uploads.thread2_live = true;
      const uint64_t deadline = monotonic_ms() + 5000u; sync_mutex_lock(&uploads.mutex);
      while (uploads.writes < 2 && monotonic_ms() < deadline) sync_cond_timedwait(&uploads.changed, &uploads.mutex, UINT64_C(10000000));
      const int resources = uploads.resources; uploads.release = true; sync_cond_broadcast(&uploads.changed); sync_mutex_unlock(&uploads.mutex);
      check_equal(resources, 2);
      check_equal(sync_thread_join(&uploads.thread), SALTS_OK); uploads.thread_live = false;
      check_equal(sync_thread_join(&uploads.thread2), SALTS_OK); uploads.thread2_live = false;
      check_equal(uploads.background.status, SALTS_OK); check_equal(uploads.background.code, 201u);
      check_equal(uploads.background2.status, SALTS_OK); check_equal(uploads.background2.code, 201u);
      check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK); check_equal(uploads.cleanups, 2); check_equal(uploads.actions, 2);
    }
    it("cleans up an upload when CORS bypasses the final Controller (protocol=%d)", protocol) {
      const char *origins[] = {"https://allowed.example"};
      const chttp_server_cors_options cors = {.origins = origins, .origin_count = 1u, .methods = "GET, POST"};
      uploads.config.cors = &cors; upload_start();
      upload_call call = {.protocol = (chttp_protocol)protocol, .size = 4096u, .origin = "https://denied.example"}; upload_perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 403u);
      check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK);
      check_equal(uploads.opens, 1); check_equal(uploads.closes, 1); check_equal(uploads.cleanups, 1); check_equal(uploads.actions, 0);
      check_equal(uploads.outcome.business_status, SALTS_ECANCELED); check_equal(uploads.outcome.transport_status, SALTS_OK);
    }
    it("shares one admission slot between header approval and upload body (protocol=%d)", protocol) {
      uploads.admission = true; uploads.hold_admission = true; uploads.limits.capacity = 1u; upload_start();
      uploads.background = (upload_call){.protocol = (chttp_protocol)protocol, .size = 128u * 1024u};
      check_equal(sync_thread_create(&uploads.thread, upload_perform, &uploads.background), SALTS_OK); uploads.thread_live = true; upload_wait_entered();
      sync_mutex_lock(&uploads.mutex); const int opens = uploads.opens; uploads.release = true; sync_cond_broadcast(&uploads.changed); sync_mutex_unlock(&uploads.mutex);
      check_equal(opens, 0);
      check_equal(sync_thread_join(&uploads.thread), SALTS_OK); uploads.thread_live = false;
      check_equal(uploads.background.status, SALTS_OK); check_equal(uploads.background.code, 201u);
      check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK); check_equal(uploads.admissions, 1); check_equal(uploads.cleanups, 1);
    }
    it("fails late Session admission without entering the final Controller (protocol=%d)", protocol) {
      size_t chunk = uploads.config.http.network.receive_buffer_bytes;
      if (uploads.config.http.h2_input_buffer_bytes > chunk) chunk = uploads.config.http.h2_input_buffer_bytes;
      if (chunk > uploads.config.http.max_request_body_bytes) chunk = uploads.config.http.max_request_body_bytes;
      uploads.limits.retained_bytes = chunk + uploads.limits.reply_bytes +
          (uploads.limits.header_capacity + 1u) * sizeof(chttp_header) + uploads.limits.header_bytes + 512u;
      upload_start(); upload_call call = {.protocol = (chttp_protocol)protocol, .size = 4096u}; upload_perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 503u);
      check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK);
      check_equal(uploads.opens, 1); check_equal(uploads.closes, 1); check_equal(uploads.cleanups, 1); check_equal(uploads.actions, 0);
      check_equal(uploads.outcome.publication_status, SALTS_ENOBUFS);
    }
    it("retains a body view after its observed deadline and accepts a later request (protocol=%d)", protocol) {
      uploads.hold = true; uploads.config.deadlines.body_ms = 100u; upload_start();
      uploads.background = (upload_call){.protocol = (chttp_protocol)protocol, .size = 128u * 1024u, .timeout_ms = 200u};
      check_equal(sync_thread_create(&uploads.thread, upload_perform, &uploads.background), SALTS_OK); uploads.thread_live = true; upload_wait_entered();
      check_equal(sync_thread_join(&uploads.thread), SALTS_OK); uploads.thread_live = false;
      check_not_equal(uploads.background.status, SALTS_OK); check_not_equal(uploads.background.status, SALTS_ETIMEDOUT);
      sync_mutex_lock(&uploads.mutex); const int retained = uploads.resources; uploads.release = true; sync_cond_broadcast(&uploads.changed);
      const uint64_t deadline = monotonic_ms() + 5000u;
      while (!uploads.cleanups && monotonic_ms() < deadline) sync_cond_timedwait(&uploads.changed, &uploads.mutex, UINT64_C(10000000));
      const int cleaned = uploads.cleanups; sync_mutex_unlock(&uploads.mutex); check_equal(cleaned, 1);
      check_equal(retained, 1);
      upload_call next = {.protocol = (chttp_protocol)protocol, .size = 4096u}; upload_perform(&next);
      check_equal(next.status, SALTS_OK); check_equal(next.code, 201u);
      check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK); check_equal(uploads.cleanups, 2); check_equal(uploads.actions, 1);
    }
    for (int known = 0; known < 2; ++known) {
      it("streams upload open/chunks/close/final action/cleanup on one worker (protocol=%d known=%d)", protocol, known) {
        upload_start(); upload_call call = {.protocol = (chttp_protocol)protocol, .size = 128u * 1024u, .known = known != 0}; upload_perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.output, "done"); check_not_equal(call.next_cookie, "");
        check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK);
        check_equal(uploads.received, call.size); check_equal(uploads.opens, 1); check_greater(uploads.writes, 1);
        check_equal(uploads.closes, 1); check_equal(uploads.actions, 1); check_equal(uploads.cleanups, 1); check_equal(uploads.completions, 1);
        check_equal(uploads.outcome.business_status, SALTS_OK); check_equal(uploads.outcome.session_status, SALTS_OK); check_equal(uploads.outcome.transport_status, SALTS_OK);
      }
    }
    for (int failure = 0; failure < 3; ++failure) {
      it("settles upload resources after stage failure (protocol=%d failure=%d)", protocol, failure) {
        uploads.open_status = failure == 0 ? SALTS_EPERM : SALTS_OK; uploads.write_status = failure == 1 ? SALTS_EIO : SALTS_OK; uploads.fail_action = failure == 2;
        upload_start(); upload_call call = {.protocol = (chttp_protocol)protocol, .size = failure ? 4096u : 0u, .known = true}; upload_perform(&call);
        if (failure == 1) { check_not_equal(call.status, SALTS_OK); check_not_equal(call.status, SALTS_ETIMEDOUT); }
        else { check_equal(call.status, SALTS_OK); check_equal(call.code, 500u); }
        check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK);
        check_equal(uploads.opens, 1); check_equal(uploads.cleanups, failure ? 1 : 0); check_equal(uploads.closes, failure ? 1 : 0);
      }
    }
    for (int authorized = 0; authorized < 2; ++authorized) {
      it("runs multipart validation/CSRF/commit or abort on workers (protocol=%d authorized=%d)", protocol, authorized) {
        static const char body[] = "--worker\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\nContent-Type: text/plain\r\n\r\nuploaded\r\n--worker--\r\n";
        uploads.multipart = true; upload_start(); upload_call session = {.protocol = (chttp_protocol)protocol, .path = "/session"}; upload_perform(&session);
        check_equal(session.status, SALTS_OK); check_equal(session.code, 200u); check_not_equal(session.next_cookie, "");
        upload_call call = {.protocol = (chttp_protocol)protocol, .body = body, .size = sizeof(body) - 1u, .input_chunk = 3u,
            .cookie = session.next_cookie, .csrf = authorized ? session.output : "invalid"}; upload_perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, authorized ? 201u : 403u);
        check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK);
        check_equal(uploads.commits, authorized); check_equal(uploads.aborts, authorized ? 0 : 1); check_equal(uploads.cleanups, 1);
        if (authorized) check_equal(uploads.part_bytes, 8u);
      }
    }
    it("retains a blocked body callback and its record through finite close (protocol=%d)", protocol) {
      uploads.hold = true; uploads.limits.capacity = 1u; upload_start();
      uploads.background = (upload_call){.protocol = (chttp_protocol)protocol, .size = 128u * 1024u};
      check_equal(sync_thread_create(&uploads.thread, upload_perform, &uploads.background), SALTS_OK); uploads.thread_live = true; upload_wait_entered();
      upload_call missing = {.protocol = (chttp_protocol)protocol, .path = "/missing"}; upload_perform(&missing);
      check_equal(missing.status, SALTS_OK); check_equal(missing.code, 404u);
      check_equal(chttp_application_close(&uploads.app, 5u), SALTS_ETIMEDOUT); check_not_null(uploads.app.impl); check_equal(uploads.destroyed, 0);
      sync_mutex_lock(&uploads.mutex); uploads.release = true; sync_cond_broadcast(&uploads.changed); sync_mutex_unlock(&uploads.mutex);
      check_equal(sync_thread_join(&uploads.thread), SALTS_OK); uploads.thread_live = false;
      check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK); check_equal(uploads.cleanups, 1); check_equal(uploads.destroyed, 1);
    }
  }
  for (int trailer = 0; trailer < 2; ++trailer) {
  it("preserves H1 close framing and nonempty-trailer rejection after worker pause (trailer=%d)", trailer) {
    uploads.expect_trailer = trailer != 0; upload_start();
    uint16_t port = 0u; check_equal(chttp_application_port(&uploads.app, &port), SALTS_OK);
    uploads.peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); check_not_equal(uploads.peer, UPLOAD_INVALID_SOCKET);
#if defined(_WIN32)
    const DWORD timeout = 5000u;
#else
    const struct timeval timeout = {5, 0};
#endif
    check_equal(setsockopt(uploads.peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)), 0);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    unsigned char *ip = (unsigned char *)&address.sin_addr; ip[0] = 127u; ip[3] = 1u;
    unsigned char *network_port = (unsigned char *)&address.sin_port; network_port[0] = (unsigned char)(port >> 8u); network_port[1] = (unsigned char)port;
    check_equal(connect(uploads.peer, (const struct sockaddr *)&address, sizeof(address)), 0);
    const char *wire = trailer ? "POST /upload HTTP/1.1\r\nHost: upload.test\r\nTransfer-Encoding: chunked\r\nTrailer: X-Upload-Proof\r\nConnection: close\r\n\r\n4\r\nxxxx\r\n0\r\nX-Upload-Proof: final\r\n\r\n" :
        "POST /upload HTTP/1.1\r\nHost: upload.test\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n4\r\nxxxx\r\n0\r\n\r\n";
    const size_t wire_size = strlen(wire);
    size_t sent = 0u;
    while (sent < wire_size) { const int bytes = (int)send(uploads.peer, wire + sent, (int)(wire_size - sent), 0); check_greater(bytes, 0); sent += (size_t)bytes; }
    char response[4096]; size_t used = 0u;
    for (;;) { check_less(used, sizeof(response) - 1u); const int bytes = (int)recv(uploads.peer, response + used, (int)(sizeof(response) - 1u - used), 0); check_greater_equal(bytes, 0); if (!bytes) break; used += (size_t)bytes; }
    response[used] = '\0'; upload_socket_close(uploads.peer); uploads.peer = UPLOAD_INVALID_SOCKET;
    chttp_server_stats stats = {0}; check_equal(chttp_application_stats(&uploads.app, &stats), SALTS_OK);
    check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK);
    info("wire=%zu open=%d write=%d close=%d action=%d clean=%d close_status=%d business=%d publication=%d transport=%d protocol_errors=%llu handler_errors=%llu",
        used, uploads.opens, uploads.writes, uploads.closes, uploads.actions, uploads.cleanups, uploads.close_status,
        uploads.outcome.business_status, uploads.outcome.publication_status, uploads.outcome.transport_status,
        (unsigned long long)stats.protocol_errors, (unsigned long long)stats.handler_errors);
    check_contains(response, trailer ? "HTTP/1.1 400" : "HTTP/1.1 201");
    if (!trailer) check_contains(response, "\r\n\r\ndone");
    check_equal(chttp_application_close(&uploads.app, 5000u), SALTS_OK); check_equal(uploads.received, 4u); check_equal(uploads.cleanups, 1);
  }
  }
  it("validates the body ABI and canonical Bean before publishing a route") {
    check_equal(chttp_application_init(&uploads.app, &uploads.config), SALTS_OK);
    check_equal(chttp_application_worker_prepare(&uploads.app, &uploads.limits), SALTS_OK);
    check_true(cmeta_function_abi_desc_valid(FunctionAbi(upload_open)));
    uploads.binding.open_abi = FunctionAbi(upload_action); check_equal(chttp_application_worker_upload(&uploads.app, &uploads.binding), SALTS_EINVAL);
    uploads.binding.open_abi = FunctionAbi(upload_open);
    uploads.binding.cleanup = NULL; check_equal(chttp_application_worker_upload(&uploads.app, &uploads.binding), SALTS_EINVAL); uploads.binding.cleanup = upload_cleanup;
    uploads.binding.controller.component_id = "missing"; check_equal(chttp_application_worker_upload(&uploads.app, &uploads.binding), SALTS_ENOENT);
    uploads.binding.controller.component_id = "UploadComponent";
    check_equal(chttp_application_worker_upload(&uploads.app, &uploads.binding), SALTS_OK);
    check_equal(chttp_application_worker_upload(&uploads.app, &uploads.binding), SALTS_EALREADY);
    check_equal(chttp_application_start(&uploads.app), SALTS_OK);
    check_equal(chttp_application_worker_upload(&uploads.app, &uploads.binding), SALTS_EBUSY);
  }
}
