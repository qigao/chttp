/**
 * @file chttp_h2_proto_test.c
 * @brief In-memory protocol engine round-trip tests (client <-> server).
 */

#include "chttp_h2_frame.h"
#include "chttp_h2_proto.h"
#include "tinytest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct e2e_s {
  chttp_h2_proto *client;
  chttp_h2_proto *server;
  /* request captured by the server */
  char method[16];
  char path[64];
  char authority[64];
  int req_headers;
  int req_end;
  int defer_response;
  int32_t req_stream;
  /* response captured by the client */
  char status[8];
  char body[64];
  size_t body_len;
  int resp_headers;
  int resp_closed;
  int response_closes;
  uint32_t last_close_error;
  int consume_data;
  int pause_data;
  int consume_error;
  int data_calls;
  int32_t last_data_stream;
  char request_body[64];
  size_t request_body_len;
  int request_data_calls;
  int req_end_stream;
  int client_recv_error;
  int server_recv_error;
  uint32_t goaway_error;
  int ping_acks;
  uint8_t ping_ack[8];
} e2e_t;

typedef struct h2_waiting_source_probe {
  const uint8_t *data;
  size_t size;
  size_t offset;
  int ready;
  int calls;
} h2_waiting_source_probe;

static chttp_h2_proto_source_result h2_waiting_source(void *user, uint8_t *buffer,
                                                      size_t capacity) {
  h2_waiting_source_probe *probe = (h2_waiting_source_probe *)user;
  size_t size;
  ++probe->calls;
  if (!probe->ready) return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_WAIT, 0u};
  if (probe->offset == probe->size)
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_EOF, 0u};
  size = probe->size - probe->offset;
  if (size > capacity) size = capacity;
  memcpy(buffer, probe->data + probe->offset, size);
  probe->offset += size;
  return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_DATA, size};
}

static int srv_begin(void *ud, int32_t stream_id) {
  e2e_t *e = (e2e_t *)ud;
  e->req_headers = 1;
  e->req_stream = stream_id;
  e->method[0] = e->path[0] = e->authority[0] = '\0';
  return 0;
}

static int srv_hdr(void *ud, int32_t stream_id, const char *name, size_t nl, const char *value,
                   size_t vl) {
  e2e_t *e = (e2e_t *)ud;
  (void)stream_id;
  if (nl == 7 && memcmp(name, ":method", 7) == 0 && vl < sizeof(e->method)) {
    memcpy(e->method, value, vl);
    e->method[vl] = '\0';
  } else if (nl == 5 && memcmp(name, ":path", 5) == 0 && vl < sizeof(e->path)) {
    memcpy(e->path, value, vl);
    e->path[vl] = '\0';
  } else if (nl == 10 && memcmp(name, ":authority", 10) == 0 && vl < sizeof(e->authority)) {
    memcpy(e->authority, value, vl);
    e->authority[vl] = '\0';
  }
  return 0;
}

static int srv_end(void *ud, int32_t stream_id, int end_stream) {
  e2e_t *e = (e2e_t *)ud;
  static const char *status = ":status";
  static const char *cl = "content-length";
  static const char *body = "hello from server";
  chttp_h2_hpack_header hdrs[2];
  hdrs[0].name = status;
  hdrs[0].name_size = 7;
  hdrs[0].value = "200";
  hdrs[0].value_size = 3;
  hdrs[1].name = cl;
  hdrs[1].name_size = 14;
  hdrs[1].value = "17";
  hdrs[1].value_size = 2;
  e->req_end = 1;
  e->req_end_stream = end_stream;
  if (e->defer_response) return 0;
  (void)chttp_h2_proto_submit_response(e->server, stream_id, hdrs, 2, (const uint8_t *)body,
                                       strlen(body));
  return 0;
}

static int srv_data(void *ud, int32_t stream_id, const uint8_t *data, size_t len) {
  e2e_t *e = (e2e_t *)ud;
  ++e->request_data_calls;
  if (e->request_body_len + len <= sizeof(e->request_body)) {
    memcpy(e->request_body + e->request_body_len, data, len);
    e->request_body_len += len;
  }
  if (len != 0u && (chttp_h2_proto_consume_stream(e->server, stream_id, len) != 0 ||
                    chttp_h2_proto_consume_connection(e->server, len) != 0))
    e->consume_error = 1;
  return 0;
}

static int cli_begin(void *ud, int32_t stream_id) {
  e2e_t *e = (e2e_t *)ud;
  (void)stream_id;
  e->resp_headers = 1;
  e->status[0] = '\0';
  e->body_len = 0;
  return 0;
}

static int cli_hdr(void *ud, int32_t stream_id, const char *name, size_t nl, const char *value,
                   size_t vl) {
  e2e_t *e = (e2e_t *)ud;
  (void)stream_id;
  if (nl == 7 && memcmp(name, ":status", 7) == 0 && vl < sizeof(e->status)) {
    memcpy(e->status, value, vl);
    e->status[vl] = '\0';
  }
  return 0;
}

static int cli_data(void *ud, int32_t stream_id, const uint8_t *data, size_t len) {
  e2e_t *e = (e2e_t *)ud;
  ++e->data_calls;
  e->last_data_stream = stream_id;
  if (e->body_len + len <= sizeof(e->body)) {
    memcpy(e->body + e->body_len, data, len);
    e->body_len += len;
  }
  if (e->consume_data && len != 0u &&
      (chttp_h2_proto_consume_stream(e->client, stream_id, len) != 0 ||
       chttp_h2_proto_consume_connection(e->client, len) != 0))
    e->consume_error = 1;
  if (e->pause_data) {
    e->pause_data = 0;
    return CHTTP_H2_PROTO_DATA_PAUSE;
  }
  return 0;
}

static int cli_close(void *ud, int32_t stream_id, uint32_t error_code) {
  e2e_t *e = (e2e_t *)ud;
  (void)stream_id;
  e->resp_closed = 1;
  ++e->response_closes;
  e->last_close_error = error_code;
  return 0;
}

static void capture_goaway(void *ud, uint32_t last_stream_id, uint32_t error_code) {
  e2e_t *e = (e2e_t *)ud;
  (void)last_stream_id;
  e->goaway_error = error_code;
}

static void capture_ping_ack(void *ud, const uint8_t opaque[8]) {
  e2e_t *e = (e2e_t *)ud;
  ++e->ping_acks;
  memcpy(e->ping_ack, opaque, sizeof(e->ping_ack));
}

static void pump(e2e_t *e) {
  int iter;
  for (iter = 0; iter < 200; iter++) {
    int progress = 0;
    while (chttp_h2_proto_want_write(e->client)) {
      const uint8_t *wire = NULL;
      ptrdiff_t n = chttp_h2_proto_send(e->client, &wire);
      if (n <= 0) break;
      if (chttp_h2_proto_recv(e->server, wire, (size_t)n) < 0) {
        e->server_recv_error = 1;
      }
      progress = 1;
    }
    while (chttp_h2_proto_want_write(e->server)) {
      const uint8_t *wire = NULL;
      ptrdiff_t n = chttp_h2_proto_send(e->server, &wire);
      if (n <= 0) break;
      if (chttp_h2_proto_recv(e->client, wire, (size_t)n) < 0) {
        e->client_recv_error = 1;
      }
      progress = 1;
    }
    if (!progress) break;
  }
}

static void e2e_init_with_config(e2e_t *e, const chttp_h2_proto_config *config) {
  chttp_h2_proto_callbacks scbs, ccbs;
  memset(e, 0, sizeof(*e));
  memset(&scbs, 0, sizeof(scbs));
  memset(&ccbs, 0, sizeof(ccbs));
  scbs.user_data = e;
  scbs.on_begin_headers = srv_begin;
  scbs.on_header = srv_hdr;
  scbs.on_end_headers = srv_end;
  scbs.on_data = srv_data;
  scbs.on_goaway = capture_goaway;
  ccbs.user_data = e;
  ccbs.on_begin_headers = cli_begin;
  ccbs.on_header = cli_hdr;
  ccbs.on_data = cli_data;
  ccbs.on_stream_close = cli_close;
  ccbs.on_goaway = capture_goaway;
  ccbs.on_ping_ack = capture_ping_ack;
  e->server = chttp_h2_proto_create(CHTTP_H2_PROTO_SERVER, config, &scbs);
  e->client = chttp_h2_proto_create(CHTTP_H2_PROTO_CLIENT, config, &ccbs);
}

static void e2e_init(e2e_t *e) { e2e_init_with_config(e, NULL); }

static void e2e_request_headers(chttp_h2_hpack_header headers[4], const char *method,
                                const char *target) {
  headers[0] = (chttp_h2_hpack_header){":method", 7u, method, strlen(method)};
  headers[1] = (chttp_h2_hpack_header){":scheme", 7u, "http", 4u};
  headers[2] = (chttp_h2_hpack_header){":path", 5u, target, strlen(target)};
  headers[3] = (chttp_h2_hpack_header){":authority", 10u, "localhost", 9u};
}

spec("CHTTP HTTP/2 protocol engine") {
  it("keeps an extended CONNECT stream open for bidirectional DATA") {
    static const chttp_h2_hpack_header request[] = {
        {":method", sizeof(":method") - 1u, "CONNECT", sizeof("CONNECT") - 1u},
        {":protocol", sizeof(":protocol") - 1u, "websocket", sizeof("websocket") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/chat", sizeof("/chat") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_h2_hpack_header response[] = {
        {":status", sizeof(":status") - 1u, "200", sizeof("200") - 1u}};
    static const uint8_t client_frame[] = {0x81u, 0x80u};
    static const uint8_t server_frame[] = {0x81u, 0x00u};
    e2e_t e;
    int32_t stream_id = 0;

    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);
    e.defer_response = 1;
    e.consume_data = 1;
    chttp_h2_proto_set_local_enable_connect_protocol(e.server, 1u);

    check_equal(chttp_h2_proto_submit_request_headers(
                    e.client, request, sizeof(request) / sizeof(request[0]), 0, &stream_id),
                0);
    pump(&e);
    check(chttp_h2_proto_peer_settings_received(e.client));
    check_equal(chttp_h2_proto_peer_enable_connect_protocol(e.client), 1u);
    check_equal(e.req_end_stream, 0);
    check_equal(
        chttp_h2_proto_submit_data(e.client, stream_id, client_frame, sizeof(client_frame), 0), 0);
    pump(&e);
    check_equal(e.request_data_calls, 1);
    check_equal(e.request_body_len, sizeof(client_frame));
    check_equal(memcmp(e.request_body, client_frame, sizeof(client_frame)), 0);

    check_equal(chttp_h2_proto_submit_headers(e.server, stream_id, response, 1u, 0), 0);
    check_equal(
        chttp_h2_proto_submit_data(e.server, stream_id, server_frame, sizeof(server_frame), 0), 0);
    pump(&e);
    check_equal(e.body_len, sizeof(server_frame));
    check_equal(memcmp(e.body, server_frame, sizeof(server_frame)), 0);

    check_equal(chttp_h2_proto_submit_data(e.client, stream_id, NULL, 0u, 1), 0);
    check_equal(chttp_h2_proto_submit_data(e.server, stream_id, NULL, 0u, 1), 0);
    pump(&e);
    check(e.resp_closed);
    check_equal(e.consume_error, 0);
    check_equal(e.client_recv_error, 0);
    check_equal(e.server_recv_error, 0);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("rejects disabling extended CONNECT after the peer enabled it") {
    const uint32_t identifiers[] = {CHTTP_H2_SETTING_ENABLE_CONNECT_PROTOCOL};
    const uint32_t enabled_values[] = {1u};
    const uint32_t disabled_values[] = {0u};
    uint8_t enabled_wire[CHTTP_H2_FRAME_HEADER_SIZE + 6u] = {0};
    uint8_t disabled_wire[CHTTP_H2_FRAME_HEADER_SIZE + 6u] = {0};
    size_t enabled_payload_size = 0u;
    size_t disabled_payload_size = 0u;
    size_t frame_header_size = 0u;
    const uint8_t *output = NULL;
    ptrdiff_t output_size;
    chttp_h2_frame_header output_header;
    uint32_t last_stream_id = 0u;
    uint32_t error_code = CHTTP_H2_ERR_NO_ERROR;
    e2e_t e;

    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);
    check_equal(chttp_h2_proto_submit_settings(e.client), 0);
    check_equal(chttp_h2_proto_submit_settings(e.server), 0);
    pump(&e);
    check(chttp_h2_proto_peer_settings_received(e.client));
    check_equal(chttp_h2_frame_settings_encode(enabled_wire + CHTTP_H2_FRAME_HEADER_SIZE, 6u,
                                               &enabled_payload_size, identifiers, enabled_values,
                                               1u),
                0);
    check_equal(chttp_h2_frame_header_encode(enabled_wire, CHTTP_H2_FRAME_HEADER_SIZE,
                                             &frame_header_size, (uint32_t)enabled_payload_size,
                                             CHTTP_H2_FRAME_SETTINGS, 0u, 0u),
                0);
    check_equal(chttp_h2_proto_recv(e.client, enabled_wire,
                                    CHTTP_H2_FRAME_HEADER_SIZE + enabled_payload_size),
                (ptrdiff_t)(CHTTP_H2_FRAME_HEADER_SIZE + enabled_payload_size));
    check_equal(chttp_h2_proto_peer_enable_connect_protocol(e.client), 1u);
    output_size = chttp_h2_proto_send(e.client, &output);
    check_equal(output_size, (ptrdiff_t)CHTTP_H2_FRAME_HEADER_SIZE);
    check_equal(chttp_h2_frame_header_decode(output, (size_t)output_size, &frame_header_size,
                                             &output_header, 16u * 1024u),
                0);
    check_equal(output_header.type, CHTTP_H2_FRAME_SETTINGS);
    check_equal(output_header.flags, CHTTP_H2_FLAG_ACK);

    check_equal(chttp_h2_frame_settings_encode(disabled_wire + CHTTP_H2_FRAME_HEADER_SIZE, 6u,
                                               &disabled_payload_size, identifiers, disabled_values,
                                               1u),
                0);
    check_equal(chttp_h2_frame_header_encode(disabled_wire, CHTTP_H2_FRAME_HEADER_SIZE,
                                             &frame_header_size, (uint32_t)disabled_payload_size,
                                             CHTTP_H2_FRAME_SETTINGS, 0u, 0u),
                0);
    check_equal(chttp_h2_proto_recv(e.client, disabled_wire,
                                    CHTTP_H2_FRAME_HEADER_SIZE + disabled_payload_size),
                (ptrdiff_t)-1);
    output_size = chttp_h2_proto_send(e.client, &output);
    check_true(output_size >= (ptrdiff_t)CHTTP_H2_FRAME_HEADER_SIZE);
    check_equal(chttp_h2_frame_header_decode(output, (size_t)output_size, &frame_header_size,
                                             &output_header, 16u * 1024u),
                0);
    check_equal(output_header.type, CHTTP_H2_FRAME_GOAWAY);
    check_equal(chttp_h2_frame_goaway_parse(output + frame_header_size, output_header.length,
                                            &last_stream_id, &error_code),
                0);
    check_equal(error_code, CHTTP_H2_ERR_PROTOCOL_ERROR);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("retains later input and delays END_STREAM while a DATA consumer is paused") {
    static const uint8_t body[] = "write-before-window-update";
    static const chttp_h2_hpack_header response_headers[] = {{":status", 7u, "200", 3u}};
    e2e_t e;
    chttp_h2_hpack_header request_headers[4];
    int32_t stream_id = 0;

    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);
    e.defer_response = 1;
    e.pause_data = 1;
    e2e_request_headers(request_headers, "GET", "/paused-input");
    check_equal(chttp_h2_proto_submit_request(e.client, request_headers, 4u, NULL, 0u, &stream_id),
                0);
    pump(&e);
    check_equal(chttp_h2_proto_submit_response(e.server, stream_id, response_headers, 1u, body,
                                               sizeof(body) - 1u),
                0);
    pump(&e);

    check_equal(e.data_calls, 1);
    check_equal(e.body_len, sizeof(body) - 1u);
    check_equal(e.resp_closed, 0);
    check_true(chttp_h2_proto_input_paused(e.client));
    check_equal(chttp_h2_proto_consume_stream(e.client, stream_id, sizeof(body) - 1u), 0);
    check_equal(chttp_h2_proto_consume_connection(e.client, sizeof(body) - 1u), 0);
    check_equal(chttp_h2_proto_resume_input(e.client, stream_id), 0);
    check_false(chttp_h2_proto_input_paused(e.client));
    check_equal(e.resp_closed, 1);
    check_equal(e.last_close_error, CHTTP_H2_ERR_NO_ERROR);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("keeps a waiting response source open until it is explicitly resumed") {
    static const uint8_t body[] = "ready-after-file-completion";
    static const chttp_h2_hpack_header response_headers[] = {{":status", 7u, "200", 3u}};
    e2e_t e;
    chttp_h2_hpack_header request_headers[4];
    h2_waiting_source_probe source = {.data = body, .size = sizeof(body) - 1u};
    int32_t stream_id = 0;

    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);
    e.defer_response = 1;
    e.consume_data = 1;
    e2e_request_headers(request_headers, "GET", "/waiting-source");
    check_equal(chttp_h2_proto_submit_request(e.client, request_headers, 4u, NULL, 0u, &stream_id),
                0);
    pump(&e);
    check_equal(e.req_stream, stream_id);
    check_equal(chttp_h2_proto_submit_response_ex(e.server, stream_id, response_headers, 1u, NULL,
                                                  0u, h2_waiting_source, &source),
                0);
    pump(&e);

    check_equal(source.calls, 1);
    check_equal(e.body_len, (size_t)0u);
    check_equal(e.resp_closed, 0);
    check_equal(chttp_h2_proto_want_write(e.server), 0);

    source.ready = 1;
    check_equal(chttp_h2_proto_resume_source(e.server, stream_id), 0);
    check_equal(chttp_h2_proto_want_write(e.server), 1);
    pump(&e);
    check_equal(e.body_len, sizeof(body) - 1u);
    check_equal(e.body, body, sizeof(body) - 1u);
    check_equal(e.resp_closed, 1);
    check_equal(chttp_h2_proto_resume_source(e.server, stream_id), -1);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("round-trips a PING acknowledgement with its opaque payload") {
    static const uint8_t opaque[8] = {'c', 'h', 't', 't', 'p', 'h', '2', '!'};
    e2e_t e;
    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);

    check_equal(chttp_h2_proto_submit_ping(e.client, opaque), 0);
    pump(&e);

    check_equal(e.ping_acks, 1);
    check_equal(e.ping_ack, opaque, sizeof(opaque));
    check_equal(e.client_recv_error, 0);
    check_equal(e.server_recv_error, 0);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("round-trips a GET request and 200 response through HPACK and DATA") {
    e2e_t e;
    chttp_h2_hpack_header req[4];
    int32_t sid = 0;
    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);
    e.consume_data = 1;

    e2e_request_headers(req, "GET", "/index");

    check_equal(chttp_h2_proto_submit_request(e.client, req, 4, NULL, 0, &sid), 0);
    check(sid == 1);
    pump(&e);

    check(e.req_headers);
    check(e.req_end);
    check_equal(e.method, "GET");
    check_equal(e.path, "/index");
    check_equal(e.authority, "localhost");
    check_equal(e.goaway_error, 0);
    check_equal(e.consume_error, 0);
    check_equal(e.client_recv_error, 0);
    check_equal(e.server_recv_error, 0);
    check_equal(chttp_h2_proto_get_last_proc_stream_id(e.client), 0u);
    check_equal(chttp_h2_proto_get_last_proc_stream_id(e.server), 1u);
    check(e.resp_headers);
    check_equal(e.status, "200");
    check_equal(e.body_len, 17);
    check(memcmp(e.body, "hello from server", 17) == 0);
    check(e.resp_closed);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("ignores response frames already in flight after a local reset") {
    e2e_t e;
    chttp_h2_hpack_header first[4];
    chttp_h2_hpack_header second[4];
    int32_t first_stream = 0;
    int32_t second_stream = 0;
    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);
    e2e_request_headers(first, "GET", "/cancel");
    e2e_request_headers(second, "GET", "/sibling");

    check_equal(chttp_h2_proto_submit_request(e.client, first, 4u, NULL, 0u, &first_stream), 0);
    check_equal(chttp_h2_proto_submit_rst_stream(e.client, first_stream, CHTTP_H2_ERR_CANCEL), 0);
    check_equal(chttp_h2_proto_submit_request(e.client, second, 4u, NULL, 0u, &second_stream), 0);
    pump(&e);

    check_equal(first_stream, 1);
    check_equal(second_stream, 3);
    check_equal(e.server_recv_error, 0);
    check_equal(e.client_recv_error, 0);
    check_equal(e.response_closes, 2);
    check_equal(e.last_close_error, CHTTP_H2_ERR_NO_ERROR);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("reuses an active stream slot while retaining bounded local-reset history") {
    const chttp_h2_proto_config config = {.stream_capacity = 1u,
                                          .output_buffer_bytes = 64u * 1024u,
                                          .input_buffer_bytes = 64u * 1024u,
                                          .header_block_bytes = 4096u,
                                          .max_header_list_bytes = 4096u,
                                          .hpack_dynamic_table_bytes = 4096u,
                                          .max_hpack_string_bytes = 4096u,
                                          .max_settings_count = 16u};
    e2e_t e;
    chttp_h2_hpack_header first[4];
    chttp_h2_hpack_header second[4];
    int32_t first_stream = 0;
    int32_t second_stream = 0;
    e2e_init_with_config(&e, &config);
    check_not_null(e.client);
    check_not_null(e.server);
    e2e_request_headers(first, "GET", "/cancel-and-reuse");
    e2e_request_headers(second, "GET", "/after-reuse");

    check_equal(chttp_h2_proto_submit_request(e.client, first, 4u, NULL, 0u, &first_stream), 0);
    check_equal(chttp_h2_proto_submit_rst_stream(e.client, first_stream, CHTTP_H2_ERR_CANCEL), 0);
    check_equal(chttp_h2_proto_submit_request(e.client, second, 4u, NULL, 0u, &second_stream), 0);
    pump(&e);

    check_equal(first_stream, 1);
    check_equal(second_stream, 3);
    check_equal(e.server_recv_error, 0);
    check_equal(e.client_recv_error, 0);
    check_equal(e.response_closes, 2);
    check_equal(e.last_close_error, CHTTP_H2_ERR_NO_ERROR);
    check_equal(e.path, "/after-reuse");
    check_equal(e.status, "200");

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("refuses locally initiated streams above a peer GOAWAY last-stream-id") {
    e2e_t e;
    chttp_h2_hpack_header first[4];
    chttp_h2_hpack_header second[4];
    int32_t first_stream = 0;
    int32_t second_stream = 0;
    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);
    e.defer_response = 1;
    e2e_request_headers(first, "GET", "/accepted");
    e2e_request_headers(second, "GET", "/not-processed");

    check_equal(chttp_h2_proto_submit_request(e.client, first, 4u, NULL, 0u, &first_stream), 0);
    check_equal(chttp_h2_proto_submit_request(e.client, second, 4u, NULL, 0u, &second_stream), 0);
    pump(&e);
    check_equal(
        chttp_h2_proto_submit_goaway(e.server, (uint32_t)first_stream, CHTTP_H2_ERR_NO_ERROR), 0);
    pump(&e);

    check_equal(first_stream, 1);
    check_equal(second_stream, 3);
    check_equal(e.goaway_error, CHTTP_H2_ERR_NO_ERROR);
    check_equal(e.response_closes, 1);
    check_equal(e.last_close_error, CHTTP_H2_ERR_REFUSED_STREAM);
    check_equal(e.client_recv_error, 0);
    check_equal(e.server_recv_error, 0);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("streams a request body with flow control") {
    e2e_t e;
    chttp_h2_hpack_header req[4];
    static const char body[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    int32_t sid = 0;
    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);

    e2e_request_headers(req, "POST", "/upload");

    check_equal(chttp_h2_proto_submit_request(e.client, req, 4, (const uint8_t *)body,
                                              sizeof(body) - 1, &sid),
                0);
    pump(&e);
    /* The server saw the request; the response round-trips. */
    check_equal(e.method, "POST");
    check_equal(e.goaway_error, 0);
    check_equal(e.client_recv_error, 0);
    check_equal(e.server_recv_error, 0);
    check(e.req_end);
    check(e.resp_closed);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }

  it("restores flow-control credit for DATA padding hidden from the application") {
    static const chttp_h2_hpack_header response[] = {
        {":status", sizeof(":status") - 1u, "200", sizeof("200") - 1u}};
    e2e_t e;
    chttp_h2_hpack_header request[4];
    uint8_t wire[CHTTP_H2_FRAME_HEADER_SIZE + 10u] = {0};
    size_t header_size = 0u;
    int32_t stream_id = 0;
    e2e_init(&e);
    check_not_null(e.client);
    check_not_null(e.server);
    e.defer_response = 1;
    e.consume_data = 1;
    check_equal(
        chttp_h2_proto_set_local_settings(e.client, 4096u, 0u, 100u, 16u, 16384u, 64u * 1024u), 0);
    e2e_request_headers(request, "GET", "/padded");
    check_equal(chttp_h2_proto_submit_request(e.client, request, 4u, NULL, 0u, &stream_id), 0);
    pump(&e);
    check_equal(chttp_h2_proto_submit_headers(e.server, stream_id, response, 1u, 0), 0);
    pump(&e);

    check_equal(chttp_h2_frame_header_encode(wire, sizeof(wire), &header_size, 10u,
                                             CHTTP_H2_FRAME_DATA, CHTTP_H2_FLAG_PADDED,
                                             (uint32_t)stream_id),
                0);
    check_equal(header_size, CHTTP_H2_FRAME_HEADER_SIZE);
    wire[CHTTP_H2_FRAME_HEADER_SIZE] = 8u;
    wire[CHTTP_H2_FRAME_HEADER_SIZE + 1u] = 'x';
    check_equal(chttp_h2_proto_recv(e.client, wire, sizeof(wire)), (ptrdiff_t)sizeof(wire));
    check_equal(chttp_h2_proto_recv(e.client, wire, sizeof(wire)), (ptrdiff_t)sizeof(wire));
    check_equal(e.consume_error, 0);
    check_equal(e.client_recv_error, 0);
    check_equal(e.body_len, 2u);

    chttp_h2_proto_destroy(e.client);
    chttp_h2_proto_destroy(e.server);
  }
}
