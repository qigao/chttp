#include <http_client/http.h>
#include <http_server/http.h>
#include <salts/clock.h>
#include "tinytest.h"

#include <stdio.h>
#include <string.h>

enum { CORS_TEST_TIMEOUT_MS = 5000, CORS_TEST_BUFFER_BYTES = 4096,
       CORS_TEST_H2_BYTES = 64 * 1024 };
static const char test_origin[] = "https://app.example";
static const char origin_header[] = "Origin";
static const char method_header[] = "Access-Control-Request-Method";
static const char headers_header[] = "Access-Control-Request-Headers";

static cnet_client_config cors_network(void) {
  const cnet_client_config config = {
#if defined(_WIN32)
      .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
      .backend = NATIVE_IO_BACKEND_EPOLL,
#else
      .backend = NATIVE_IO_BACKEND_KQUEUE,
#endif
      .connection_capacity = 4, .command_capacity = 16, .request_capacity = 8,
      .completion_batch_capacity = 8, .event_capacity = 16,
      .max_send_bytes = CORS_TEST_H2_BYTES, .receive_buffer_bytes = CORS_TEST_BUFFER_BYTES,
      .connect_timeout_ms = CORS_TEST_TIMEOUT_MS, .read_timeout_ms = CORS_TEST_TIMEOUT_MS,
      .write_timeout_ms = CORS_TEST_TIMEOUT_MS};
  return config;
}

static chttp_server_config cors_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1", .backlog = 8, .network = cors_network(),
      .route_capacity = 4, .middleware_capacity = 4, .max_route_middleware_count = 4,
      .max_route_param_count = 4, .max_route_param_bytes = 128, .max_target_bytes = 256,
      .max_header_count = 16, .max_header_bytes = CORS_TEST_BUFFER_BYTES,
      .max_request_body_bytes = CORS_TEST_BUFFER_BYTES, .max_response_header_count = 16,
      .max_response_header_bytes = CORS_TEST_BUFFER_BYTES,
      .max_response_body_bytes = CORS_TEST_BUFFER_BYTES, .poll_slice_ms = 2,
      .enable_http2 = 1, .h2_stream_capacity = 4, .h2_input_buffer_bytes = CORS_TEST_H2_BYTES,
      .h2_output_buffer_bytes = CORS_TEST_H2_BYTES, .h2_hpack_dynamic_table_bytes = 4096,
      .h2_max_settings_count = 16};
  return config;
}

static int cors_handler(void *user, const chttp_server_request_view *request,
                          chttp_server_response *response) {
  (void)user;
  (void)request;
  return chttp_server_reply(response, 200u, "text/plain", "handler", 7u);
}

static int cors_prior_middleware(void *user, const chttp_server_request_view *request,
                                  chttp_server_response *response, chttp_server_next *next) {
  (void)user;
  (void)request;
  const int status = chttp_server_response_set_header(response, "Vary", "Accept-Encoding");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

typedef struct cors_case {
  chttp_method method;
  chttp_header headers[4];
  size_t header_count;
  unsigned int status;
  int shared;
  int h1_only;
} cors_case;

typedef struct cors_result {
  const cors_case *test;
  const char *allowed_origin;
  int credentials;
  int done;
  int status;
  chttp_response_view response;
  chttp_header headers[32];
  char header_storage[CORS_TEST_BUFFER_BYTES];
  unsigned char body[32];
} cors_result;

static void cors_complete(void *user, chttp_request request,
                            const chttp_response_view *response, const chttp_error *error) {
  cors_result *result = (cors_result *)user;
  (void)request;
  result->done = 1;
  result->status = error == NULL ? SALTS_OK : error->status;
  if (result->status != SALTS_OK) return;
  if (response == NULL) {
    result->status = SALTS_EPROTO;
    return;
  }
  if (response->header_count > sizeof(result->headers) / sizeof(result->headers[0]) ||
      response->body_size > sizeof(result->body)) {
    result->status = SALTS_ENOBUFS;
    return;
  }
  result->response = *response;
  result->response.headers = result->headers;
  result->response.body = result->body;
  size_t used = 0;
  for (size_t index = 0; index < response->header_count; ++index) {
    const chttp_header *header = &response->headers[index];
    const size_t name_size = strlen(header->name) + 1u;
    const size_t value_size = strlen(header->value) + 1u;
    if (name_size + value_size > sizeof(result->header_storage) - used) {
      result->status = SALTS_ENOBUFS;
      return;
    }
    result->headers[index].name = result->header_storage + used;
    memcpy(result->header_storage + used, header->name, name_size);
    used += name_size;
    result->headers[index].value = result->header_storage + used;
    memcpy(result->header_storage + used, header->value, value_size);
    used += value_size;
  }
  if (response->body_size != 0) memcpy(result->body, response->body, response->body_size);
}

/* Assertions run after poll, outside the client's callback/coroutine stack. */
static void cors_check_result(const cors_result *result) {
  const chttp_response_view *response = &result->response;
  check_equal(result->status, SALTS_OK);
  check_equal(response->status_code, result->test->status);
  if (response->status_code == 500u) {
    check_null(chttp_response_view_header(response, "Access-Control-Allow-Origin"));
    check_null(chttp_response_view_header(response, "Access-Control-Allow-Credentials"));
    check_not_null(response->body);
    return;
  }
  check_equal(response->body_size, result->test->status == 200u ? 7u : 0u);
  const char *vary = chttp_response_view_header(response, "Vary");
  check_not_null(vary);
  if (vary != NULL) {
    check_not_null(strstr(vary, "Accept-Encoding"));
    check_not_null(strstr(vary, "Origin"));
  }
  const char *allowed = chttp_response_view_header(response, "Access-Control-Allow-Origin");
  if (!result->test->shared) {
    check_null(allowed);
    return;
  }
  check_equal(allowed, result->allowed_origin);
  const char *credentials = chttp_response_view_header(response, "Access-Control-Allow-Credentials");
  if (result->credentials) check_equal(credentials, "true");
  else check_null(credentials);
  if (response->status_code == 204u) {
    check_equal(chttp_response_view_header(response, "Access-Control-Allow-Methods"), "GET, POST");
    check_equal(chttp_response_view_header(response, "Access-Control-Allow-Headers"),
                "Content-Type, Authorization");
    check_equal(chttp_response_view_header(response, "Access-Control-Max-Age"), "600");
    if (vary != NULL) {
      check_not_null(strstr(vary, "Access-Control-Request-Method"));
      check_not_null(strstr(vary, "Access-Control-Request-Headers"));
    }
  } else {
    check_equal(chttp_response_view_header(response, "Access-Control-Expose-Headers"), "ETag");
    check_null(chttp_response_view_header(response, "Access-Control-Allow-Methods"));
  }
}

spec("CHTTP CORS") {
  it("validates policies before consuming middleware capacity") {
    chttp_server server = {0};
    chttp_server_config config = cors_server_config();
    const char *origins[] = {"*"};
    chttp_server_cors_options options = {.origins = origins, .origin_count = 1,
                                         .methods = "GET", .allow_credentials = 1};
    config.middleware_capacity = 1;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_use_cors(&server, &options), SALTS_EINVAL);
    options.allow_credentials = 0;
    options.allowed_headers = "*";
    check_equal(chttp_server_use_cors(&server, &options), SALTS_EINVAL);
    options.allowed_headers = "Authorization\r\nInjected: true";
    check_equal(chttp_server_use_cors(&server, &options), SALTS_EINVAL);
    options.allowed_headers = NULL;
    origins[0] = "https://*.example";
    check_equal(chttp_server_use_cors(&server, &options), SALTS_EINVAL);
    origins[0] = test_origin;
    options.methods = "GET POST";
    check_equal(chttp_server_use_cors(&server, &options), SALTS_EINVAL);
    options.methods = "GET";
    check_equal(chttp_server_use_cors(&server, &options), SALTS_OK);
    check_equal(chttp_server_use_cors(&server, &options), SALTS_ENOBUFS);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("applies CORS and preflight decisions consistently over reusable H1 and H2 connections") {
    const cors_case cases[] = {
      {CHTTP_METHOD_GET, {{0}}, 0, 200, 0},
      {CHTTP_METHOD_GET, {{origin_header, test_origin}}, 1, 200, 1},
      {CHTTP_METHOD_GET, {{origin_header, "https://app.example \t"}}, 1, 200, 1, 1},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST \t"}}, 2, 204, 1, 1},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}}, 1, 200, 1},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST"}}, 2, 204, 1},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST"},
         {headers_header, "content-type, AUTHORIZATION"}}, 3, 204, 1},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST"},
         {headers_header, "Content-Type"}, {headers_header, "Authorization"}}, 4, 204, 1},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "post"}}, 2, 403, 0},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "DELETE"}}, 2, 403, 0},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST"},
         {headers_header, "X-Secret"}}, 3, 403, 0},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST"},
         {headers_header, "Content-Type"}, {headers_header, "X-Secret"}}, 4, 403, 0},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST, GET"}}, 2, 400, 0},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST"},
         {method_header, "GET"}}, 3, 400, 0},
      {CHTTP_METHOD_OPTIONS, {{origin_header, test_origin}, {method_header, "POST"},
         {headers_header, "[bad]"}}, 3, 400, 0},
      {CHTTP_METHOD_GET, {{origin_header, test_origin}, {origin_header, test_origin}}, 2, 400, 0},
      {CHTTP_METHOD_GET, {{origin_header, "https://app.example https://other.example"}}, 1, 400, 0},
    };
    for (int mode = 0; mode < 3; ++mode) {
      const int wildcard = mode == 1;
      const int quota = mode == 2;
      chttp_server server = {0};
      chttp_server_config config = cors_server_config();
      if (quota) config.max_response_header_count = 2;
      const char *origins[] = {wildcard ? "*" : test_origin};
      const chttp_server_cors_options policy = {.origins = origins, .origin_count = 1,
          .methods = "GET, POST", .allowed_headers = "Content-Type, Authorization",
          .exposed_headers = "ETag", .max_age_seconds = 600, .allow_credentials = !wildcard};
      chttp_async_client client = {0};
      const chttp_client_config client_config = {.network = cors_network(), .request_capacity = 2,
          .max_start_line_bytes = 256, .max_header_count = 16, .max_header_bytes = CORS_TEST_BUFFER_BYTES,
          .max_request_body_bytes = CORS_TEST_BUFFER_BYTES, .max_response_body_bytes = CORS_TEST_BUFFER_BYTES,
          .max_informational_responses = 2, .h2_input_buffer_bytes = CORS_TEST_H2_BYTES,
          .h2_hpack_dynamic_table_bytes = 4096, .h2_max_settings_count = 16};
      uint16_t port = 0;
      char uri[64];
      check_equal(chttp_server_init(&server, &config), SALTS_OK);
      check_equal(chttp_server_use(&server, cors_prior_middleware, NULL), SALTS_OK);
      check_equal(chttp_server_use_cors(&server, &policy), SALTS_OK);
      check_equal(chttp_server_get(&server, "/", cors_handler, NULL), SALTS_OK);
      check_equal(chttp_server_options(&server, "/", cors_handler, NULL), SALTS_OK);
      check_equal(chttp_server_start(&server), SALTS_OK);
      check_equal(chttp_server_use_cors(&server, &policy), SALTS_EBUSY);
      check_equal(chttp_server_port(&server, &port), SALTS_OK);
      (void)snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port);
      check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
      for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
        const cors_case unknown_origin = {CHTTP_METHOD_GET,
            {{origin_header, "https://app.example.attacker.test"}}, 1, wildcard ? 200u : 403u, wildcard};
        for (size_t index = 0; index <= sizeof(cases) / sizeof(cases[0]); ++index) {
          cors_case test = index == sizeof(cases) / sizeof(cases[0]) ? unknown_origin : cases[index];
          if (test.h1_only && protocol == CHTTP_HTTP_2) continue;
          if (quota && test.shared) test.status = 500u;
          cors_result result = {.test = &test, .allowed_origin = origins[0], .credentials = !wildcard};
          chttp_request request = {0};
          const chttp_request_options options = {.connection_uri = uri, .authority = "127.0.0.1",
              .target = "/", .method = test.method, .headers = test.headers, .header_count = test.header_count,
              .protocol = (chttp_protocol)protocol, .on_complete = cors_complete, .user = &result};
          check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_OK);
          const uint64_t deadline = salts_monotonic_ms() + CORS_TEST_TIMEOUT_MS;
          while (!result.done && salts_monotonic_ms() < deadline) {
            size_t completions = 0;
            check_equal(chttp_async_client_poll(&client, 10u, &completions), SALTS_OK);
          }
          check_equal(result.done, 1);
          cors_check_result(&result);
        }
      }
      check_equal(chttp_async_client_stop(&client, CORS_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_async_client_destroy(&client), SALTS_OK);
      chttp_server_stats stats = {0};
      check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
      check_equal(stats.accepted_connections, 2u);
      check_equal(chttp_server_stop(&server, CORS_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_server_destroy(&server), SALTS_OK);
    }
  }
}
