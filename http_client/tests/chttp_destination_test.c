#include <http_client/http.h>
#include <http_server/http.h>
#include <salts/clock.h>
#include <fmt.h>
#include <string.h>
#include "tinytest.h"

enum { DESTINATION_TEST_TIMEOUT_MS = 5000 };

static struct {
  chttp_server servers[2];
  chttp_async_client async;
  chttp_client blocking;
  chttp_client_config config;
  chttp_response response;
  tstr uris[2];
  const char *mapping[2];
  cnet_destination_hint hints[2];
  chttp_destination_options destinations;
  size_t completed;
  int status;
  char peer;
} fixture;

static int destination_reply(void *user, const chttp_server_request_view *request,
                              chttp_server_response *response) {
  (void)request;
  return chttp_server_reply(response, 200u, "text/plain", user, 1u);
}

static void destination_complete(void *user, chttp_request request,
                                  const chttp_response_view *response, const chttp_error *error) {
  (void)user;
  (void)request;
  ++fixture.completed;
  fixture.status = error != NULL ? error->status : SALTS_OK;
  fixture.peer = response != NULL && response->body_size == 1u
      ? ((const char *)response->body)[0] : '?';
}

static chttp_request_options destination_request(chttp_protocol protocol) {
  return (chttp_request_options){.authority = "destination.test", .target = "/peer",
      .method = CHTTP_METHOD_GET, .protocol = protocol, .on_complete = destination_complete};
}

static size_t destination_accepts(size_t peer) {
  chttp_server_stats stats;
  check_equal(chttp_server_get_stats(&fixture.servers[peer], &stats), SALTS_OK);
  return (size_t)stats.accepted_connections;
}

static void destination_wait(size_t completed) {
  const uint64_t deadline = cmeta_monotonic_ms() + DESTINATION_TEST_TIMEOUT_MS;
  size_t count = 0u;
  while (fixture.completed < completed && cmeta_monotonic_ms() < deadline)
    check_equal(chttp_async_client_poll(&fixture.async, 5u, &count), SALTS_OK);
  check_equal(fixture.completed, completed);
  check_equal(fixture.status, SALTS_OK);
}

static void destination_exchange(chttp_protocol protocol, uint64_t expected_id) {
  chttp_request_options options = destination_request(protocol);
  chttp_request request = {0};
  cnet_destination_result selected;
  const size_t next = fixture.completed + 1u;
  check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                           &selected, &request), SALTS_OK);
  check_equal(selected.endpoint_id, expected_id);
  check_equal(selected.snapshot_generation, fixture.destinations.selection.snapshot_generation);
  destination_wait(next);
  check_equal(fixture.peer, expected_id == 10u ? 'a' : 'b');
}

spec("CHttp configured CNet destination selection") {
  before_each() {
    chttp_client_config config = {
        .network = {.backend =
#if defined(_WIN32)
          NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          NATIVE_IO_BACKEND_EPOLL,
#else
          NATIVE_IO_BACKEND_KQUEUE,
#endif
          .connection_capacity = 4u, .command_capacity = 16u, .request_capacity = 16u,
          .completion_batch_capacity = 8u, .event_capacity = 16u, .max_send_bytes = 65536u,
          .receive_buffer_bytes = 1024u, .connect_timeout_ms = DESTINATION_TEST_TIMEOUT_MS,
          .read_timeout_ms = DESTINATION_TEST_TIMEOUT_MS,
          .write_timeout_ms = DESTINATION_TEST_TIMEOUT_MS},
        .request_capacity = 4u, .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 512u, .max_request_body_bytes = 128u,
        .max_response_body_bytes = 128u, .max_informational_responses = 4u};
    chttp_server_config server = {
        .host = "127.0.0.1", .backlog = 8u, .network = config.network,
        .route_capacity = 1u, .max_target_bytes = 128u, .max_header_count = 16u,
        .max_header_bytes = 512u, .max_request_body_bytes = 128u,
        .max_response_header_count = 8u, .max_response_header_bytes = 512u,
        .max_response_body_bytes = 128u, .poll_slice_ms = 1u,
        .enable_http2 = 1, .h2_stream_capacity = 4u, .h2_input_buffer_bytes = 65536u,
        .h2_output_buffer_bytes = 65536u, .h2_hpack_dynamic_table_bytes = 2048u,
        .h2_max_settings_count = 16u};
    memset(&fixture, 0, sizeof(fixture));
    fixture.config = config;
    for (size_t index = 0u; index < 2u; ++index) {
      uint16_t port = 0u;
      check_equal(chttp_server_init(&fixture.servers[index], &server), SALTS_OK);
      check_equal(chttp_server_get(&fixture.servers[index], "/peer", destination_reply,
                                    index == 0u ? (void *)"a" : (void *)"b"), SALTS_OK);
      check_equal(chttp_server_start(&fixture.servers[index]), SALTS_OK);
      check_equal(chttp_server_port(&fixture.servers[index], &port), SALTS_OK);
      fixture.uris[index] = tstr_format("tcp://127.0.0.1:{}", (unsigned int)port);
      check_not_null(fixture.uris[index]);
      fixture.mapping[index] = fixture.uris[index];
      fixture.hints[index] = (cnet_destination_hint){.endpoint_id = (index + 1u) * 10u,
          .weight = 1u, .eligible = true};
    }
    fixture.destinations = (chttp_destination_options)CHTTP_DESTINATION_OPTIONS_INIT;
    fixture.destinations.selection.endpoints = fixture.hints;
    fixture.destinations.selection.endpoint_count = 2u;
    fixture.destinations.selection.snapshot_generation = 1u;
    fixture.destinations.connection_uris = fixture.mapping;
    check_equal(chttp_async_client_init(&fixture.async, &config), SALTS_OK);
    check_equal(chttp_client_init(&fixture.blocking, &config), SALTS_OK);
  }

  after_each() {
    chttp_response_destroy(&fixture.response);
    if (fixture.async.impl != NULL) {
      check_equal(chttp_async_client_stop(&fixture.async, DESTINATION_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_async_client_destroy(&fixture.async), SALTS_OK);
    }
    check_equal(chttp_client_destroy(&fixture.blocking, DESTINATION_TEST_TIMEOUT_MS), SALTS_OK);
    for (size_t index = 0u; index < 2u; ++index) {
      if (fixture.servers[index].impl != NULL) {
        check_equal(chttp_server_stop(&fixture.servers[index], DESTINATION_TEST_TIMEOUT_MS), SALTS_OK);
        check_equal(chttp_server_destroy(&fixture.servers[index]), SALTS_OK);
      }
      tstr_free(fixture.uris[index]);
    }
  }

  it("routes round-robin H1 requests and reuses only their selected endpoint") {
    for (size_t index = 0u; index < 4u; ++index) {
      fixture.destinations.selection.sequence = index;
      destination_exchange(CHTTP_HTTP_1_1, index % 2u == 0u ? 10u : 20u);
    }
    check_equal(destination_accepts(0u), (size_t)1u);
    check_equal(destination_accepts(1u), (size_t)1u);
  }

  it("applies weighted and least-inflight policies to real H2 requests") {
    fixture.destinations.selection.kind = CNET_DESTINATION_WEIGHTED_RR;
    fixture.hints[1].weight = 2u;
    for (size_t index = 0u; index < 3u; ++index) {
      fixture.destinations.selection.sequence = index;
      destination_exchange(CHTTP_HTTP_2, index == 0u ? 10u : 20u);
    }
    fixture.destinations.selection.kind = CNET_DESTINATION_LEAST_INFLIGHT;
    fixture.hints[0].inflight = 2u;
    fixture.hints[1].inflight = 1u;
    destination_exchange(CHTTP_HTTP_2, 20u);
    check_equal(destination_accepts(0u), (size_t)1u);
    check_equal(destination_accepts(1u), (size_t)1u);
  }

  it("does not spill explicit or strict-key selection to another healthy peer") {
    chttp_request_options options = destination_request(CHTTP_HTTP_1_1);
    chttp_request request;
    cnet_destination_result selected;
    fixture.destinations.selection.kind = CNET_DESTINATION_EXPLICIT;
    fixture.destinations.selection.explicit_endpoint_id = 10u;
    fixture.hints[0].eligible = false;
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_ENOBUFS);
    check_equal(selected.index, SIZE_MAX);
    check_equal(request.slot, (uint32_t)0u);
    fixture.hints[0].eligible = true;
    fixture.destinations.selection.kind = CNET_DESTINATION_STRICT_KEY;
    fixture.destinations.selection.key_known = true;
    fixture.destinations.selection.key_hash = 123u;
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_OK);
    destination_wait(1u);
    fixture.hints[selected.index].eligible = false;
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_ENOBUFS);
    check_equal(destination_accepts(0u) + destination_accepts(1u), (size_t)1u);
    check_equal(fixture.completed, (size_t)1u);
  }

  it("separates H1 pool identities when a snapshot generation changes") {
    destination_exchange(CHTTP_HTTP_1_1, 10u);
    fixture.destinations.selection.snapshot_generation = 2u;
    destination_exchange(CHTTP_HTTP_1_1, 10u);
    destination_exchange(CHTTP_HTTP_1_1, 10u);
    check_equal(destination_accepts(0u), (size_t)2u);
  }

  it("isolates H2 generations even when both connections are still awaiting SETTINGS") {
    chttp_request_options options = destination_request(CHTTP_HTTP_2);
    chttp_request request;
    cnet_destination_result selected;
    for (size_t index = 0u; index < 3u; ++index) {
      fixture.destinations.selection.snapshot_generation = index < 2u ? 1u : 2u;
      check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                               &selected, &request), SALTS_OK);
    }
    destination_wait(3u);
    check_equal(destination_accepts(0u), (size_t)2u);
    destination_exchange(CHTTP_HTTP_2, 10u);
    check_equal(destination_accepts(0u), (size_t)2u);
  }

  it("keeps endpoint IDs distinct even when they map to the same H2 URI") {
    fixture.mapping[1] = fixture.mapping[0];
    destination_exchange(CHTTP_HTTP_2, 10u);
    fixture.destinations.selection.sequence = 1u;
    chttp_request_options options = destination_request(CHTTP_HTTP_2);
    chttp_request request;
    cnet_destination_result selected;
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_OK);
    check_equal(selected.endpoint_id, (uint64_t)20u);
    destination_wait(2u);
    check_equal(fixture.peer, 'a');
    check_equal(destination_accepts(0u), (size_t)2u);
  }

  it("copies the selected URI and identity before returning from async admission") {
    chttp_request_options options = destination_request(CHTTP_HTTP_1_1);
    chttp_request request;
    cnet_destination_result selected;
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_OK);
    tstr_free(fixture.uris[0]);
    fixture.uris[0] = NULL;
    fixture.mapping[0] = NULL;
    memset(fixture.hints, 0, sizeof(fixture.hints));
    memset(&fixture.destinations, 0, sizeof(fixture.destinations));
    destination_wait(1u);
    check_equal(fixture.peer, 'a');
    check_equal(selected.endpoint_id, (uint64_t)10u);
    check_equal(selected.snapshot_generation, (uint64_t)1u);
  }

  it("reports the chosen endpoint on physical FULL without using another warm peer") {
    check_equal(chttp_async_client_stop(&fixture.async, DESTINATION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&fixture.async), SALTS_OK);
    fixture.config.network.connection_capacity = 1u;
    check_equal(chttp_async_client_init(&fixture.async, &fixture.config), SALTS_OK);
    fixture.destinations.selection.sequence = 1u;
    destination_exchange(CHTTP_HTTP_2, 20u);
    fixture.destinations.selection.sequence = 0u;
    chttp_request_options options = destination_request(CHTTP_HTTP_2);
    chttp_request request;
    cnet_destination_result selected;
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_ENOBUFS);
    check_equal(selected.endpoint_id, (uint64_t)10u);
    check_equal(request.slot, (uint32_t)0u);
    check_equal(fixture.completed, (size_t)1u);
    check_equal(destination_accepts(0u), (size_t)0u);
    check_equal(destination_accepts(1u), (size_t)1u);
  }

  it("rejects expired or malformed policy and mixed TLS transport before admission") {
    chttp_request_options options = destination_request(CHTTP_HTTP_1_1);
    chttp_request request;
    cnet_destination_result selected;
    fixture.destinations.selection.expires_at_ms = 1u;
    fixture.destinations.selection.now_ms = 0u; /* Cannot bypass the actual owner clock. */
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_ETIMEDOUT);
    fixture.destinations.selection.expires_at_ms = UINT64_MAX;
    fixture.destinations.selection.kind = (cnet_destination_policy_kind)99;
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_EINVAL);
    fixture.destinations.selection.kind = CNET_DESTINATION_STRICT_KEY;
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_EINVAL);
    fixture.destinations.selection.kind = CNET_DESTINATION_ROUND_ROBIN;
    fixture.mapping[1] = "tls://127.0.0.1:1";
    check_equal(chttp_async_client_submit_to(&fixture.async, &options, &fixture.destinations,
                                             &selected, &request), SALTS_EINVAL);
    check_equal(selected.index, SIZE_MAX);
    check_equal(fixture.completed, (size_t)0u);
    check_equal(destination_accepts(0u) + destination_accepts(1u), (size_t)0u);
  }

  it("provides the same explicit policy for blocking calls without altering ordinary calls") {
    chttp_options options = {.authority = "destination.test", .target = "/peer",
        .timeout_ms = DESTINATION_TEST_TIMEOUT_MS};
    chttp_error error;
    cnet_destination_result selected;
    fixture.destinations.selection.kind = CNET_DESTINATION_EXPLICIT;
    fixture.destinations.selection.explicit_endpoint_id = 20u;
    check_equal(chttp_request_to(&fixture.blocking, CHTTP_METHOD_GET, &options,
                                  &fixture.destinations, &selected, &fixture.response, &error), SALTS_OK);
    check_equal(selected.endpoint_id, (uint64_t)20u);
    check_equal(fixture.response.body_size, (size_t)1u);
    check_equal(((const char *)fixture.response.body)[0], 'b');
    chttp_response_destroy(&fixture.response);
    options.connection_uri = fixture.mapping[0];
    check_equal(chttp_get(&fixture.blocking, &options, &fixture.response, &error), SALTS_OK);
    check_equal(((const char *)fixture.response.body)[0], 'a');
  }

  it("fails blocking selection without a deadline or callback when no endpoint is eligible") {
    chttp_options options = {.authority = "destination.test", .target = "/peer"};
    chttp_error error;
    cnet_destination_result selected;
    fixture.hints[0].eligible = fixture.hints[1].eligible = false;
    check_equal(chttp_request_to(&fixture.blocking, CHTTP_METHOD_GET, &options,
                                  &fixture.destinations, &selected, &fixture.response, &error),
                SALTS_ENOBUFS);
    check_equal(error.status, SALTS_ENOBUFS);
    check_equal(strcmp(error.stage, "destination-select"), 0);
    check_equal(selected.index, SIZE_MAX);
    check_equal(destination_accepts(0u) + destination_accepts(1u), (size_t)0u);
  }

  it("returns blocking physical FULL after one selection even with no request deadline") {
    check_equal(chttp_client_destroy(&fixture.blocking, DESTINATION_TEST_TIMEOUT_MS), SALTS_OK);
    fixture.config.network.connection_capacity = 1u;
    check_equal(chttp_client_init(&fixture.blocking, &fixture.config), SALTS_OK);
    chttp_options options = {.authority = "destination.test", .target = "/peer"};
    chttp_error error;
    cnet_destination_result selected;
    fixture.destinations.selection.sequence = 1u;
    check_equal(chttp_request_to(&fixture.blocking, CHTTP_METHOD_GET, &options,
                                  &fixture.destinations, &selected, &fixture.response, &error), SALTS_OK);
    chttp_response_destroy(&fixture.response);
    fixture.destinations.selection.sequence = 0u;
    check_equal(chttp_request_to(&fixture.blocking, CHTTP_METHOD_GET, &options,
                                  &fixture.destinations, &selected, &fixture.response, &error),
                SALTS_ENOBUFS);
    check_equal(error.status, SALTS_ENOBUFS);
    check_equal(strcmp(error.stage, "request-submit"), 0);
    check_equal(selected.endpoint_id, (uint64_t)10u);
    check_equal(destination_accepts(0u), (size_t)0u);
    check_equal(destination_accepts(1u), (size_t)1u);
  }
}
