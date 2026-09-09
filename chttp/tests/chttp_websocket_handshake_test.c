#include "chttp_websocket_handshake.h"
#include "tinytest.h"

#include <base64_utils.h>

#include <stdlib.h>
#include <string.h>

static chttp_server_request_view websocket_request(const chttp_header *headers,
                                                   size_t header_count) {
  const chttp_server_request_view request = {.http_major = 1u,
                                             .http_minor = 1u,
                                             .method = CHTTP_METHOD_GET,
                                             .target = "/chat",
                                             .path = "/chat",
                                             .headers = headers,
                                             .header_count = header_count};
  return request;
}

spec("CHTTP WebSocket opening handshake") {
  it("generates a canonical 16-byte client nonce") {
    char key[CHTTP_WEBSOCKET_KEY_CAPACITY];
    tn_base64_bytes_result_t decoded;

    check_equal(chttp_websocket_client_key_generate(key, sizeof(key)), SALTS_OK);
    check_equal(strlen(key), CHTTP_WEBSOCKET_KEY_BYTES);
    decoded = tn_base64_decode_ex(key);
    check_true(decoded.ok);
    check_equal(decoded.value.len, 16u);
    free(decoded.value.data);
  }

  it("computes the RFC 6455 example accept value") {
    char accept[CHTTP_WEBSOCKET_ACCEPT_CAPACITY];

    check_equal(chttp_websocket_accept_compute("dGhlIHNhbXBsZSBub25jZQ==", accept, sizeof(accept)),
                SALTS_OK);
    check_equal(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), 0);
  }

  it("accepts case-insensitive comma-separated Upgrade tokens") {
    const chttp_header headers[] = {
        {"Host", "server.example.com"},        {"Upgrade", "h2c, WebSocket"},
        {"Connection", "keep-alive, Upgrade"}, {"Sec-WebSocket-Key", " dGhlIHNhbXBsZSBub25jZQ==\t"},
        {"Sec-WebSocket-Version", "13"},       {"Content-Length", "0"}};
    chttp_server_request_view request =
        websocket_request(headers, sizeof(headers) / sizeof(*headers));
    char accept[CHTTP_WEBSOCKET_ACCEPT_CAPACITY];
    unsigned int http_status = 999u;

    check_equal(
        chttp_websocket_server_handshake_validate(&request, accept, sizeof(accept), &http_status),
        SALTS_OK);
    check_equal(http_status, 0u);
    check_equal(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), 0);
  }

  it("rejects missing, duplicate, malformed, and non-16-byte keys") {
    const chttp_header missing[] = {{"Host", "server.example.com"},
                                    {"Upgrade", "websocket"},
                                    {"Connection", "Upgrade"},
                                    {"Sec-WebSocket-Version", "13"}};
    const chttp_header duplicate[] = {{"Host", "server.example.com"},
                                      {"Upgrade", "websocket"},
                                      {"Connection", "Upgrade"},
                                      {"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="},
                                      {"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="},
                                      {"Sec-WebSocket-Version", "13"}};
    const chttp_header malformed[] = {{"Host", "server.example.com"},
                                      {"Upgrade", "websocket"},
                                      {"Connection", "Upgrade"},
                                      {"Sec-WebSocket-Key", "not-base64"},
                                      {"Sec-WebSocket-Version", "13"}};
    const chttp_header short_key[] = {{"Host", "server.example.com"},
                                      {"Upgrade", "websocket"},
                                      {"Connection", "Upgrade"},
                                      {"Sec-WebSocket-Key", "c2hvcnQ="},
                                      {"Sec-WebSocket-Version", "13"}};
    const struct {
      const chttp_header *headers;
      size_t count;
    } cases[] = {{missing, sizeof(missing) / sizeof(*missing)},
                 {duplicate, sizeof(duplicate) / sizeof(*duplicate)},
                 {malformed, sizeof(malformed) / sizeof(*malformed)},
                 {short_key, sizeof(short_key) / sizeof(*short_key)}};
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(*cases); ++index) {
      chttp_server_request_view request =
          websocket_request(cases[index].headers, cases[index].count);
      char accept[CHTTP_WEBSOCKET_ACCEPT_CAPACITY];
      unsigned int http_status = 0u;
      check_equal(
          chttp_websocket_server_handshake_validate(&request, accept, sizeof(accept), &http_status),
          SALTS_EPROTO);
      check_equal(http_status, 400u);
    }
  }

  it("returns 426 for an unsupported WebSocket version") {
    const chttp_header headers[] = {{"Host", "server.example.com"},
                                    {"Upgrade", "websocket"},
                                    {"Connection", "Upgrade"},
                                    {"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="},
                                    {"Sec-WebSocket-Version", "12"}};
    chttp_server_request_view request =
        websocket_request(headers, sizeof(headers) / sizeof(*headers));
    char accept[CHTTP_WEBSOCKET_ACCEPT_CAPACITY];
    unsigned int http_status = 0u;

    check_equal(
        chttp_websocket_server_handshake_validate(&request, accept, sizeof(accept), &http_status),
        SALTS_EPROTONOSUPPORT);
    check_equal(http_status, 426u);
  }

  it("rejects a request body and Transfer-Encoding") {
    const chttp_header length_headers[] = {
        {"Host", "server.example.com"},  {"Upgrade", "websocket"},
        {"Connection", "Upgrade"},       {"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="},
        {"Sec-WebSocket-Version", "13"}, {"Content-Length", "1"}};
    const chttp_header transfer_headers[] = {
        {"Host", "server.example.com"},  {"Upgrade", "websocket"},
        {"Connection", "Upgrade"},       {"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="},
        {"Sec-WebSocket-Version", "13"}, {"Transfer-Encoding", "chunked"}};
    const chttp_header *cases[] = {length_headers, transfer_headers};
    const size_t counts[] = {sizeof(length_headers) / sizeof(*length_headers),
                             sizeof(transfer_headers) / sizeof(*transfer_headers)};
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(*cases); ++index) {
      chttp_server_request_view request = websocket_request(cases[index], counts[index]);
      char accept[CHTTP_WEBSOCKET_ACCEPT_CAPACITY];
      unsigned int http_status = 0u;
      check_equal(
          chttp_websocket_server_handshake_validate(&request, accept, sizeof(accept), &http_status),
          SALTS_EPROTO);
      check_equal(http_status, 400u);
    }
  }

  it("validates a strict 101 client handshake response") {
    static const char response[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                   "Connection: keep-alive, Upgrade\r\n"
                                   "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    unsigned int http_status = 0u;

    check_equal(chttp_websocket_client_handshake_validate(
                    response, sizeof(response) - 1u, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", NULL,
                    &http_status),
                SALTS_OK);
    check_equal(http_status, 101u);
  }

  it("rejects a non-101 response and a mismatched or duplicate accept") {
    static const char rejected[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    static const char mismatched[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n\r\n";
    static const char duplicate[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    const struct {
      const char *wire;
      size_t size;
      unsigned int status;
    } cases[] = {{rejected, sizeof(rejected) - 1u, 403u},
                 {mismatched, sizeof(mismatched) - 1u, 101u},
                 {duplicate, sizeof(duplicate) - 1u, 101u}};
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(*cases); ++index) {
      unsigned int http_status = 0u;
      check_equal(
          chttp_websocket_client_handshake_validate(cases[index].wire, cases[index].size,
                                                    "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", NULL,
                                                    &http_status),
          SALTS_EPROTO);
      check_equal(http_status, cases[index].status);
    }
  }

  it("rejects HTTP/1.0, response framing, and unsupported negotiation") {
    static const char http10[] =
        "HTTP/1.0 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    static const char framed[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nContent-Length: 1\r\n\r\n";
    static const char extension[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
    const struct {
      const char *wire;
      size_t size;
    } cases[] = {{http10, sizeof(http10) - 1u},
                 {framed, sizeof(framed) - 1u},
                 {extension, sizeof(extension) - 1u}};
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(*cases); ++index) {
      unsigned int http_status = 0u;
      check_equal(
          chttp_websocket_client_handshake_validate(cases[index].wire, cases[index].size,
                                                    "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", NULL,
                                                    &http_status),
          SALTS_EPROTO);
      check_equal(http_status, 101u);
    }
  }

  it("requires the server to select the requested WebSocket subprotocol exactly once") {
    static const char matching[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Protocol: mqtt\r\n\r\n";
    static const char missing[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    static const char mismatched[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Protocol: MQTT\r\n\r\n";
    unsigned int http_status = 0u;

    check_equal(chttp_websocket_client_handshake_validate(
                    matching, sizeof(matching) - 1u, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "mqtt",
                    &http_status),
                SALTS_OK);
    check_equal(chttp_websocket_client_handshake_validate(
                    missing, sizeof(missing) - 1u, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "mqtt",
                    &http_status),
                SALTS_EPROTO);
    check_equal(chttp_websocket_client_handshake_validate(
                    mismatched, sizeof(mismatched) - 1u, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "mqtt",
                    &http_status),
                SALTS_EPROTO);
    check_equal(chttp_websocket_client_handshake_validate(
                    matching, sizeof(matching) - 1u, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", NULL,
                    &http_status),
                SALTS_EPROTO);
  }
}
