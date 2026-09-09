#include <s3/s3.h>

#include "s3_test_support.h"
#include "tinytest.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct s3_client_test_probe {
  char target[256];
  char authorization[512];
  char date[32];
  char payload_hash[80];
  unsigned int http_major;
  size_t calls;
} s3_client_test_probe;

typedef struct s3_client_test_async_probe {
  size_t calls;
  int status;
  unsigned int response_status;
  char body[16];
  size_t body_size;
} s3_client_test_async_probe;

static int s3_client_test_copy_header(char *destination, size_t capacity, const char *source) {
  size_t size;
  if (destination == NULL || capacity == 0u || source == NULL) return SALTS_EINVAL;
  size = strlen(source);
  if (size >= capacity) return SALTS_EMSGSIZE;
  memcpy(destination, source, size + 1u);
  return SALTS_OK;
}

static int s3_client_test_success(void *user, const chttp_server_request_view *request,
                                  chttp_server_response *response) {
  s3_client_test_probe *probe = (s3_client_test_probe *)user;
  const char *authorization = chttp_server_request_header(request, "authorization");
  const char *date = chttp_server_request_header(request, "x-amz-date");
  const char *payload_hash = chttp_server_request_header(request, "x-amz-content-sha256");
  int status;
  if (probe == NULL || request == NULL || authorization == NULL || date == NULL ||
      payload_hash == NULL)
    return SALTS_EPROTO;
  status = s3_client_test_copy_header(probe->target, sizeof(probe->target), request->target);
  if (status == SALTS_OK)
    status = s3_client_test_copy_header(probe->authorization, sizeof(probe->authorization),
                                        authorization);
  if (status == SALTS_OK)
    status = s3_client_test_copy_header(probe->date, sizeof(probe->date), date);
  if (status == SALTS_OK)
    status =
        s3_client_test_copy_header(probe->payload_hash, sizeof(probe->payload_hash), payload_hash);
  if (status != SALTS_OK) return status;
  probe->http_major = request->http_major;
  ++probe->calls;
  return chttp_server_reply(response, 200u, "application/octet-stream", "object", 6u);
}

static int s3_client_test_missing(void *user, const chttp_server_request_view *request,
                                  chttp_server_response *response) {
  static const char body[] = "<Error><Code>NoSuchKey</Code><Message>missing</Message>"
                             "<RequestId>request-1</RequestId><HostId>host-1</HostId></Error>";
  (void)user;
  (void)request;
  return chttp_server_reply(response, 404u, "application/xml", body, sizeof(body) - 1u);
}

static int64_t s3_client_test_clock(void *user) {
  (void)user;
  return INT64_C(1369353600);
}

static void s3_client_test_complete(void *user, s3_request_handle request,
                                    const s3_response_view *response, const s3_error *error) {
  s3_client_test_async_probe *probe = (s3_client_test_async_probe *)user;
  (void)request;
  if (probe == NULL) return;
  ++probe->calls;
  if (error != NULL) {
    probe->status = error->status;
    return;
  }
  if (response == NULL || response->http == NULL ||
      response->http->body_size > sizeof(probe->body)) {
    probe->status = SALTS_EPROTO;
    return;
  }
  probe->status = SALTS_OK;
  probe->response_status = response->http->status_code;
  probe->body_size = response->http->body_size;
  if (probe->body_size != 0u) memcpy(probe->body, response->http->body, probe->body_size);
}

static void s3_client_test_run(chttp_protocol protocol) {
  static const s3_static_credentials static_credentials = {
      "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", NULL};
  static const s3_query_param query[] = {{"uploadId", "id"}, {"partNumber", "1"}};
  chttp_server server = {0};
  chttp_client http_client = {0};
  s3_client client = {0};
  s3_client_test_probe probe = {0};
  chttp_server_config server_config = s3_test_server_config();
  chttp_client_config http_config = s3_test_client_config();
  s3_client_config config;
  s3_request_options options;
  s3_response response = {0};
  s3_error error = {0};
  char connection_uri[64];
  char authority[64];
  uint16_t port = 0u;

  check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
  check_equal(chttp_server_get(&server, "/bucket/a%20b", s3_client_test_success, &probe), SALTS_OK);
  check_equal(chttp_server_get(&server, "/bucket/missing", s3_client_test_missing, NULL), SALTS_OK);
  check_equal(chttp_server_start(&server), SALTS_OK);
  check_equal(chttp_server_port(&server, &port), SALTS_OK);
  check_equal(
      s3_test_endpoint(port, connection_uri, sizeof(connection_uri), authority, sizeof(authority)),
      SALTS_OK);
  check_equal(chttp_client_init(&http_client, &http_config), SALTS_OK);

  config = (s3_client_config){.size = sizeof(config),
                              .connection_uri = connection_uri,
                              .authority = authority,
                              .region = "us-east-1",
                              .addressing_style = S3_ADDRESSING_PATH,
                              .protocol = protocol,
                              .credentials = s3_credentials_provider_static(&static_credentials),
                              .clock = s3_client_test_clock,
                              .timeout_ms = S3_TEST_TIMEOUT_MS};
  check_equal(s3_client_init(&client, &http_client, &config), SALTS_OK);
  options = (s3_request_options){.size = sizeof(options),
                                 .method = S3_METHOD_GET,
                                 .bucket = "bucket",
                                 .key = "a b",
                                 .query = query,
                                 .query_count = sizeof(query) / sizeof(query[0])};
  check_equal(s3_request(&client, &options, &response, &error), SALTS_OK);
  check_equal(response.http.status_code, 200u);
  check_equal(response.http.body, "object", 6u);
  check_equal(probe.calls, (size_t)1u);
  check_equal(probe.http_major, protocol == CHTTP_HTTP_2 ? 2u : 1u);
  check_equal(probe.target, "/bucket/a%20b?partNumber=1&uploadId=id");
  check_contains(probe.authorization, "Credential=AKIAIOSFODNN7EXAMPLE/");
  check_contains(probe.authorization, "SignedHeaders=host;x-amz-content-sha256;x-amz-date");
  check_equal(probe.date, "20130524T000000Z");
  check_equal(probe.payload_hash,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  s3_response_destroy(&response);

  options.key = "missing";
  options.query = NULL;
  options.query_count = 0u;
  check_equal(s3_request(&client, &options, &response, &error), SALTS_EPROTO);
  check_equal(response.http.status_code, 404u);
  check_equal(response.service_error.code, "NoSuchKey");
  check_equal(response.service_error.message, "missing");
  check_equal(response.service_error.request_id, "request-1");
  check_equal(response.service_error.host_id, "host-1");
  check_equal(error.stage, "s3-service");
  s3_response_destroy(&response);

  check_equal(s3_client_destroy(&client), SALTS_OK);
  check_equal(chttp_client_destroy(&http_client, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_stop(&server, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_destroy(&server), SALTS_OK);
}

static void s3_client_test_async_run(void) {
  static const s3_static_credentials static_credentials = {"access", "secret", NULL};
  chttp_server server = {0};
  chttp_async_client http_client = {0};
  s3_async_client client = {0};
  s3_client_test_probe server_probe = {0};
  s3_client_test_async_probe completion = {0};
  chttp_server_config server_config = s3_test_server_config();
  chttp_client_config http_config = s3_test_client_config();
  s3_client_config config;
  s3_request_options request;
  s3_async_request_options options;
  s3_request_handle handle = {0};
  char connection_uri[64];
  char authority[64];
  uint16_t port = 0u;
  size_t completions = 0u;
  size_t polls = 0u;

  check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
  check_equal(chttp_server_get(&server, "/bucket/a%20b", s3_client_test_success, &server_probe),
              SALTS_OK);
  check_equal(chttp_server_start(&server), SALTS_OK);
  check_equal(chttp_server_port(&server, &port), SALTS_OK);
  check_equal(
      s3_test_endpoint(port, connection_uri, sizeof(connection_uri), authority, sizeof(authority)),
      SALTS_OK);
  check_equal(chttp_async_client_init(&http_client, &http_config), SALTS_OK);
  config = (s3_client_config){.size = sizeof(config),
                              .connection_uri = connection_uri,
                              .authority = authority,
                              .region = "us-east-1",
                              .addressing_style = S3_ADDRESSING_PATH,
                              .protocol = CHTTP_HTTP_2,
                              .credentials = s3_credentials_provider_static(&static_credentials),
                              .clock = s3_client_test_clock,
                              .timeout_ms = S3_TEST_TIMEOUT_MS};
  check_equal(s3_async_client_init(&client, &http_client, &config), SALTS_OK);
  request = (s3_request_options){
      .size = sizeof(request), .method = S3_METHOD_GET, .bucket = "bucket", .key = "a b"};
  options = (s3_async_request_options){.size = sizeof(options),
                                       .request = &request,
                                       .on_complete = s3_client_test_complete,
                                       .user = &completion};
  check_equal(s3_async_client_submit(&client, &options, &handle), SALTS_OK);
  check_equal(s3_async_client_destroy(&client), SALTS_EBUSY);
  while (completion.calls == 0u && polls++ < 40u)
    check_equal(s3_async_client_poll(&client, 250u, &completions), SALTS_OK);
  check_equal(completion.calls, (size_t)1u);
  check_equal(completion.status, SALTS_OK);
  check_equal(completion.response_status, 200u);
  check_equal(completion.body_size, (size_t)6u);
  check_equal(completion.body, "object", 6u);
  check_equal(server_probe.http_major, 2u);
  check_equal(s3_async_request_cancel(&client, handle), SALTS_ENOENT);
  check_equal(s3_async_client_destroy(&client), SALTS_OK);
  check_equal(chttp_async_client_stop(&http_client, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_async_client_destroy(&http_client), SALTS_OK);
  check_equal(chttp_server_stop(&server, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_destroy(&server), SALTS_OK);
}

spec("S3 CHTTP adapter") {
  it("executes the same signed request over HTTP/1.1") { s3_client_test_run(CHTTP_HTTP_1_1); }

  it("executes the same signed request over HTTP/2") { s3_client_test_run(CHTTP_HTTP_2); }

  it("validates lifecycle and public bounds before transport") {
    chttp_client http_client = {0};
    chttp_client_config http_config = s3_test_client_config();
    s3_client client = {0};
    s3_client_config config = {.size = sizeof(config),
                               .connection_uri = "tcp://127.0.0.1:1",
                               .authority = "127.0.0.1:1",
                               .region = "us-east-1"};
    check_equal(chttp_client_init(&http_client, &http_config), SALTS_OK);
    check_equal(s3_client_init(&client, &http_client, &config), SALTS_EINVAL);
    check_equal(s3_client_destroy(&client), SALTS_OK);
    config.credentials = s3_credentials_provider_environment();
    config.max_header_count = 2u;
    check_equal(s3_client_init(&client, &http_client, &config), SALTS_EINVAL);
#if SIZE_MAX > INT_MAX
    config.max_header_count = 3u;
    config.max_multipart_part_bytes = (size_t)INT_MAX + 1u;
    check_equal(s3_client_init(&client, &http_client, &config), SALTS_EINVAL);
#endif
    config.max_multipart_part_bytes = 0u;
    config.max_header_count = 3u;
    check_equal(s3_client_init(&client, &http_client, &config), SALTS_OK);
    check_equal(s3_client_destroy(&client), SALTS_OK);
    check_equal(chttp_client_destroy(&http_client, S3_TEST_TIMEOUT_MS), SALTS_OK);
  }

  it("delivers one terminal callback through the advanced HTTP/2 adapter") {
    s3_client_test_async_run();
  }
}
