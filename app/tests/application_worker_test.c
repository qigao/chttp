#include "chttp_application_work.h"
#include "../../http_server/src/chttp_server_engine.h"
#include <chttp_app/application.h>
#include <http_client/http.h>
#include <threads/threads.h>
#include <core/clock.h>
#include "tinytest.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct {
  chttp_application_work work;
  chttp_server server;
  chttp_server_config http;
  sync_mutex_t mutex;
  sync_cond_t changed;
  thrd_t client_thread, second_thread;
  int second_started;
  size_t workers, owners;
  const void *worker_tokens[2];
  int thread_started, held, release, entered, finalized, calls;
  int snapshot_ok, fail, no_reply, oversized, close_from_worker;
  int same_executor_close, draft_status, empty;
  uint32_t deadline_ms;
  int with_hooks, reject_hook, hook_ids[2], trace[16], trace_count, wrong_hook_thread;
  int completion_count, hold_completion, release_completion, wrong_completion_thread;
  int sink_entered, release_sink, completed_large, completed_small, large_marker, cancelled_before_run;
  size_t large_size, received;
  unsigned char *large_body;
  chttp_application_work_outcome outcome;
  const void *owner_token, *worker_token;
  uint16_t port;
} fixture;
typedef struct call_result { chttp_protocol protocol; int large; int status; unsigned int code; char body[128]; } call_result;
static call_result background, second_background;

static void trace_hook(int event) {
  sync_mutex_lock(&fixture.mutex);
  if (fixture.trace_count < 16) fixture.trace[fixture.trace_count++] = event;
  if (sync_thread_current_token() == fixture.owner_token) fixture.wrong_hook_thread = 1;
  sync_mutex_unlock(&fixture.mutex);
}
static cmeta_status before_hook(void *user, const chttp_application_work_exchange *exchange,
    bool *proceed) {
  const int id = *(int *)user;
  trace_hook(id);
  if (fixture.reject_hook == id) {
    *proceed = false;
    return chttp_application_work_reject(exchange, 403u, "text/plain", "denied", 6u) == SALTS_OK ?
        CMETA_OK : CMETA_CALLBACK_ERROR;
  }
  return CMETA_OK;
}
static void after_hook(void *user, const chttp_application_work_exchange *exchange,
    const chttp_application_work_result *result) {
  (void)exchange; (void)result; trace_hook(10 + *(int *)user);
}
static void error_hook(void *user, const chttp_application_work_exchange *exchange, cmeta_status error) {
  (void)exchange; (void)error; trace_hook(100 + *(int *)user);
}
static int business(void *user, const chttp_server_request_view *request,
    chttp_application_work_reply *reply) {
  (void)user;
  if (fixture.with_hooks) trace_hook(3);
  sync_mutex_lock(&fixture.mutex);
  fixture.worker_token = sync_thread_current_token();
  ++fixture.calls; fixture.entered = 1;
  if (fixture.calls <= 2) fixture.worker_tokens[fixture.calls - 1] = fixture.worker_token;
  sync_cond_broadcast(&fixture.changed);
  while (fixture.held && !fixture.release) sync_cond_wait(&fixture.changed, &fixture.mutex);
  fixture.snapshot_ok = request->target && strcmp(request->target, "/snapshot?q=one") == 0 &&
      request->path && strcmp(request->path, "/snapshot") == 0 &&
      request->body_size == 3u && memcmp(request->body, "abc", 3u) == 0 &&
      request->header_count == 1u && strcmp(request->headers[0].name, "X-Input") == 0 &&
      strcmp(request->headers[0].value, "original") == 0 &&
      request->param_count == 1u && strcmp(request->params[0].name, "id") == 0 &&
      strcmp(request->params[0].value, "42") == 0 && request->peer && request->peer->port == 1234u &&
      request->jwt_claims && strcmp(request->jwt_claims->subject, "alice") == 0 &&
      request->jwt_claims->issuer == NULL && request->jwt_claims->audience_count == 1u &&
      strcmp(request->jwt_claims->audiences[0], "service") == 0 &&
      request->jwt_claims->expires_at && *request->jwt_claims->expires_at == 123456 &&
      request->jwt_claims->issued_at == NULL;
  sync_mutex_unlock(&fixture.mutex);
  if (fixture.close_from_worker)
    fixture.same_executor_close = chttp_application_work_close(&fixture.work, 1u);
  if (user == &fixture.large_marker)
    return chttp_application_work_respond(reply, 200u, "application/octet-stream",
        fixture.large_body, fixture.large_size);
  if (fixture.empty) return chttp_application_work_respond(reply, 204u, "text/plain", NULL, 0u);
  if (fixture.fail) return SALTS_EINVAL;
  if (fixture.no_reply) return SALTS_OK;
  if (fixture.oversized) {
    fixture.draft_status = chttp_application_work_respond(reply, 200u, "text/plain", "x", SIZE_MAX);
    return fixture.draft_status;
  }
  char bytes[] = "worker";
  const int status = chttp_application_work_respond(reply, 201u, "text/plain", bytes, 6u);
  memset(bytes, '!', 6u);
  const int duplicate = chttp_application_work_respond(reply, 202u, "text/plain", "bad", 3u);
  sync_mutex_lock(&fixture.mutex); fixture.draft_status = duplicate; sync_mutex_unlock(&fixture.mutex);
  return status;
}
static void finalized(void *user) {
  (void)user;
  sync_mutex_lock(&fixture.mutex); ++fixture.finalized;
  sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
}
static void completed(void *user, const chttp_server_request_view *request,
    const chttp_application_work_outcome *outcome) {
  sync_mutex_lock(&fixture.mutex);
  if (sync_thread_current_token() == fixture.owner_token) fixture.wrong_completion_thread = 1;
  if (!request->body || request->body_size != 3u || memcmp(request->body, "abc", 3u) != 0)
    fixture.snapshot_ok = 0;
  fixture.outcome = *outcome; ++fixture.completion_count;
  if (outcome->business_status == SALTS_ECANCELED) ++fixture.cancelled_before_run;
  if (user == &fixture.large_marker) ++fixture.completed_large;
  else ++fixture.completed_small;
  sync_cond_broadcast(&fixture.changed);
  while (fixture.hold_completion && !fixture.release_completion)
    sync_cond_wait(&fixture.changed, &fixture.mutex);
  sync_mutex_unlock(&fixture.mutex);
}
static int receive_large(void *user, const void *data, size_t size) {
  (void)user;
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; ++i) if (bytes[i] != 'x') return SALTS_EPROTO;
  sync_mutex_lock(&fixture.mutex);
  fixture.sink_entered = 1;
  sync_cond_broadcast(&fixture.changed);
  while (!fixture.release_sink) sync_cond_wait(&fixture.changed, &fixture.mutex);
  fixture.received += size;
  sync_mutex_unlock(&fixture.mutex);
  return SALTS_OK;
}
static int route(void *user, const chttp_server_request_view *original, chttp_server_response *response) {
  (void)user;
  /* A real owner callback passes transient storage that is overwritten before
   * allowing business execution. A shallow request copy deterministically fails. */
  char target[] = "/snapshot?q=one", path[] = "/snapshot", body[] = "abc";
  char name[] = "X-Input", value[] = "original", id[] = "id", number[] = "42";
  char subject[] = "alice", audience[] = "service";
  const char *audiences[] = {audience};
  int64_t expires = 123456;
  cnet_stream_peer peer = {.port = 1234u};
  const chttp_header header = {name, value};
  const chttp_server_param param = {id, number};
  const chttp_jwt_claims_view jwt = {.subject = subject, .audiences = audiences,
      .audience_count = 1u, .expires_at = &expires};
  chttp_server_request_view request = *original;
  request.target = target; request.path = path; request.body = body; request.body_size = 3u;
  request.headers = &header; request.header_count = 1u; request.params = &param; request.param_count = 1u;
  request.jwt_claims = &jwt; request.peer = &peer;
  sync_mutex_lock(&fixture.mutex); fixture.owner_token = sync_thread_current_token();
  chttp_application_work_chain_hook hooks[2] = {
      {&fixture.hook_ids[0], before_hook, after_hook, error_hook},
      {&fixture.hook_ids[1], before_hook, after_hook, error_hook}};
  const chttp_application_work_task task = {.run = business, .finalize = finalized, .user = user,
      .after_completion = completed,
      .hooks = fixture.with_hooks ? hooks : NULL, .hook_count = fixture.with_hooks ? 2u : 0u};
  const int status = chttp_application_work_submit(&fixture.work, &request, response, &task);
  memset(target, '!', sizeof(target)-1u); memset(path, '!', sizeof(path)-1u);
  memset(body, '!', 3u); memset(name, '!', sizeof(name)-1u); memset(value, '!', sizeof(value)-1u);
  memset(id, '!', sizeof(id)-1u); memset(number, '!', sizeof(number)-1u);
  memset(subject, '!', sizeof(subject)-1u); memset(audience, '!', sizeof(audience)-1u);
  expires = -1; peer.port = 0u;
  memset(hooks, 0, sizeof(hooks));
  sync_mutex_unlock(&fixture.mutex);
  if (status == SALTS_ENOBUFS || status == SALTS_ESHUTDOWN)
    return chttp_server_reply(response, 503u, "text/plain", "full", 4u);
  return status;
}
static void start(size_t capacity, size_t bytes) {
  const chttp_application_work_config config = {fixture.workers, capacity, bytes,
      fixture.large_size ? fixture.large_size + 64u : 256u};
  check_equal(chttp_application_work_init(&fixture.work, &config), SALTS_OK);
  check_equal(chttp_server_init(&fixture.server, &fixture.http), SALTS_OK);
  chttp_server_execution_options execution = CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  execution.owner_count = fixture.owners;
  check_equal(chttp_server_set_execution_options(&fixture.server, &execution), SALTS_OK);
  check_equal(chttp_server_get(&fixture.server, "/work", route, NULL), SALTS_OK);
  if (fixture.large_size) {
    check_equal(chttp_server_get(&fixture.server, "/large", route, &fixture.large_marker), SALTS_OK);
  }
  const chttp_server_deadlines deadline = {.handler_ms = fixture.deadline_ms};
  check_equal(chttp_server_set_deadlines(&fixture.server, &deadline), SALTS_OK);
  check_equal(chttp_server_start(&fixture.server), SALTS_OK);
  check_equal(chttp_server_port(&fixture.server, &fixture.port), SALTS_OK);
}
static int perform(void *arg) {
  call_result *call = arg;
  chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  const chttp_client_config config = {.network = fixture.http.network,
      .request_capacity = 1u, .max_start_line_bytes = 256u, .max_header_count = 16u,
      .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
      .max_response_body_bytes = call->large ? fixture.large_size : 8192u, .max_informational_responses = 4u};
  call->status = chttp_client_init(&client, &config);
  if (call->status != SALTS_OK) return 0;
  const chttp_body_sink sink = {receive_large, NULL};
  char uri[64]; snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)fixture.port);
  const chttp_options options = {.connection_uri = uri, .authority = "work.test",
      .target = call->large ? "/large" : "/work", .protocol = call->protocol,
      .body_sink = call->large ? &sink : NULL, .timeout_ms = call->large ? 20000u : 5000u};
  call->status = chttp_get(&client, &options, &response, &error);
  call->code = response.status_code;
  if (response.body_size < sizeof(call->body)) {
    if (response.body_size) memcpy(call->body, response.body, response.body_size);
    call->body[response.body_size] = '\0';
  }
  chttp_response_destroy(&response);
  const int closed = chttp_client_destroy(&client, 5000u);
  if (call->status == SALTS_OK) call->status = closed;
  return 0;
}
static void await_entered(void) {
  const uint64_t start_time = monotonic_ms();
  sync_mutex_lock(&fixture.mutex);
  while (!fixture.entered && monotonic_ms() - start_time < 5000u)
    sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(100000000));
  const int entered = fixture.entered;
  sync_mutex_unlock(&fixture.mutex);
  check_equal(entered, 1);
}
static void release_worker(void) {
  sync_mutex_lock(&fixture.mutex); fixture.release = 1;
  fixture.release_completion = 1; fixture.release_sink = 1;
  sync_cond_broadcast(&fixture.changed); sync_mutex_unlock(&fixture.mutex);
}
spec("Application internal worker handoff") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture)); memset(&background, 0, sizeof(background));
    memset(&second_background, 0, sizeof(second_background));
    fixture.workers = 1u; fixture.owners = 1u;
    sync_mutex_init(&fixture.mutex); sync_cond_init(&fixture.changed);
    fixture.http = chttp_application_config_default().http;
    fixture.hook_ids[0] = 1; fixture.hook_ids[1] = 2;
  }
  after_each() {
    release_worker();
    if (fixture.thread_started) { int result; check_equal(thrd_join(fixture.client_thread, &result), thrd_success); }
    if (fixture.second_started) { int result; check_equal(thrd_join(fixture.second_thread, &result), thrd_success); }
    check_equal(chttp_server_stop(&fixture.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&fixture.server), SALTS_OK);
    check_equal(chttp_application_work_close(&fixture.work, 5000u), SALTS_OK);
    free(fixture.large_body);
    sync_cond_destroy(&fixture.changed); sync_mutex_destroy(&fixture.mutex);
  }
  it("copies transient H1 request identity and payload before invoking a worker") {
    start(2u, 8192u);
    call_result call = {0}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.body, "worker");
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.draft_status, SALTS_EALREADY);
    check_equal(fixture.snapshot_ok, 1); check_equal(fixture.calls, 1); check_equal(fixture.finalized, 1);
    check_not_equal(fixture.owner_token, fixture.worker_token);
    chttp_application_work_stats stats;
    check_equal(chttp_application_work_get_stats(&fixture.work, &stats), SALTS_OK);
    check_equal(stats.pending, 0u); check_equal(stats.retained_bytes, 0u);
    check_equal(stats.accepted, 1u); check_equal(stats.completed, 1u);
  }
  it("runs the same owned business invocation over H2") {
    start(2u, 8192u);
    call_result call = {.protocol = CHTTP_HTTP_2}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u); check_equal(call.body, "worker");
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.snapshot_ok, 1); check_not_equal(fixture.owner_token, fixture.worker_token);
  }
  it("refuses a full invocation budget while the owner continues serving") {
    fixture.held = 1; start(1u, 8192u);
    check_equal(thrd_create(&fixture.client_thread, perform, &background), thrd_success);
    fixture.thread_started = 1; await_entered();
    call_result rejected = {.protocol = CHTTP_HTTP_2}; perform(&rejected);
    check_equal(rejected.status, SALTS_OK); check_equal(rejected.code, 503u);
    check_equal(fixture.calls, 1);
    release_worker(); int result;
    check_equal(thrd_join(fixture.client_thread, &result), thrd_success); fixture.thread_started = 0;
    check_equal(background.status, SALTS_OK); check_equal(background.code, 201u);
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.finalized, 1);
    chttp_application_work_stats stats;
    check_equal(chttp_application_work_get_stats(&fixture.work, &stats), SALTS_OK);
    check_equal(stats.rejected, 1u); check_equal(stats.accepted, 1u);
  }
  it("rejects retained-byte exhaustion before business callbacks") {
    start(2u, 256u);
    call_result call = {0}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 503u);
    check_equal(fixture.calls, 0); check_equal(fixture.finalized, 0);
  }
  it("retains a timed-out close until the blocked worker actually settles") {
    fixture.held = 1; start(1u, 8192u);
    check_equal(thrd_create(&fixture.client_thread, perform, &background), thrd_success);
    fixture.thread_started = 1; await_entered();
    check_equal(chttp_application_work_close(&fixture.work, 5u), SALTS_ETIMEDOUT);
    check_not_equal(fixture.work.impl, NULL);
    call_result rejected = {.protocol = CHTTP_HTTP_2}; perform(&rejected);
    check_equal(rejected.status, SALTS_OK); check_equal(rejected.code, 503u);
    release_worker(); int result;
    check_equal(thrd_join(fixture.client_thread, &result), thrd_success); fixture.thread_started = 0;
    check_equal(background.status, SALTS_OK); check_equal(background.code, 201u);
  }
  it("rejects waiting or destroying the worker executor from its own callback") {
    fixture.close_from_worker = 1; start(1u, 8192u);
    call_result call = {0}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.same_executor_close, SALTS_EBUSY);
  }
  it("publishes a bounded 500 and finalizes once when the user method fails") {
    fixture.fail = 1; start(1u, 8192u);
    call_result call = {.protocol = CHTTP_HTTP_2}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 500u);
    check_equal(call.body, "Internal Server Error");
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.finalized, 1);
  }
  it("does not turn a missing result into a successful response") {
    fixture.no_reply = 1; start(1u, 8192u);
    call_result call = {0}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 500u);
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.finalized, 1);
  }
  it("checks response arithmetic before reading an overflowing body") {
    fixture.oversized = 1; start(1u, 8192u);
    call_result call = {0}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 500u);
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.finalized, 1);
    check_equal(fixture.draft_status, SALTS_ERANGE);
  }
  group("late worker completion after a phase deadline") {
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      it("retains request storage after protocol %d aborts the response", protocol) {
        fixture.held = 1; fixture.deadline_ms = 10u; start(2u, 8192u);
        background.protocol = (chttp_protocol)protocol;
        check_equal(thrd_create(&fixture.client_thread, perform, &background), thrd_success);
        fixture.thread_started = 1; await_entered();
        int result;
        check_equal(thrd_join(fixture.client_thread, &result), thrd_success); fixture.thread_started = 0;
        check_not_equal(background.status, SALTS_OK);
        check_not_equal(background.status, SALTS_ETIMEDOUT); /* Real peer terminal, not client timer. */
        chttp_application_work_stats stats;
        check_equal(chttp_application_work_get_stats(&fixture.work, &stats), SALTS_OK);
        check_equal(stats.pending, 1u); check_not_equal(stats.retained_bytes, 0u);
        check_equal(fixture.finalized, 0); check_equal(fixture.completion_count, 0);
        release_worker();
        check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
        check_equal(chttp_application_work_get_stats(&fixture.work, &stats), SALTS_OK);
        check_equal(stats.cancelled, 1u); check_equal(stats.pending, 0u);
        check_equal(stats.retained_bytes, 0u); check_equal(fixture.snapshot_ok, 1);
        check_equal(fixture.finalized, 1);
        call_result reuse = {.protocol = (chttp_protocol)protocol}; perform(&reuse);
        check_equal(reuse.status, SALTS_OK); check_equal(reuse.code, 201u);
        check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
        check_equal(fixture.finalized, 2);
      }
    }
  }

  it("runs typed Interceptors on the worker and unwinds after the actual method") {
    fixture.with_hooks = 1; fixture.held = 1; start(1u, 8192u);
    check_equal(thrd_create(&fixture.client_thread, perform, &background), thrd_success);
    fixture.thread_started = 1; await_entered();
    check_equal(fixture.trace_count, 3); /* No after hook merely for queueing. */
    release_worker(); int result;
    check_equal(thrd_join(fixture.client_thread, &result), thrd_success); fixture.thread_started = 0;
    check_equal(background.status, SALTS_OK); check_equal(background.code, 201u);
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    const int expected[] = {1, 2, 3, 12, 11};
    check_equal(fixture.trace_count, 5); check_equal(fixture.trace, expected, sizeof(expected));
    check_equal(fixture.wrong_hook_thread, 0);
  }
  it("preserves a before-hook rejection without calling the business method") {
    fixture.with_hooks = 1; fixture.reject_hook = 2; start(1u, 8192u);
    call_result call = {.protocol = CHTTP_HTTP_2}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 403u); check_equal(call.body, "denied");
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    const int expected[] = {1, 2, 102, 101};
    check_equal(fixture.trace_count, 4); check_equal(fixture.trace, expected, sizeof(expected));
    check_equal(fixture.calls, 0); check_equal(fixture.finalized, 1);
    check_equal(fixture.wrong_hook_thread, 0);
  }
  it("unwinds entered worker error hooks after a failing method") {
    fixture.with_hooks = 1; fixture.fail = 1; start(1u, 8192u);
    call_result call = {0}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 500u);
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    const int expected[] = {1, 2, 3, 102, 101};
    check_equal(fixture.trace_count, 5); check_equal(fixture.trace, expected, sizeof(expected));
    check_equal(fixture.calls, 1); check_equal(fixture.finalized, 1);
    check_equal(fixture.wrong_hook_thread, 0);
  }

  it("holds provider payload until the worker completion observer returns") {
    fixture.hold_completion = 1; start(1u, 8192u);
    call_result call = {0}; perform(&call);
    check_equal(call.status, SALTS_OK); check_equal(call.code, 201u);
    sync_mutex_lock(&fixture.mutex);
    const uint64_t start_time = monotonic_ms();
    while (!fixture.completion_count && monotonic_ms() - start_time < 5000u)
      sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(100000000));
    const int completed_count = fixture.completion_count;
    sync_mutex_unlock(&fixture.mutex);
    check_equal(completed_count, 1); check_equal(fixture.finalized, 0);
    check_equal(chttp_application_work_close(&fixture.work, 5u), SALTS_ETIMEDOUT);
    check_not_equal(fixture.work.impl, NULL);
    release_worker();
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.finalized, 1); check_equal(fixture.wrong_completion_thread, 0);
    check_equal(fixture.outcome.business_status, SALTS_OK);
    check_equal(fixture.outcome.publication_status, SALTS_OK);
    check_equal(fixture.outcome.transport_status, SALTS_OK);
  }
  group("business completion is separate from response transport") {
      it("waits for H2 flow-control release while other work uses the same worker") {
        const chttp_protocol protocol = CHTTP_HTTP_2;
        fixture.large_size = 1024u * 1024u;
        fixture.large_body = malloc(fixture.large_size);
        check_equal(fixture.large_body != NULL, true); memset(fixture.large_body, 'x', fixture.large_size);
        fixture.http.max_response_body_bytes = fixture.large_size;
        fixture.http.max_buffered_response_body_bytes = fixture.large_size;
        fixture.http.network.max_send_bytes = fixture.large_size * 2u;
        fixture.http.buffer_capacity_bytes = fixture.large_size * 16u;
        start(2u, fixture.large_size * 4u);
        background.protocol = (chttp_protocol)protocol; background.large = 1;
        check_equal(thrd_create(&fixture.client_thread, perform, &background), thrd_success);
        fixture.thread_started = 1;
        sync_mutex_lock(&fixture.mutex);
        const uint64_t start_time = monotonic_ms();
        while (!fixture.sink_entered && monotonic_ms() - start_time < 5000u)
          sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(100000000));
        const int entered = fixture.sink_entered;
        sync_mutex_unlock(&fixture.mutex);
        check_equal(entered, 1);
        /* A second invocation on the sole worker proves the first business task
         * and its predecessor finalizer have returned. The first client has
         * stopped consuming DATA: H2 window credit cannot cover the entire
         * response. Unread H1 bytes alone would not prove native backpressure;
         * a local send may legitimately finish into the peer's kernel buffer. */
        call_result small = {.protocol = (chttp_protocol)protocol}; perform(&small);
        check_equal(small.status, SALTS_OK); check_equal(small.code, 201u);
        sync_mutex_lock(&fixture.mutex);
        const uint64_t completion_start = monotonic_ms();
        while (!fixture.completed_small && monotonic_ms() - completion_start < 5000u)
          sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(100000000));
        const int small_done = fixture.completed_small, large_done = fixture.completed_large;
        sync_mutex_unlock(&fixture.mutex);
        check_equal(small_done, 1); check_equal(large_done, 0);
        release_worker(); int result;
        check_equal(thrd_join(fixture.client_thread, &result), thrd_success); fixture.thread_started = 0;
        check_equal(background.status, SALTS_OK); check_equal(background.code, 200u);
        check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
        check_equal(fixture.received, fixture.large_size); check_equal(fixture.completed_large, 1);
        check_equal(fixture.completion_count, 2); check_equal(fixture.finalized, 2);
        check_equal(fixture.wrong_completion_thread, 0);
      }
  }

  it("skips queued user code after the owner has cancelled that request") {
    fixture.held = 1; fixture.deadline_ms = 500u; start(2u, 8192u);
    background.protocol = CHTTP_HTTP_2;
    check_equal(thrd_create(&fixture.client_thread, perform, &background), thrd_success);
    fixture.thread_started = 1; await_entered();
    call_result queued = {.protocol = CHTTP_HTTP_2}; perform(&queued);
    check_not_equal(queued.status, SALTS_OK); check_not_equal(queued.status, SALTS_ETIMEDOUT);
    check_equal(fixture.calls, 1); check_equal(fixture.finalized, 0);
    release_worker(); int result;
    check_equal(thrd_join(fixture.client_thread, &result), thrd_success); fixture.thread_started = 0;
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.calls, 1); check_equal(fixture.finalized, 2);
    check_equal(fixture.completion_count, 2); check_equal(fixture.cancelled_before_run, 1);
    chttp_application_work_stats stats;
    check_equal(chttp_application_work_get_stats(&fixture.work, &stats), SALTS_OK);
    check_equal(stats.pending, 0u); check_equal(stats.cancelled, 2u);
    check_equal(stats.retained_bytes, 0u); check_equal(stats.publication_status, SALTS_OK);
  }

  group("empty buffered response terminals") {
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      it("settles protocol %d headers-only responses exactly once", protocol) {
        fixture.empty = 1; start(1u, 8192u);
        call_result call = {.protocol = (chttp_protocol)protocol}; perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 204u); check_equal(call.body, "");
        check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
        check_equal(fixture.completion_count, 1); check_equal(fixture.finalized, 1);
        check_equal(fixture.outcome.transport_status, SALTS_OK);
      }
    }
  }

  it("runs independent owned requests concurrently on two workers and two HTTP owners") {
    fixture.held = 1; fixture.workers = 2u; fixture.owners = 2u; start(2u, 8192u);
    second_background.protocol = CHTTP_HTTP_2;
    check_equal(thrd_create(&fixture.client_thread, perform, &background), thrd_success);
    fixture.thread_started = 1;
    check_equal(thrd_create(&fixture.second_thread, perform, &second_background), thrd_success);
    fixture.second_started = 1;
    sync_mutex_lock(&fixture.mutex);
    const uint64_t started = monotonic_ms();
    while (fixture.calls < 2 && monotonic_ms() - started < 5000u)
      sync_cond_timedwait(&fixture.changed, &fixture.mutex, UINT64_C(100000000));
    const int calls = fixture.calls;
    sync_mutex_unlock(&fixture.mutex);
    check_equal(calls, 2); check_not_equal(fixture.worker_tokens[0], fixture.worker_tokens[1]);
    check_equal(fixture.finalized, 0);
    release_worker(); int result;
    check_equal(thrd_join(fixture.client_thread, &result), thrd_success); fixture.thread_started = 0;
    check_equal(thrd_join(fixture.second_thread, &result), thrd_success); fixture.second_started = 0;
    check_equal(background.status, SALTS_OK); check_equal(background.code, 201u);
    check_equal(second_background.status, SALTS_OK); check_equal(second_background.code, 201u);
    check_equal(chttp_application_work_wait(&fixture.work, 5000u), SALTS_OK);
    check_equal(fixture.completion_count, 2); check_equal(fixture.finalized, 2);
    check_equal(fixture.wrong_completion_thread, 0);
  }

}
