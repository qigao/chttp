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

typedef struct StreamController { int marker; } StreamController;
cmeta_reflect_data(StreamController, "test.StreamController", cmeta_field(int, marker));
cmeta_component_empty(StreamComponent);
CHTTP_INVOCATION_ACTION(stream_action);
typedef struct stream_call {
  chttp_protocol protocol;
  const char *path;
  bool head;
  bool cancel;
  int status;
  unsigned int code;
  size_t bytes;
  char body[8192], type[64], cache[64];
} stream_call;
static struct {
  chttp_application app;
  chttp_application_config config;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  chttp_application_worker_binding binding;
  chttp_invocation_interceptor hook;
  chttp_application_work_config limits;
  chttp_web_sse_stream sse;
  char scratch[128];
  sync_mutex_t mutex;
  sync_cond_t changed;
  sync_thread_t thread;
  bool thread_live, hold, release, entered, sse_enabled, known, fail_business, recover;
  size_t total, offset, chunk, declared;
  int read_status, reads, cleanups, cleanup_status, afters, completions, destroyed, selection_status;
  _Atomic int invalid;
  chttp_application_work_outcome outcome;
  char uri[64];
  stream_call background;
} streams;
static void stream_destroy(void *user, void *object) { (void)user; ++streams.destroyed; free(object); }
static const cmeta_object_lifecycle stream_lifecycle = {sizeof(stream_lifecycle), NULL, NULL, NULL, stream_destroy};
static cmeta_status stream_create(void *user, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *deps, size_t count, cmeta_object_ref *out) {
  (void)user; (void)data; (void)config; (void)deps;
  if (count) return CMETA_INVALID_ARGUMENT;
  StreamController *object = malloc(sizeof(*object));
  if (!object) return CMETA_OUT_OF_MEMORY;
  object->marker = 42;
  cmeta_status status = cmeta_object_borrow(out, object, cmeta_reflected_data(StreamController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &stream_lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(object); }
  return status;
}
static void stream_worker(void) {
  if (!chttp_application_worker_current(&streams.app)) atomic_fetch_add(&streams.invalid, 1);
}
static int stream_read(void *user, void *buffer, size_t capacity, size_t *out) {
  if (user != &streams) atomic_fetch_add(&streams.invalid, 1);
  stream_worker();
  sync_mutex_lock(&streams.mutex);
  if (streams.afters != 1 || !capacity || capacity > streams.chunk) atomic_fetch_add(&streams.invalid, 1);
  ++streams.reads;
  if (streams.hold) {
    streams.entered = true; sync_cond_broadcast(&streams.changed);
    while (!streams.release) sync_cond_wait(&streams.changed, &streams.mutex);
  }
  sync_mutex_unlock(&streams.mutex);
  if (streams.read_status) return streams.read_status;
  *out = streams.total - streams.offset < capacity ? streams.total - streams.offset : capacity;
  memset(buffer, 'x', *out); streams.offset += *out;
  return SALTS_OK;
}
static void stream_cleanup(void *user, int status) {
  if (user != &streams) atomic_fetch_add(&streams.invalid, 1);
  stream_worker();
  sync_mutex_lock(&streams.mutex); ++streams.cleanups; streams.cleanup_status = status; sync_mutex_unlock(&streams.mutex);
}
static int stream_sse_next(void *user, chttp_web_sse_event *event) {
  stream_worker();
  if (user != &streams) atomic_fetch_add(&streams.invalid, 1);
  if (streams.offset++) return SALTS_ENOENT;
  *event = (chttp_web_sse_event)CHTTP_WEB_SSE_EVENT_INIT;
  event->data = (chttp_web_string_view){"hello\nworld", sizeof("hello\nworld") - 1u};
  event->has_data = true;
  return SALTS_OK;
}
static void stream_after(void *self, const chttp_invocation *invocation, const chttp_invocation_result *result) {
  (void)invocation; (void)result; stream_worker();
  if (!self || ((StreamController *)self)->marker != 42) atomic_fetch_add(&streams.invalid, 1);
  sync_mutex_lock(&streams.mutex); ++streams.afters; sync_mutex_unlock(&streams.mutex);
}
static void stream_completed(void *self, const chttp_server_request_view *request, const chttp_application_work_outcome *outcome) {
  (void)self; (void)request; stream_worker();
  sync_mutex_lock(&streams.mutex); ++streams.completions; streams.outcome = *outcome; sync_mutex_unlock(&streams.mutex);
}
int stream_action(void *self, const chttp_invocation *invocation) {
  stream_worker();
  if (!self || ((StreamController *)self)->marker != 42) return SALTS_EPROTO;
  int status;
  if (streams.sse_enabled) {
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    if (chttp_web_sse_stream_init(&streams.sse, stream_sse_next, stream_cleanup, &streams,
        streams.scratch, sizeof(streams.scratch), &error) != CHTTP_WEB_OK) return SALTS_EPROTO;
    status = chttp_invocation_sse_response(invocation, &streams.sse, streams.chunk);
  } else {
    chttp_body_source source = {stream_read, &streams, streams.declared, streams.known};
    status = chttp_invocation_response_source(invocation, 200u, "text/plain", &source, stream_cleanup, streams.chunk);
    memset(&source, 0, sizeof(source));
  }
  streams.selection_status = status;
  if (status != SALTS_OK && streams.recover) return chttp_invocation_respond(invocation, 200u, NULL, "recovered", 9u);
  return status == SALTS_OK && streams.fail_business ? SALTS_EPERM : status;
}
static void stream_start(void) {
  check_equal(chttp_application_init(&streams.app, &streams.config), SALTS_OK);
  check_equal(chttp_application_worker_prepare(&streams.app, &streams.limits), SALTS_OK);
  check_equal(chttp_application_worker_mount(&streams.app, &streams.binding), SALTS_OK);
  check_equal(chttp_application_start(&streams.app), SALTS_OK);
  uint16_t port = 0u; check_equal(chttp_application_port(&streams.app, &port), SALTS_OK);
  snprintf(streams.uri, sizeof(streams.uri), "tcp://127.0.0.1:%u", (unsigned)port);
}
static int stream_cancel_sink(void *user, const void *data, size_t size) {
  (void)user; (void)data; return size ? SALTS_ECANCELED : SALTS_OK;
}
static void stream_perform(void *user) {
  stream_call *call = user;
  chttp_client client = {0}; chttp_response response = {0}; chttp_error error = {0};
  const chttp_client_config config = {streams.config.http.network, 1u, 256u, 16u, 4096u, 16384u, 16384u, 4u};
  call->status = chttp_client_init(&client, &config);
  if (call->status != SALTS_OK) return;
  chttp_options options = {0}; options.connection_uri = streams.uri; options.authority = "stream.test";
  options.protocol = call->protocol; options.target = call->path ? call->path : "/stream"; options.timeout_ms = 5000u;
  const chttp_body_sink sink = {stream_cancel_sink, NULL}; options.body_sink = call->cancel ? &sink : NULL;
  call->status = call->head ? chttp_head(&client, &options, &response, &error) : chttp_get(&client, &options, &response, &error);
  call->code = response.status_code; call->bytes = response.body_size;
  if (response.body_size < sizeof(call->body) && response.body_size) memcpy(call->body, response.body, response.body_size);
  const char *field = chttp_response_header(&response, "Content-Type"); snprintf(call->type, sizeof(call->type), "%s", field ? field : "");
  field = chttp_response_header(&response, "Cache-Control"); snprintf(call->cache, sizeof(call->cache), "%s", field ? field : "");
  chttp_response_destroy(&response);
  const int closed = chttp_client_destroy(&client, 5000u); if (call->status == SALTS_OK) call->status = closed;
}
static void stream_wait_entered(void) {
  const uint64_t deadline = monotonic_ms() + 5000u;
  sync_mutex_lock(&streams.mutex);
  while (!streams.entered && monotonic_ms() < deadline) sync_cond_timedwait(&streams.changed, &streams.mutex, UINT64_C(10000000));
  const bool entered = streams.entered; sync_mutex_unlock(&streams.mutex); check_true(entered);
}
spec("Application worker streaming") {
  before_each() {
    memset(&streams, 0, sizeof(streams)); atomic_init(&streams.invalid, 0);
    sync_mutex_init(&streams.mutex); sync_cond_init(&streams.changed);
    streams.provider = (salts_configurator_provider_binding){sizeof(streams.provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(StreamComponent), NULL, NULL, stream_create, NULL, NULL};
    streams.deployment = (salts_configurator_deployment){&streams.provider, NULL, NULL};
    streams.controller = (chttp_controller){.component_id = "StreamComponent", .receiver = cmeta_reflected_data(StreamController)};
    streams.config = chttp_application_config_default(); streams.config.components = &streams.deployment; streams.config.component_count = 1u;
    streams.config.controllers = &streams.controller; streams.config.controller_count = 1u;
    streams.limits = (chttp_application_work_config){1u, 4u, 32768u, 128u, 2u, 128u};
    streams.hook = (chttp_invocation_interceptor){"StreamComponent", cmeta_reflected_data(StreamController), NULL, stream_after, NULL};
    streams.binding = (chttp_application_worker_binding){.component_id = "StreamComponent", .receiver = cmeta_reflected_data(StreamController),
        .method = CHTTP_METHOD_GET, .path = "/stream", .abi = FunctionAbi(stream_action), .invoke = stream_action,
        .after_completion = stream_completed, .hooks = &streams.hook, .hook_count = 1u};
    streams.total = 4096u; streams.declared = streams.total; streams.chunk = 31u;
  }
  after_each() {
    sync_mutex_lock(&streams.mutex); streams.release = true; sync_cond_broadcast(&streams.changed); sync_mutex_unlock(&streams.mutex);
    if (streams.thread_live) check_equal(sync_thread_join(&streams.thread), SALTS_OK);
    check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK); check_equal(atomic_load(&streams.invalid), 0);
    sync_cond_destroy(&streams.changed); sync_mutex_destroy(&streams.mutex);
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
    it("settles worker source cleanup after client cancellation (protocol=%d)", protocol) {
      stream_start(); stream_call call = {.protocol = (chttp_protocol)protocol, .cancel = true}; stream_perform(&call);
      check_equal(call.status, SALTS_ECANCELED);
      check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK);
      check_equal(streams.cleanups, 1); check_equal(streams.completions, 1); check_not_equal(streams.outcome.transport_status, SALTS_OK);
    }
    for (int mismatch = 0; mismatch < 3; ++mismatch) {
      it("enforces source EOF and declared length (protocol=%d mismatch=%d)", protocol, mismatch) {
        streams.known = true; streams.total = mismatch ? 32u : 0u;
        streams.declared = mismatch == 1 ? 31u : mismatch == 2 ? 33u : 0u;
        stream_start(); stream_call call = {.protocol = (chttp_protocol)protocol}; stream_perform(&call);
        /* H1 may deliver its complete Content-Length before the owner's EOF
         * check discovers excess producer data. The server terminal still fails. */
        if (mismatch == 1 && protocol == CHTTP_HTTP_1_1) {
          check_equal(call.status, SALTS_OK); check_equal(call.bytes, 31u);
        } else if (mismatch) { check_not_equal(call.status, SALTS_OK); check_not_equal(call.status, SALTS_ETIMEDOUT); }
        else { check_equal(call.status, SALTS_OK); check_equal(call.bytes, 0u); }
        check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK); check_equal(streams.cleanups, 1);
        if (mismatch) { check_equal(streams.cleanup_status, SALTS_EPROTO); check_not_equal(streams.outcome.transport_status, SALTS_OK); }
      }
    }
    for (int known = 0; known < 2; ++known) {
      it("transfers a body larger than reply_bytes with worker-only source/cleanup (protocol=%d known=%d)", protocol, known) {
        streams.known = known != 0; stream_start(); stream_call call = {.protocol = (chttp_protocol)protocol}; stream_perform(&call);
        check_equal(call.status, SALTS_OK); check_equal(call.code, 200u); check_equal(call.bytes, streams.total);
        for (size_t i = 0u; i < call.bytes; ++i) check_equal(call.body[i], 'x');
        check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK);
        check_greater(streams.reads, 1); check_equal(streams.cleanups, 1); check_equal(streams.cleanup_status, SALTS_OK);
        check_equal(streams.completions, 1); check_equal(streams.destroyed, 1); check_equal(streams.outcome.transport_status, SALTS_OK);
      }
    }
    it("formats SSE events and closes the producer on a worker (protocol=%d)", protocol) {
      streams.sse_enabled = true; stream_start(); stream_call call = {.protocol = (chttp_protocol)protocol}; stream_perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 200u); check_equal(call.type, "text/event-stream"); check_equal(call.cache, "no-cache");
      check_equal(call.body, "data: hello\ndata: world\n\n");
      check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK); check_equal(streams.cleanups, 1); check_false(streams.sse.active);
    }
    it("cleans up HEAD without calling the user producer (protocol=%d)", protocol) {
      stream_start(); stream_call call = {.protocol = (chttp_protocol)protocol, .head = true}; stream_perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 200u); check_equal(call.bytes, 0u);
      check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK); check_equal(streams.reads, 0); check_equal(streams.cleanups, 1);
    }
    it("cleans up a selected source after business failure (protocol=%d)", protocol) {
      streams.fail_business = true; stream_start(); stream_call call = {.protocol = (chttp_protocol)protocol}; stream_perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.code, 500u);
      check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK);
      check_equal(streams.reads, 0); check_equal(streams.cleanups, 1); check_equal(streams.cleanup_status, SALTS_EPERM);
    }
    it("propagates a negative user status rather than treating it as internal WAIT (protocol=%d)", protocol) {
      streams.read_status = SALTS_EBUSY; stream_start(); stream_call call = {.protocol = (chttp_protocol)protocol}; stream_perform(&call);
      check_not_equal(call.status, SALTS_OK); check_not_equal(call.status, SALTS_ETIMEDOUT);
      check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK); check_equal(streams.cleanups, 1); check_equal(streams.reads, 1);
      check_not_equal(streams.outcome.transport_status, SALTS_OK);
    }
    it("does not transfer resource ownership when the chunk budget is rejected (protocol=%d)", protocol) {
      streams.chunk = streams.limits.reply_bytes; streams.recover = true; stream_start();
      stream_call call = {.protocol = (chttp_protocol)protocol}; stream_perform(&call);
      check_equal(call.status, SALTS_OK); check_equal(call.body, "recovered");
      check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK);
      check_equal(streams.selection_status, SALTS_EMSGSIZE); check_equal(streams.reads, 0); check_equal(streams.cleanups, 0);
    }
    it("keeps owner progress and shared-slot overload live while a producer blocks (protocol=%d)", protocol) {
      streams.hold = true; streams.limits.capacity = 1u; stream_start();
      streams.background = (stream_call){.protocol = (chttp_protocol)protocol};
      check_equal(sync_thread_create(&streams.thread, stream_perform, &streams.background), SALTS_OK); streams.thread_live = true; stream_wait_entered();
      stream_call overload = {.protocol = (chttp_protocol)protocol}; stream_perform(&overload); check_equal(overload.status, SALTS_OK); check_equal(overload.code, 503u);
      stream_call missing = {.protocol = (chttp_protocol)protocol, .path = "/missing"}; stream_perform(&missing); check_equal(missing.status, SALTS_OK); check_equal(missing.code, 404u);
      check_equal(chttp_application_close(&streams.app, 5u), SALTS_ETIMEDOUT); check_not_null(streams.app.impl); check_equal(streams.destroyed, 0);
      sync_mutex_lock(&streams.mutex); streams.release = true; sync_cond_broadcast(&streams.changed); sync_mutex_unlock(&streams.mutex);
      check_equal(sync_thread_join(&streams.thread), SALTS_OK); streams.thread_live = false;
      check_equal(chttp_application_close(&streams.app, 5000u), SALTS_OK);
      check_equal(streams.cleanups, 1); check_equal(streams.completions, 1); check_equal(streams.destroyed, 1);
    }
  }
}
