#include <s3/s3_object.h>
#include <s3/s3_signer.h>

#include "s3_test_support.h"
#include "tinytest.h"

#include <stdlib.h>
#include <string.h>

enum { S3_FILE_TEST_BYTES = 256u * 1024u + 123u };

typedef struct s3_file_probe {
  const unsigned char *payload;
  const char *payload_path;
  size_t payload_size;
  char payload_sha256[S3_SIGNER_SHA256_HEX_SIZE + 1u];
  size_t puts;
  size_t gets;
  size_t customer_puts;
  size_t customer_gets;
  size_t missing_gets;
} s3_file_probe;

typedef struct s3_file_progress {
  size_t transferred;
  size_t total;
  int monotonic;
} s3_file_progress;

static int64_t s3_file_clock(void *user) {
  (void)user;
  return INT64_C(1369353600);
}

static void s3_file_on_progress(void *user, size_t transferred, size_t total) {
  s3_file_progress *progress = (s3_file_progress *)user;
  if (progress == NULL) return;
  if (transferred < progress->transferred) progress->monotonic = 0;
  progress->transferred = transferred;
  progress->total = total;
}

static int s3_file_put(void *user, const chttp_server_request_view *request,
                       chttp_server_response *response) {
  s3_file_probe *probe = (s3_file_probe *)user;
  if (probe == NULL || request == NULL || request->body_size != probe->payload_size ||
      memcmp(request->body, probe->payload, probe->payload_size) != 0 ||
      strcmp(chttp_server_request_header(request, "content-type"), "application/octet-stream") !=
          0 ||
      strcmp(chttp_server_request_header(request, "x-amz-content-sha256"), probe->payload_sha256) !=
          0 ||
      strcmp(chttp_server_request_header(request, "x-amz-server-side-encryption"), "aws:kms") !=
          0 ||
      strcmp(chttp_server_request_header(request, "x-amz-server-side-encryption-aws-kms-key-id"),
             "alias/archive") != 0 ||
      strcmp(chttp_server_request_header(request, "x-amz-server-side-encryption-context"),
             "eyJ0ZW5hbnQiOiJhbHBoYSJ9") != 0)
    return SALTS_EPROTO;
  ++probe->puts;
  return chttp_server_reply(response, 200u, NULL, NULL, 0u);
}

static int s3_file_get(void *user, const chttp_server_request_view *request,
                       chttp_server_response *response) {
  s3_file_probe *probe = (s3_file_probe *)user;
  if (probe == NULL || request == NULL) return SALTS_EPROTO;
  ++probe->gets;
  return chttp_server_response_file(response, 200u, "application/octet-stream",
                                    probe->payload_path);
}

static int s3_file_customer_headers_valid(const chttp_server_request_view *request) {
  return request != NULL &&
         strcmp(chttp_server_request_header(request,
                                            "x-amz-server-side-encryption-customer-algorithm"),
                "AES256") == 0 &&
         strcmp(chttp_server_request_header(request, "x-amz-server-side-encryption-customer-key"),
                "MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY=") == 0 &&
         strcmp(
             chttp_server_request_header(request, "x-amz-server-side-encryption-customer-key-md5"),
             "hRasmdxgYDKV3nvbahU1MA==") == 0;
}

static int s3_file_customer_put(void *user, const chttp_server_request_view *request,
                                chttp_server_response *response) {
  s3_file_probe *probe = (s3_file_probe *)user;
  if (probe == NULL || !s3_file_customer_headers_valid(request) || request->body_size != 6u ||
      memcmp(request->body, "secret", 6u) != 0)
    return SALTS_EPROTO;
  ++probe->customer_puts;
  return chttp_server_reply(response, 200u, NULL, NULL, 0u);
}

static int s3_file_customer_get(void *user, const chttp_server_request_view *request,
                                chttp_server_response *response) {
  s3_file_probe *probe = (s3_file_probe *)user;
  if (probe == NULL || !s3_file_customer_headers_valid(request)) return SALTS_EPROTO;
  ++probe->customer_gets;
  return chttp_server_reply(response, 200u, "application/octet-stream", "secret", 6u);
}

static int s3_file_missing(void *user, const chttp_server_request_view *request,
                           chttp_server_response *response) {
  static const char body[] = "<Error><Code>NoSuchKey</Code></Error>";
  s3_file_probe *probe = (s3_file_probe *)user;
  if (probe == NULL || request == NULL) return SALTS_EPROTO;
  ++probe->missing_gets;
  return chttp_server_reply(response, 404u, "application/xml", body, sizeof(body) - 1u);
}

static void s3_file_run(chttp_protocol protocol) {
  static const s3_static_credentials credentials = {"access", "secret", NULL};
  static const char kms_context[] = "{\"tenant\":\"alpha\"}";
  chttp_server server = {0};
  chttp_client http_client = {0};
  s3_client client = {0};
  chttp_server_config server_config = s3_test_server_config();
  chttp_client_config http_config = s3_test_client_config();
  s3_client_config config;
  s3_file_probe probe = {0};
  s3_file_progress upload_progress = {.monotonic = 1};
  s3_file_progress download_progress = {.monotonic = 1};
  s3_response response = {0};
  s3_error error = {0};
  s3_sse_options sse = {.size = sizeof(sse),
                        .mode = S3_SSE_KMS,
                        .kms_key_id = "alias/archive",
                        .kms_context = kms_context,
                        .kms_context_size = sizeof(kms_context) - 1u};
  s3_put_object_options put_options = {
      .size = sizeof(put_options), .content_type = "application/octet-stream", .sse = &sse};
  static const unsigned char customer_key[] = "0123456789abcdef0123456789abcdef";
  s3_sse_options customer_sse = {.size = sizeof(customer_sse),
                                 .mode = S3_SSE_CUSTOMER,
                                 .customer_key = customer_key,
                                 .customer_key_size = sizeof(customer_key) - 1u};
  s3_put_object_options customer_put_options = {.size = sizeof(customer_put_options),
                                                .sse = &customer_sse};
  s3_get_object_options customer_get_options = {.size = sizeof(customer_get_options),
                                                .sse = &customer_sse};
  unsigned char *payload = (unsigned char *)malloc(S3_FILE_TEST_BYTES);
  char *upload_path = tt_make_temp_file("s3-upload", ".bin");
  char *download_path = tt_make_temp_file("s3-download", ".bin");
  char *downloaded;
  size_t downloaded_size = 0u;
  char connection_uri[64];
  char authority[64];
  uint16_t port = 0u;
  size_t index;

  check_not_null(payload);
  check_not_null(upload_path);
  check_not_null(download_path);
  if (payload == NULL || upload_path == NULL || download_path == NULL) goto done;
  for (index = 0u; index < S3_FILE_TEST_BYTES; ++index)
    payload[index] = (unsigned char)((index * 31u + 7u) & 0xffu);
  probe.payload = payload;
  probe.payload_path = upload_path;
  probe.payload_size = S3_FILE_TEST_BYTES;
  check_equal(s3_signer_sha256_hex(payload, S3_FILE_TEST_BYTES, probe.payload_sha256), SALTS_OK);
  check_equal(tt_write_file(upload_path, payload, S3_FILE_TEST_BYTES), 0);
  check_equal(tt_write_file(download_path, "sentinel", sizeof("sentinel") - 1u), 0);

  check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
  check_equal(chttp_server_put(&server, "/bucket/file", s3_file_put, &probe), SALTS_OK);
  check_equal(chttp_server_get(&server, "/bucket/file", s3_file_get, &probe), SALTS_OK);
  check_equal(chttp_server_put(&server, "/bucket/customer", s3_file_customer_put, &probe),
              SALTS_OK);
  check_equal(chttp_server_get(&server, "/bucket/customer", s3_file_customer_get, &probe),
              SALTS_OK);
  check_equal(chttp_server_get(&server, "/bucket/missing", s3_file_missing, &probe), SALTS_OK);
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
                              .credentials = s3_credentials_provider_static(&credentials),
                              .clock = s3_file_clock,
                              .timeout_ms = S3_TEST_TIMEOUT_MS};
  check_equal(s3_client_init(&client, &http_client, &config), SALTS_OK);

  check_equal(s3_put_object_file(&client, "bucket", "file", upload_path, &put_options,
                                 s3_file_on_progress, &upload_progress, &response, &error),
              SALTS_OK);
  check_equal(response.http.status_code, 200u);
  s3_response_destroy(&response);
  check_equal(upload_progress.transferred, (size_t)S3_FILE_TEST_BYTES);
  check_equal(upload_progress.total, (size_t)S3_FILE_TEST_BYTES);
  check_true(upload_progress.monotonic);

  check_equal(s3_get_object_file(&client, "bucket", "file", download_path, NULL,
                                 s3_file_on_progress, &download_progress, &response, &error),
              SALTS_OK);
  check_equal(response.http.status_code, 200u);
  check_null(response.http.body);
  check_equal(response.http.body_size, (size_t)S3_FILE_TEST_BYTES);
  s3_response_destroy(&response);
  downloaded = tt_read_file(download_path, &downloaded_size);
  check_not_null(downloaded);
  if (downloaded != NULL) {
    check_equal(downloaded_size, (size_t)S3_FILE_TEST_BYTES);
    check_equal(downloaded, payload, S3_FILE_TEST_BYTES);
    free(downloaded);
  }
  check_equal(download_progress.transferred, (size_t)S3_FILE_TEST_BYTES);
  check_equal(download_progress.total, (size_t)S3_FILE_TEST_BYTES);
  check_true(download_progress.monotonic);

  check_equal(s3_put_object_with_options(&client, "bucket", "customer", "secret", 6u,
                                         &customer_put_options, &response, &error),
              SALTS_OK);
  s3_response_destroy(&response);
  check_equal(s3_get_object_with_options(&client, "bucket", "customer", &customer_get_options,
                                         &response, &error),
              SALTS_OK);
  check_equal(response.http.body, "secret", 6u);
  s3_response_destroy(&response);
  customer_sse.customer_key_size = sizeof(customer_key) - 2u;
  check_equal(s3_put_object_with_options(&client, "bucket", "customer", "secret", 6u,
                                         &customer_put_options, &response, &error),
              SALTS_EINVAL);
  customer_sse.customer_key_size = sizeof(customer_key) - 1u;

  check_equal(tt_write_file(download_path, "sentinel", sizeof("sentinel") - 1u), 0);
  check_equal(s3_get_object_file(&client, "bucket", "missing", download_path, NULL, NULL, NULL,
                                 &response, &error),
              SALTS_EPROTO);
  check_equal(response.http.status_code, 404u);
  s3_response_destroy(&response);
  downloaded = tt_read_file(download_path, &downloaded_size);
  check_not_null(downloaded);
  if (downloaded != NULL) {
    check_equal(downloaded_size, sizeof("sentinel") - 1u);
    check_equal(downloaded, "sentinel", sizeof("sentinel") - 1u);
    free(downloaded);
  }
  check_equal(probe.puts, (size_t)1u);
  check_equal(probe.gets, (size_t)1u);
  check_equal(probe.customer_puts, (size_t)1u);
  check_equal(probe.customer_gets, (size_t)1u);
  check_equal(probe.missing_gets, (size_t)1u);

  check_equal(s3_client_destroy(&client), SALTS_OK);
  check_equal(chttp_client_destroy(&http_client, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_stop(&server, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_destroy(&server), SALTS_OK);

done:
  if (upload_path != NULL) check_equal(tt_remove_file(upload_path), 0);
  if (download_path != NULL) check_equal(tt_remove_file(download_path), 0);
  free(upload_path);
  free(download_path);
  free(payload);
}

spec("S3 native asynchronous file transfer") {
  it("streams signed files over HTTP/1.1") { s3_file_run(CHTTP_HTTP_1_1); }
  it("streams signed files over HTTP/2") { s3_file_run(CHTTP_HTTP_2); }
}
