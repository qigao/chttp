#include "chttp_internal.h"
#include "tinytest.h"

#include <stdlib.h>
#include <string.h>

static void chttp_request_test_complete(void *user, chttp_request request,
                                        const chttp_response_view *response,
                                        const chttp_error *error) {
  (void)user;
  (void)request;
  (void)response;
  (void)error;
}

static int chttp_request_test_read(void *user, void *buffer, size_t capacity, size_t *out_size) {
  (void)user;
  (void)buffer;
  (void)capacity;
  if (out_size == NULL) return SALTS_EINVAL;
  *out_size = 0u;
  return SALTS_OK;
}

static chttp_limits chttp_request_test_limits(void) {
  const chttp_limits limits = {.max_start_line_bytes = 128u,
                               .max_header_count = 8u,
                               .max_header_bytes = 256u,
                               .max_request_body_bytes = 64u,
                               .max_response_body_bytes = 64u,
                               .max_informational_responses = 2u,
                               .max_request_bytes = 512u};
  return limits;
}

spec("CHTTP bounded request serializer") {
  it("serializes an origin-form request and copies its body") {
    static const char expected[] = "POST /items?mode=fast HTTP/1.1\r\n"
                                   "Host: example.test:8080\r\n"
                                   "Content-Length: 5\r\n"
                                   "Connection: keep-alive\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "\r\n"
                                   "hello";
    const chttp_header headers[] = {{"Content-Type", "text/plain"}};
    const chttp_request_options options = {.connection_uri = "tcp://127.0.0.1:8080",
                                           .authority = "example.test:8080",
                                           .target = "/items?mode=fast",
                                           .method = CHTTP_METHOD_POST,
                                           .headers = headers,
                                           .header_count = 1u,
                                           .body = "hello",
                                           .body_size = 5u,
                                           .on_complete = chttp_request_test_complete};
    chttp_limits limits = chttp_request_test_limits();
    unsigned char *data = NULL;
    size_t size = 0u;

    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_OK);
    check_equal(size, sizeof(expected) - 1u);
    check_equal(data, expected, size);
    free(data);
  }

  it("rejects framing headers and header injection") {
    const chttp_header reserved[] = {{"Content-Length", "7"}};
    const chttp_header injected[] = {{"X-Value", "safe\r\nInjected: yes"}};
    chttp_request_options options = {.connection_uri = "tcp://127.0.0.1:80",
                                     .authority = "example.test",
                                     .target = "/",
                                     .method = CHTTP_METHOD_GET,
                                     .headers = reserved,
                                     .header_count = 1u,
                                     .on_complete = chttp_request_test_complete};
    chttp_limits limits = chttp_request_test_limits();
    unsigned char *data = NULL;
    size_t size = 0u;

    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EINVAL);
    options.headers = injected;
    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EINVAL);
    check_null(data);
    check_equal(size, (size_t)0u);
  }

  it("enforces start-line header and body bounds") {
    const chttp_header headers[] = {{"X-Test", "value"}};
    chttp_request_options options = {.connection_uri = "tcp://127.0.0.1:80",
                                     .authority = "example.test",
                                     .target = "/resource",
                                     .method = CHTTP_METHOD_POST,
                                     .headers = headers,
                                     .header_count = 1u,
                                     .body = "body",
                                     .body_size = 4u,
                                     .on_complete = chttp_request_test_complete};
    chttp_limits limits = chttp_request_test_limits();
    unsigned char *data = NULL;
    size_t size = 0u;

    limits.max_start_line_bytes = 16u;
    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EMSGSIZE);
    limits = chttp_request_test_limits();
    limits.max_header_count = 3u;
    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EMSGSIZE);
    limits = chttp_request_test_limits();
    limits.max_request_body_bytes = 3u;
    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EMSGSIZE);
  }

  it("validates a borrowed streaming source before admission") {
    chttp_body_source source = {
        .read = chttp_request_test_read, .content_length = 65u, .content_length_known = 1};
    chttp_request_options options = {.connection_uri = "tcp://127.0.0.1:80",
                                     .authority = "example.test",
                                     .target = "/upload",
                                     .method = CHTTP_METHOD_POST,
                                     .body_source = &source,
                                     .on_complete = chttp_request_test_complete};
    chttp_limits limits = chttp_request_test_limits();
    unsigned char *data = NULL;
    size_t size = 0u;

    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EMSGSIZE);
    source.content_length = 4u;
    options.body = "body";
    options.body_size = 4u;
    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EINVAL);
    options.body = NULL;
    options.body_size = 0u;
    source.read = NULL;
    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EINVAL);
    source.read = chttp_request_test_read;
    source.content_length_known = 2;
    check_equal(chttp_request_build(&options, &limits, &data, &size), SALTS_EINVAL);
    check_null(data);
    check_equal(size, (size_t)0u);
  }
}
