#include "chttp_server_internal.h"
#include "tinytest.h"

#include <stdint.h>
#include <string.h>

typedef struct chttp_server_parser_probe {
  int requests;
  int continues;
  int headers;
  int body_opens;
  int stop_at_headers;
  chttp_method method;
  unsigned int http_major;
  unsigned int http_minor;
  int keep_alive;
  char target[128];
  char path[128];
  char host[128];
  unsigned char body[128];
  size_t body_size;
  int upgrades;
} chttp_server_parser_probe;

static int chttp_server_parser_test_request(void *user, const chttp_server_request_view *request) {
  chttp_server_parser_probe *probe = (chttp_server_parser_probe *)user;
  const char *host = chttp_server_request_header(request, "host");
  ++probe->requests;
  probe->method = request->method;
  probe->http_major = request->http_major;
  probe->http_minor = request->http_minor;
  probe->keep_alive = request->protocol_keep_alive;
  (void)strncpy(probe->target, request->target, sizeof(probe->target) - 1u);
  (void)strncpy(probe->path, request->path, sizeof(probe->path) - 1u);
  if (host != NULL) (void)strncpy(probe->host, host, sizeof(probe->host) - 1u);
  probe->body_size = request->body_size;
  if (request->body_size <= sizeof(probe->body))
    memcpy(probe->body, request->body, request->body_size);
  return SALTS_OK;
}

static int chttp_server_parser_test_continue(void *user) {
  chttp_server_parser_probe *probe = (chttp_server_parser_probe *)user;
  ++probe->continues;
  return SALTS_OK;
}

static int chttp_server_parser_test_headers(void *user,
                                            const chttp_server_request_view *request,
                                            chttp_server_parser_headers_action *out_action) {
  chttp_server_parser_probe *probe = (chttp_server_parser_probe *)user;
  (void)request;
  ++probe->headers;
  *out_action = probe->stop_at_headers ? CHTTP_SERVER_HEADERS_STOP
                                       : CHTTP_SERVER_HEADERS_CONTINUE;
  return SALTS_OK;
}

static int chttp_server_parser_test_body_open(void *user,
                                              const chttp_server_request_view *request,
                                              chttp_body_sink *out_sink) {
  chttp_server_parser_probe *probe = (chttp_server_parser_probe *)user;
  (void)request;
  ++probe->body_opens;
  *out_sink = (chttp_body_sink){0};
  return SALTS_OK;
}

static void chttp_server_parser_test_body_close(void *user, chttp_body_sink *sink, int status) {
  (void)user;
  (void)sink;
  (void)status;
}

static int chttp_server_parser_test_upgrade(void *user, const chttp_server_request_view *request,
                                            chttp_server_parser_upgrade_action *out_action,
                                            unsigned int *out_http_status) {
  chttp_server_parser_probe *probe = (chttp_server_parser_probe *)user;
  (void)request;
  ++probe->upgrades;
  *out_action = CHTTP_SERVER_UPGRADE_STOP;
  *out_http_status = 0u;
  return SALTS_OK;
}

static chttp_server_parser_config
chttp_server_parser_test_config(chttp_server_parser_probe *probe) {
  const chttp_server_parser_config config = {.max_target_bytes = 64u,
                                             .max_header_count = 8u,
                                             .max_header_bytes = 128u,
                                             .max_body_bytes = 64u,
                                             .on_request = chttp_server_parser_test_request,
                                             .on_continue = chttp_server_parser_test_continue,
                                             .user = probe};
  return config;
}

static int chttp_server_parser_test_execute(chttp_server_parser *parser, const char *wire,
                                            unsigned int *out_http_status) {
  return chttp_server_parser_execute(parser, wire, strlen(wire), out_http_status);
}

static int chttp_server_parser_test_execute_fragments(chttp_server_parser *parser, const char *wire,
                                                      size_t wire_size, size_t fragment_size,
                                                      unsigned int *out_http_status) {
  size_t offset = 0u;
  int status = SALTS_OK;
  while (offset < wire_size && status == SALTS_OK) {
    const size_t remaining = wire_size - offset;
    const size_t size = remaining < fragment_size ? remaining : fragment_size;
    status = chttp_server_parser_execute(parser, wire + offset, size, out_http_status);
    offset += size;
  }
  return status;
}

spec("CHTTP server request parser") {
  it("stops exactly after an accepted Upgrade and preserves coalesced frame bytes") {
    static const char request[] =
        "GET /chat HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    static const unsigned char frame[] = {0x81u, 0x82u, 0x01u, 0x02u, 0x03u, 0x04u, 0x69u, 0x6bu};
    unsigned char wire[sizeof(request) - 1u + sizeof(frame)];
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 999u;
    size_t consumed = 0u;

    memcpy(wire, request, sizeof(request) - 1u);
    memcpy(wire + sizeof(request) - 1u, frame, sizeof(frame));
    config.max_header_bytes = 256u;
    config.on_upgrade = chttp_server_parser_test_upgrade;
    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(
        chttp_server_parser_execute_consumed(&parser, wire, sizeof(wire), &consumed, &http_status),
        SALTS_OK);
    check_equal(consumed, sizeof(request) - 1u);
    check_equal(http_status, 0u);
    check_equal(probe.upgrades, 1);
    check_equal(probe.requests, 0);
    check_equal(memcmp(wire + consumed, frame, sizeof(frame)), 0);
    check_equal(chttp_server_parser_execute_consumed(&parser, frame, sizeof(frame), &consumed,
                                                     &http_status),
                SALTS_ESHUTDOWN);
    chttp_server_parser_destroy(&parser);
  }

  it("rejects aggregate header descriptor overflow before allocation") {
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};

    config.max_target_bytes = 1u;
    config.max_header_count = SIZE_MAX / sizeof(chttp_header) + 1u;
    config.max_header_bytes = 1u;
    config.max_body_bytes = 1u;
    check_equal(chttp_server_parser_init(&parser, &config), SALTS_ERANGE);
    check_null(parser.impl);
  }

  it("parses fragmented requests and sequential keep-alive messages") {
    static const char first[] = "POST /items?kind=book HTTP/1.1\r\nHo";
    static const char second[] = "st: example.test\r\nX-Mode: one\r\nContent-Length: 5\r\n\r\nhe";
    static const char third[] = "lloGET /health HTTP/1.1\r\nHost: example.test\r\n\r\n";
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 99u;

    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_execute(&parser, first, sizeof(first) - 1u, &http_status),
                SALTS_OK);
    check_equal(http_status, 0u);
    check_equal(chttp_server_parser_execute(&parser, second, sizeof(second) - 1u, &http_status),
                SALTS_OK);
    check_equal(probe.requests, 0);
    check_equal(chttp_server_parser_execute(&parser, third, sizeof(third) - 1u, &http_status),
                SALTS_OK);
    check_equal(probe.requests, 2);
    check_equal(probe.method, CHTTP_METHOD_GET);
    check_equal(probe.target, "/health");
    check_equal(probe.path, "/health");
    check_equal(probe.host, "example.test");
    check_equal(probe.body_size, (size_t)0u);
    check_equal(probe.keep_alive, 1);
    chttp_server_parser_destroy(&parser);
  }

  it("stops at header admission before 100 continue and body bytes") {
    static const char headers[] = "POST /protected HTTP/1.1\r\n"
                                  "Host: example.test\r\n"
                                  "Expect: 100-continue\r\n"
                                  "Content-Length: 4\r\n\r\n";
    static const char body[] = "data";
    char wire[sizeof(headers) - 1u + sizeof(body) - 1u];
    chttp_server_parser_probe probe = {.stop_at_headers = 1};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    size_t consumed = 0u;
    unsigned int http_status = 0u;

    memcpy(wire, headers, sizeof(headers) - 1u);
    memcpy(wire + sizeof(headers) - 1u, body, sizeof(body) - 1u);
    config.on_headers = chttp_server_parser_test_headers;
    config.on_body_open = chttp_server_parser_test_body_open;
    config.on_body_close = chttp_server_parser_test_body_close;
    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_execute_consumed(&parser, wire, sizeof(wire), &consumed,
                                                     &http_status),
                SALTS_OK);
    check_equal(consumed, sizeof(headers) - 1u);
    check_equal(probe.headers, 1);
    check_equal(probe.continues, 0);
    check_equal(probe.body_opens, 0);
    check_equal(probe.requests, 0);
    chttp_server_parser_destroy(&parser);
  }

  it("emits 100-continue before accepting a bounded request body") {
    static const char wire[] = "POST /upload HTTP/1.1\r\n"
                               "Host: example.test\r\n"
                               "Expect: 100-continue\r\n"
                               "Content-Length: 4\r\n\r\n"
                               "data";
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 0u;

    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_test_execute(&parser, wire, &http_status), SALTS_OK);
    check_equal(http_status, 0u);
    check_equal(probe.continues, 1);
    check_equal(probe.requests, 1);
    check_equal(probe.method, CHTTP_METHOD_POST);
    check_equal(probe.body_size, (size_t)4u);
    check_equal(probe.body, "data", 4u);
    chttp_server_parser_destroy(&parser);
  }

  it("ignores an HTTP/1.0 100-continue expectation") {
    static const char wire[] = "POST /upload HTTP/1.0\r\n"
                               "Expect: 100-continue\r\n"
                               "Content-Length: 4\r\n\r\n"
                               "data";
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 0u;

    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_test_execute(&parser, wire, &http_status), SALTS_OK);
    check_equal(http_status, 0u);
    check_equal(probe.continues, 0);
    check_equal(probe.requests, 1);
    check_equal(probe.http_minor, 0u);
    check_equal(probe.body, "data", 4u);
    chttp_server_parser_destroy(&parser);
  }

  it("ignores an unsupported Upgrade invitation and handles the HTTP request normally") {
    static const char wire[] = "GET /upgrade HTTP/1.1\r\n"
                               "Host: example.test\r\n"
                               "Connection: Upgrade\r\n"
                               "Upgrade: websocket\r\n\r\n";
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 0u;

    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_test_execute(&parser, wire, &http_status), SALTS_OK);
    check_equal(http_status, 0u);
    check_equal(probe.requests, 1);
    check_equal(probe.target, "/upgrade");
    chttp_server_parser_destroy(&parser);
  }

  it("maps request bounds and required Host to HTTP errors") {
    static const struct {
      const char *wire;
      unsigned int status;
    } cases[] = {{"GET /missing-host HTTP/1.1\r\n\r\n", 400u},
                 {"GET /this-target-is-far-too-long-for-the-configured-parser-limit-0123456789 "
                  "HTTP/1.1\r\nHost: x\r\n\r\n",
                  414u},
                 {"POST /body HTTP/1.1\r\nHost: x\r\nContent-Length: 65\r\n\r\n", 413u},
                 {"GET /headers HTTP/1.1\r\nHost: x\r\nX-Large: "
                  "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                  "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\r\n\r\n",
                  431u},
                 {"GET /duplicate-host HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n", 400u},
                 {"GET /expect HTTP/1.1\r\nHost: x\r\nExpect: something-else\r\n\r\n", 417u},
                 {"POST /coding HTTP/1.1\r\nHost: x\r\n"
                  "Transfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
                  501u},
                 {"POST /coding HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400u},
                 {"GET /fragment#section HTTP/1.1\r\nHost: x\r\n\r\n", 400u},
                 {"TRACE /unsupported HTTP/1.1\r\nHost: x\r\n\r\n", 501u},
                 {"GET /version HTTP/2.0\r\nHost: x\r\n\r\n", 505u}};
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
      chttp_server_parser_probe probe = {0};
      chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
      chttp_server_parser parser = {0};
      unsigned int http_status = 0u;
      check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
      check_equal(chttp_server_parser_test_execute(&parser, cases[index].wire, &http_status),
                  SALTS_EPROTO);
      check_equal(http_status, cases[index].status);
      check_equal(probe.requests, 0);
      chttp_server_parser_destroy(&parser);
    }
  }

  it("bounds raw request-line and header bytes across input fragments") {
    static const char spaces_suffix[] = "HTTP/1.1\r\nHost: x\r\n\r\n";
    char leading_wire[320];
    char ows_wire[256];
    char spaces_wire[256];
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 0u;
    size_t prefix_size;

    memset(leading_wire, '\r', 200u);
    memcpy(leading_wire + 200u, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27u);
    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(
        chttp_server_parser_test_execute_fragments(&parser, leading_wire, 227u, 7u, &http_status),
        SALTS_EPROTO);
    check_equal(http_status, 400u);
    check_equal(probe.requests, 0);
    chttp_server_parser_destroy(&parser);

    probe = (chttp_server_parser_probe){0};
    parser = (chttp_server_parser){0};
    prefix_size = strlen("GET / HTTP/1.1\r\nHost:");
    memcpy(ows_wire, "GET / HTTP/1.1\r\nHost:", prefix_size);
    memset(ows_wire + prefix_size, ' ', 160u);
    memcpy(ows_wire + prefix_size + 160u, "x\r\n\r\n", 5u);
    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_test_execute_fragments(&parser, ows_wire, prefix_size + 165u,
                                                           11u, &http_status),
                SALTS_EPROTO);
    check_equal(http_status, 431u);
    check_equal(probe.requests, 0);
    chttp_server_parser_destroy(&parser);

    probe = (chttp_server_parser_probe){0};
    parser = (chttp_server_parser){0};
    prefix_size = strlen("GET /");
    memcpy(spaces_wire, "GET /", prefix_size);
    memset(spaces_wire + prefix_size, ' ', 160u);
    memcpy(spaces_wire + prefix_size + 160u, spaces_suffix, sizeof(spaces_suffix) - 1u);
    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_test_execute_fragments(
                    &parser, spaces_wire, prefix_size + 160u + sizeof(spaces_suffix) - 1u, 13u,
                    &http_status),
                SALTS_EPROTO);
    check_equal(http_status, 400u);
    check_equal(probe.requests, 0);
    chttp_server_parser_destroy(&parser);
  }

  it("bounds chunk extensions across input fragments") {
    static const char prefix[] =
        "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1;";
    static const char suffix[] = "=x\r\na\r\n0\r\n\r\n";
    char wire[256];
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 0u;
    const size_t extension_size = 160u;
    const size_t wire_size = sizeof(prefix) - 1u + extension_size + sizeof(suffix) - 1u;

    memcpy(wire, prefix, sizeof(prefix) - 1u);
    memset(wire + sizeof(prefix) - 1u, 'a', extension_size);
    memcpy(wire + sizeof(prefix) - 1u + extension_size, suffix, sizeof(suffix) - 1u);
    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(
        chttp_server_parser_test_execute_fragments(&parser, wire, wire_size, 9u, &http_status),
        SALTS_EPROTO);
    check_equal(http_status, 413u);
    check_equal(probe.requests, 0);
    chttp_server_parser_destroy(&parser);
  }

  it("rejects request trailers instead of merging them into ordinary headers") {
    static const char wire[] = "POST /upload HTTP/1.1\r\n"
                               "Host: x\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n"
                               "1\r\na\r\n0\r\nAuthorization: trailer-secret\r\n\r\n";
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 0u;

    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_test_execute_fragments(&parser, wire, sizeof(wire) - 1u, 5u,
                                                           &http_status),
                SALTS_EPROTO);
    check_equal(http_status, 400u);
    check_equal(probe.requests, 0);
    chttp_server_parser_destroy(&parser);
  }

  it("parses a chunked message followed by a pipelined request") {
    static const char wire[] = "POST /upload HTTP/1.1\r\n"
                               "Host: x\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n"
                               "1\r\na\r\n0\r\n\r\n"
                               "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    chttp_server_parser_probe probe = {0};
    chttp_server_parser_config config = chttp_server_parser_test_config(&probe);
    chttp_server_parser parser = {0};
    unsigned int http_status = 0u;

    check_equal(chttp_server_parser_init(&parser, &config), SALTS_OK);
    check_equal(chttp_server_parser_test_execute_fragments(&parser, wire, sizeof(wire) - 1u, 6u,
                                                           &http_status),
                SALTS_OK);
    check_equal(http_status, 0u);
    check_equal(probe.requests, 2);
    check_equal(probe.method, CHTTP_METHOD_GET);
    check_equal(probe.target, "/next");
    chttp_server_parser_destroy(&parser);
  }
}
