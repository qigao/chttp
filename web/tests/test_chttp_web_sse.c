#include <chttp_web/web.h>
#include "tinytest.h"

#include <http_client/http.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  WEB_SSE_TIMEOUT_MS = 5000,
  WEB_SSE_STREAM_CHUNK_BYTES = 32,
  WEB_SSE_BODY_BYTES = 4 * 1024 * 1024
};

typedef enum web_sse_mode {
  WEB_SSE_FINITE,
  WEB_SSE_DISCONNECT
} web_sse_mode;

typedef struct web_sse_app {
  chttp_web_sse_stream stream;
  char scratch[256];
  web_sse_mode mode;
  size_t index;
  atomic_int close_count;
  atomic_int close_status;
} web_sse_app;

typedef struct web_sse_sink_probe {
  size_t calls;
} web_sse_sink_probe;

static native_io_backend_kind web_sse_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config web_sse_network(size_t connections) {
  const cnet_client_config config = {
      .backend = web_sse_backend(),
      .connection_capacity = connections,
      .command_capacity = 32u,
      .request_capacity = 16u,
      .completion_batch_capacity = 16u,
      .event_capacity = 32u,
      .max_send_bytes = 64u * 1024u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = WEB_SSE_TIMEOUT_MS,
      .read_timeout_ms = WEB_SSE_TIMEOUT_MS,
      .write_timeout_ms = WEB_SSE_TIMEOUT_MS};
  return config;
}

static chttp_server_config web_sse_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = web_sse_network(4u),
      .route_capacity = 4u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 64u,
      .max_target_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = 1024u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 4096u,
      .max_response_body_bytes = WEB_SSE_BODY_BYTES,
      .poll_slice_ms = 1u,
      .enable_http2 = 1,
      .h2_stream_capacity = 8u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_output_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u,
      .stream_chunk_bytes = WEB_SSE_STREAM_CHUNK_BYTES,
      .max_buffered_response_body_bytes = 4096u,
      .buffer_capacity_bytes = 512u * 1024u};
  return config;
}

static chttp_client_config web_sse_client_config(void) {
  const chttp_client_config config = {
      .network = web_sse_network(2u),
      .request_capacity = 4u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = 1024u,
      .max_response_body_bytes = WEB_SSE_BODY_BYTES,
      .max_informational_responses = 2u,
      .stream_chunk_bytes = 4096u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
  return config;
}

static chttp_web_string_view web_sse_view(const char *text) {
  return (chttp_web_string_view){text, text ? strlen(text) : 0u};
}

static int web_sse_next(void *user, chttp_web_sse_event *out_event) {
  web_sse_app *app = (web_sse_app *)user;
  if (app == NULL || out_event == NULL) return SALTS_EINVAL;

  *out_event = (chttp_web_sse_event)CHTTP_WEB_SSE_EVENT_INIT;
  if (app->mode == WEB_SSE_DISCONNECT) {
    out_event->has_data = true;
    out_event->data = web_sse_view("tick");
    ++app->index;
    return SALTS_OK;
  }

  if (app->index == 0u) {
    out_event->has_event = true;
    out_event->event = web_sse_view("ready");
    out_event->has_id = true;
    out_event->id = web_sse_view("1");
    out_event->has_retry = true;
    out_event->retry_ms = 1000u;
    out_event->has_data = true;
    out_event->data = web_sse_view("hello\r\nworld");
  } else if (app->index == 1u) {
    out_event->has_data = true;
    out_event->data = web_sse_view("done");
  } else {
    return SALTS_ENOENT;
  }
  ++app->index;
  return SALTS_OK;
}

static void web_sse_close(void *user, int status) {
  web_sse_app *app = (web_sse_app *)user;
  if (app == NULL) return;
  atomic_store_explicit(&app->close_status, status, memory_order_release);
  atomic_fetch_add_explicit(&app->close_count, 1, memory_order_acq_rel);
}

static int web_sse_handler(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_sse_app *app = (web_sse_app *)user;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)request;

  if (app == NULL) return SALTS_EINVAL;
  app->index = 0u;
  atomic_store_explicit(&app->close_count, 0, memory_order_release);
  atomic_store_explicit(&app->close_status, SALTS_EBUSY, memory_order_release);
  status = chttp_web_sse_stream_init(
      &app->stream, web_sse_next, web_sse_close, app,
      app->scratch, sizeof(app->scratch), &error);
  if (status != CHTTP_WEB_OK) return error.native_status;
  status = chttp_web_sse_response(response, &app->stream, &error);
  return status == CHTTP_WEB_OK ? SALTS_OK : error.native_status;
}

static int web_sse_wait_closed(web_sse_app *app, uint32_t timeout_ms) {
  const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
  while (atomic_load_explicit(&app->close_count, memory_order_acquire) == 0) {
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    salts_thread_yield();
  }
  return SALTS_OK;
}

static int web_sse_start(
    web_sse_app *app, chttp_server *server, uint16_t *out_port) {
  chttp_server_config config = web_sse_server_config();
  int status = chttp_server_init(server, &config);
  if (status == SALTS_OK)
    status = chttp_server_get(server, "/events", web_sse_handler, app);
  if (status == SALTS_OK) status = chttp_server_start(server);
  if (status == SALTS_OK) status = chttp_server_port(server, out_port);
  return status;
}

static int web_sse_call(
    chttp_client *client,
    uint16_t port,
    chttp_protocol protocol,
    const chttp_body_sink *sink,
    chttp_response *response) {
  char uri[64];
  char authority[64];
  chttp_options options = {0};
  chttp_error error = {0};
  if (snprintf(
          uri, sizeof(uri), "tcp://127.0.0.1:%u",
          (unsigned int)port) <= 0 ||
      snprintf(
          authority, sizeof(authority), "127.0.0.1:%u",
          (unsigned int)port) <= 0)
    return SALTS_EMSGSIZE;
  options.connection_uri = uri;
  options.authority = authority;
  options.target = "/events";
  options.body_sink = sink;
  options.timeout_ms = WEB_SSE_TIMEOUT_MS;
  options.protocol = protocol;
  return chttp_get(client, &options, response, &error);
}

static int web_sse_abort_sink(
    void *user, const void *data, size_t size) {
  web_sse_sink_probe *probe = (web_sse_sink_probe *)user;
  if (probe == NULL || (size != 0u && data == NULL)) return SALTS_EINVAL;
  ++probe->calls;
  return SALTS_EIO;
}

spec("CHttp::Web SSE") {
  it("formats fields byte-exactly and normalizes data newlines") {
    chttp_web_sse_event event =
        (chttp_web_sse_event)CHTTP_WEB_SSE_EVENT_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    char output[256];
    size_t size = 0u;
    static const char expected[] =
        "event: update\n"
        "id: 42\n"
        "retry: 1500\n"
        "data: one\n"
        "data: two\n"
        "data: three\n"
        "data:\n"
        "\n";

    event.has_event = true;
    event.event = web_sse_view("update");
    event.has_id = true;
    event.id = web_sse_view("42");
    event.has_retry = true;
    event.retry_ms = 1500u;
    event.has_data = true;
    event.data = web_sse_view("one\r\ntwo\rthree\n");

    check_equal(
        chttp_web_sse_format_event(
            &event, output, sizeof(output), &size, &error),
        CHTTP_WEB_OK);
    check_equal(size, sizeof(expected) - 1u);
    check_equal(output, expected, size);

    size = 123u;
    check_equal(
        chttp_web_sse_format_event(
            &event, output, sizeof(expected) - 2u, &size, &error),
        CHTTP_WEB_CAPACITY);
    check_equal(size, (size_t)0u);
    check_equal(error.native_status, SALTS_EMSGSIZE);

    event.event = web_sse_view("bad\nevent");
    check_equal(
        chttp_web_sse_format_event(
            &event, output, sizeof(output), &size, &error),
        CHTTP_WEB_INVALID_ARGUMENT);
  }

  it("streams the same finite event sequence over HTTP/1.1 and HTTP/2") {
    static const char expected[] =
        "event: ready\n"
        "id: 1\n"
        "retry: 1000\n"
        "data: hello\n"
        "data: world\n"
        "\n"
        "data: done\n"
        "\n";
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_client_config client_config = web_sse_client_config();
    web_sse_app app = {
        .stream = CHTTP_WEB_SSE_STREAM_INIT,
        .mode = WEB_SSE_FINITE};
    uint16_t port = 0u;

    atomic_init(&app.close_count, 0);
    atomic_init(&app.close_status, SALTS_EBUSY);
    check_equal(web_sse_start(&app, &server, &port), SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      chttp_response response = {0};
      check_equal(
          web_sse_call(
              &client, port, (chttp_protocol)protocol, NULL, &response),
          SALTS_OK);
      check_equal(response.status_code, 200u);
      check_equal(
          chttp_response_header(&response, "Content-Type"),
          "text/event-stream");
      check_equal(
          chttp_response_header(&response, "Cache-Control"), "no-cache");
      check_equal(response.body_size, sizeof(expected) - 1u);
      check_equal(response.body, expected, response.body_size);
      if (protocol == CHTTP_HTTP_2)
        check_equal(response.http_major, 2u);
      else
        check_equal(response.http_major, 1u);
      chttp_response_destroy(&response);
      check_equal(
          web_sse_wait_closed(&app, WEB_SSE_TIMEOUT_MS), SALTS_OK);
      check_equal(
          atomic_load_explicit(&app.close_count, memory_order_acquire), 1);
      check_equal(
          atomic_load_explicit(&app.close_status, memory_order_acquire),
          SALTS_OK);
    }

    check_equal(
        chttp_client_destroy(&client, WEB_SSE_TIMEOUT_MS), SALTS_OK);
    check_equal(
        chttp_server_stop(&server, WEB_SSE_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("runs source cleanup exactly once after client-side disconnect") {
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_client_config client_config = web_sse_client_config();
    web_sse_app app = {
        .stream = CHTTP_WEB_SSE_STREAM_INIT,
        .mode = WEB_SSE_DISCONNECT};
    web_sse_sink_probe probe = {0};
    const chttp_body_sink sink = {
        .write = web_sse_abort_sink,
        .user = &probe};
    chttp_response response = {0};
    uint16_t port = 0u;
    int call_status;

    atomic_init(&app.close_count, 0);
    atomic_init(&app.close_status, SALTS_EBUSY);
    check_equal(web_sse_start(&app, &server, &port), SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    call_status = web_sse_call(
        &client, port, CHTTP_HTTP_1_1, &sink, &response);
    check_equal(call_status, SALTS_EIO);
    check_greater(probe.calls, (size_t)0u);
    chttp_response_destroy(&response);
    check_equal(
        web_sse_wait_closed(&app, WEB_SSE_TIMEOUT_MS), SALTS_OK);
    check_equal(
        atomic_load_explicit(&app.close_count, memory_order_acquire), 1);
    check_true(
        atomic_load_explicit(&app.close_status, memory_order_acquire) !=
        SALTS_OK);

    check_equal(
        chttp_client_destroy(&client, WEB_SSE_TIMEOUT_MS), SALTS_OK);
    check_equal(
        chttp_server_stop(&server, WEB_SSE_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }
}
