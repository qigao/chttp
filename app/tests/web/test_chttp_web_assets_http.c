#include "../../src/chttp_web_registration.h"
#include "../../src/chttp_application_worker.h"
#include "../../../http_server/src/chttp_server_engine.h"
#include <chttp_app/web.h>
#include <http_client/http.h>

#include "tinytest.h"

#include <core/error_codes.h>
#include <file/fs.h>
#include <cmeta/data_reflect.h>
#include <core/clock.h>
#include <threads/threads.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ASSETS_HTTP_TIMEOUT_MS = 5000,
  ASSETS_HTTP_BODY_BYTES = 64 * 1024,
  ASSETS_HTTP_PATH_BYTES = 1024
};

static native_io_backend_kind assets_http_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config assets_http_network(size_t connections) {
  return (cnet_client_config){
      .backend = assets_http_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 16u,
      .completion_batch_capacity = 8u,
      .event_capacity = 32u,
      .max_send_bytes = ASSETS_HTTP_BODY_BYTES + 8192u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = ASSETS_HTTP_TIMEOUT_MS,
      .read_timeout_ms = ASSETS_HTTP_TIMEOUT_MS,
      .write_timeout_ms = ASSETS_HTTP_TIMEOUT_MS};
}

static chttp_server_config assets_http_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = assets_http_network(4u),
      .route_capacity = 4u,
      .middleware_capacity = 2u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 128u,
      .max_target_bytes = 4096u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = 4096u,
      .max_response_header_count = 24u,
      .max_response_header_bytes = 8192u,
      .max_response_body_bytes = ASSETS_HTTP_BODY_BYTES,
      .max_buffered_response_body_bytes = 4096u,
      .buffer_capacity_bytes = 2u * 1024u * 1024u,
      .poll_slice_ms = 1u,
      .enable_http2 = 1,
      .h2_stream_capacity = 8u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_output_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static chttp_client_config assets_http_client_config(void) {
  return (chttp_client_config){
      .network = assets_http_network(4u),
      .request_capacity = 4u,
      .max_start_line_bytes = 4096u,
      .max_header_count = 24u,
      .max_header_bytes = 8192u,
      .max_request_body_bytes = 4096u,
      .max_response_body_bytes = ASSETS_HTTP_BODY_BYTES,
      .max_informational_responses = 2u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static int assets_http_write(const char *path, const char *bytes) {
  fs_buf_t buffer = {(char *)bytes, strlen(bytes)};
  return fs_write_file(path, &buffer);
}

static int assets_http_path(
    char output[ASSETS_HTTP_PATH_BYTES],
    const char *root,
    const char *relative) {
  const int written = snprintf(
      output, ASSETS_HTTP_PATH_BYTES, "%s/%s", root, relative);
  return written > 0 && written < ASSETS_HTTP_PATH_BYTES
             ? SALTS_OK
             : SALTS_ENAMETOOLONG;
}

static bool assets_http_immutable(void *user, const char *relative_path) {
  (void)user;
  return relative_path != NULL &&
         strcmp(relative_path, "app.01234567.js") == 0;
}

static const char *assets_http_etag(void *user, const char *relative_path) {
  (void)user;
  return relative_path != NULL &&
                 strcmp(relative_path, "app.01234567.js") == 0
             ? "\"asset-v1\""
             : NULL;
}

static int assets_http_api_ping(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user;
  (void)request;
  return chttp_server_reply(
      response, 200u, "text/plain", "api", sizeof("api") - 1u);
}

static int assets_http_call(
    chttp_client *client,
    const char *uri,
    chttp_method method,
    const char *target,
    const chttp_header *headers,
    size_t header_count,
    chttp_protocol protocol,
    chttp_response *response) {
  chttp_options options = {
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = target,
      .headers = headers,
      .header_count = header_count,
      .timeout_ms = ASSETS_HTTP_TIMEOUT_MS,
      .protocol = protocol};
  chttp_error error = {0};

  switch (method) {
  case CHTTP_METHOD_GET:
    return chttp_get(client, &options, response, &error);
  case CHTTP_METHOD_HEAD:
    return chttp_head(client, &options, response, &error);
  case CHTTP_METHOD_POST:
    return chttp_post(client, &options, response, &error);
  default:
    return SALTS_EINVAL;
  }
}

typedef struct WorkerAssetBean { int marker; char etag[32]; } WorkerAssetBean;
cmeta_reflect_data(WorkerAssetBean, "test.WorkerAssetBean", cmeta_field(int, marker));
cmeta_component_empty(WorkerAssetComponent);
CHTTP_ASSET_POLICY_ACTION(worker_asset_policy);
CHTTP_INVOCATION_ACTION(worker_asset_api);

typedef struct worker_asset_call {
  chttp_protocol protocol;
  chttp_method method;
  const char *target;
  const chttp_header *headers;
  size_t header_count;
  int status;
  unsigned int code;
  size_t bytes;
  char body[128], cache[128], etag[96], type[96], range[96], allow[24], cookie[256];
} worker_asset_call;
static struct {
  chttp_application app;
  chttp_application_config config;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  chttp_web_asset_mount mount;
  chttp_application_asset_binding binding;
  chttp_invocation_interceptor hooks[2];
  chttp_application_work_config limits;
  char *root;
  char *outside;
  chttp_jwt_bearer_validator jwt;
  char *token;
  char uri[64];
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t thread;
  bool thread_live, hold, release, reject, fail, mutate_etag, override_cache, session_track;
  int entered, policies, completions, api_calls, destroyed, invalid, session_reads;
  int trace[256]; size_t trace_count;
  const cmeta_function_desc *operation;
  chttp_application_work_outcome outcome;
  worker_asset_call background;
} wa;
static void worker_asset_destroy(void *user, void *object) { (void)user; ++wa.destroyed; free(object); }
static const cmeta_object_lifecycle worker_asset_lifecycle = {sizeof(worker_asset_lifecycle), NULL, NULL, NULL, worker_asset_destroy};
static cmeta_status worker_asset_create(void *user, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)user; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  WorkerAssetBean *bean = malloc(sizeof(*bean));
  if (!bean) return CMETA_OUT_OF_MEMORY;
  bean->marker = 42; snprintf(bean->etag, sizeof(bean->etag), "\"asset-v1\"");
  cmeta_status status = cmeta_object_borrow(out, bean, cmeta_reflected_data(WorkerAssetBean), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &worker_asset_lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(bean); }
  return status;
}
static void worker_asset_trace(int event) {
  sync_mutex_lock(&wa.mutex);
  if (wa.trace_count < sizeof(wa.trace) / sizeof(wa.trace[0])) wa.trace[wa.trace_count++] = event;
  else ++wa.invalid;
  sync_mutex_unlock(&wa.mutex);
}
static void worker_asset_scope(void *self, const cmeta_function_desc *operation) {
  sync_mutex_lock(&wa.mutex);
  if (!self || ((WorkerAssetBean *)self)->marker != 42 || !chttp_application_worker_current(&wa.app) ||
      !cmeta_function_desc_valid(operation)) ++wa.invalid;
  if (!wa.operation) wa.operation = operation;
  else if (!cmeta_function_desc_equal(wa.operation, operation)) ++wa.invalid;
  sync_mutex_unlock(&wa.mutex);
}
int worker_asset_policy(void *self, const char *relative_path, chttp_application_asset_policy_result *out) {
  WorkerAssetBean *bean = self;
  worker_asset_trace(3);
  sync_mutex_lock(&wa.mutex);
  if (!bean || bean->marker != 42 || !chttp_application_worker_current(&wa.app) || !relative_path ||
      relative_path[0] == '/' || strchr(relative_path, '\\')) ++wa.invalid;
  ++wa.policies;
  if (wa.hold && strcmp(relative_path, "app.01234567.js") == 0) {
    ++wa.entered; sync_cond_broadcast(&wa.changed);
    while (!wa.release) sync_cond_wait(&wa.changed, &wa.mutex);
    if (strcmp(relative_path, "app.01234567.js") != 0) ++wa.invalid;
  }
  sync_mutex_unlock(&wa.mutex);
  if (wa.fail) return SALTS_EPERM;
  *out = (chttp_application_asset_policy_result){strcmp(relative_path, "app.01234567.js") == 0, NULL};
  if (out->immutable) out->etag = bean->etag;
  return SALTS_OK;
}
int worker_asset_api(void *self, const chttp_invocation *invocation) {
  sync_mutex_lock(&wa.mutex);
  if (!self || ((WorkerAssetBean *)self)->marker != 42 || !chttp_application_worker_current(&wa.app)) ++wa.invalid;
  ++wa.api_calls; sync_mutex_unlock(&wa.mutex);
  return chttp_invocation_respond(invocation, 200u, "text/plain", "api", 3u);
}
static cmeta_status worker_asset_before1(void *self, const chttp_invocation *invocation, bool *proceed) {
  (void)proceed; worker_asset_scope(self, chttp_invocation_operation(invocation)); worker_asset_trace(1);
  if (wa.jwt.impl) {
    const chttp_server_request_view *request = chttp_invocation_request(invocation);
    if (!request || !request->jwt_claims || !request->jwt_claims->subject || strcmp(request->jwt_claims->subject, "alice") != 0)
      return CMETA_CALLBACK_ERROR;
  }
  if (wa.session_track) {
    const chttp_server_request_view *request = chttp_invocation_request(invocation);
    chttp_session *session = chttp_invocation_session(invocation);
    if (!session || request->session) return CMETA_CALLBACK_ERROR;
    const char *seen = chttp_session_get(session, "asset_seen");
    if (seen) {
      if (strcmp(seen, "1") != 0) return CMETA_CALLBACK_ERROR;
      sync_mutex_lock(&wa.mutex); ++wa.session_reads; sync_mutex_unlock(&wa.mutex);
    }
    if (chttp_session_set(session, "asset_seen", "1") != SALTS_OK) return CMETA_CALLBACK_ERROR;
  }
  return CMETA_OK;
}
static cmeta_status worker_asset_before2(void *self, const chttp_invocation *invocation, bool *proceed) {
  worker_asset_scope(self, chttp_invocation_operation(invocation)); worker_asset_trace(2);
  return wa.reject && chttp_invocation_reject(invocation, proceed, 403u, "text/plain", "deny", 4u) != SALTS_OK ?
      CMETA_CALLBACK_ERROR : CMETA_OK;
}
static void worker_asset_after1(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  (void)result; worker_asset_scope(self, chttp_invocation_operation(invocation)); worker_asset_trace(11);
  if (wa.mutate_etag) snprintf(((WorkerAssetBean *)self)->etag, sizeof(((WorkerAssetBean *)self)->etag), "\"changed\"");
  if (wa.override_cache && chttp_invocation_set_header(invocation, "Cache-Control", "private, max-age=0") != SALTS_OK) ++wa.invalid;
}
static void worker_asset_after2(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  (void)result; worker_asset_scope(self, chttp_invocation_operation(invocation)); worker_asset_trace(12);
}
static void worker_asset_error1(void *self, const chttp_invocation *invocation, cmeta_status status) {
  (void)status; worker_asset_scope(self, chttp_invocation_operation(invocation)); worker_asset_trace(-1);
}
static void worker_asset_error2(void *self, const chttp_invocation *invocation, cmeta_status status) {
  (void)status; worker_asset_scope(self, chttp_invocation_operation(invocation)); worker_asset_trace(-2);
}
static void worker_asset_completed(void *self, const chttp_server_request_view *request, const chttp_application_work_outcome *outcome) {
  worker_asset_scope(self, request->operation); worker_asset_trace(200);
  sync_mutex_lock(&wa.mutex); ++wa.completions; wa.outcome = *outcome; sync_mutex_unlock(&wa.mutex);
}
static void worker_asset_initialize(void) {
  check_equal(chttp_application_init(&wa.app, &wa.config), SALTS_OK);
  check_equal(chttp_application_worker_prepare(&wa.app, &wa.limits), SALTS_OK);
}
static void worker_asset_listen(void) {
  const char *paths[] = {"/api/ping", "/assets/app.01234567.js", "/app/route"};
  for (size_t i = 0u; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    const chttp_application_worker_binding binding = {.component_id = "WorkerAssetComponent", .receiver = cmeta_reflected_data(WorkerAssetBean),
        .method = CHTTP_METHOD_GET, .path = paths[i], .abi = FunctionAbi(worker_asset_api), .invoke = worker_asset_api};
    check_equal(chttp_application_worker_mount(&wa.app, &binding), SALTS_OK);
  }
  check_equal(chttp_application_start(&wa.app), SALTS_OK);
  uint16_t port = 0u; check_equal(chttp_application_port(&wa.app, &port), SALTS_OK);
  snprintf(wa.uri, sizeof(wa.uri), "tcp://127.0.0.1:%u", (unsigned)port);
}
static void worker_asset_start(void) {
  worker_asset_initialize(); check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_OK); worker_asset_listen();
}
static void worker_asset_call_run(void *user) {
  worker_asset_call *call = user;
  chttp_client client = {0}; chttp_response response = {0};
  const chttp_client_config config = assets_http_client_config();
  call->status = chttp_client_init(&client, &config);
  if (call->status != SALTS_OK) return;
  call->status = assets_http_call(&client, wa.uri, call->method ? call->method : CHTTP_METHOD_GET,
      call->target, call->headers, call->header_count, call->protocol, &response);
  call->code = response.status_code; call->bytes = response.body_size;
  if (response.body_size < sizeof(call->body) && response.body_size) memcpy(call->body, response.body, response.body_size);
#define ASSET_CAPTURE(field_, member_) do { const char *value = chttp_response_header(&response, field_); \
  snprintf(call->member_, sizeof(call->member_), "%s", value ? value : ""); } while (0)
  ASSET_CAPTURE("Cache-Control", cache); ASSET_CAPTURE("ETag", etag); ASSET_CAPTURE("Content-Type", type);
  ASSET_CAPTURE("Content-Range", range); ASSET_CAPTURE("Allow", allow);
  ASSET_CAPTURE("Set-Cookie", cookie);
#undef ASSET_CAPTURE
  chttp_response_destroy(&response);
  const int closed = chttp_client_destroy(&client, ASSETS_HTTP_TIMEOUT_MS);
  if (call->status == SALTS_OK) call->status = closed;
}
static void worker_asset_wait_entered(void) {
  const uint64_t deadline = monotonic_ms() + ASSETS_HTTP_TIMEOUT_MS;
  sync_mutex_lock(&wa.mutex);
  while (!wa.entered && monotonic_ms() < deadline) sync_cond_timedwait(&wa.changed, &wa.mutex, UINT64_C(10000000));
  const int entered = wa.entered; sync_mutex_unlock(&wa.mutex); check_equal(entered, 1);
}

spec("CHttp::App safe asset mount") {
  it("serves bounded assets over H1/H2 without swallowing API or asset misses") {
    static const char css[] = "body{}";
    static const char js[] = "console.log(1);";
    static const char shell[] = "<main>shell</main>";
    static const char secret[] = "outside-secret";
    static const chttp_header range_header[] = {
        {"Range", "bytes=0-3"}};
    static const chttp_header conditional_header[] = {
        {"If-None-Match", "\"asset-v1\""}};

    char *root = tt_make_temp_dir("chttp-web-assets-");
    char *outside = tt_make_temp_dir("chttp-web-assets-outside-");
    char css_dir[ASSETS_HTTP_PATH_BYTES];
    char css_path[ASSETS_HTTP_PATH_BYTES];
    char js_path[ASSETS_HTTP_PATH_BYTES];
    char shell_path[ASSETS_HTTP_PATH_BYTES];
    char outside_path[ASSETS_HTTP_PATH_BYTES];
    char link_path[ASSETS_HTTP_PATH_BYTES];
    chttp_web_asset_mount mount =
        (chttp_web_asset_mount)CHTTP_WEB_ASSET_MOUNT_INIT;
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = assets_http_server_config();
    chttp_client_config client_config = assets_http_client_config();
    chttp_response css_h1 = {0};
    chttp_response css_h2 = {0};
    chttp_response head = {0};
    chttp_response ranged = {0};
    chttp_response immutable = {0};
    chttp_response conditional = {0};
    chttp_response traversal = {0};
    chttp_response encoded_traversal = {0};
    chttp_response encoded_separator = {0};
    chttp_response invalid_utf8 = {0};
    chttp_response missing = {0};
    chttp_response symlink_escape = {0};
    chttp_response shell_route = {0};
    chttp_response api = {0};
    chttp_response api_missing = {0};
    chttp_response wrong_method = {0};
    char uri[64];
    uint16_t port = 0u;
    int symlink_status;

    check_not_null(root);
    check_not_null(outside);
    check_equal(assets_http_path(css_dir, root, "css"), SALTS_OK);
    check_equal(assets_http_path(css_path, root, "css/app.css"), SALTS_OK);
    check_equal(assets_http_path(js_path, root, "app.01234567.js"), SALTS_OK);
    check_equal(assets_http_path(shell_path, root, "index.html"), SALTS_OK);
    check_equal(assets_http_path(outside_path, outside, "secret.txt"), SALTS_OK);
    check_equal(assets_http_path(link_path, root, "escape.txt"), SALTS_OK);
    check_equal(fs_mkdir(css_dir, 0700), SALTS_OK);
    check_equal(assets_http_write(css_path, css), SALTS_OK);
    check_equal(assets_http_write(js_path, js), SALTS_OK);
    check_equal(assets_http_write(shell_path, shell), SALTS_OK);
    check_equal(assets_http_write(outside_path, secret), SALTS_OK);
    symlink_status = fs_symlink(outside_path, link_path, 0);

    mount.url_prefix = "/assets";
    mount.filesystem_root = root;
    mount.max_relative_path_bytes = 256u;
    mount.cache_control = "public, max-age=60";
    mount.immutable_cache_control =
        "public, max-age=31536000, immutable";
    mount.immutable = assets_http_immutable;
    mount.etag = assets_http_etag;
    mount.spa_url_prefix = "/app";
    mount.spa_fallback_relative_path = "index.html";

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_web_assets_use(&server, &mount), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/api/ping", assets_http_api_ping, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(
        uri, sizeof(uri), "tcp://127.0.0.1:%u",
        (unsigned int)port) > 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/css/app.css",
        NULL, 0u, CHTTP_HTTP_1_1, &css_h1), SALTS_OK);
    check_equal(css_h1.status_code, 200u);
    check_equal(css_h1.body, css, sizeof(css) - 1u);
    check_equal(
        chttp_response_header(&css_h1, "Content-Type"),
        "text/css; charset=utf-8",
        strlen("text/css; charset=utf-8"));
    check_equal(
        chttp_response_header(&css_h1, "Cache-Control"),
        "public, max-age=60", strlen("public, max-age=60"));

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/css/app.css",
        NULL, 0u, CHTTP_HTTP_2, &css_h2), SALTS_OK);
    check_equal(css_h2.status_code, 200u);
    check_equal(css_h2.body, css, sizeof(css) - 1u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_HEAD, "/assets/css/app.css",
        NULL, 0u, CHTTP_HTTP_1_1, &head), SALTS_OK);
    check_equal(head.status_code, 200u);
    check_equal(head.body_size, (size_t)0u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/css/app.css",
        range_header, 1u, CHTTP_HTTP_1_1, &ranged), SALTS_OK);
    check_equal(ranged.status_code, 206u);
    check_equal(ranged.body, "body", 4u);
    check_not_null(chttp_response_header(&ranged, "Content-Range"));

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/app.01234567.js",
        NULL, 0u, CHTTP_HTTP_1_1, &immutable), SALTS_OK);
    check_equal(immutable.status_code, 200u);
    check_equal(
        chttp_response_header(&immutable, "ETag"),
        "\"asset-v1\"", strlen("\"asset-v1\""));
    check_equal(
        chttp_response_header(&immutable, "Cache-Control"),
        "public, max-age=31536000, immutable",
        strlen("public, max-age=31536000, immutable"));

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/app.01234567.js",
        conditional_header, 1u, CHTTP_HTTP_1_1, &conditional), SALTS_OK);
    check_equal(conditional.status_code, 304u);
    check_equal(conditional.body_size, (size_t)0u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/../index.html",
        NULL, 0u, CHTTP_HTTP_1_1, &traversal), SALTS_OK);
    check_equal(traversal.status_code, 404u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/%2e%2e/index.html",
        NULL, 0u, CHTTP_HTTP_1_1, &encoded_traversal), SALTS_OK);
    check_equal(encoded_traversal.status_code, 404u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/css%2fapp.css",
        NULL, 0u, CHTTP_HTTP_1_1, &encoded_separator), SALTS_OK);
    check_equal(encoded_separator.status_code, 404u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/%ff.js",
        NULL, 0u, CHTTP_HTTP_1_1, &invalid_utf8), SALTS_OK);
    check_equal(invalid_utf8.status_code, 404u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/missing.js",
        NULL, 0u, CHTTP_HTTP_1_1, &missing), SALTS_OK);
    check_equal(missing.status_code, 404u);

    if (symlink_status == SALTS_OK) {
      check_equal(assets_http_call(
          &client, uri, CHTTP_METHOD_GET, "/assets/escape.txt",
          NULL, 0u, CHTTP_HTTP_1_1, &symlink_escape), SALTS_OK);
      check_equal(symlink_escape.status_code, 404u);
    } else {
      info("symlink creation unavailable at runtime: %d", symlink_status);
    }

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/app/dashboard",
        NULL, 0u, CHTTP_HTTP_2, &shell_route), SALTS_OK);
    check_equal(shell_route.status_code, 200u);
    check_equal(shell_route.body, shell, sizeof(shell) - 1u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/api/ping",
        NULL, 0u, CHTTP_HTTP_1_1, &api), SALTS_OK);
    check_equal(api.status_code, 200u);
    check_equal(api.body, "api", 3u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/api/missing",
        NULL, 0u, CHTTP_HTTP_1_1, &api_missing), SALTS_OK);
    check_equal(api_missing.status_code, 404u);
    check_true(api_missing.body_size != sizeof(shell) - 1u ||
               memcmp(api_missing.body, shell, sizeof(shell) - 1u) != 0);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_POST, "/assets/css/app.css",
        NULL, 0u, CHTTP_HTTP_1_1, &wrong_method), SALTS_OK);
    check_equal(wrong_method.status_code, 405u);
    check_equal(
        chttp_response_header(&wrong_method, "Allow"),
        "GET, HEAD", strlen("GET, HEAD"));

    chttp_response_destroy(&wrong_method);
    chttp_response_destroy(&api_missing);
    chttp_response_destroy(&api);
    chttp_response_destroy(&shell_route);
    chttp_response_destroy(&symlink_escape);
    chttp_response_destroy(&missing);
    chttp_response_destroy(&invalid_utf8);
    chttp_response_destroy(&encoded_separator);
    chttp_response_destroy(&encoded_traversal);
    chttp_response_destroy(&traversal);
    chttp_response_destroy(&conditional);
    chttp_response_destroy(&immutable);
    chttp_response_destroy(&ranged);
    chttp_response_destroy(&head);
    chttp_response_destroy(&css_h2);
    chttp_response_destroy(&css_h1);

    check_equal(
        chttp_client_destroy(&client, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
    check_equal(
        chttp_server_stop(&server, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(tt_remove_tree(root), 0);
    check_equal(tt_remove_tree(outside), 0);
    free(root);
    free(outside);
  }
}

spec("Application worker assets") {
  before_each() {
    memset(&wa, 0, sizeof(wa)); sync_mutex_init(&wa.mutex); sync_cond_init(&wa.changed);
    wa.root = tt_make_temp_dir("chttp-worker-assets-"); check_not_null(wa.root);
    char path[ASSETS_HTTP_PATH_BYTES];
    check_equal(assets_http_path(path, wa.root, "css"), SALTS_OK); check_equal(fs_mkdir(path, 0700), SALTS_OK);
    check_equal(assets_http_path(path, wa.root, "css/app.css"), SALTS_OK); check_equal(assets_http_write(path, "body{}"), SALTS_OK);
    check_equal(assets_http_path(path, wa.root, "app.01234567.js"), SALTS_OK); check_equal(assets_http_write(path, "console.log(1);"), SALTS_OK);
    check_equal(assets_http_path(path, wa.root, "index.html"), SALTS_OK); check_equal(assets_http_write(path, "<main>shell</main>"), SALTS_OK);
    wa.provider = (salts_configurator_provider_binding){sizeof(wa.provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(WorkerAssetComponent), NULL, NULL, worker_asset_create, NULL, NULL};
    wa.deployment = (salts_configurator_deployment){&wa.provider, NULL, NULL};
    wa.controller = (chttp_controller){.component_id = "WorkerAssetComponent", .receiver = cmeta_reflected_data(WorkerAssetBean)};
    wa.config = chttp_application_config_default(); wa.config.components = &wa.deployment; wa.config.component_count = 1u;
    wa.config.controllers = &wa.controller; wa.config.controller_count = 1u;
    wa.limits = (chttp_application_work_config){1u, 4u, 32768u, 512u, 6u, 512u};
    wa.mount = (chttp_web_asset_mount)CHTTP_WEB_ASSET_MOUNT_INIT;
    wa.mount.url_prefix = "/assets"; wa.mount.filesystem_root = wa.root; wa.mount.max_relative_path_bytes = 256u;
    wa.mount.cache_control = "public, max-age=60"; wa.mount.immutable_cache_control = "public, max-age=31536000, immutable";
    wa.mount.spa_url_prefix = "/app"; wa.mount.spa_fallback_relative_path = "index.html";
    wa.hooks[0] = (chttp_invocation_interceptor){"WorkerAssetComponent", cmeta_reflected_data(WorkerAssetBean),
        worker_asset_before1, worker_asset_after1, worker_asset_error1};
    wa.hooks[1] = (chttp_invocation_interceptor){"WorkerAssetComponent", cmeta_reflected_data(WorkerAssetBean),
        worker_asset_before2, worker_asset_after2, worker_asset_error2};
    wa.binding = (chttp_application_asset_binding){&wa.mount, "WorkerAssetComponent", cmeta_reflected_data(WorkerAssetBean),
        FunctionAbi(worker_asset_policy), worker_asset_policy, worker_asset_completed, wa.hooks, 2u};
  }
  after_each() {
    sync_mutex_lock(&wa.mutex); wa.release = true; sync_cond_broadcast(&wa.changed); sync_mutex_unlock(&wa.mutex);
    if (wa.thread_live) check_equal(sync_thread_join(&wa.thread), SALTS_OK);
    check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK); check_equal(wa.invalid, 0);
    chttp_jwt_token_destroy(wa.token); check_equal(chttp_jwt_bearer_validator_destroy(&wa.jwt), SALTS_OK);
    if (wa.root) { check_equal(tt_remove_tree(wa.root), 0); free(wa.root); }
    if (wa.outside) { check_equal(tt_remove_tree(wa.outside), 0); free(wa.outside); }
    sync_cond_destroy(&wa.changed); sync_mutex_destroy(&wa.mutex);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    it("uses detached Session state around worker assets and cached responses (protocol=%d)", protocol) {
      wa.session_track = true;
      wa.config.http.session_capacity = 4u; wa.config.http.session_entry_capacity = 4u;
      wa.config.http.max_session_key_bytes = 32u; wa.config.http.max_session_value_bytes = 128u;
      wa.config.http.session_idle_timeout_ms = 60000u; wa.config.http.session_cookie_name = "asset_sid";
      worker_asset_start();
      worker_asset_call first = {.protocol = (chttp_protocol)protocol, .target = "/assets/css/app.css"}; worker_asset_call_run(&first);
      check_equal(first.status, SALTS_OK); check_equal(first.code, 200u); check_not_equal(first.cookie, "");
      char *end = strchr(first.cookie, ';'); check_not_null(end); *end = '\0';
      const chttp_header headers[] = {{"Cookie", first.cookie}, {"If-None-Match", "\"asset-v1\""}};
      worker_asset_call cached = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js", .headers = headers, .header_count = 2u};
      worker_asset_call_run(&cached); check_equal(cached.status, SALTS_OK); check_equal(cached.code, 304u);
      check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
      check_equal(wa.session_reads, 1); check_equal(wa.completions, 2);
      check_equal(wa.outcome.session_status, SALTS_OK);
    }
    for (int authenticated = 0; authenticated < 2; ++authenticated) {
      it("validates global JWT before worker asset policy (protocol=%d authenticated=%d)", protocol, authenticated) {
        static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
        const chttp_jwt_bearer_validator_options options = {.size = sizeof(options), .key = key, .key_size = sizeof(key) - 1u};
        check_equal(chttp_jwt_bearer_validator_init(&wa.jwt, &options), SALTS_OK); wa.config.jwt = &wa.jwt;
        chttp_header authorization = {"Authorization", NULL}; char value[1024];
        if (authenticated) {
          const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
          check_equal(chttp_jwt_hs256_token_create(&claims, key, sizeof(key) - 1u, &wa.token), SALTS_OK);
          snprintf(value, sizeof(value), "Bearer %s", wa.token); authorization.value = value;
        }
        worker_asset_start();
        worker_asset_call call = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js",
            .headers = &authorization, .header_count = authenticated ? 1u : 0u}; worker_asset_call_run(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, authenticated ? 200u : 401u);
        check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
        check_equal(wa.policies, authenticated); check_equal(wa.completions, authenticated);
        if (!authenticated) check_equal(wa.trace_count, 0u);
      }
    }
    it("isolates overlapping resource selections across workers and HTTP owners (protocol=%d)", protocol) {
      chttp_server_execution_options execution = CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
      execution.owner_count = 2u; wa.config.execution = &execution; wa.limits.workers = 2u; wa.hold = true; worker_asset_start();
      wa.background = (worker_asset_call){.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"};
      check_equal(sync_thread_create(&wa.thread, worker_asset_call_run, &wa.background), SALTS_OK); wa.thread_live = true; worker_asset_wait_entered();
      worker_asset_call css = {.protocol = (chttp_protocol)protocol, .target = "/assets/css/app.css"}; worker_asset_call_run(&css);
      check_equal(css.status, SALTS_OK); check_equal(css.code, 200u); check_equal(css.body, "body{}");
      sync_mutex_lock(&wa.mutex); wa.release = true; sync_cond_broadcast(&wa.changed); sync_mutex_unlock(&wa.mutex);
      check_equal(sync_thread_join(&wa.thread), SALTS_OK); wa.thread_live = false;
      check_equal(wa.background.status, SALTS_OK); check_equal(wa.background.body, "console.log(1);");
      check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
      check_equal(wa.policies, 2); check_equal(wa.completions, 2);
    }
    it("serves CSS, immutable, ranged, HEAD, cached and SPA responses on workers (protocol=%d)", protocol) {
      worker_asset_start();
      worker_asset_call css = {.protocol = (chttp_protocol)protocol, .target = "/assets/css/app.css"}; worker_asset_call_run(&css);
      check_equal(css.status, SALTS_OK); check_equal(css.code, 200u); check_equal(css.body, "body{}");
      check_equal(css.cache, "public, max-age=60"); check_equal(css.type, "text/css; charset=utf-8");
      worker_asset_call js = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"}; worker_asset_call_run(&js);
      check_equal(js.status, SALTS_OK); check_equal(js.code, 200u); check_equal(js.body, "console.log(1);");
      check_equal(js.cache, "public, max-age=31536000, immutable"); check_equal(js.etag, "\"asset-v1\"");
      const chttp_header range = {"Range", "bytes=0-3"};
      worker_asset_call ranged = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js", .headers = &range, .header_count = 1u};
      worker_asset_call_run(&ranged); check_equal(ranged.status, SALTS_OK); check_equal(ranged.code, 206u);
      check_equal(ranged.body, "cons"); check_equal(ranged.range, "bytes 0-3/15");
      worker_asset_call head = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js", .method = CHTTP_METHOD_HEAD};
      worker_asset_call_run(&head); check_equal(head.status, SALTS_OK); check_equal(head.code, 200u); check_equal(head.bytes, 0u);
      const chttp_header condition = {"If-None-Match", "\"asset-v1\""};
      worker_asset_call cached = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js", .headers = &condition, .header_count = 1u};
      worker_asset_call_run(&cached); check_equal(cached.status, SALTS_OK); check_equal(cached.code, 304u); check_equal(cached.bytes, 0u);
      worker_asset_call spa = {.protocol = (chttp_protocol)protocol, .target = "/app/route"}; worker_asset_call_run(&spa);
      check_equal(spa.status, SALTS_OK); check_equal(spa.code, 200u); check_equal(spa.body, "<main>shell</main>");
      worker_asset_call api = {.protocol = (chttp_protocol)protocol, .target = "/api/ping"}; worker_asset_call_run(&api);
      check_equal(api.status, SALTS_OK); check_equal(api.code, 200u); check_equal(api.body, "api");
      check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
      check_equal(wa.policies, 6); check_equal(wa.completions, 6); check_equal(wa.api_calls, 1); check_equal(wa.destroyed, 1);
    }
    it("rejects unsafe paths and preserves reserved namespace boundaries (protocol=%d)", protocol) {
      worker_asset_start();
      const char *targets[] = {"/assets/../index.html", "/assets/%2e%2e/index.html", "/assets/css%2fapp.css",
          "/assets/%ff.js", "/assets/%00.js", "/assets/css%5capp.css", "/assets/file:stream", "/assets/missing.js",
          "/assets", "/assetsfoo/index.html", "/appfoo/route", "/api/missing"};
      for (size_t i = 0u; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        worker_asset_call call = {.protocol = (chttp_protocol)protocol, .target = targets[i]}; worker_asset_call_run(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 404u); check_not_equal(call.body, "<main>shell</main>");
      }
      worker_asset_call valid = {.protocol = (chttp_protocol)protocol, .target = "/assets//css/./app.css"}; worker_asset_call_run(&valid);
      check_equal(valid.status, SALTS_OK); check_equal(valid.code, 200u); check_equal(valid.body, "body{}");
      worker_asset_call method = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js", .method = CHTTP_METHOD_POST};
      worker_asset_call_run(&method); check_equal(method.status, SALTS_OK); check_equal(method.code, 405u); check_equal(method.allow, "GET, HEAD");
      check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
      check_equal(wa.policies, 1); check_equal(wa.api_calls, 0);
    }
    for (int rejected = 0; rejected < 2; ++rejected) {
      it("unwinds worker hooks around rejection or policy failure (protocol=%d rejected=%d)", protocol, rejected) {
        wa.reject = rejected != 0; wa.fail = !rejected; worker_asset_start();
        worker_asset_call call = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"}; worker_asset_call_run(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, rejected ? 403u : 500u);
        check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
        check_equal(wa.policies, rejected ? 0 : 1); check_equal(wa.completions, 1);
        const int failure[] = {1,2,3,-2,-1,200}, rejection[] = {1,2,-2,-1,200};
        check_equal(wa.trace_count, rejected ? 5u : 6u);
        check_equal(wa.trace, rejected ? rejection : failure, rejected ? sizeof(rejection) : sizeof(failure));
        check_equal(wa.outcome.business_status, rejected ? SALTS_OK : SALTS_EPERM);
      }
    }
    it("copies borrowed policy output before after hooks and preserves cache overrides (protocol=%d)", protocol) {
      wa.mutate_etag = true; wa.override_cache = true; worker_asset_start();
      worker_asset_call call = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"}; worker_asset_call_run(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 200u);
      check_equal(call.etag, "\"asset-v1\""); check_equal(call.cache, "private, max-age=0");
      check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
      const int expected[] = {1,2,3,12,11,200}; check_equal(wa.trace_count, 6u); check_equal(wa.trace, expected, sizeof(expected));
    }
    it("keeps native progress live and rejects excess Invocation slots before hooks (protocol=%d)", protocol) {
      wa.hold = true; wa.limits.capacity = 1u; worker_asset_start();
      wa.background = (worker_asset_call){.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"};
      check_equal(sync_thread_create(&wa.thread, worker_asset_call_run, &wa.background), SALTS_OK); wa.thread_live = true; worker_asset_wait_entered();
      worker_asset_call rejected = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"}; worker_asset_call_run(&rejected);
      check_equal(rejected.status, SALTS_OK); check_equal(rejected.code, 503u);
      worker_asset_call missing = {.protocol = (chttp_protocol)protocol, .target = "/api/missing"}; worker_asset_call_run(&missing);
      check_equal(missing.status, SALTS_OK); check_equal(missing.code, 404u);
      sync_mutex_lock(&wa.mutex); const int policies = wa.policies; wa.release = true; sync_cond_broadcast(&wa.changed); sync_mutex_unlock(&wa.mutex);
      check_equal(policies, 1); check_equal(sync_thread_join(&wa.thread), SALTS_OK); wa.thread_live = false;
      check_equal(wa.background.status, SALTS_OK); check_equal(wa.background.code, 200u);
      check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK); check_equal(wa.completions, 1);
    }
    it("rejects retained-byte exhaustion before entering the asset chain (protocol=%d)", protocol) {
      wa.limits.retained_bytes = wa.limits.reply_bytes + wa.limits.header_bytes + (wa.limits.header_capacity + 1u) * sizeof(chttp_header);
      worker_asset_start();
      worker_asset_call call = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"}; worker_asset_call_run(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 503u); check_equal(wa.trace_count, 0u);
      check_equal(wa.policies, 0); check_equal(wa.completions, 0);
    }
    it("retains the asset context and Bean after a running policy deadline (protocol=%d)", protocol) {
      wa.hold = true; wa.config.deadlines.handler_ms = 500u; worker_asset_start();
      wa.background = (worker_asset_call){.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"};
      check_equal(sync_thread_create(&wa.thread, worker_asset_call_run, &wa.background), SALTS_OK); wa.thread_live = true; worker_asset_wait_entered();
      check_equal(sync_thread_join(&wa.thread), SALTS_OK); wa.thread_live = false;
      check_not_equal(wa.background.status, SALTS_OK); check_not_equal(wa.background.status, SALTS_ETIMEDOUT);
      check_equal(chttp_application_close(&wa.app, 5u), SALTS_ETIMEDOUT); check_not_null(wa.app.impl); check_equal(wa.destroyed, 0);
      sync_mutex_lock(&wa.mutex); wa.release = true; sync_cond_broadcast(&wa.changed); sync_mutex_unlock(&wa.mutex);
      check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
      check_equal(wa.policies, 1); check_equal(wa.completions, 1); check_equal(wa.destroyed, 1);
      check_not_equal(wa.outcome.transport_status, SALTS_OK);
    }
    it("fails bounded metadata publication without a partial asset response (protocol=%d)", protocol) {
      wa.limits.header_capacity = 3u; worker_asset_start();
      worker_asset_call call = {.protocol = (chttp_protocol)protocol, .target = "/assets/app.01234567.js"}; worker_asset_call_run(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 500u); check_equal(call.etag, ""); check_equal(call.cache, "");
      check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
      check_equal(wa.outcome.business_status, SALTS_ENOBUFS);
    }
  }
#if !defined(_WIN32)
  it("rejects a symlink escaping the asset root before invoking worker policy") {
    wa.outside = tt_make_temp_dir("chttp-worker-assets-outside-"); check_not_null(wa.outside);
    char secret[ASSETS_HTTP_PATH_BYTES], link[ASSETS_HTTP_PATH_BYTES];
    check_equal(assets_http_path(secret, wa.outside, "secret.txt"), SALTS_OK);
    check_equal(assets_http_path(link, wa.root, "escape.txt"), SALTS_OK);
    check_equal(assets_http_write(secret, "private-secret"), SALTS_OK); check_equal(fs_symlink(secret, link, 0), SALTS_OK);
    worker_asset_start(); worker_asset_call call = {.target = "/assets/escape.txt"}; worker_asset_call_run(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 404u); check_not_equal(call.body, "private-secret");
    check_equal(chttp_application_close(&wa.app, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK); check_equal(wa.policies, 0);
  }
#endif
  it("validates canonical policy ABI/hooks and snapshots the mount before publication") {
    worker_asset_initialize();
    check_true(cmeta_type_desc_valid(&chttp_asset_relative_pointer));
    check_true(cmeta_function_abi_desc_valid(FunctionAbi(worker_asset_policy)));
    const cmeta_function_abi_desc *abi = wa.binding.policy_abi;
    wa.mount.size = 0u; check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EINVAL); wa.mount.size = sizeof(wa.mount);
    wa.mount.max_relative_path_bytes = 0u; check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EINVAL); wa.mount.max_relative_path_bytes = 256u;
    wa.binding.policy_abi = FunctionAbi(worker_asset_api); check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EINVAL);
    wa.binding.policy_abi = NULL; check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EINVAL); wa.binding.policy_abi = abi;
    wa.mount.user = &wa; check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EINVAL); wa.mount.user = NULL;
    wa.mount.immutable = assets_http_immutable; check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EINVAL); wa.mount.immutable = NULL;
    wa.mount.etag = assets_http_etag; check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EINVAL); wa.mount.etag = NULL;
    wa.binding.component_id = "missing"; check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_ENOENT);
    wa.binding.component_id = "WorkerAssetComponent";
    const chttp_invocation_interceptor invalid = {0}; wa.binding.hooks = &invalid; wa.binding.hook_count = 1u;
    check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EINVAL);
    wa.binding.hooks = wa.hooks; wa.binding.hook_count = 2u;
    check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_OK);
    check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EALREADY);
    memset(&wa.mount, 0, sizeof(wa.mount)); memset(wa.hooks, 0, sizeof(wa.hooks)); worker_asset_listen();
    check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_EBUSY);
    worker_asset_call call = {.target = "/assets/app.01234567.js"}; worker_asset_call_run(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 200u); check_equal(call.etag, "\"asset-v1\"");
  }
  it("serves built-in assets without a callback receiver or legacy raw context") {
    wa.binding = (chttp_application_asset_binding){.mount = &wa.mount}; worker_asset_start();
    worker_asset_call call = {.target = "/assets/app.01234567.js"}; worker_asset_call_run(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 200u); check_equal(call.cache, "public, max-age=60");
    check_equal(wa.policies, 0); check_equal(wa.completions, 0);
  }
  it("leaves registration transactional when the native middleware capacity is full") {
    wa.config.http.middleware_capacity = 1u; worker_asset_initialize();
    check_equal(chttp_application_worker_assets(&wa.app, &wa.binding), SALTS_ENOBUFS); worker_asset_listen();
    worker_asset_call call = {.target = "/assets/app.01234567.js"}; worker_asset_call_run(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 200u); check_equal(call.body, "api");
    check_equal(wa.policies, 0); check_equal(wa.completions, 0);
  }
}
