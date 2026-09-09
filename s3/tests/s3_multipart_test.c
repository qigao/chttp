#include <s3/s3_multipart.h>

#include "s3_test_support.h"
#include "tinytest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S3_MULTIPART_TEST_LAST_BYTES = 123u };

typedef struct s3_multipart_probe {
  const unsigned char *first_part;
  size_t initiated;
  size_t uploaded;
  size_t completed;
  size_t aborted;
  size_t part_one_attempts;
  size_t part_two_attempts;
  int fail_part_two_once;
  int complete_error_once;
  int complete_empty_once;
} s3_multipart_probe;

typedef struct s3_multipart_progress {
  s3_client *client;
  size_t transferred;
  size_t total;
  int destroy_status;
} s3_multipart_progress;

static const unsigned char *s3_test_find_bytes(const void *haystack, size_t haystack_size,
                                               const char *needle) {
  const unsigned char *bytes = (const unsigned char *)haystack;
  const size_t needle_size = strlen(needle);
  size_t index;
  if (bytes == NULL || needle_size == 0u || needle_size > haystack_size) return NULL;
  for (index = 0u; index <= haystack_size - needle_size; ++index) {
    if (memcmp(bytes + index, needle, needle_size) == 0) return bytes + index;
  }
  return NULL;
}

static int64_t s3_multipart_clock(void *user) {
  (void)user;
  return INT64_C(1369353600);
}

static void s3_multipart_on_progress(void *user, size_t transferred, size_t total) {
  s3_multipart_progress *progress = (s3_multipart_progress *)user;
  if (progress == NULL) return;
  progress->transferred = transferred;
  progress->total = total;
  if (progress->client != NULL && transferred == total)
    progress->destroy_status = s3_client_destroy(progress->client);
}

static int s3_multipart_customer_headers_valid(const chttp_server_request_view *request) {
  const char *algorithm =
      chttp_server_request_header(request, "x-amz-server-side-encryption-customer-algorithm");
  const char *key =
      chttp_server_request_header(request, "x-amz-server-side-encryption-customer-key");
  const char *md5 =
      chttp_server_request_header(request, "x-amz-server-side-encryption-customer-key-md5");
  return algorithm != NULL && key != NULL && md5 != NULL && strcmp(algorithm, "AES256") == 0 &&
         strcmp(key, "MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY=") == 0 &&
         strcmp(md5, "hRasmdxgYDKV3nvbahU1MA==") == 0;
}

static int s3_multipart_post(void *user, const chttp_server_request_view *request,
                             chttp_server_response *response) {
  static const char initiated[] = "<InitiateMultipartUploadResult><UploadId>upload/one</UploadId>"
                                  "</InitiateMultipartUploadResult>";
  s3_multipart_probe *probe = (s3_multipart_probe *)user;
  const unsigned char *part_one;
  const unsigned char *part_two;
  if (probe == NULL || request == NULL) return SALTS_EPROTO;
  if (strcmp(request->target, "/bucket/large?uploads=") == 0) {
    if (!s3_multipart_customer_headers_valid(request)) return SALTS_EPROTO;
    ++probe->initiated;
    return chttp_server_reply(response, 200u, "application/xml", initiated, sizeof(initiated) - 1u);
  }
  if (strcmp(request->target, "/bucket/large?uploadId=upload%2Fone") != 0) return SALTS_EPROTO;
  if (request->body == NULL) return SALTS_EPROTO;
  part_one = s3_test_find_bytes(request->body, request->body_size, "<PartNumber>1</PartNumber>");
  part_two = s3_test_find_bytes(request->body, request->body_size, "<PartNumber>2</PartNumber>");
  if (part_one == NULL || part_two == NULL || part_one >= part_two ||
      s3_test_find_bytes(request->body, request->body_size, "<ETag>&quot;etag-1&quot;</ETag>") ==
          NULL ||
      s3_test_find_bytes(request->body, request->body_size, "<ETag>&quot;etag-2&quot;</ETag>") ==
          NULL)
    return SALTS_EPROTO;
  if (probe->complete_error_once) {
    static const char completion_error[] =
        "<Error><Code>InternalError</Code><Message>retry completion</Message></Error>";
    probe->complete_error_once = 0;
    return chttp_server_reply(response, 200u, "application/xml", completion_error,
                              sizeof(completion_error) - 1u);
  }
  if (probe->complete_empty_once) {
    probe->complete_empty_once = 0;
    return chttp_server_reply(response, 200u, "application/xml", NULL, 0u);
  }
  ++probe->completed;
  return chttp_server_reply(response, 200u, "application/xml", "<CompleteMultipartUploadResult/>",
                            sizeof("<CompleteMultipartUploadResult/>") - 1u);
}

static int s3_multipart_put(void *user, const chttp_server_request_view *request,
                            chttp_server_response *response) {
  s3_multipart_probe *probe = (s3_multipart_probe *)user;
  const char *etag;
  if (probe == NULL || request == NULL) return SALTS_EPROTO;
  if (!s3_multipart_customer_headers_valid(request)) return SALTS_EPROTO;
  if (strcmp(request->target, "/bucket/large?partNumber=1&uploadId=upload%2Fone") == 0) {
    ++probe->part_one_attempts;
    if (request->body_size != S3_MULTIPART_MIN_PART_BYTES ||
        memcmp(request->body, probe->first_part, S3_MULTIPART_MIN_PART_BYTES) != 0)
      return SALTS_EPROTO;
    etag = "\"etag-1\"";
  } else if (strcmp(request->target, "/bucket/large?partNumber=2&uploadId=upload%2Fone") == 0) {
    ++probe->part_two_attempts;
    if (request->body_size != S3_MULTIPART_TEST_LAST_BYTES) return SALTS_EPROTO;
    if (probe->fail_part_two_once) {
      static const char failure[] = "<Error><Code>InternalError</Code></Error>";
      probe->fail_part_two_once = 0;
      return chttp_server_reply(response, 500u, "application/xml", failure, sizeof(failure) - 1u);
    }
    etag = "\"etag-2\"";
  } else {
    return SALTS_EPROTO;
  }
  ++probe->uploaded;
  if (chttp_server_response_set_header(response, "ETag", etag) != SALTS_OK) return SALTS_EIO;
  return chttp_server_reply(response, 200u, NULL, NULL, 0u);
}

static int s3_multipart_delete(void *user, const chttp_server_request_view *request,
                               chttp_server_response *response) {
  s3_multipart_probe *probe = (s3_multipart_probe *)user;
  if (probe == NULL || request == NULL ||
      strcmp(request->target, "/bucket/large?uploadId=upload%2Fone") != 0)
    return SALTS_EPROTO;
  ++probe->aborted;
  return chttp_server_reply(response, 204u, NULL, NULL, 0u);
}

static void s3_multipart_run(chttp_protocol protocol) {
  static const s3_static_credentials credentials = {"access", "secret", NULL};
  static const unsigned char customer_key[] = "0123456789abcdef0123456789abcdef";
  static const unsigned char wrong_customer_key[] = "1123456789abcdef0123456789abcdef";
  const s3_sse_options customer_sse = {.size = sizeof(customer_sse),
                                       .mode = S3_SSE_CUSTOMER,
                                       .customer_key = customer_key,
                                       .customer_key_size = sizeof(customer_key) - 1u};
  const s3_sse_options wrong_customer_sse = {.size = sizeof(wrong_customer_sse),
                                             .mode = S3_SSE_CUSTOMER,
                                             .customer_key = wrong_customer_key,
                                             .customer_key_size = sizeof(wrong_customer_key) - 1u};
  const s3_put_object_options put_options = {.size = sizeof(put_options),
                                             .content_type = "application/octet-stream",
                                             .sse = &customer_sse};
  const s3_put_object_options wrong_put_options = {.size = sizeof(wrong_put_options),
                                                   .content_type = "application/octet-stream",
                                                   .sse = &wrong_customer_sse};
  chttp_server server = {0};
  chttp_client http_client = {0};
  s3_client client = {0};
  s3_multipart upload = {0};
  s3_multipart_probe probe = {0};
  chttp_server_config server_config = s3_test_server_config();
  chttp_client_config http_config = s3_test_client_config();
  s3_client_config config;
  s3_response response = {0};
  s3_error error = {0};
  s3_multipart_state state = S3_MULTIPART_ABORTED;
  s3_multipart_progress progress = {0};
  unsigned char *first_part = (unsigned char *)malloc(S3_MULTIPART_MIN_PART_BYTES);
  unsigned char *file_payload =
      (unsigned char *)malloc(S3_MULTIPART_MIN_PART_BYTES + S3_MULTIPART_TEST_LAST_BYTES);
  unsigned char last_part[S3_MULTIPART_TEST_LAST_BYTES];
  char *source_path = tt_make_temp_file("s3-multipart-source", ".bin");
  char *checkpoint_path = tt_make_temp_file("s3-multipart-resume", ".xml");
  char connection_uri[64];
  char authority[64];
  uint16_t port = 0u;
  size_t index;

  check_not_null(first_part);
  check_not_null(file_payload);
  check_not_null(source_path);
  check_not_null(checkpoint_path);
  if (first_part == NULL || file_payload == NULL || source_path == NULL || checkpoint_path == NULL)
    goto done;
  for (index = 0u; index < S3_MULTIPART_MIN_PART_BYTES; ++index)
    first_part[index] = (unsigned char)((index * 17u + 3u) & 0xffu);
  memset(last_part, 0xa5, sizeof(last_part));
  memcpy(file_payload, first_part, S3_MULTIPART_MIN_PART_BYTES);
  memcpy(file_payload + S3_MULTIPART_MIN_PART_BYTES, last_part, sizeof(last_part));
  check_equal(tt_write_file(source_path, file_payload,
                            S3_MULTIPART_MIN_PART_BYTES + S3_MULTIPART_TEST_LAST_BYTES),
              0);
  check_equal(tt_remove_file(checkpoint_path), 0);
  probe.first_part = first_part;
  server_config.max_request_body_bytes = 6u * 1024u * 1024u;
  http_config.max_request_body_bytes = 6u * 1024u * 1024u;
  check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
  check_equal(chttp_server_post(&server, "/bucket/large", s3_multipart_post, &probe), SALTS_OK);
  check_equal(chttp_server_put(&server, "/bucket/large", s3_multipart_put, &probe), SALTS_OK);
  check_equal(chttp_server_delete(&server, "/bucket/large", s3_multipart_delete, &probe), SALTS_OK);
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
                              .clock = s3_multipart_clock,
                              .timeout_ms = S3_TEST_TIMEOUT_MS,
                              .max_multipart_parts = 8u};
  check_equal(s3_client_init(&client, &http_client, &config), SALTS_OK);
  progress.client = &client;

  check_equal(
      s3_multipart_initiate(&client, "bucket", "large", &put_options, &upload, &response, &error),
      SALTS_OK);
  check_equal(s3_multipart_upload_id(&upload), "upload/one");
  s3_response_destroy(&response);
  check_equal(s3_multipart_upload_part(&client, &upload, 2u, last_part, sizeof(last_part),
                                       &response, &error),
              SALTS_OK);
  s3_response_destroy(&response);
  check_equal(s3_multipart_upload_part(&client, &upload, 1u, first_part,
                                       S3_MULTIPART_MIN_PART_BYTES, &response, &error),
              SALTS_OK);
  s3_response_destroy(&response);
  probe.complete_error_once = 1;
  check_equal(s3_multipart_complete(&client, &upload, &response, &error), SALTS_EPROTO);
  check_equal(response.http.status_code, 200u);
  check_equal(response.service_error.code, "InternalError");
  check_equal(s3_multipart_state_get(&upload, &state), SALTS_OK);
  check_equal(state, S3_MULTIPART_ACTIVE);
  s3_response_destroy(&response);
  probe.complete_empty_once = 1;
  check_equal(s3_multipart_complete(&client, &upload, &response, &error), SALTS_EPROTO);
  check_equal(response.http.status_code, 200u);
  check_equal(error.stage, "s3-multipart-complete-parse");
  check_equal(s3_multipart_state_get(&upload, &state), SALTS_OK);
  check_equal(state, S3_MULTIPART_ACTIVE);
  s3_response_destroy(&response);
  check_equal(s3_multipart_complete(&client, &upload, &response, &error), SALTS_OK);
  check_equal(s3_multipart_state_get(&upload, &state), SALTS_OK);
  check_equal(state, S3_MULTIPART_COMPLETED);
  s3_response_destroy(&response);
  check_equal(s3_multipart_destroy(&upload), SALTS_OK);

  check_equal(
      s3_multipart_initiate(&client, "bucket", "large", &put_options, &upload, &response, &error),
      SALTS_OK);
  s3_response_destroy(&response);
  check_equal(s3_multipart_destroy(&upload), SALTS_EBUSY);
  check_equal(s3_multipart_abort(&client, &upload, &response, &error), SALTS_OK);
  s3_response_destroy(&response);
  check_equal(s3_multipart_state_get(&upload, &state), SALTS_OK);
  check_equal(state, S3_MULTIPART_ABORTED);
  check_equal(s3_multipart_destroy(&upload), SALTS_OK);

  {
    const size_t invalid_size = strlen(checkpoint_path) + sizeof("/missing.xml");
    char *invalid_checkpoint = (char *)malloc(invalid_size);
    s3_multipart_file_options file_options;
    check_not_null(invalid_checkpoint);
    if (invalid_checkpoint != NULL) {
      (void)snprintf(invalid_checkpoint, invalid_size, "%s/missing.xml", checkpoint_path);
      file_options = (s3_multipart_file_options){.size = sizeof(file_options),
                                                 .part_size = S3_MULTIPART_MIN_PART_BYTES,
                                                 .checkpoint_path = invalid_checkpoint,
                                                 .preserve_on_failure = 1,
                                                 .put_options = &put_options};
      check_equal(s3_put_object_multipart_file(&client, "bucket", "large", source_path,
                                               &file_options, &response, &error),
                  SALTS_EIO);
      check_equal(probe.initiated, (size_t)3u);
      check_equal(probe.aborted, (size_t)2u);
      free(invalid_checkpoint);
    }
  }

  {
    s3_multipart_file_options file_options = {.size = sizeof(file_options),
                                              .part_size = S3_MULTIPART_MIN_PART_BYTES,
                                              .checkpoint_path = checkpoint_path,
                                              .preserve_on_failure = 1,
                                              .put_options = &put_options,
                                              .progress = s3_multipart_on_progress,
                                              .progress_user = &progress};
    char *checkpoint_contents;
    size_t checkpoint_size = 0u;
    probe.fail_part_two_once = 1;
    check_equal(s3_put_object_multipart_file(&client, "bucket", "large", source_path, &file_options,
                                             &response, &error),
                SALTS_EPROTO);
    check_equal(response.http.status_code, 500u);
    s3_response_destroy(&response);
    checkpoint_contents = tt_read_file(checkpoint_path, &checkpoint_size);
    check_not_null(checkpoint_contents);
    check_greater(checkpoint_size, (size_t)0u);
    free(checkpoint_contents);
    file_options.resume_existing = 1;
    file_options.put_options = &wrong_put_options;
    check_equal(s3_put_object_multipart_file(&client, "bucket", "large", source_path, &file_options,
                                             &response, &error),
                SALTS_EINVAL);
    file_options.put_options = &put_options;
    check_equal(s3_put_object_multipart_file(&client, "bucket", "other", source_path, &file_options,
                                             &response, &error),
                SALTS_EINVAL);
    check_equal(s3_put_object_multipart_file(&client, "bucket", "large", source_path, &file_options,
                                             &response, &error),
                SALTS_OK);
    s3_response_destroy(&response);
    checkpoint_contents = tt_read_file(checkpoint_path, &checkpoint_size);
    check_null(checkpoint_contents);
    check_equal(progress.transferred,
                (size_t)S3_MULTIPART_MIN_PART_BYTES + S3_MULTIPART_TEST_LAST_BYTES);
    check_equal(progress.total, (size_t)S3_MULTIPART_MIN_PART_BYTES + S3_MULTIPART_TEST_LAST_BYTES);
    check_equal(progress.destroy_status, SALTS_EBUSY);
  }

  check_equal(probe.initiated, (size_t)4u);
  check_equal(probe.uploaded, (size_t)4u);
  check_equal(probe.completed, (size_t)2u);
  check_equal(probe.aborted, (size_t)2u);
  check_equal(probe.part_one_attempts, (size_t)2u);
  check_equal(probe.part_two_attempts, (size_t)3u);
  check_equal(s3_client_destroy(&client), SALTS_OK);
  check_equal(chttp_client_destroy(&http_client, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_stop(&server, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_destroy(&server), SALTS_OK);

done:
  if (source_path != NULL) (void)tt_remove_file(source_path);
  if (checkpoint_path != NULL) (void)tt_remove_file(checkpoint_path);
  free(source_path);
  free(checkpoint_path);
  free(file_payload);
  free(first_part);
}

spec("S3 multipart state machine") {
  it("orders and completes parts over HTTP/1.1") { s3_multipart_run(CHTTP_HTTP_1_1); }
  it("orders and completes parts over HTTP/2") { s3_multipart_run(CHTTP_HTTP_2); }
}
