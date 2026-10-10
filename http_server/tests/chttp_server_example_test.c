/* CTest sets an immutable process environment; getenv's borrowed view is safe. */
#define _CRT_SECURE_NO_WARNINGS
#include "http_example_app.h"
#include "chttp_server_runtime.h"
#include <http_client/http.h>
#include <fmt.h>
#include <tinytest.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct example_fixture {
  http_example_app app;
  chttp_client client;
  chttp_client_config client_config;
  chttp_async_client async_client;
  atomic_int gate_release;
  atomic_int gate_started;
  unsigned completions;
  chttp_response response;
  tstr uri;
} example_fixture;

static void example_start(example_fixture *fixture, const http_example_config *config) {
  chttp_client_config client = {0};
  uint16_t port = 0u;
  check_equal(http_example_configure(&fixture->app, config), SALTS_OK);
  check_equal(crpc_server_start(&fixture->app.rpc), SALTS_OK);
  check_equal(crpc_server_port(&fixture->app.rpc, &port), SALTS_OK);
  fixture->uri = tstr_format("tcp://127.0.0.1:{}", (unsigned)port);
  check_not_null(fixture->uri);
#if defined(_WIN32)
  client.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  client.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  client.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  client.network.connection_capacity = 2u;
  client.network.command_capacity = 16u;
  client.network.request_capacity = 8u;
  client.network.completion_batch_capacity = 8u;
  client.network.event_capacity = 16u;
  client.network.max_send_bytes = 64u * 1024u;
  client.network.receive_buffer_bytes = 4096u;
  client.network.connect_timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS;
  client.network.read_timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS;
  client.network.write_timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS;
  client.request_capacity = 1u;
  client.max_start_line_bytes = 256u;
  client.max_header_count = 16u;
  client.max_header_bytes = 4096u;
  client.max_request_body_bytes = HTTP_EXAMPLE_BODY_BYTES;
  client.max_response_body_bytes = HTTP_EXAMPLE_BODY_BYTES;
  client.max_informational_responses = 2u;
  check_equal(chttp_client_init(&fixture->client, &client), SALTS_OK);
  fixture->client_config = client;
}

static void example_gate(void *user) {
  example_fixture *fixture = user;
  atomic_store_explicit(&fixture->gate_started, 1, memory_order_release);
  while (!atomic_load_explicit(&fixture->gate_release, memory_order_acquire))
    cmeta_sleep_ms(1u);
}

static void example_completed(void *user, chttp_request request,
    const chttp_response_view *response, const chttp_error *error) {
  example_fixture *fixture = user;
  (void)request;
  (void)response;
  (void)error;
  ++fixture->completions;
}

static void example_get(example_fixture *fixture, const char *target, unsigned status) {
  chttp_error error = {0};
  const chttp_options options = {.connection_uri = fixture->uri, .authority = "127.0.0.1",
      .target = target, .timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS};
  chttp_response_destroy(&fixture->response);
  check_equal(chttp_get(&fixture->client, &options, &fixture->response, &error), SALTS_OK);
  check_equal(fixture->response.status_code, status);
}

static int example_contains(const chttp_response *response, const char *text) {
  return vstr_contains(vstr_from_buf(response->body, response->body_size), vstr_from_cstr(text));
}

spec("HTTP example IDL, configurator, interceptors and Jinja") {
  static example_fixture fixture;
  static http_example_config config;

  before_each() {
    fixture = (example_fixture){0};
    atomic_init(&fixture.gate_release, 0);
    atomic_init(&fixture.gate_started, 0);
    config = http_example_config_default();
    config.plugin_path = getenv("HTTP_EXAMPLE_PLUGIN");
  }
  after_each() {
    /* Release test-owned work even after an assertion aborts the test body. */
    atomic_store_explicit(&fixture.gate_release, 1, memory_order_release);
    fixture.app.shutdown_timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS;
    if (fixture.async_client.impl != NULL) {
      check_equal(chttp_async_client_stop(&fixture.async_client, HTTP_EXAMPLE_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_async_client_destroy(&fixture.async_client), SALTS_OK);
    }
    chttp_response_destroy(&fixture.response);
    if (fixture.client.impl != NULL)
      check_equal(chttp_client_destroy(&fixture.client, HTTP_EXAMPLE_TIMEOUT_MS), SALTS_OK);
    check_equal(http_example_close(&fixture.app), SALTS_OK);
    check_null(fixture.app.plugins.impl);
    check_false(cflow_executor_valid(&fixture.app.executor));
    tstr_free(fixture.uri);
  }

  it("serves generated scalar binding, validation and exact uint64 output") {
    example_start(&fixture, &config);
    example_get(&fixture, "/api/add?left=3&right=4", 200u);
    check_equal(fixture.response.body_size, 1u);
    check_equal(fixture.response.body, "7", 1u);
    check_equal(chttp_response_header(&fixture.response, "Cache-Control"), "no-store");
    check_equal(chttp_response_header(&fixture.response, "X-Example"), "rpc-middleware");
    check_equal(chttp_response_header(&fixture.response, "X-Content-Type-Options"), "nosniff");
    example_get(&fixture, "/api/add?left=4294967295&right=4294967295", 200u);
    check_equal(fixture.response.body_size, 10u);
    check_equal(fixture.response.body, "8589934590", 10u);
    example_get(&fixture, "/api/add?left=0&right=4", 422u);
    example_get(&fixture, "/api/add?left=3", 400u);
    example_get(&fixture, "/api/add?left=word&right=4", 400u);
    example_get(&fixture, "/api/add?left=4294967296&right=4", 400u);
    example_get(&fixture, "/missing", 404u);
    check_equal(chttp_response_header(&fixture.response, "X-Example"), "rpc-middleware");
  }

  it("releases the stopped application and plugin despite a retained runtime error") {
    example_start(&fixture, &config);
    example_get(&fixture, "/api/add?left=3&right=4", 200u);
    check_equal(crpc_server_stop(&fixture.app.rpc, HTTP_EXAMPLE_TIMEOUT_MS), SALTS_OK);
    /* Model the documented stopped-with-error state, without a platform I/O
     * failure hook. The real RPC/server, executor and DSO still own resources. */
    chttp_server_impl *server = crpc_server_http(&fixture.app.rpc)->impl;
    cmeta_mutex_lock(&server->mutex);
    server->stats.terminal_status = SALTS_EIO;
    cmeta_mutex_unlock(&server->mutex);
    check_equal(http_example_close(&fixture.app), SALTS_EIO);
    check_null(fixture.app.rpc.impl);
    check_null(fixture.app.service.impl);
    check_null(fixture.app.plugins.impl);
    check_null(fixture.app.contract);
    check_false(cflow_executor_valid(&fixture.app.executor));
    check_equal(http_example_close(&fixture.app), SALTS_OK);
    check_equal(http_example_configure(&fixture.app, &config), SALTS_OK);
  }

  it("renders inherited Jinja templates with copied and HTML-escaped IDL data") {
    char title[] = "<Admin & Users>";
    config.title = title;
    example_start(&fixture, &config);
    title[0] = 'X'; /* Configurator owns a copy, not this caller buffer. */
    example_get(&fixture, "/", 200u);
    check_equal(chttp_response_header(&fixture.response, "Content-Type"), "text/html; charset=utf-8");
    check_equal(chttp_response_header(&fixture.response, "Cache-Control"), "no-store");
    check_true(example_contains(&fixture.response, "<!doctype html>"));
    check_true(example_contains(&fixture.response, "&lt;Admin &amp; Users&gt;"));
    check_false(example_contains(&fixture.response, "<Admin & Users>"));
    check_true(example_contains(&fixture.response, "3 + 4 = 7"));
    check_true(example_contains(&fixture.response, "action=\"/api/add\""));
    check_false(example_contains(&fixture.response, "href=\"/file\""));
    example_get(&fixture, "/file", 404u);
  }

  it("preserves JSON-RPC ping and CORS") {
    static const char body[] = "{\"jsonrpc\":\"2.0\",\"method\":\"example.ping\",\"id\":1}";
    const chttp_header headers[] = {{"Content-Type", "application/json"},
        {"Origin", "http://localhost:3000"}};
    chttp_error error = {0};
    example_start(&fixture, &config);
    const chttp_options options = {.connection_uri = fixture.uri, .authority = "127.0.0.1",
        .target = "/rpc", .timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS,
        .headers = headers, .header_count = 2u, .body = body, .body_size = sizeof(body) - 1u};
    check_equal(chttp_post(&fixture.client, &options, &fixture.response, &error), SALTS_OK);
    check_equal(fixture.response.status_code, 200u);
    check_true(example_contains(&fixture.response, "\"result\":null"));
    check_true(example_contains(&fixture.response, "\"id\":1"));
    check_equal(chttp_response_header(&fixture.response, "Access-Control-Allow-Origin"),
        "http://localhost:3000");
    check_equal(chttp_response_header(&fixture.response, "X-Example"), "rpc-middleware");
  }

  it("retains the optional file endpoint and its conditional caching") {
    chttp_error error = {0};
    config.file_path = HTTP_EXAMPLE_TEST_FILE;
    example_start(&fixture, &config);
    example_get(&fixture, "/", 200u);
    check_true(example_contains(&fixture.response, "href=\"/file\""));
    example_get(&fixture, "/file", 200u);
    check_true(example_contains(&fixture.response, "schema HttpExample"));
    check_null(chttp_response_header(&fixture.response, "Cache-Control"));
    check_not_null(chttp_response_header(&fixture.response, "ETag"));
    /* Keep the first response alive while the conditional request borrows its ETag. */
    const chttp_header header = {"If-None-Match", chttp_response_header(&fixture.response, "ETag")};
    const chttp_options options = {.connection_uri = fixture.uri, .authority = "127.0.0.1",
        .target = "/file", .timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS,
        .headers = &header, .header_count = 1u};
    chttp_response conditional = {0};
    check_equal(chttp_get(&fixture.client, &options, &conditional, &error), SALTS_OK);
    check_equal(conditional.status_code, 304u);
    check_equal(conditional.body_size, 0u);
    chttp_response_destroy(&conditional);
  }

  it("applies configured admission before middleware or business handlers") {
    config.rate.burst = 1u;
    config.rate.refill_tokens = 1u;
    config.rate.refill_period_ms = 60000u;
    example_start(&fixture, &config);
    example_get(&fixture, "/api/add?left=3&right=4", 200u);
    example_get(&fixture, "/", 429u);
    check_not_null(chttp_response_header(&fixture.response, "Retry-After"));
    check_null(chttp_response_header(&fixture.response, "X-Example"));
  }

  it("rolls back a late configuration failure and allows a clean retry") {
    config.rate.burst = 0u;
    check_equal(http_example_configure(&fixture.app, &config), SALTS_EINVAL);
    check_null(fixture.app.rpc.impl);
    check_null(fixture.app.service.impl);
    check_null(fixture.app.renderer.impl);
    check_null(fixture.app.contract);
    check_null(fixture.app.page.title);
    check_null(fixture.app.plugins.impl);
    check_false(cflow_executor_valid(&fixture.app.executor));
    config.rate.burst = 100u;
    example_start(&fixture, &config);
    check_equal(http_example_configure(&fixture.app, &config), SALTS_EALREADY);
    example_get(&fixture, "/", 200u);
  }

  it("rejects a missing DSO without falling back and can retry a valid provider") {
    config.plugin_path = HTTP_EXAMPLE_PLUGIN_PATH ".missing";
    check_equal(http_example_configure(&fixture.app, &config), SALTS_EIO);
    check_null(fixture.app.plugins.impl);
    check_null(fixture.app.rpc.impl);
    config.plugin_path = HTTP_EXAMPLE_PLUGIN_PATH;
    example_start(&fixture, &config);
    example_get(&fixture, "/api/add?left=3&right=4", 200u);
    check_equal(fixture.response.body, "7", 1u);
  }

  it("rejects the same operation ID with incompatible result storage") {
    config.plugin_path = HTTP_EXAMPLE_BAD_PLUGIN_PATH;
    check_equal(http_example_configure(&fixture.app, &config), SALTS_EINVAL);
    check_null(fixture.app.plugins.impl);
    check_null(fixture.app.rpc.impl);
    check_false(cflow_executor_valid(&fixture.app.executor));
    config.plugin_path = HTTP_EXAMPLE_PLUGIN_PATH;
    example_start(&fixture, &config);
    example_get(&fixture, "/api/add?left=4294967295&right=4294967295", 200u);
    check_equal(fixture.response.body_size, 10u);
    check_equal(fixture.response.body, "8589934590", 10u);
  }

  it("retains the mounted DSO across queued work and a timed-out close") {
    cmeta_plugin_lifecycle_info lifecycle = {0};
    chttp_request request = {0};
    config.plugin_path = HTTP_EXAMPLE_PLUGIN_PATH;
    config.shutdown_timeout_ms = 20u;
    example_start(&fixture, &config);
    check_equal(cmeta_plugin_registry_get_lifecycle(&fixture.app.plugins,
        fixture.app.plugin_ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, 1u);
    const cflow_executor_task gate = {.run = example_gate, .user = &fixture};
    check_equal(cflow_executor_try_post_task(&fixture.app.executor, &gate), CFLOW_ADMISSION_ACCEPTED);
    check_equal(chttp_async_client_init(&fixture.async_client, &fixture.client_config), SALTS_OK);
    const chttp_request_options options = {.connection_uri = fixture.uri, .authority = "127.0.0.1",
        .target = "/api/add?left=3&right=4", .method = CHTTP_METHOD_GET,
        .on_complete = example_completed, .user = &fixture};
    check_equal(chttp_async_client_submit(&fixture.async_client, &options, &request), SALTS_OK);
    const uint64_t started = cmeta_monotonic_ms();
    while (cflow_executor_pending(&fixture.app.executor) < 2u &&
           cmeta_monotonic_ms() - started < HTTP_EXAMPLE_TIMEOUT_MS) {
      size_t completed = 0u;
      check_equal(chttp_async_client_poll(&fixture.async_client, 1u, &completed), SALTS_OK);
    }
    check_equal(cflow_executor_pending(&fixture.app.executor), 2u);
    check_equal(cmeta_plugin_registry_request_stop(&fixture.app.plugins, fixture.app.plugin_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_unload(&fixture.app.plugins, fixture.app.plugin_ref), CMETA_PLUGIN_BUSY);
    check_equal(http_example_close(&fixture.app), SALTS_ETIMEDOUT);
    check_not_null(fixture.app.rpc.impl);
    check_not_null(fixture.app.service.impl);
    check_not_null(fixture.app.plugins.impl);
    check_equal(cmeta_plugin_registry_get_lifecycle(&fixture.app.plugins,
        fixture.app.plugin_ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, 1u);
    atomic_store_explicit(&fixture.gate_release, 1, memory_order_release);
    check_true(cflow_executor_wait_idle(&fixture.app.executor));
    check_equal(http_example_close(&fixture.app), SALTS_OK);
    check_null(fixture.app.plugins.impl);
    check_null(fixture.app.service.impl);
    check_equal(chttp_async_client_stop(&fixture.async_client, HTTP_EXAMPLE_TIMEOUT_MS), SALTS_OK);
    check_equal(fixture.completions, 1u);
  }

  it("rejects requests with 503 when the bounded plugin executor is full") {
    config.plugin_path = HTTP_EXAMPLE_PLUGIN_PATH;
    example_start(&fixture, &config);
    const cflow_executor_task gate = {.run = example_gate, .user = &fixture};
    check_equal(cflow_executor_try_post_task(&fixture.app.executor, &gate), CFLOW_ADMISSION_ACCEPTED);
    const uint64_t started = cmeta_monotonic_ms();
    while (!atomic_load_explicit(&fixture.gate_started, memory_order_acquire) &&
           cmeta_monotonic_ms() - started < HTTP_EXAMPLE_TIMEOUT_MS)
      cmeta_sleep_ms(1u);
    check_equal(atomic_load_explicit(&fixture.gate_started, memory_order_acquire), 1);
    for (size_t index = 0u; index < HTTP_EXAMPLE_PLUGIN_QUEUE_CAPACITY; ++index)
      check_equal(cflow_executor_try_post_task(&fixture.app.executor, &gate), CFLOW_ADMISSION_ACCEPTED);
    example_get(&fixture, "/api/add?left=3&right=4", 503u);
    atomic_store_explicit(&fixture.gate_release, 1, memory_order_release);
    check_true(cflow_executor_wait_idle(&fixture.app.executor));
    example_get(&fixture, "/api/add?left=3&right=4", 200u);
    check_equal(fixture.response.body, "7", 1u);
  }
}
