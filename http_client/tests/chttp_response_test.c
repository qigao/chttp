#include "chttp_internal.h"
#include "tinytest.h"

#include <string.h>

typedef struct chttp_response_sink_probe {
  char data[64];
  size_t size;
  size_t calls;
  int status;
} chttp_response_sink_probe;

static int chttp_response_test_sink(void *user, const void *data, size_t size) {
  chttp_response_sink_probe *probe = (chttp_response_sink_probe *)user;
  if (probe == NULL || (size != 0u && data == NULL) || size > sizeof(probe->data) - probe->size)
    return SALTS_EINVAL;
  ++probe->calls;
  if (probe->status != SALTS_OK) return probe->status;
  memcpy(probe->data + probe->size, data, size);
  probe->size += size;
  return SALTS_OK;
}

static chttp_limits chttp_response_test_limits(void) {
  const chttp_limits limits = {.max_start_line_bytes = 128u,
                               .max_header_count = 8u,
                               .max_header_bytes = 256u,
                               .max_request_body_bytes = 64u,
                               .max_response_body_bytes = 64u,
                               .max_informational_responses = 2u,
                               .max_request_bytes = 512u};
  return limits;
}

spec("CHTTP strict incremental response parser") {
  it("assembles fragmented headers and a fixed-length body") {
    static const char input[] = "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/plain\r\n"
                                "X-Test: split\r\n"
                                "Content-Length: 5\r\n"
                                "\r\n"
                                "hello";
    chttp_limits limits = chttp_response_test_limits();
    chttp_response_parser parser;
    size_t index;

    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
    for (index = 0u; index < sizeof(input) - 1u; ++index)
      check_equal(chttp_response_parser_execute(&parser, input + index, 1u), SALTS_OK);
    check_true(parser.complete);
    check_equal(parser.response.http_major, 1u);
    check_equal(parser.response.http_minor, 1u);
    check_equal(parser.response.status_code, 200u);
    check_equal(parser.response.reason, "OK");
    check_equal(parser.response.header_count, (size_t)3u);
    check_equal(chttp_response_view_header(&parser.response, "content-type"), "text/plain");
    check_equal(parser.response.body_size, (size_t)5u);
    check_equal(parser.response.body, "hello", 5u);
    chttp_response_parser_destroy(&parser);
  }

  it("accepts bounded informational and chunked responses") {
    static const char input[] = "HTTP/1.1 100 Continue\r\n\r\n"
                                "HTTP/1.1 201 Created\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n";
    chttp_limits limits = chttp_response_test_limits();
    chttp_response_parser parser;

    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_POST, &limits), SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, input, sizeof(input) - 1u), SALTS_OK);
    check_true(parser.complete);
    check_equal(parser.informational_responses, (size_t)1u);
    check_equal(parser.response.status_code, 201u);
    check_equal(parser.response.reason, "Created");
    check_equal(parser.response.body, "hello", 5u);
    chttp_response_parser_destroy(&parser);
  }

  it("finishes an EOF-delimited HTTP/1.0 body") {
    static const char input[] = "HTTP/1.0 200 OK\r\n\r\nlegacy";
    chttp_limits limits = chttp_response_test_limits();
    chttp_response_parser parser;

    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, input, sizeof(input) - 1u), SALTS_OK);
    check_false(parser.complete);
    check_equal(chttp_response_parser_finish(&parser), SALTS_OK);
    check_true(parser.complete);
    check_equal(parser.response.body, "legacy", 6u);
    chttp_response_parser_destroy(&parser);
  }

  it("treats a HEAD response as bodyless") {
    static const char input[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 999\r\nConnection: close\r\n\r\n";
    chttp_limits limits = chttp_response_test_limits();
    chttp_response_parser parser;

    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_HEAD, &limits), SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, input, sizeof(input) - 1u), SALTS_OK);
    check_true(parser.complete);
    check_equal(parser.response.body_size, (size_t)0u);
    chttp_response_parser_destroy(&parser);
  }

  it("fails when headers or body exceed configured bounds") {
    static const char oversized_header[] =
        "HTTP/1.1 200 OK\r\nX-Long: abcdefghijklmnop\r\nContent-Length: 0\r\n\r\n";
    static const char oversized_body[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    chttp_limits limits = chttp_response_test_limits();
    chttp_response_parser parser;

    limits.max_header_bytes = 16u;
    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
    check_equal(
        chttp_response_parser_execute(&parser, oversized_header, sizeof(oversized_header) - 1u),
        SALTS_EMSGSIZE);
    chttp_response_parser_destroy(&parser);

    limits = chttp_response_test_limits();
    limits.max_response_body_bytes = 4u;
    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, oversized_body, sizeof(oversized_body) - 1u),
                SALTS_EMSGSIZE);
    chttp_response_parser_destroy(&parser);
  }

  it("rejects ambiguous framing truncation and bytes after the final response") {
    static const char conflicting[] = "HTTP/1.1 200 OK\r\n"
                                      "Content-Length: 5\r\n"
                                      "Transfer-Encoding: chunked\r\n\r\n"
                                      "0\r\n\r\n";
    static const char truncated[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc";
    static const char trailing[] = "HTTP/1.1 204 No Content\r\n\r\nunexpected";
    chttp_limits limits = chttp_response_test_limits();
    chttp_response_parser parser;

    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, conflicting, sizeof(conflicting) - 1u),
                SALTS_EPROTO);
    chttp_response_parser_destroy(&parser);

    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, truncated, sizeof(truncated) - 1u),
                SALTS_OK);
    check_equal(chttp_response_parser_finish(&parser), SALTS_EPROTO);
    chttp_response_parser_destroy(&parser);

    check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, trailing, sizeof(trailing) - 1u),
                SALTS_EPROTO);
    chttp_response_parser_destroy(&parser);
  }

  it("delivers body fragments to a bounded sink without retaining a body copy") {
    static const char first[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe";
    static const char second[] = "llo";
    chttp_response_sink_probe probe = {0};
    const chttp_body_sink sink = {.write = chttp_response_test_sink, .user = &probe};
    chttp_limits limits = chttp_response_test_limits();
    chttp_response_parser parser;

    check_equal(chttp_response_parser_init_with_sink(&parser, CHTTP_METHOD_GET, &limits, &sink),
                SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, first, sizeof(first) - 1u), SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, second, sizeof(second) - 1u), SALTS_OK);
    check_true(parser.complete);
    check_equal(probe.data, "hello", 5u);
    check_equal(probe.size, (size_t)5u);
    check_equal(probe.calls, (size_t)2u);
    check_null(parser.response.body);
    check_equal(parser.response.body_size, (size_t)5u);
    chttp_response_parser_destroy(&parser);

    probe = (chttp_response_sink_probe){.status = SALTS_EIO};
    check_equal(chttp_response_parser_init_with_sink(&parser, CHTTP_METHOD_GET, &limits, &sink),
                SALTS_OK);
    check_equal(chttp_response_parser_execute(&parser, first, sizeof(first) - 1u), SALTS_EIO);
    check_equal(parser.failure_stage, "response-sink");
    check_equal(probe.calls, (size_t)1u);
    chttp_response_parser_destroy(&parser);
  }
}
