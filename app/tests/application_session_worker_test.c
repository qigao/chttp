#include "chttp_application_worker.h"
#include <chttp_app/web.h>
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include <core/clock.h>
#include <threads/threads.h>
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SessionController { int marker; } SessionController;
cmeta_reflect_data(SessionController, "test.WorkerSessionController", cmeta_field(int, marker));
cmeta_component_empty(SessionComponent);
typedef struct session_call {
  const char *target;
  chttp_protocol protocol;
  const char *cookie;
  bool post;
  const char *csrf;
  int status;
  unsigned int code;
  char body[128], next_cookie[128], set_cookie[256];
} session_call;
static struct {
  chttp_application app;
  chttp_application_config config;
  chttp_server_execution_options execution;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t first, second;
  bool first_live, second_live;
  int held, release, calls, increment_calls, completed, completed_large, readonly_violations;
  int destroyed, wrong_thread, sink_entered, release_sink;
  bool regenerate_held;
  bool invalidate_held;
  bool invalidate_before_failure;
  bool occupy_header, override_cookie;
  bool file_for_increment, file_removed;
  bool cancel_sink;
  char *file_path;
  char cookie[128];
  size_t workers, slots, retained, reply_bytes, large_size, received;
  size_t header_capacity, header_bytes;
  unsigned char *large_body;
  uint16_t port;
  chttp_application_work_outcome last;
} fixture;
static session_call first_call, second_call;

static void destroy(void *context, void *object) {
  (void)context; ++fixture.destroyed; free(object);
}
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, destroy};
static cmeta_status create(void *context, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  SessionController *self = malloc(sizeof(*self));
  if (!self) return CMETA_OUT_OF_MEMORY;
  self->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, self, cmeta_reflected_data(SessionController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(self); }
  return status;
}
CHTTP_INVOCATION_ACTION(invoke);
int invoke(void *self, const chttp_invocation *invocation) {
  /* Existing browser helpers consume an explicit detached Session view; this
   * is not the engine's owner-affine request/Session context. */
  chttp_server_request_view view = *chttp_invocation_request(invocation);
  view.session = chttp_invocation_session(invocation);
  const chttp_server_request_view *request = &view;
  if (!self || ((SessionController *)self)->marker != 42 || !request->session) return SALTS_EPROTO;
  const bool hold = strcmp(request->path, "/hold") == 0;
  const bool increment = hold || strcmp(request->path, "/increment") == 0 ||
      strcmp(request->path, "/large") == 0;
  sync_mutex_lock(&fixture.mutex);
  ++fixture.calls;
  if (!chttp_application_worker_current(&fixture.app)) ++fixture.wrong_thread;
  if (strcmp(request->path, "/increment") == 0) ++fixture.increment_calls;
  if (hold) {
    ++fixture.held; sync_cond_broadcast(&fixture.changed);
    while (!fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
  }
  sync_mutex_unlock(&fixture.mutex);
  chttp_session *session = request->session;
  int status = SALTS_OK;
  if (hold && fixture.invalidate_held) {
    status = chttp_session_invalidate(session);
    return status == SALTS_OK ? chttp_invocation_respond(invocation, 200u, "text/plain", "bye", 3u) : status;
  }
  char value[32];
  const char *body = chttp_session_get(session, "n");
  unsigned int code = body ? 200u : 404u;
  if (!body) body = "missing";
  if (strcmp(request->path, "/init") == 0 || strcmp(request->path, "/init-no-type") == 0) {
    status = chttp_session_set(session, "n", "0"); body = "0"; code = 200u;
  } else if (increment) {
    if (hold && fixture.regenerate_held) {
      status = chttp_session_regenerate(session);
      if (status != SALTS_OK) return status;
    }
    const int n = atoi(body);
    const int size = snprintf(value, sizeof(value), "%d", n + 1);
    if (size < 0 || (size_t)size >= sizeof(value)) return SALTS_ERANGE;
    status = chttp_session_set(session, "n", value); body = value; code = 200u;
  } else if (strcmp(request->path, "/failure") == 0) {
    status = chttp_session_set(session, "n", "99");
    if (status == SALTS_OK && fixture.invalidate_before_failure) status = chttp_session_invalidate(session);
    return status == SALTS_OK ? SALTS_EINVAL : status;
  } else if (strcmp(request->path, "/regenerate") == 0) {
    status = chttp_session_regenerate(session);
    if (status == SALTS_OK && chttp_web_csrf_rotate(session, &body, NULL) != CHTTP_WEB_OK)
      return SALTS_EIO;
    code = 200u;
  } else if (strcmp(request->path, "/logout") == 0) {
    status = chttp_session_invalidate(session); body = "bye"; code = 200u;
  } else if (strcmp(request->path, "/csrf") == 0) {
    if (chttp_web_csrf_ensure(session, &body, NULL) != CHTTP_WEB_OK) return SALTS_EIO;
    code = 200u;
  } else if (strcmp(request->path, "/login") == 0) {
    const chttp_web_principal_input principal = {sizeof(principal), "alice", "admin", "Alice"};
    const chttp_web_status web = chttp_web_principal_sign_in(request, NULL, &principal, &body, NULL);
    code = web == CHTTP_WEB_OK ? 200u : 403u;
    if (web != CHTTP_WEB_OK) body = "denied";
  } else if (strcmp(request->path, "/signout") == 0) {
    const chttp_web_status web = chttp_web_principal_sign_out(request, NULL, NULL);
    code = web == CHTTP_WEB_OK ? 200u : 403u; body = web == CHTTP_WEB_OK ? "bye" : "denied";
  } else if (strcmp(request->path, "/principal") == 0) {
    chttp_web_principal principal = CHTTP_WEB_PRINCIPAL_INIT;
    if (chttp_web_principal_get(request, &principal, NULL) != CHTTP_WEB_OK) return SALTS_EIO;
    return chttp_invocation_respond(invocation, principal.authenticated ? 200u : 401u, "text/plain",
        principal.authenticated ? principal.subject.data : "anonymous",
        principal.authenticated ? principal.subject.size : 9u);
  }
  if (status != SALTS_OK) return status;
  if (fixture.occupy_header && strcmp(request->path, "/increment") == 0) {
    status = chttp_invocation_set_header(invocation, "X-Occupy", "reserved");
    if (status != SALTS_OK) return status;
  }
  if (fixture.override_cookie) {
    status = chttp_invocation_set_header(invocation, "Set-Cookie", "sid=forged; Path=/");
    if (status != SALTS_OK) return status;
  }
  if (fixture.file_path && (strcmp(request->path, "/large") == 0 ||
      (fixture.file_for_increment && strcmp(request->path, "/increment") == 0)))
    return chttp_invocation_response_file(invocation, code, "text/plain", fixture.file_path);
  if (strcmp(request->path, "/large") == 0)
    return chttp_invocation_respond(invocation, code, "application/octet-stream",
        fixture.large_body, fixture.large_size);
  const bool no_type = strstr(request->path, "-no-type") != NULL;
  return chttp_invocation_respond(invocation, code, no_type ? NULL : "text/plain", body, strlen(body));
}
static void completed(void *self, const chttp_server_request_view *request,
    const chttp_application_work_outcome *outcome) {
  (void)self;
  const int mutation = chttp_session_set(request->session, "after", "forbidden");
  sync_mutex_lock(&fixture.mutex);
  if (mutation == SALTS_OK) ++fixture.readonly_violations;
  if (!chttp_application_worker_current(&fixture.app)) ++fixture.wrong_thread;
  ++fixture.completed;
  if (strcmp(request->path, "/large") == 0) ++fixture.completed_large;
  fixture.last = *outcome;
  sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
}
static void start(void) {
  check_equal(chttp_application_init(&fixture.app, &fixture.config), SALTS_OK);
  const chttp_application_work_config work = {
      fixture.workers, fixture.slots, fixture.retained, fixture.reply_bytes,
      fixture.header_capacity, fixture.header_bytes};
  check_equal(chttp_application_worker_prepare(&fixture.app, &work), SALTS_OK);
  static const char *const paths[] = {"/init", "/init-no-type", "/read", "/read-no-type",
      "/increment", "/hold", "/failure", "/regenerate", "/logout", "/csrf", "/large", "/principal"};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    const chttp_application_worker_binding binding = {.component_id = "SessionComponent",
        .receiver = cmeta_reflected_data(SessionController), .method = CHTTP_METHOD_GET,
        .path = paths[i], .abi = FunctionAbi(invoke), .invoke = invoke, .after_completion = completed};
    check_equal(chttp_application_worker_mount(&fixture.app, &binding), SALTS_OK);
  }
  static const char *const post_paths[] = {"/login", "/signout"};
  for (size_t i = 0; i < sizeof(post_paths) / sizeof(post_paths[0]); ++i) {
    const chttp_application_worker_binding binding = {.component_id = "SessionComponent",
        .receiver = cmeta_reflected_data(SessionController), .method = CHTTP_METHOD_POST,
        .path = post_paths[i], .abi = FunctionAbi(invoke), .invoke = invoke, .after_completion = completed};
    check_equal(chttp_application_worker_mount(&fixture.app, &binding), SALTS_OK);
  }
  check_equal(chttp_application_start(&fixture.app), SALTS_OK);
  check_equal(chttp_application_port(&fixture.app, &fixture.port), SALTS_OK);
}
static int receive_large(void *user, const void *data, size_t size) {
  (void)user;
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; ++i) if (bytes[i] != 'x') return SALTS_EPROTO;
  sync_mutex_lock(&fixture.mutex);
  fixture.sink_entered = 1; sync_cond_broadcast(&fixture.changed);
  while (!fixture.release_sink) sync_cond_wait(&fixture.changed, &fixture.mutex);
  const bool cancel = fixture.cancel_sink;
  if (!cancel) fixture.received += size;
  sync_mutex_unlock(&fixture.mutex);
  return cancel ? SALTS_ECANCELED : SALTS_OK;
}
static void perform(void *user) {
  session_call *call = user;
  chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  const bool large = strcmp(call->target, "/large") == 0;
  const chttp_client_config config = {.network = fixture.config.http.network,
      .request_capacity = 1u, .max_start_line_bytes = 256u, .max_header_count = 16u,
      .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
      .max_response_body_bytes = large ? fixture.large_size : 8192u, .max_informational_responses = 4u};
  call->status = chttp_client_init(&client, &config);
  if (call->status != SALTS_OK) return;
  char uri[64]; snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)fixture.port);
  chttp_header headers[3]; size_t header_count = 0u;
  if (call->cookie) headers[header_count++] = (chttp_header){"Cookie", call->cookie};
  if (call->post) headers[header_count++] = (chttp_header){"HX-Request", "true"};
  if (call->csrf) headers[header_count++] = (chttp_header){"X-CSRF-Token", call->csrf};
  const chttp_body_sink sink = {receive_large, NULL};
  const chttp_options options = {.connection_uri = uri, .authority = "session.test",
      .target = call->target, .protocol = call->protocol, .headers = headers,
      .header_count = header_count, .body_sink = large ? &sink : NULL,
      .timeout_ms = large ? 20000u : 5000u};
  call->status = call->post ? chttp_post(&client, &options, &response, &error) :
      chttp_get(&client, &options, &response, &error);
  call->code = response.status_code;
  if (response.body_size < sizeof(call->body)) {
    if (response.body_size) memcpy(call->body, response.body, response.body_size);
    call->body[response.body_size] = 0;
  }
  const char *set = chttp_response_header(&response, "Set-Cookie");
  if (set) {
    snprintf(call->set_cookie, sizeof(call->set_cookie), "%s", set);
    const size_t length = strcspn(set, ";");
    if (length < sizeof(call->next_cookie)) { memcpy(call->next_cookie, set, length); call->next_cookie[length] = 0; }
  }
  chttp_response_destroy(&response);
  const int closed = chttp_client_destroy(&client, 5000u);
  if (call->status == SALTS_OK) call->status = closed;
}
static void await_value(int *value, int expected) {
  const uint64_t started = monotonic_ms();
  sync_mutex_lock(&fixture.mutex);
  while (*value < expected && monotonic_ms() - started < 5000u)
    sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(100000000));
  const int observed = *value; sync_mutex_unlock(&fixture.mutex);
  check_equal(observed, expected);
}
static void await_waiter(void) {
  const uint64_t started = monotonic_ms(); chttp_application_work_stats stats = {0};
  int status;
  do {
    status = chttp_application_worker_stats(&fixture.app, &stats);
    if (status != SALTS_OK || stats.session_waiting) break;
    sync_thread_yield();
  } while (monotonic_ms() - started < 5000u);
  check_equal(status, SALTS_OK); check_equal(stats.session_waiting, 1u);
}
static void release(void) {
  sync_mutex_lock(&fixture.mutex); fixture.release = 1; fixture.release_sink = 1;
  sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
}
static void prepare_large(bool file) {
  fixture.workers = 1u; fixture.large_size = 1024u * 1024u;
  fixture.large_body = malloc(fixture.large_size); check_true(fixture.large_body != NULL);
  memset(fixture.large_body, 'x', fixture.large_size);
  if (file) {
    fixture.file_path = tt_make_temp_file("chttp-flow-file-", ".bin"); check_not_null(fixture.file_path);
    check_equal(tt_write_file(fixture.file_path, fixture.large_body, fixture.large_size), 0);
  }
  fixture.reply_bytes = file ? 512u : fixture.large_size + 64u;
  fixture.retained = file ? 32768u : fixture.large_size * 4u;
  fixture.config.http.max_response_body_bytes = fixture.large_size;
  fixture.config.http.max_buffered_response_body_bytes = file ? 512u : fixture.large_size;
  fixture.config.http.network.max_send_bytes = fixture.large_size * 2u;
  fixture.config.http.buffer_capacity_bytes = fixture.large_size * 16u;
}

spec("Application staged worker Sessions") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture)); memset(&first_call, 0, sizeof(first_call));
    memset(&second_call, 0, sizeof(second_call));
    sync_mutex_init(&fixture.mutex); sync_cond_init(&fixture.changed);
    fixture.workers = 2u; fixture.slots = 4u; fixture.retained = 32768u; fixture.reply_bytes = 512u;
    fixture.provider = (salts_configurator_provider_binding){sizeof(fixture.provider),
        SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(SessionComponent),
        NULL, NULL, create, NULL, NULL};
    fixture.deployment = (salts_configurator_deployment){&fixture.provider, NULL, NULL};
    fixture.controller = (chttp_controller){.component_id = "SessionComponent",
        .receiver = cmeta_reflected_data(SessionController)};
    fixture.config = chttp_application_config_default();
    fixture.config.components = &fixture.deployment; fixture.config.component_count = 1u;
    fixture.config.controllers = &fixture.controller; fixture.config.controller_count = 1u;
    fixture.execution = (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    fixture.execution.owner_count = 2u; fixture.config.execution = &fixture.execution;
    fixture.config.http.session_capacity = 4u; fixture.config.http.session_entry_capacity = 8u;
    fixture.config.http.max_session_key_bytes = 32u; fixture.config.http.max_session_value_bytes = 128u;
    fixture.config.http.session_idle_timeout_ms = 60000u;
    fixture.config.http.session_cookie_name = "worker_sid";
    fixture.config.http.session_cookie_secure = 1;
  }
  after_each() {
    release();
    if (fixture.first_live) check_equal(sync_thread_join(&fixture.first), SALTS_OK);
    if (fixture.second_live) check_equal(sync_thread_join(&fixture.second), SALTS_OK);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_equal(fixture.wrong_thread, 0); check_equal(fixture.readonly_violations, 0);
    if (fixture.file_path) {
      if (!fixture.file_removed) check_equal(tt_remove_file(fixture.file_path), 0);
      free(fixture.file_path);
    }
    free(fixture.large_body);
    sync_cond_destroy(&fixture.changed); sync_mutex_destroy(&fixture.mutex);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    for (int cookie_full = 0; cookie_full < 2; ++cookie_full) {
      it("joins native file preparation with Session commit (protocol=%d cookie_full=%d)", protocol, cookie_full) {
        fixture.file_path = tt_make_temp_file("chttp-session-file-", ".txt"); check_not_null(fixture.file_path);
        check_equal(tt_write_file(fixture.file_path, "file", 4u), 0); fixture.file_for_increment = true;
        if (cookie_full) fixture.config.http.max_response_header_count = 1u;
        start(); session_call init = {.target = "/init-no-type", .protocol = (chttp_protocol)protocol}; perform(&init);
        check_equal(init.status, SALTS_OK); await_value(&fixture.completed, 1);
        session_call file = {.target = "/increment", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&file);
        if (cookie_full) { check_not_equal(file.status, SALTS_OK); check_not_equal(file.status, SALTS_ETIMEDOUT); }
        else { check_equal(file.status, SALTS_OK); check_equal(file.body, "file"); }
        await_value(&fixture.completed, 2);
        sync_mutex_lock(&fixture.mutex); const chttp_application_work_outcome outcome = fixture.last; sync_mutex_unlock(&fixture.mutex);
        check_equal(outcome.business_status, SALTS_OK); check_equal(outcome.publication_status, SALTS_OK);
        check_equal(outcome.session_status, cookie_full ? SALTS_ENOBUFS : SALTS_OK);
        session_call read = {.target = "/read-no-type", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&read);
        check_equal(read.status, SALTS_OK); check_equal(read.body, cookie_full ? "0" : "1");
      }
    }
    it("reports missing-file preparation without committing Session data or poisoning the executor (protocol=%d)", protocol) {
      fixture.file_path = tt_make_temp_file("chttp-missing-file-", ".txt"); check_not_null(fixture.file_path);
      check_equal(tt_remove_file(fixture.file_path), 0); fixture.file_removed = true; fixture.file_for_increment = true;
      start(); session_call init = {.target = "/init", .protocol = (chttp_protocol)protocol}; perform(&init);
      check_equal(init.status, SALTS_OK); await_value(&fixture.completed, 1);
      session_call file = {.target = "/increment", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&file);
      check_not_equal(file.status, SALTS_OK); check_not_equal(file.status, SALTS_ETIMEDOUT); await_value(&fixture.completed, 2);
      sync_mutex_lock(&fixture.mutex); const chttp_application_work_outcome outcome = fixture.last; sync_mutex_unlock(&fixture.mutex);
      check_equal(outcome.business_status, SALTS_OK); check_equal(outcome.publication_status, SALTS_EIO);
      check_not_equal(outcome.transport_status, SALTS_OK);
      session_call read = {.target = "/read", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&read);
      check_equal(read.status, SALTS_OK); check_equal(read.body, "0");
    }
    it("does not commit ordinary Session writes when app headers exhaust cookie capacity on protocol %d", protocol) {
      fixture.header_capacity = 1u; fixture.header_bytes = 64u; fixture.occupy_header = true;
      fixture.config.http.max_response_header_count = 2u; start();
      session_call init = {.target = "/init", .protocol = (chttp_protocol)protocol}; perform(&init);
      check_equal(init.status, SALTS_OK); check_not_equal(init.next_cookie, ""); await_value(&fixture.completed, 1);
      session_call failed = {.target = "/increment", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&failed);
      check_not_equal(failed.status, SALTS_OK); check_not_equal(failed.status, SALTS_ETIMEDOUT); await_value(&fixture.completed, 2);
      sync_mutex_lock(&fixture.mutex); const chttp_application_work_outcome outcome = fixture.last; sync_mutex_unlock(&fixture.mutex);
      check_equal(outcome.business_status, SALTS_OK); check_equal(outcome.session_status, SALTS_ENOBUFS);
      session_call read = {.target = "/read", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&read);
      check_equal(read.status, SALTS_OK); check_equal(read.body, "0");
    }
    it("keeps the managed Session cookie authoritative over worker headers on protocol %d", protocol) {
      fixture.header_capacity = 1u; fixture.header_bytes = 128u; fixture.override_cookie = true; start();
      session_call init = {.target = "/init", .protocol = (chttp_protocol)protocol}; perform(&init);
      check_equal(init.status, SALTS_OK); check_not_equal(init.next_cookie, "");
      check_false(strstr(init.set_cookie, "forged") != NULL);
      check_true(strstr(init.set_cookie, "HttpOnly") != NULL); check_true(strstr(init.set_cookie, "SameSite=Lax") != NULL);
      session_call read = {.target = "/read", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&read);
      check_equal(read.status, SALTS_OK); check_equal(read.body, "0");
    }
    it("commits Session data/cookies and discards failed business intents over protocol %d", protocol) {
      start(); session_call init = {.target = "/init", .protocol = (chttp_protocol)protocol}; perform(&init);
      check_equal(init.status, SALTS_OK); check_equal(init.code, 200u); check_equal(init.body, "0");
      check_equal(strstr(init.set_cookie, "HttpOnly") != NULL, true);
      check_equal(strstr(init.set_cookie, "SameSite=Lax") != NULL, true);
      check_equal(strstr(init.set_cookie, "Secure") != NULL, true);
      session_call failed = {.target = "/failure", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&failed);
      check_equal(failed.status, SALTS_OK); check_equal(failed.code, 500u); check_equal(failed.next_cookie, "");
      session_call read = {.target = "/read", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&read);
      check_equal(read.status, SALTS_OK); check_equal(read.code, 200u); check_equal(read.body, "0");
    }
    it("refreshes FIFO same-Session snapshots without occupying the second worker for protocol %d", protocol) {
      start(); session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
      await_value(&fixture.completed, 1);
      snprintf(fixture.cookie, sizeof(fixture.cookie), "%s", init.next_cookie);
      first_call = (session_call){.target = "/hold", .protocol = (chttp_protocol)protocol, .cookie = fixture.cookie};
      check_equal(sync_thread_create(&fixture.first, perform, &first_call), SALTS_OK); fixture.first_live = true;
      await_value(&fixture.held, 1);
      second_call = (session_call){.target = "/increment", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
      check_equal(sync_thread_create(&fixture.second, perform, &second_call), SALTS_OK); fixture.second_live = true;
      await_waiter();
      session_call other = {.target = "/init", .protocol = CHTTP_HTTP_2}; perform(&other);
      check_equal(other.status, SALTS_OK); check_equal(other.code, 200u);
      sync_mutex_lock(&fixture.mutex); const int increments = fixture.increment_calls; sync_mutex_unlock(&fixture.mutex);
      check_equal(increments, 0);
      release(); check_equal(sync_thread_join(&fixture.first), SALTS_OK); fixture.first_live = false;
      check_equal(sync_thread_join(&fixture.second), SALTS_OK); fixture.second_live = false;
      check_equal(first_call.status, SALTS_OK); check_equal(first_call.body, "1");
      check_equal(second_call.status, SALTS_OK); check_equal(second_call.body, "2");
    }
    it("regenerates identity/CSRF and invalidates the old cookie over protocol %d", protocol) {
      start(); session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
      session_call csrf = {.target = "/csrf", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&csrf);
      check_equal(csrf.status, SALTS_OK); check_equal(csrf.code, 200u); check_not_equal(csrf.body, "");
      session_call regen = {.target = "/regenerate", .protocol = (chttp_protocol)protocol, .cookie = init.next_cookie}; perform(&regen);
      check_equal(regen.status, SALTS_OK); check_equal(regen.code, 200u);
      check_not_equal(regen.next_cookie, init.next_cookie); check_not_equal(regen.body, csrf.body);
      session_call old = {.target = "/read", .cookie = init.next_cookie}; perform(&old);
      check_equal(old.status, SALTS_OK); check_equal(old.code, 404u);
      session_call fresh = {.target = "/read", .cookie = regen.next_cookie}; perform(&fresh);
      check_equal(fresh.status, SALTS_OK); check_equal(fresh.body, "0");
      session_call logout = {.target = "/logout", .protocol = (chttp_protocol)protocol, .cookie = regen.next_cookie}; perform(&logout);
      check_equal(logout.status, SALTS_OK); check_equal(strstr(logout.set_cookie, "Max-Age=0") != NULL, true);
      session_call gone = {.target = "/read", .cookie = regen.next_cookie}; perform(&gone);
      check_equal(gone.status, SALTS_OK); check_equal(gone.code, 404u);
    }
  }
  group("browser authentication on the detached worker Session") {
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      it("keeps CSRF/login/logout security transitions over protocol %d", protocol) {
        start(); session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
        session_call csrf = {.target = "/csrf", .cookie = init.next_cookie}; perform(&csrf); check_equal(csrf.status, SALTS_OK);
        session_call denied = {.target = "/login", .protocol = (chttp_protocol)protocol,
            .cookie = init.next_cookie, .post = true, .csrf = "forged"}; perform(&denied);
        check_equal(denied.status, SALTS_OK); check_equal(denied.code, 403u);
        session_call anon = {.target = "/principal", .cookie = init.next_cookie}; perform(&anon);
        check_equal(anon.status, SALTS_OK); check_equal(anon.code, 401u);
        session_call login = {.target = "/login", .protocol = (chttp_protocol)protocol,
            .cookie = init.next_cookie, .post = true, .csrf = csrf.body}; perform(&login);
        check_equal(login.status, SALTS_OK); check_equal(login.code, 200u);
        check_not_equal(login.next_cookie, init.next_cookie); check_not_equal(login.body, csrf.body);
        session_call authenticated = {.target = "/principal", .cookie = login.next_cookie}; perform(&authenticated);
        check_equal(authenticated.status, SALTS_OK); check_equal(authenticated.code, 200u); check_equal(authenticated.body, "alice");
        session_call stale = {.target = "/principal", .cookie = init.next_cookie}; perform(&stale);
        check_equal(stale.status, SALTS_OK); check_equal(stale.code, 401u);
        session_call stale_csrf = {.target = "/signout", .protocol = (chttp_protocol)protocol,
            .cookie = login.next_cookie, .post = true, .csrf = csrf.body}; perform(&stale_csrf);
        check_equal(stale_csrf.status, SALTS_OK); check_equal(stale_csrf.code, 403u);
        session_call logout = {.target = "/signout", .protocol = (chttp_protocol)protocol,
            .cookie = login.next_cookie, .post = true, .csrf = login.body}; perform(&logout);
        check_equal(logout.status, SALTS_OK); check_equal(logout.code, 200u);
        check_equal(strstr(logout.set_cookie, "Max-Age=0") != NULL, true);
        session_call gone = {.target = "/principal", .cookie = login.next_cookie}; perform(&gone);
        check_equal(gone.status, SALTS_OK); check_equal(gone.code, 401u);
      }
      it("fails closed when login principal staging runs out of space on protocol %d", protocol) {
        fixture.config.http.session_entry_capacity = 2u; start();
        session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
        session_call csrf = {.target = "/csrf", .cookie = init.next_cookie}; perform(&csrf); check_equal(csrf.status, SALTS_OK);
        session_call login = {.target = "/login", .protocol = (chttp_protocol)protocol,
            .cookie = init.next_cookie, .post = true, .csrf = csrf.body}; perform(&login);
        check_equal(login.status, SALTS_OK); check_equal(login.code, 403u);
        check_equal(strstr(login.set_cookie, "Max-Age=0") != NULL, true);
        session_call gone = {.target = "/read", .cookie = init.next_cookie}; perform(&gone);
        check_equal(gone.status, SALTS_OK); check_equal(gone.code, 404u);
      }
    }
  }
  it("does not discard explicit invalidation with an otherwise failed business result") {
    fixture.invalidate_before_failure = true; start();
    session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
    session_call failed = {.target = "/failure", .protocol = CHTTP_HTTP_2, .cookie = init.next_cookie}; perform(&failed);
    check_equal(failed.status, SALTS_OK); check_equal(failed.code, 500u);
    check_equal(strstr(failed.set_cookie, "Max-Age=0") != NULL, true);
    session_call gone = {.target = "/read", .cookie = init.next_cookie}; perform(&gone);
    check_equal(gone.status, SALTS_OK); check_equal(gone.code, 404u);
  }
  it("reconciles completed invalidation after transport cancellation without resurrecting authentication") {
    fixture.invalidate_held = true; fixture.config.deadlines.handler_ms = 1000u; start();
    session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
    await_value(&fixture.completed, 1);
    snprintf(fixture.cookie, sizeof(fixture.cookie), "%s", init.next_cookie);
    first_call = (session_call){.target = "/hold", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
    check_equal(sync_thread_create(&fixture.first, perform, &first_call), SALTS_OK); fixture.first_live = true;
    await_value(&fixture.held, 1);
    check_equal(sync_thread_join(&fixture.first), SALTS_OK); fixture.first_live = false;
    check_not_equal(first_call.status, SALTS_OK); check_not_equal(first_call.status, SALTS_ETIMEDOUT);
    release(); await_value(&fixture.completed, 2);
    session_call gone = {.target = "/read", .cookie = fixture.cookie}; perform(&gone);
    check_equal(gone.status, SALTS_OK); check_equal(gone.code, 404u);
  }
  it("charges detached Session storage before accepting user work") {
    fixture.retained = 512u; fixture.reply_bytes = 256u; start();
    session_call init = {.target = "/init", .protocol = CHTTP_HTTP_2}; perform(&init);
    check_equal(init.status, SALTS_OK); check_equal(init.code, 503u); check_equal(fixture.calls, 0);
    chttp_application_work_stats stats;
    check_equal(chttp_application_worker_stats(&fixture.app, &stats), SALTS_OK);
    check_equal(stats.pending, 0u); check_equal(stats.retained_bytes, 0u);
  }
  it("does not publish a new Session when the cookie header cannot be reserved") {
    fixture.config.http.session_capacity = 1u; fixture.config.http.max_response_header_count = 1u; start();
    session_call fail = {.target = "/init", .protocol = CHTTP_HTTP_2}; perform(&fail);
    check_not_equal(fail.status, SALTS_OK); check_not_equal(fail.status, SALTS_ETIMEDOUT);
    session_call success = {.target = "/init-no-type", .protocol = CHTTP_HTTP_2}; perform(&success);
    check_equal(success.status, SALTS_OK); check_equal(success.code, 200u); check_not_equal(success.next_cookie, "");
    session_call read = {.target = "/read-no-type", .protocol = CHTTP_HTTP_2, .cookie = success.next_cookie}; perform(&read);
    check_equal(read.status, SALTS_OK); check_equal(read.body, "0");
  }
  it("still invalidates the original Session when publishing its expired cookie fails") {
    fixture.config.http.session_capacity = 1u; fixture.config.http.max_response_header_count = 1u; start();
    session_call init = {.target = "/init-no-type"}; perform(&init); check_equal(init.status, SALTS_OK);
    session_call logout = {.target = "/logout", .protocol = CHTTP_HTTP_2, .cookie = init.next_cookie}; perform(&logout);
    check_not_equal(logout.status, SALTS_OK); check_not_equal(logout.status, SALTS_ETIMEDOUT);
    session_call gone = {.target = "/read-no-type", .cookie = init.next_cookie}; perform(&gone);
    check_equal(gone.status, SALTS_OK); check_equal(gone.code, 404u);
  }
  it("rejects an old queued generation without invoking it or overwriting the new cookie") {
    fixture.regenerate_held = true; start();
    session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
    await_value(&fixture.completed, 1);
    snprintf(fixture.cookie, sizeof(fixture.cookie), "%s", init.next_cookie);
    first_call = (session_call){.target = "/hold", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
    check_equal(sync_thread_create(&fixture.first, perform, &first_call), SALTS_OK); fixture.first_live = true;
    await_value(&fixture.held, 1);
    second_call = (session_call){.target = "/increment", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
    check_equal(sync_thread_create(&fixture.second, perform, &second_call), SALTS_OK); fixture.second_live = true;
    await_waiter(); release();
    check_equal(sync_thread_join(&fixture.first), SALTS_OK); fixture.first_live = false;
    check_equal(sync_thread_join(&fixture.second), SALTS_OK); fixture.second_live = false;
    check_equal(first_call.status, SALTS_OK); check_not_equal(first_call.next_cookie, fixture.cookie);
    check_equal(second_call.status, SALTS_OK); check_equal(second_call.code, 409u);
    check_equal(second_call.next_cookie, "");
    sync_mutex_lock(&fixture.mutex); const int increments = fixture.increment_calls; sync_mutex_unlock(&fixture.mutex);
    check_equal(increments, 0);
    session_call read = {.target = "/read", .cookie = first_call.next_cookie}; perform(&read);
    check_equal(read.status, SALTS_OK); check_equal(read.body, "1");
  }
  it("settles a canceled Session waiter without running it or committing the held predecessor") {
    fixture.config.deadlines.handler_ms = 1000u; start();
    session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
    await_value(&fixture.completed, 1);
    snprintf(fixture.cookie, sizeof(fixture.cookie), "%s", init.next_cookie);
    first_call = (session_call){.target = "/hold", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
    check_equal(sync_thread_create(&fixture.first, perform, &first_call), SALTS_OK); fixture.first_live = true;
    await_value(&fixture.held, 1);
    second_call = (session_call){.target = "/increment", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
    check_equal(sync_thread_create(&fixture.second, perform, &second_call), SALTS_OK); fixture.second_live = true;
    await_waiter();
    check_equal(sync_thread_join(&fixture.first), SALTS_OK); fixture.first_live = false;
    check_equal(sync_thread_join(&fixture.second), SALTS_OK); fixture.second_live = false;
    check_not_equal(first_call.status, SALTS_OK); check_not_equal(first_call.status, SALTS_ETIMEDOUT);
    check_not_equal(second_call.status, SALTS_OK); check_not_equal(second_call.status, SALTS_ETIMEDOUT);
    await_value(&fixture.completed, 2);
    sync_mutex_lock(&fixture.mutex); const int increments = fixture.increment_calls; sync_mutex_unlock(&fixture.mutex);
    check_equal(increments, 0);
    release(); await_value(&fixture.completed, 3);
    session_call read = {.target = "/read", .cookie = fixture.cookie}; perform(&read);
    check_equal(read.status, SALTS_OK); check_equal(read.body, "0");
    chttp_application_work_stats stats;
    check_equal(chttp_application_worker_stats(&fixture.app, &stats), SALTS_OK);
    check_equal(stats.session_waiting, 0u);
  }
  it("retains the Session store and Bean when finite close outlasts a canceled running method") {
    fixture.config.deadlines.handler_ms = 1000u; start();
    session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
    await_value(&fixture.completed, 1);
    snprintf(fixture.cookie, sizeof(fixture.cookie), "%s", init.next_cookie);
    first_call = (session_call){.target = "/hold", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
    check_equal(sync_thread_create(&fixture.first, perform, &first_call), SALTS_OK); fixture.first_live = true;
    await_value(&fixture.held, 1);
    check_equal(sync_thread_join(&fixture.first), SALTS_OK); fixture.first_live = false;
    check_not_equal(first_call.status, SALTS_OK); check_not_equal(first_call.status, SALTS_ETIMEDOUT);
    check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT);
    check_not_equal(fixture.app.impl, NULL); check_equal(fixture.destroyed, 0);
    chttp_server_stats stopped = {0};
    check_equal(chttp_application_stats(&fixture.app, &stopped), SALTS_OK);
    check_equal(stopped.terminal_status, SALTS_OK);
    release(); check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK);
    check_null(fixture.app.impl); check_equal(fixture.destroyed, 1);
  }
  it("fails store exhaustion without corrupting a previously committed Session") {
    fixture.config.http.session_capacity = 1u; start();
    session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
    session_call exhausted = {.target = "/init", .protocol = CHTTP_HTTP_2}; perform(&exhausted);
    check_not_equal(exhausted.status, SALTS_OK); check_not_equal(exhausted.status, SALTS_ETIMEDOUT);
    session_call read = {.target = "/read", .cookie = init.next_cookie}; perform(&read);
    check_equal(read.status, SALTS_OK); check_equal(read.body, "0");
  }
  it("retains a flow-controlled file through finite close until native file and wire drain") {
    prepare_large(true); start();
    session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
    await_value(&fixture.completed, 1); snprintf(fixture.cookie, sizeof(fixture.cookie), "%s", init.next_cookie);
    first_call = (session_call){.target = "/large", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
    check_equal(sync_thread_create(&fixture.first, perform, &first_call), SALTS_OK); fixture.first_live = true;
    await_value(&fixture.sink_entered, 1);
    chttp_application_work_stats stats = {0};
    check_equal(chttp_application_worker_stats(&fixture.app, &stats), SALTS_OK); check_equal(stats.pending, 1u);
    check_not_equal(stats.retained_bytes, 0u);
    sync_mutex_lock(&fixture.mutex); const int completed = fixture.completed_large; sync_mutex_unlock(&fixture.mutex);
    check_equal(completed, 0);
    check_equal(chttp_application_close(&fixture.app, 5u), SALTS_ETIMEDOUT); check_not_null(fixture.app.impl);
    check_equal(fixture.destroyed, 0);
    check_equal(chttp_application_worker_stats(&fixture.app, &stats), SALTS_OK); check_equal(stats.pending, 1u);
    release(); check_equal(sync_thread_join(&fixture.first), SALTS_OK); fixture.first_live = false;
    check_equal(first_call.status, SALTS_OK); check_equal(fixture.received, fixture.large_size);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_null(fixture.app.impl);
    check_equal(fixture.destroyed, 1); check_equal(fixture.completed_large, 1);
    check_equal(fixture.last.business_status, SALTS_OK); check_equal(fixture.last.publication_status, SALTS_OK);
    check_equal(fixture.last.session_status, SALTS_OK); check_equal(fixture.last.transport_status, SALTS_OK);
  }
  it("joins a canceled file source with its native terminal before Bean destruction") {
    prepare_large(true); fixture.cancel_sink = true; start();
    session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
    await_value(&fixture.completed, 1); snprintf(fixture.cookie, sizeof(fixture.cookie), "%s", init.next_cookie);
    first_call = (session_call){.target = "/large", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
    check_equal(sync_thread_create(&fixture.first, perform, &first_call), SALTS_OK); fixture.first_live = true;
    await_value(&fixture.sink_entered, 1);
    release(); check_equal(sync_thread_join(&fixture.first), SALTS_OK); fixture.first_live = false;
    check_not_equal(first_call.status, SALTS_OK); check_not_equal(first_call.status, SALTS_ETIMEDOUT);
    await_value(&fixture.completed_large, 1);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_null(fixture.app.impl);
    check_equal(fixture.destroyed, 1); check_equal(fixture.last.business_status, SALTS_OK);
    check_equal(fixture.last.publication_status, SALTS_OK); check_equal(fixture.last.session_status, SALTS_OK);
    check_not_equal(fixture.last.transport_status, SALTS_OK);
  }
  it("streams a multichunk H1 file beyond the buffered reply budget") {
    prepare_large(true); fixture.release_sink = 1; start();
    session_call file = {.target = "/large", .protocol = CHTTP_HTTP_1_1}; perform(&file);
    check_equal(file.status, SALTS_OK); check_equal(file.code, 200u); check_equal(fixture.received, fixture.large_size);
    await_value(&fixture.completed_large, 1);
    check_equal(chttp_application_close(&fixture.app, 5000u), SALTS_OK); check_null(fixture.app.impl);
    check_equal(fixture.last.transport_status, SALTS_OK);
  }
  for (int file = 0; file < 2; ++file) {
    it("releases the same-Session gate before H2 DATA flow control completes (file=%d)", file) {
      prepare_large(file != 0);
      start(); session_call init = {.target = "/init"}; perform(&init); check_equal(init.status, SALTS_OK);
      await_value(&fixture.completed, 1);
      snprintf(fixture.cookie, sizeof(fixture.cookie), "%s", init.next_cookie);
      first_call = (session_call){.target = "/large", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie};
      check_equal(sync_thread_create(&fixture.first, perform, &first_call), SALTS_OK); fixture.first_live = true;
      await_value(&fixture.sink_entered, 1);
      session_call next = {.target = "/increment", .protocol = CHTTP_HTTP_2, .cookie = fixture.cookie}; perform(&next);
      check_equal(next.status, SALTS_OK); check_equal(next.code, 200u); check_equal(next.body, "2");
      sync_mutex_lock(&fixture.mutex); const int large_done = fixture.completed_large; sync_mutex_unlock(&fixture.mutex);
      check_equal(large_done, 0);
      release(); check_equal(sync_thread_join(&fixture.first), SALTS_OK); fixture.first_live = false;
      check_equal(first_call.status, SALTS_OK); check_equal(fixture.received, fixture.large_size);
      await_value(&fixture.completed_large, 1);
    }
  }
}
