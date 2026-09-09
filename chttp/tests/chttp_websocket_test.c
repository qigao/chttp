#include "chttp_tls_test_material.h"
#include "tinytest.h"

#include <chttp/chttp.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CHTTP_WEBSOCKET_TEST_TIMEOUT_MS = 5000,
  CHTTP_WEBSOCKET_TEST_H2_BUFFER_BYTES = 64u * 1024u,
  CHTTP_WEBSOCKET_TEST_H2_STREAM_CAPACITY = 8u,
  CHTTP_WEBSOCKET_TEST_H2_HPACK_BYTES = 4096u,
  CHTTP_WEBSOCKET_TEST_H2_SETTINGS_COUNT = 16u,
  CHTTP_WEBSOCKET_TEST_DUPLEX_ROUNDS = 128u
};

typedef struct chttp_websocket_test_probe {
  atomic_int global_middleware;
  atomic_int route_middleware;
  atomic_int opened;
  atomic_int messages;
  atomic_int pings;
  atomic_int pongs;
  atomic_int closes;
} chttp_websocket_test_probe;

typedef struct chttp_websocket_test_server_session_probe {
  chttp_server_websocket_session session;
  atomic_int captured;
  atomic_int peer_present;
} chttp_websocket_test_server_session_probe;

typedef struct chttp_websocket_test_jwt_probe {
  atomic_int opened;
  char subject[32];
} chttp_websocket_test_jwt_probe;

static native_io_backend_kind chttp_websocket_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config chttp_websocket_test_network(size_t connections) {
  const cnet_client_config config = {.backend = chttp_websocket_test_backend(),
                                     .connection_capacity = connections,
                                     .command_capacity = 16u,
                                     .request_capacity = 8u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 16u,
                                     .max_send_bytes = 4096u,
                                     .receive_buffer_bytes = 4096u,
                                     .connect_timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS,
                                     .read_timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS,
                                     .write_timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS};
  return config;
}

static chttp_server_config chttp_websocket_test_server_config(void) {
  const chttp_server_config config = {.host = "127.0.0.1",
                                      .port = 0u,
                                      .backlog = 8u,
                                      .network = chttp_websocket_test_network(4u),
                                      .route_capacity = 4u,
                                      .middleware_capacity = 2u,
                                      .max_route_middleware_count = 2u,
                                      .max_route_param_count = 2u,
                                      .max_route_param_bytes = 64u,
                                      .max_target_bytes = 256u,
                                      .max_header_count = 16u,
                                      .max_header_bytes = 2048u,
                                      .max_request_body_bytes = 256u,
                                      .max_response_header_count = 16u,
                                      .max_response_header_bytes = 1024u,
                                      .max_response_body_bytes = 256u,
                                      .session_capacity = 4u,
                                      .session_entry_capacity = 4u,
                                      .max_session_key_bytes = 32u,
                                      .max_session_value_bytes = 64u,
                                      .session_idle_timeout_ms = 60000u,
                                      .session_cookie_name = "ws_sid",
                                      .poll_slice_ms = 2u};
  return config;
}

static chttp_websocket_client_config chttp_websocket_test_client_config(void) {
  const chttp_websocket_client_config config = {.size = sizeof(config),
                                                .network = chttp_websocket_test_network(1u),
                                                .max_handshake_header_bytes = 4096u,
                                                .event_capacity = 8u};
  return config;
}

static void chttp_websocket_test_enable_h2(chttp_server_config *server,
                                           chttp_websocket_client_config *client) {
  server->network.max_send_bytes = CHTTP_WEBSOCKET_TEST_H2_BUFFER_BYTES;
  server->network.receive_buffer_bytes = CHTTP_WEBSOCKET_TEST_H2_BUFFER_BYTES;
  server->enable_http2 = 1;
  server->h2_stream_capacity = CHTTP_WEBSOCKET_TEST_H2_STREAM_CAPACITY;
  server->h2_input_buffer_bytes = CHTTP_WEBSOCKET_TEST_H2_BUFFER_BYTES;
  server->h2_output_buffer_bytes = CHTTP_WEBSOCKET_TEST_H2_BUFFER_BYTES;
  server->h2_hpack_dynamic_table_bytes = CHTTP_WEBSOCKET_TEST_H2_HPACK_BYTES;
  server->h2_max_settings_count = CHTTP_WEBSOCKET_TEST_H2_SETTINGS_COUNT;
  client->network.max_send_bytes = CHTTP_WEBSOCKET_TEST_H2_BUFFER_BYTES;
  client->network.receive_buffer_bytes = CHTTP_WEBSOCKET_TEST_H2_BUFFER_BYTES;
}

static int chttp_websocket_test_global(void *user, const chttp_server_request_view *request,
                                       chttp_server_response *response, chttp_server_next *next) {
  chttp_websocket_test_probe *probe = (chttp_websocket_test_probe *)user;
  (void)request;
  (void)response;
  atomic_fetch_add_explicit(&probe->global_middleware, 1, memory_order_relaxed);
  return chttp_server_next_call(next);
}

static int chttp_websocket_test_route(void *user, const chttp_server_request_view *request,
                                      chttp_server_response *response, chttp_server_next *next) {
  chttp_websocket_test_probe *probe = (chttp_websocket_test_probe *)user;
  (void)request;
  (void)response;
  atomic_fetch_add_explicit(&probe->route_middleware, 1, memory_order_relaxed);
  return chttp_server_next_call(next);
}

static int chttp_websocket_test_open(void *user, chttp_websocket *websocket,
                                     const chttp_server_request_view *request,
                                     chttp_server_response *response) {
  chttp_websocket_test_probe *probe = (chttp_websocket_test_probe *)user;
  const char *id = chttp_server_request_param(request, "id");
  int status;
  if (id == NULL || strcmp(id, "42") != 0 || request->session == NULL) return SALTS_EPROTO;
  if (chttp_server_response_set_header(response, "Sec-WebSocket-Extensions",
                                       "permessage-deflate") != SALTS_EPERM)
    return SALTS_EPROTO;
  status = chttp_session_set(request->session, "peer", id);
  if (status == SALTS_OK)
    status = chttp_server_response_set_header(response, "X-WebSocket-Route", "echo");
  if (status == SALTS_OK) status = chttp_websocket_send_text(websocket, "ready", 5u);
  if (status == SALTS_OK) atomic_fetch_add_explicit(&probe->opened, 1, memory_order_relaxed);
  return status;
}

static int chttp_websocket_test_h2_open(void *user, chttp_websocket *websocket,
                                        const chttp_server_request_view *request,
                                        chttp_server_response *response) {
  if (request == NULL || request->http_major != 2u || request->http_minor != 0u ||
      request->method != CHTTP_METHOD_CONNECT)
    return SALTS_EPROTO;
  return chttp_websocket_test_open(user, websocket, request, response);
}

static int chttp_websocket_test_subprotocol_open(void *user, chttp_websocket *websocket,
                                                 const chttp_server_request_view *request,
                                                 chttp_server_response *response) {
  int status = chttp_server_response_select_websocket_subprotocol(response, request, "mqtt");
  if (status != SALTS_OK) return status;
  return chttp_websocket_test_open(user, websocket, request, response);
}

static int chttp_websocket_test_pool_open(void *user, chttp_websocket *websocket,
                                          const chttp_server_request_view *request,
                                          chttp_server_response *response) {
  chttp_websocket_test_probe *probe = (chttp_websocket_test_probe *)user;
  const char *id = chttp_server_request_param(request, "id");
  int status;
  (void)response;
  if (request == NULL || request->http_major != 2u || request->http_minor != 0u ||
      request->method != CHTTP_METHOD_CONNECT || id == NULL)
    return SALTS_EPROTO;
  status = chttp_websocket_send_text(websocket, id, strlen(id));
  if (status == SALTS_OK) atomic_fetch_add_explicit(&probe->opened, 1, memory_order_relaxed);
  return status;
}

static int chttp_websocket_test_reject_open(void *user, chttp_websocket *websocket,
                                            const chttp_server_request_view *request,
                                            chttp_server_response *response) {
  (void)user;
  (void)websocket;
  (void)request;
  (void)response;
  return SALTS_EPERM;
}

static int chttp_websocket_test_jwt_open(void *user, chttp_websocket *websocket,
                                          const chttp_server_request_view *request,
                                          chttp_server_response *response) {
  chttp_websocket_test_jwt_probe *probe = (chttp_websocket_test_jwt_probe *)user;
  size_t subject_size;
  (void)websocket;
  (void)response;
  if (probe == NULL || request == NULL || request->jwt_claims == NULL ||
      request->jwt_claims->subject == NULL)
    return SALTS_EPERM;
  subject_size = strlen(request->jwt_claims->subject);
  if (subject_size >= sizeof(probe->subject)) return SALTS_ENOBUFS;
  memcpy(probe->subject, request->jwt_claims->subject, subject_size + 1u);
  atomic_fetch_add_explicit(&probe->opened, 1, memory_order_relaxed);
  return SALTS_OK;
}

static int chttp_websocket_test_capture_open(void *user, chttp_websocket *websocket,
                                              const chttp_server_request_view *request,
                                              chttp_server_response *response) {
  chttp_websocket_test_server_session_probe *probe =
      (chttp_websocket_test_server_session_probe *)user;
  int status;
  (void)response;
  atomic_store_explicit(&probe->peer_present, request != NULL && request->peer != NULL,
                        memory_order_release);
  status = chttp_server_websocket_session_capture(websocket, &probe->session);
  if (status == SALTS_OK)
    atomic_store_explicit(&probe->captured, 1, memory_order_release);
  return status;
}

static void chttp_websocket_test_capture_event(void *user, chttp_websocket *websocket,
                                                const chttp_websocket_event *event) {
  (void)user;
  (void)websocket;
  (void)event;
}

static int chttp_websocket_test_empty_source(void *user, void *buffer, size_t capacity,
                                             size_t *out_size) {
  (void)user;
  (void)buffer;
  (void)capacity;
  *out_size = 0u;
  return SALTS_OK;
}

static int chttp_websocket_test_stream_open(void *user, chttp_websocket *websocket,
                                            const chttp_server_request_view *request,
                                            chttp_server_response *response) {
  const chttp_body_source source = {
      .read = chttp_websocket_test_empty_source, .content_length = 0u, .content_length_known = 1};
  (void)user;
  (void)websocket;
  (void)request;
  return chttp_server_response_source(response, 200u, "text/plain", &source);
}

static void chttp_websocket_test_event(void *user, chttp_websocket *websocket,
                                       const chttp_websocket_event *event) {
  chttp_websocket_test_probe *probe = (chttp_websocket_test_probe *)user;
  if (event->kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
    atomic_fetch_add_explicit(&probe->messages, 1, memory_order_relaxed);
    if (event->size == 12u && memcmp(event->data, "server-close", 12u) == 0) {
      (void)chttp_websocket_close(websocket, 1000u, NULL, 0u);
    } else if (event->size == 5u && memcmp(event->data, "pings", 5u) == 0) {
      (void)chttp_websocket_send_ping(websocket, "1", 1u);
      (void)chttp_websocket_send_ping(websocket, "2", 1u);
    } else {
      (void)chttp_websocket_send_text(websocket, event->data, event->size);
    }
  } else if (event->kind == CHTTP_WEBSOCKET_EVENT_PING) {
    atomic_fetch_add_explicit(&probe->pings, 1, memory_order_relaxed);
  } else if (event->kind == CHTTP_WEBSOCKET_EVENT_PONG) {
    atomic_fetch_add_explicit(&probe->pongs, 1, memory_order_relaxed);
  } else if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
    atomic_fetch_add_explicit(&probe->closes, 1, memory_order_relaxed);
  }
}

spec("CHTTP WebSocket client/server") {
  it("authenticates a protected HTTP1 WebSocket before the opening callback") {
    static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
    const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
    const chttp_jwt_bearer_validator_options validator_options = {
        .size = sizeof(validator_options), .key = key, .key_size = sizeof(key) - 1u};
    chttp_websocket_test_jwt_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/jwt",
                                            .on_open = chttp_websocket_test_jwt_open,
                                            .on_event = chttp_websocket_test_capture_event,
                                            .user = &probe};
    chttp_jwt_bearer_validator validator = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_client client = {0};
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .timeout_ms =
                                                           CHTTP_WEBSOCKET_TEST_TIMEOUT_MS};
    char *token = NULL;
    char authorization[512];
    chttp_header header = {0};
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    memset(&probe, 0, sizeof(probe));
    atomic_init(&probe.opened, 0);
    check_equal(chttp_jwt_hs256_token_create(&claims, key, sizeof(key) - 1u, &token), SALTS_OK);
    check_equal(chttp_jwt_bearer_header(token, authorization, sizeof(authorization), &header),
                SALTS_OK);
    check_equal(chttp_jwt_bearer_validator_init(&validator, &validator_options), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with_jwt_bearer(&server, &route, &validator), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/jwt", (unsigned int)port) > 0);

    connect_options.uri = uri;
    connect_options.headers = &header;
    connect_options.header_count = 1u;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status), SALTS_OK);
    check_equal(http_status, 101u);
    check_equal(atomic_load_explicit(&probe.opened, memory_order_relaxed), 1);
    check_equal(probe.subject, "alice");
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);

    client = (chttp_websocket_client){0};
    connect_options.headers = NULL;
    connect_options.header_count = 0u;
    http_status = 0u;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status), SALTS_EPROTO);
    check_equal(http_status, 401u);
    check_equal(atomic_load_explicit(&probe.opened, memory_order_relaxed), 1);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);

    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
    chttp_jwt_token_destroy(token);
  }

  it("admits a copied server WebSocket send from outside the callback thread") {
    chttp_websocket_test_server_session_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/async",
                                            .on_open = chttp_websocket_test_capture_open,
                                            .on_event = chttp_websocket_test_capture_event,
                                            .user = &probe};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options)};
    chttp_websocket_event event;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    memset(&probe, 0, sizeof(probe));
    atomic_init(&probe.captured, 0);
    atomic_init(&probe.peer_present, 0);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/async", (unsigned int)port) > 0);
    connect_options.uri = uri;
    connect_options.timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status), SALTS_OK);
    check_equal(http_status, 101u);
    check_equal(atomic_load_explicit(&probe.captured, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&probe.peer_present, memory_order_acquire), 1);
    check_equal(chttp_server_websocket_send_binary(&probe.session, "mqtt", 4u), SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.message_type, CHTTP_WEBSOCKET_MESSAGE_BINARY);
    check_equal(event.size, 4u);
    check_equal(memcmp(event.data, "mqtt", 4u), 0);
    check_equal(chttp_server_websocket_close(&probe.session, 1000u, NULL, 0u), SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_CLOSE);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("keeps H1 input admitted while an asynchronous WebSocket output is pending") {
    static const char outbound[] = "server-output";
    static const char ping[] = "peer-ping";
    chttp_websocket_test_server_session_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/duplex",
                                            .on_open = chttp_websocket_test_capture_open,
                                            .on_event = chttp_websocket_test_capture_event,
                                            .user = &probe};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options)};
    chttp_websocket_event event = {0};
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    memset(&probe, 0, sizeof(probe));
    atomic_init(&probe.captured, 0);
    atomic_init(&probe.peer_present, 0);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/duplex", (unsigned int)port) > 0);
    connect_options.uri = uri;
    connect_options.timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status), SALTS_OK);
    check_equal(http_status, 101u);
    check_equal(atomic_load_explicit(&probe.captured, memory_order_acquire), 1);

    for (size_t round = 0u; round < CHTTP_WEBSOCKET_TEST_DUPLEX_ROUNDS; ++round) {
      size_t messages = 0u;
      int saw_pong = 0;
      check_equal(chttp_server_websocket_send_text(&probe.session, outbound, sizeof(outbound) - 1u),
                  SALTS_OK);
      check_equal(chttp_server_websocket_send_text(&probe.session, outbound, sizeof(outbound) - 1u),
                  SALTS_OK);
      check_equal(chttp_websocket_client_send_ping(&client, ping, sizeof(ping) - 1u,
                                                   CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
                  SALTS_OK);
      for (size_t received = 0u; received < 3u; ++received) {
        check_equal(
            chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
            SALTS_OK);
        if (event.kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
          check_equal(event.message_type, CHTTP_WEBSOCKET_MESSAGE_TEXT);
          check_equal(event.size, sizeof(outbound) - 1u);
          check_equal(memcmp(event.data, outbound, sizeof(outbound) - 1u), 0);
          ++messages;
        } else {
          check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_PONG);
          check_equal(event.size, sizeof(ping) - 1u);
          check_equal(memcmp(event.data, ping, sizeof(ping) - 1u), 0);
          saw_pong = 1;
        }
      }
      check_equal(messages, 2u);
      check_true(saw_pong);
    }

    check_equal(
        chttp_websocket_client_close(&client, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("routes a copied server WebSocket send to the captured HTTP/2 stream") {
    chttp_websocket_test_server_session_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/async-h2",
                                            .on_open = chttp_websocket_test_capture_open,
                                            .on_event = chttp_websocket_test_capture_event,
                                            .user = &probe};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_pool_config pool_config;
    chttp_websocket_pool pool = {0};
    chttp_websocket_session client_session = {0};
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .protocol = CHTTP_HTTP_2,
                                                       .timeout_ms =
                                                           CHTTP_WEBSOCKET_TEST_TIMEOUT_MS};
    chttp_websocket_event event;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    memset(&probe, 0, sizeof(probe));
    atomic_init(&probe.captured, 0);
    atomic_init(&probe.peer_present, 0);
    chttp_websocket_test_enable_h2(&server_config, &client_config);
    pool_config = (chttp_websocket_pool_config){.size = sizeof(pool_config),
                                                .client = client_config,
                                                .session_capacity = 2u};
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(
        snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/async-h2", (unsigned int)port) > 0);
    connect_options.uri = uri;
    check_equal(chttp_websocket_pool_init(&pool, &pool_config), SALTS_OK);
    check_equal(chttp_websocket_pool_open(&pool, &connect_options, &client_session, &http_status),
                SALTS_OK);
    check_equal(http_status, 200u);
    check_equal(atomic_load_explicit(&probe.captured, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&probe.peer_present, memory_order_acquire), 1);
    check_equal(chttp_server_websocket_send_binary(&probe.session, "h2", 2u), SALTS_OK);
    check_equal(chttp_websocket_pool_receive(&pool, client_session,
                                             CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.message_type, CHTTP_WEBSOCKET_MESSAGE_BINARY);
    check_equal(event.size, 2u);
    check_equal(memcmp(event.data, "h2", 2u), 0);
    check_equal(chttp_server_websocket_close(&probe.session, 1000u, NULL, 0u), SALTS_OK);
    check_equal(chttp_websocket_pool_receive(&pool, client_session,
                                             CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_CLOSE);
    check_equal(chttp_websocket_pool_close(&pool, client_session, 1000u, NULL, 0u,
                                           CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(chttp_websocket_pool_destroy(&pool, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("rejects aggregate WebSocket storage overflow before allocation") {
    chttp_websocket_test_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/overflow",
                                            .max_frame_bytes = 2u,
                                            .max_message_bytes = SIZE_MAX - 5u,
                                            .max_buffered_input_bytes = 16u,
                                            .on_open = chttp_websocket_test_open,
                                            .on_event = chttp_websocket_test_event,
                                            .user = &probe};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();

    client_config.max_frame_bytes = 2u;
    client_config.max_message_bytes = SIZE_MAX - 5u;
    client_config.max_buffered_input_bytes = 16u;
    client_config.event_capacity = 1u;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_ERANGE);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_ERANGE);
    check_null(client.impl);
  }

  it("rejects an HTTP/2 input bound smaller than one peer frame") {
    chttp_websocket_client client = {0};
    chttp_websocket_client_config config = chttp_websocket_test_client_config();

    config.h2_input_buffer_bytes = 16u * 1024u;
    check_equal(chttp_websocket_client_init(&client, &config), SALTS_EMSGSIZE);
    check_null(client.impl);
  }

  it("rejects invalid WebSocket pool bounds before allocation") {
    chttp_websocket_pool pool = {0};
    chttp_websocket_pool_config config = {.size = sizeof(config),
                                          .client = chttp_websocket_test_client_config(),
                                          .session_capacity = 0u};

    check_equal(chttp_websocket_pool_init(&pool, &config), SALTS_EINVAL);
    check_null(pool.impl);
    config.session_capacity = 2u;
    config.client.size = 0u;
    check_equal(chttp_websocket_pool_init(&pool, &config), SALTS_EINVAL);
    check_null(pool.impl);
    config.client = chttp_websocket_test_client_config();
    config.client.h2_input_buffer_bytes = 16u * 1024u;
    check_equal(chttp_websocket_pool_init(&pool, &config), SALTS_EMSGSIZE);
    check_null(pool.impl);
  }

  it("upgrades a routed ws connection without exposing a poller") {
    chttp_websocket_test_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_middleware route_middleware = {chttp_websocket_test_route, &probe};
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/chat/:id",
                                            .middleware = &route_middleware,
                                            .middleware_count = 1u,
                                            .on_open = chttp_websocket_test_subprotocol_open,
                                            .on_event = chttp_websocket_test_event,
                                            .user = &probe};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .subprotocol = "mqtt"};
    chttp_websocket_event event;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    atomic_init(&probe.global_middleware, 0);
    atomic_init(&probe.route_middleware, 0);
    atomic_init(&probe.opened, 0);
    atomic_init(&probe.messages, 0);
    atomic_init(&probe.pings, 0);
    atomic_init(&probe.pongs, 0);
    atomic_init(&probe.closes, 0);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_use(&server, chttp_websocket_test_global, &probe), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(
        snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/chat/42?mode=echo", (unsigned int)port) > 0);
    connect_options.uri = uri;
    connect_options.timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status), SALTS_OK);
    check_equal(http_status, 101u);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.message_type, CHTTP_WEBSOCKET_MESSAGE_TEXT);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "ready", 5u), 0);
    check_equal(
        chttp_websocket_client_close(&client, 1006u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_EINVAL);
    check_equal(
        chttp_websocket_client_send_text(&client, "hello", 5u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_websocket_client_send_text(&client, "again", 5u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "hello", 5u), 0);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "again", 5u), 0);
    check_equal(chttp_websocket_client_send_ping(&client, "?", 1u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_PONG);
    check_equal(event.size, 1u);
    check_equal(memcmp(event.data, "?", 1u), 0);
    check_equal(
        chttp_websocket_client_send_text(&client, "pings", 5u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_PING);
    check_equal(event.size, 1u);
    check_equal(memcmp(event.data, "1", 1u), 0);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_PING);
    check_equal(event.size, 1u);
    check_equal(memcmp(event.data, "2", 1u), 0);
    check_equal(
        chttp_websocket_client_send_text(&client, "after", 5u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "after", 5u), 0);
    check_equal(
        chttp_websocket_client_close(&client, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.global_middleware, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.route_middleware, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.opened, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.messages, memory_order_relaxed), 4);
    check_equal(atomic_load_explicit(&probe.pings, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.pongs, memory_order_relaxed), 2);
    check_equal(atomic_load_explicit(&probe.closes, memory_order_relaxed), 1);
  }

  it("uses the same routed session over verified wss") {
    static const char *h1_alpn[] = {"http/1.1"};
    chttp_websocket_test_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    cnet_tls_server_config server_tls;
    cnet_tls_client_config client_tls;
    chttp_tls_profile profile = {0};
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/chat/:id",
                                            .on_open = chttp_websocket_test_open,
                                            .on_event = chttp_websocket_test_event,
                                            .user = &probe};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options)};
    chttp_websocket_event event;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];
    char *cert_path = tt_make_temp_file("chttp-websocket-cert", ".pem");
    char *key_path = tt_make_temp_file("chttp-websocket-key", ".pem");

    check_not_null(cert_path);
    check_not_null(key_path);
    check_equal(tt_write_file(cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(key_path, CHTTP_TLS_TEST_KEY, sizeof(CHTTP_TLS_TEST_KEY) - 1u), 0);
    atomic_init(&probe.global_middleware, 0);
    atomic_init(&probe.route_middleware, 0);
    atomic_init(&probe.opened, 0);
    atomic_init(&probe.messages, 0);
    atomic_init(&probe.pings, 0);
    atomic_init(&probe.pongs, 0);
    atomic_init(&probe.closes, 0);
    server_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    server_config.network.tls_handshake_timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    client_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    client_config.network.tls_handshake_timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    server_tls = (cnet_tls_server_config){.size = sizeof(server_tls),
                                          .cert_file = cert_path,
                                          .key_file = key_path,
                                          .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                          .alpn_protocols = h1_alpn,
                                          .alpn_protocol_count = 1u};
    client_tls = (cnet_tls_client_config){.size = sizeof(client_tls),
                                          .ca_file = cert_path,
                                          .server_name = "localhost",
                                          .alpn_protocols = h1_alpn,
                                          .alpn_protocol_count = 1u};
    server_config.tls = &server_tls;

    check_equal(chttp_tls_profile_init(&profile, &client_tls), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "wss://127.0.0.1:%u/chat/42", (unsigned int)port) > 0);
    connect_options.uri = uri;
    connect_options.tls = &profile;
    connect_options.timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status), SALTS_OK);
    check_equal(http_status, 101u);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.message_type, CHTTP_WEBSOCKET_MESSAGE_TEXT);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "ready", 5u), 0);
    check_equal(
        chttp_websocket_client_send_text(&client, "secure", 6u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 6u);
    check_equal(memcmp(event.data, "secure", 6u), 0);
    check_equal(
        chttp_websocket_client_close(&client, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.opened, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.messages, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.closes, memory_order_relaxed), 1);
    check_equal(tt_remove_file(cert_path), 0);
    check_equal(tt_remove_file(key_path), 0);
    free(cert_path);
    free(key_path);
  }

  it("uses the blocking API over an RFC 8441 h2c stream") {
    chttp_websocket_test_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/chat/:id",
                                            .on_open = chttp_websocket_test_subprotocol_open,
                                            .on_event = chttp_websocket_test_event,
                                            .user = &probe};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .protocol = CHTTP_HTTP_2,
                                                       .subprotocol = "mqtt"};
    chttp_websocket_event event;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    atomic_init(&probe.global_middleware, 0);
    atomic_init(&probe.route_middleware, 0);
    atomic_init(&probe.opened, 0);
    atomic_init(&probe.messages, 0);
    atomic_init(&probe.pings, 0);
    atomic_init(&probe.pongs, 0);
    atomic_init(&probe.closes, 0);
    chttp_websocket_test_enable_h2(&server_config, &client_config);

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/chat/42", (unsigned int)port) > 0);
    connect_options.uri = uri;
    connect_options.timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status), SALTS_OK);
    check_equal(http_status, 200u);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.message_type, CHTTP_WEBSOCKET_MESSAGE_TEXT);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "ready", 5u), 0);
    check_equal(
        chttp_websocket_client_send_text(&client, "hello", 5u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "hello", 5u), 0);
    check_equal(chttp_websocket_client_send_ping(&client, "?", 1u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_PONG);
    check_equal(event.size, 1u);
    check_equal(memcmp(event.data, "?", 1u), 0);
    check_equal(
        chttp_websocket_client_close(&client, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.opened, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.messages, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.pings, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.closes, memory_order_relaxed), 1);
  }

  it("multiplexes bounded RFC 8441 sessions on one H2 connection") {
    chttp_websocket_test_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/pool/:id",
                                            .on_open = chttp_websocket_test_pool_open,
                                            .on_event = chttp_websocket_test_event,
                                            .user = &probe};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_pool_config pool_config = {
        .size = sizeof(pool_config), .client = client_config, .session_capacity = 2u};
    chttp_websocket_pool pool = {0};
    chttp_websocket_session first = {0};
    chttp_websocket_session second = {0};
    chttp_websocket_session rejected = {0};
    chttp_websocket_session replacement = {0};
    chttp_websocket_connect_options options = {.size = sizeof(options),
                                               .protocol = CHTTP_HTTP_2,
                                               .timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS};
    chttp_websocket_event event;
    chttp_server_stats stats;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char first_uri[128];
    char second_uri[128];
    char other_origin_uri[128];

    atomic_init(&probe.global_middleware, 0);
    atomic_init(&probe.route_middleware, 0);
    atomic_init(&probe.opened, 0);
    atomic_init(&probe.messages, 0);
    atomic_init(&probe.pings, 0);
    atomic_init(&probe.pongs, 0);
    atomic_init(&probe.closes, 0);
    chttp_websocket_test_enable_h2(&server_config, &pool_config.client);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(first_uri, sizeof(first_uri), "ws://127.0.0.1:%u/pool/first",
                        (unsigned int)port) > 0);
    check_true(snprintf(second_uri, sizeof(second_uri), "ws://127.0.0.1:%u/pool/second",
                        (unsigned int)port) > 0);
    check_true(snprintf(other_origin_uri, sizeof(other_origin_uri), "ws://localhost:%u/pool/other",
                        (unsigned int)port) > 0);

    check_equal(chttp_websocket_pool_init(&pool, &pool_config), SALTS_OK);
    options.uri = first_uri;
    check_equal(chttp_websocket_pool_open(&pool, &options, &first, &http_status), SALTS_OK);
    check_equal(http_status, 200u);
    options.uri = second_uri;
    check_equal(chttp_websocket_pool_open(&pool, &options, &second, &http_status), SALTS_OK);
    check_equal(http_status, 200u);
    check_not_equal(first.slot, 0u);
    check_not_equal(second.slot, 0u);
    check_not_equal(first.slot, second.slot);

    options.uri = first_uri;
    check_equal(chttp_websocket_pool_open(&pool, &options, &rejected, &http_status), SALTS_ENOBUFS);
    check_equal(rejected.slot, 0u);
    check_equal(rejected.generation, 0u);
    options.uri = other_origin_uri;
    check_equal(chttp_websocket_pool_open(&pool, &options, &rejected, &http_status), SALTS_EINVAL);
    options.uri = first_uri;
    options.protocol = CHTTP_HTTP_1_1;
    check_equal(chttp_websocket_pool_open(&pool, &options, &rejected, &http_status),
                SALTS_EPROTONOSUPPORT);
    options.protocol = CHTTP_HTTP_2;

    check_equal(chttp_websocket_pool_receive(&pool, first, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "first", 5u), 0);
    check_equal(
        chttp_websocket_pool_receive(&pool, second, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
        SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 6u);
    check_equal(memcmp(event.data, "second", 6u), 0);

    check_equal(
        chttp_websocket_pool_send_text(&pool, first, "one", 3u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_websocket_pool_send_text(&pool, second, "two", 3u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_websocket_pool_receive(&pool, second, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
        SALTS_OK);
    check_equal(event.size, 3u);
    check_equal(memcmp(event.data, "two", 3u), 0);
    check_equal(chttp_websocket_pool_receive(&pool, first, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.size, 3u);
    check_equal(memcmp(event.data, "one", 3u), 0);

    check_equal(
        chttp_websocket_pool_close(&pool, first, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_websocket_pool_send_text(&pool, first, "stale", 5u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_ENOENT);
    options.uri = first_uri;
    check_equal(chttp_websocket_pool_open(&pool, &options, &replacement, &http_status), SALTS_OK);
    check_equal(http_status, 200u);
    check_equal(replacement.slot, first.slot);
    check_not_equal(replacement.generation, first.generation);
    check_equal(
        chttp_websocket_pool_receive(&pool, replacement, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
        SALTS_OK);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "first", 5u), 0);
    check_equal(chttp_websocket_pool_send_text(&pool, replacement, "server-close", 12u,
                                               CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(
        chttp_websocket_pool_receive(&pool, replacement, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
        SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_CLOSE);
    check_equal(chttp_websocket_pool_close(&pool, replacement, 1000u, NULL, 0u,
                                           CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(
        chttp_websocket_pool_send_text(&pool, second, "alive", 5u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_websocket_pool_receive(&pool, second, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
        SALTS_OK);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "alive", 5u), 0);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)1u);

    check_equal(
        chttp_websocket_pool_close(&pool, second, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_pool_destroy(&pool, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.opened, memory_order_relaxed), 3);
    check_equal(atomic_load_explicit(&probe.messages, memory_order_relaxed), 4);
    check_equal(atomic_load_explicit(&probe.closes, memory_order_relaxed), 3);
  }

  it("honors the peer stream limit and drains active sessions on pool destroy") {
    chttp_websocket_test_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/limited/:id",
                                            .on_open = chttp_websocket_test_pool_open,
                                            .on_event = chttp_websocket_test_event,
                                            .user = &probe};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_pool_config pool_config = {
        .size = sizeof(pool_config), .client = client_config, .session_capacity = 2u};
    chttp_websocket_pool pool = {0};
    chttp_websocket_session admitted = {0};
    chttp_websocket_session rejected = {0};
    chttp_websocket_connect_options options = {.size = sizeof(options),
                                               .protocol = CHTTP_HTTP_2,
                                               .timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS};
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char first_uri[128];
    char second_uri[128];

    atomic_init(&probe.global_middleware, 0);
    atomic_init(&probe.route_middleware, 0);
    atomic_init(&probe.opened, 0);
    atomic_init(&probe.messages, 0);
    atomic_init(&probe.pings, 0);
    atomic_init(&probe.pongs, 0);
    atomic_init(&probe.closes, 0);
    chttp_websocket_test_enable_h2(&server_config, &pool_config.client);
    server_config.h2_stream_capacity = 1u;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(first_uri, sizeof(first_uri), "ws://127.0.0.1:%u/limited/first",
                        (unsigned int)port) > 0);
    check_true(snprintf(second_uri, sizeof(second_uri), "ws://127.0.0.1:%u/limited/second",
                        (unsigned int)port) > 0);
    check_equal(chttp_websocket_pool_init(&pool, &pool_config), SALTS_OK);
    options.uri = first_uri;
    check_equal(chttp_websocket_pool_open(&pool, &options, &admitted, &http_status), SALTS_OK);
    check_equal(http_status, 200u);
    options.uri = second_uri;
    check_equal(chttp_websocket_pool_open(&pool, &options, &rejected, &http_status), SALTS_ENOBUFS);
    check_equal(rejected.slot, 0u);
    check_equal(chttp_websocket_pool_destroy(&pool, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.opened, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.closes, memory_order_relaxed), 1);
  }

  it("returns the HTTP status when an RFC 8441 route is absent") {
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .protocol = CHTTP_HTTP_2};
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    chttp_websocket_test_enable_h2(&server_config, &client_config);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/missing", (unsigned int)port) > 0);
    connect_options.uri = uri;
    connect_options.timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status),
                SALTS_EPROTO);
    check_equal(http_status, 404u);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("keeps H2 opening rejection semantics aligned with H1") {
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    chttp_server_websocket_options reject_route = {.size = sizeof(reject_route),
                                                   .path = "/reject",
                                                   .on_open = chttp_websocket_test_reject_open,
                                                   .on_event = chttp_websocket_test_event};
    chttp_server_websocket_options stream_route = {.size = sizeof(stream_route),
                                                   .path = "/stream",
                                                   .on_open = chttp_websocket_test_stream_open,
                                                   .on_event = chttp_websocket_test_event};
    chttp_websocket_client reject_client = {0};
    chttp_websocket_client stream_client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .protocol = CHTTP_HTTP_2};
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    chttp_websocket_test_enable_h2(&server_config, &client_config);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &reject_route), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &stream_route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);

    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/reject", (unsigned int)port) > 0);
    connect_options.uri = uri;
    connect_options.timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_websocket_client_init(&reject_client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&reject_client, &connect_options, &http_status),
                SALTS_EPROTO);
    check_equal(http_status, 500u);
    check_equal(chttp_websocket_client_destroy(&reject_client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
                SALTS_OK);

    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/stream", (unsigned int)port) > 0);
    http_status = 0u;
    check_equal(chttp_websocket_client_init(&stream_client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&stream_client, &connect_options, &http_status),
                SALTS_EPROTO);
    check_equal(http_status, 500u);
    check_equal(chttp_websocket_client_destroy(&stream_client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
                SALTS_OK);

    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("uses RFC 8441 over verified WSS with h2 ALPN") {
    static const char *h2_alpn[] = {"h2"};
    chttp_websocket_test_probe probe;
    chttp_server server = {0};
    chttp_server_config server_config = chttp_websocket_test_server_config();
    cnet_tls_server_config server_tls;
    cnet_tls_client_config client_tls;
    chttp_tls_profile profile = {0};
    chttp_server_websocket_options route = {.size = sizeof(route),
                                            .path = "/chat/:id",
                                            .on_open = chttp_websocket_test_h2_open,
                                            .on_event = chttp_websocket_test_event,
                                            .user = &probe};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = chttp_websocket_test_client_config();
    chttp_websocket_pool pool = {0};
    chttp_websocket_pool_config pool_config;
    chttp_websocket_session first = {0};
    chttp_websocket_session second = {0};
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .protocol = CHTTP_HTTP_2};
    chttp_websocket_event event;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];
    char second_uri[128];
    char *cert_path = tt_make_temp_file("chttp-h2-websocket-cert", ".pem");
    char *key_path = tt_make_temp_file("chttp-h2-websocket-key", ".pem");

    check_not_null(cert_path);
    check_not_null(key_path);
    check_equal(tt_write_file(cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(key_path, CHTTP_TLS_TEST_KEY, sizeof(CHTTP_TLS_TEST_KEY) - 1u), 0);
    atomic_init(&probe.global_middleware, 0);
    atomic_init(&probe.route_middleware, 0);
    atomic_init(&probe.opened, 0);
    atomic_init(&probe.messages, 0);
    atomic_init(&probe.pings, 0);
    atomic_init(&probe.pongs, 0);
    atomic_init(&probe.closes, 0);
    chttp_websocket_test_enable_h2(&server_config, &client_config);
    server_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    server_config.network.tls_handshake_timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    client_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    client_config.network.tls_handshake_timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    server_tls = (cnet_tls_server_config){.size = sizeof(server_tls),
                                          .cert_file = cert_path,
                                          .key_file = key_path,
                                          .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                          .alpn_protocols = h2_alpn,
                                          .alpn_protocol_count = 1u};
    client_tls = (cnet_tls_client_config){.size = sizeof(client_tls),
                                          .ca_file = cert_path,
                                          .server_name = "localhost",
                                          .alpn_protocols = h2_alpn,
                                          .alpn_protocol_count = 1u};
    server_config.tls = &server_tls;

    check_equal(chttp_tls_profile_init(&profile, &client_tls), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "wss://127.0.0.1:%u/chat/42", (unsigned int)port) > 0);
    connect_options.uri = uri;
    connect_options.tls = &profile;
    connect_options.timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status), SALTS_OK);
    check_equal(http_status, 200u);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 5u);
    check_equal(memcmp(event.data, "ready", 5u), 0);
    check_equal(
        chttp_websocket_client_send_text(&client, "secure", 6u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.size, 6u);
    check_equal(memcmp(event.data, "secure", 6u), 0);
    check_equal(
        chttp_websocket_client_close(&client, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);

    pool_config = (chttp_websocket_pool_config){
        .size = sizeof(pool_config), .client = client_config, .session_capacity = 2u};
    check_true(snprintf(second_uri, sizeof(second_uri), "wss://127.0.0.1:%u/chat/42?stream=two",
                        (unsigned int)port) > 0);
    check_equal(chttp_websocket_pool_init(&pool, &pool_config), SALTS_OK);
    connect_options.uri = uri;
    check_equal(chttp_websocket_pool_open(&pool, &connect_options, &first, &http_status), SALTS_OK);
    connect_options.uri = second_uri;
    check_equal(chttp_websocket_pool_open(&pool, &connect_options, &second, &http_status),
                SALTS_OK);
    check_equal(chttp_websocket_pool_receive(&pool, first, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(
        chttp_websocket_pool_receive(&pool, second, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS, &event),
        SALTS_OK);
    check_equal(
        chttp_websocket_pool_close(&pool, first, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_websocket_pool_close(&pool, second, 1000u, NULL, 0u, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_websocket_pool_destroy(&pool, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.opened, memory_order_relaxed), 3);
    check_equal(atomic_load_explicit(&probe.messages, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.closes, memory_order_relaxed), 3);
    check_equal(tt_remove_file(cert_path), 0);
    check_equal(tt_remove_file(key_path), 0);
    free(cert_path);
    free(key_path);
  }

  it("rejects an HTTP/2 TLS profile instead of downgrading WSS") {
    static const char *h2_alpn[] = {"h2"};
    chttp_tls_profile profile = {0};
    cnet_tls_client_config tls = {
        .size = sizeof(tls), .alpn_protocols = h2_alpn, .alpn_protocol_count = 1u};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config config = chttp_websocket_test_client_config();
    chttp_websocket_connect_options options = {.size = sizeof(options),
                                               .uri = "wss://127.0.0.1:443/chat",
                                               .tls = &profile,
                                               .timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS};
    unsigned int http_status = 99u;

    config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.network.tls_handshake_timeout_ms = CHTTP_WEBSOCKET_TEST_TIMEOUT_MS;
    check_equal(chttp_tls_profile_init(&profile, &tls), SALTS_OK);
    check_equal(chttp_websocket_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &options, &http_status),
                SALTS_EPROTONOSUPPORT);
    check_equal(http_status, 0u);
    check_equal(chttp_websocket_client_destroy(&client, CHTTP_WEBSOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
  }
}
