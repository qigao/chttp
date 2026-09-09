#include <s3/s3_bucket.h>
#include <s3/s3_object.h>

#include "s3_test_support.h"
#include "tinytest.h"

#include <string.h>

typedef struct s3_operations_probe {
  size_t puts;
  size_t gets;
  size_t heads;
  size_t deletes;
  size_t lists;
  size_t bucket_creates;
  size_t bucket_heads;
  size_t bucket_deletes;
  size_t bucket_lists;
  size_t copies;
} s3_operations_probe;

static int64_t s3_operations_clock(void *user) {
  (void)user;
  return INT64_C(1369353600);
}

static int s3_operations_put(void *user, const chttp_server_request_view *request,
                             chttp_server_response *response) {
  s3_operations_probe *probe = (s3_operations_probe *)user;
  if (probe == NULL || request == NULL || strcmp(request->target, "/bucket/key%20one") != 0 ||
      request->body_size != 7u || memcmp(request->body, "payload", 7u) != 0 ||
      strcmp(chttp_server_request_header(request, "content-type"), "text/plain") != 0)
    return SALTS_EPROTO;
  ++probe->puts;
  if (chttp_server_response_set_header(response, "ETag", "\"put-etag\"") != SALTS_OK)
    return SALTS_EIO;
  return chttp_server_reply(response, 200u, NULL, NULL, 0u);
}

static int s3_operations_get(void *user, const chttp_server_request_view *request,
                             chttp_server_response *response) {
  s3_operations_probe *probe = (s3_operations_probe *)user;
  if (probe == NULL || request == NULL) return SALTS_EPROTO;
  ++probe->gets;
  return chttp_server_reply(response, 200u, "text/plain", "payload", 7u);
}

static int s3_operations_head(void *user, const chttp_server_request_view *request,
                              chttp_server_response *response) {
  s3_operations_probe *probe = (s3_operations_probe *)user;
  (void)request;
  if (probe == NULL) return SALTS_EPROTO;
  ++probe->heads;
  if (chttp_server_response_set_header(response, "ETag", "\"head-etag\"") != SALTS_OK)
    return SALTS_EIO;
  return chttp_server_reply(response, 200u, "application/octet-stream", "ignored", 7u);
}

static int s3_operations_delete(void *user, const chttp_server_request_view *request,
                                chttp_server_response *response) {
  s3_operations_probe *probe = (s3_operations_probe *)user;
  (void)request;
  if (probe == NULL) return SALTS_EPROTO;
  ++probe->deletes;
  return chttp_server_reply(response, 204u, NULL, NULL, 0u);
}

static int s3_operations_list(void *user, const chttp_server_request_view *request,
                              chttp_server_response *response) {
  static const char body[] =
      "<ListBucketResult xmlns='http://s3.amazonaws.com/doc/2006-03-01/'>"
      "<IsTruncated>true</IsTruncated><NextContinuationToken>next token</NextContinuationToken>"
      "<Contents><Key>a.txt</Key><LastModified>2026-09-03T01:02:03.000Z</LastModified>"
      "<ETag>&quot;a&quot;</ETag><Size>3</Size><StorageClass>STANDARD</StorageClass></Contents>"
      "<Contents><Key>b.txt</Key><LastModified>2026-09-03T01:02:04Z</LastModified>"
      "<ETag>&quot;b&quot;</ETag><Size>4</Size></Contents></ListBucketResult>";
  s3_operations_probe *probe = (s3_operations_probe *)user;
  if (probe == NULL || request == NULL ||
      strcmp(request->target,
             "/bucket?continuation-token=token%20one&list-type=2&max-keys=2&prefix=logs%2F") != 0)
    return SALTS_EPROTO;
  ++probe->lists;
  return chttp_server_reply(response, 200u, "application/xml", body, sizeof(body) - 1u);
}

static int s3_operations_create_bucket(void *user, const chttp_server_request_view *request,
                                       chttp_server_response *response) {
  static const char body[] =
      "<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
      "<LocationConstraint>eu-west-1</LocationConstraint></CreateBucketConfiguration>";
  s3_operations_probe *probe = (s3_operations_probe *)user;
  if (probe == NULL || request == NULL || request->body_size != sizeof(body) - 1u ||
      memcmp(request->body, body, sizeof(body) - 1u) != 0 ||
      strcmp(chttp_server_request_header(request, "content-type"), "application/xml") != 0)
    return SALTS_EPROTO;
  ++probe->bucket_creates;
  return chttp_server_reply(response, 200u, NULL, NULL, 0u);
}

static int s3_operations_head_bucket(void *user, const chttp_server_request_view *request,
                                     chttp_server_response *response) {
  s3_operations_probe *probe = (s3_operations_probe *)user;
  (void)request;
  if (probe == NULL) return SALTS_EPROTO;
  ++probe->bucket_heads;
  return chttp_server_reply(response, 200u, NULL, NULL, 0u);
}

static int s3_operations_delete_bucket(void *user, const chttp_server_request_view *request,
                                       chttp_server_response *response) {
  s3_operations_probe *probe = (s3_operations_probe *)user;
  (void)request;
  if (probe == NULL) return SALTS_EPROTO;
  ++probe->bucket_deletes;
  return chttp_server_reply(response, 204u, NULL, NULL, 0u);
}

static int s3_operations_list_buckets(void *user, const chttp_server_request_view *request,
                                      chttp_server_response *response) {
  static const char body[] =
      "<ListAllMyBucketsResult xmlns='http://s3.amazonaws.com/doc/2006-03-01/'>"
      "<Buckets><Bucket><Name>alpha</Name><CreationDate>2026-09-01T00:00:00Z</CreationDate>"
      "</Bucket><Bucket><Name>a&amp;b</Name><CreationDate>2026-09-02T00:00:00Z</CreationDate>"
      "</Bucket></Buckets></ListAllMyBucketsResult>";
  s3_operations_probe *probe = (s3_operations_probe *)user;
  if (probe == NULL || request == NULL || strcmp(request->target, "/") != 0) return SALTS_EPROTO;
  ++probe->bucket_lists;
  return chttp_server_reply(response, 200u, "application/xml", body, sizeof(body) - 1u);
}

static int s3_operations_copy(void *user, const chttp_server_request_view *request,
                              chttp_server_response *response) {
  static const char body[] = "<CopyObjectResult><ETag>&quot;copy&quot;</ETag></CopyObjectResult>";
  s3_operations_probe *probe = (s3_operations_probe *)user;
  if (probe == NULL || request == NULL || request->body_size != 0u ||
      strcmp(chttp_server_request_header(request, "x-amz-copy-source"), "/source/source%20key") !=
          0)
    return SALTS_EPROTO;
  ++probe->copies;
  return chttp_server_reply(response, 200u, "application/xml", body, sizeof(body) - 1u);
}

static void s3_operations_run(chttp_protocol protocol) {
  static const s3_static_credentials credentials = {"access", "secret", NULL};
  chttp_server server = {0};
  chttp_client http_client = {0};
  s3_client client = {0};
  s3_operations_probe probe = {0};
  chttp_server_config server_config = s3_test_server_config();
  chttp_client_config http_config = s3_test_client_config();
  s3_client_config config;
  s3_response response = {0};
  s3_error error = {0};
  s3_object_list list = {0};
  s3_bucket_list buckets = {0};
  s3_list_objects_options list_options = {.size = sizeof(list_options),
                                          .prefix = "logs/",
                                          .continuation_token = "token one",
                                          .max_keys = 2u};
  char connection_uri[64];
  char authority[64];
  char *presigned_url = NULL;
  uint16_t port = 0u;

  check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
  check_equal(chttp_server_put(&server, "/bucket/key%20one", s3_operations_put, &probe), SALTS_OK);
  check_equal(chttp_server_get(&server, "/bucket/key%20one", s3_operations_get, &probe), SALTS_OK);
  check_equal(chttp_server_head(&server, "/bucket/key%20one", s3_operations_head, &probe),
              SALTS_OK);
  check_equal(chttp_server_delete(&server, "/bucket/key%20one", s3_operations_delete, &probe),
              SALTS_OK);
  check_equal(chttp_server_get(&server, "/bucket", s3_operations_list, &probe), SALTS_OK);
  check_equal(chttp_server_put(&server, "/new-bucket", s3_operations_create_bucket, &probe),
              SALTS_OK);
  check_equal(chttp_server_head(&server, "/new-bucket", s3_operations_head_bucket, &probe),
              SALTS_OK);
  check_equal(chttp_server_delete(&server, "/new-bucket", s3_operations_delete_bucket, &probe),
              SALTS_OK);
  check_equal(chttp_server_get(&server, "/", s3_operations_list_buckets, &probe), SALTS_OK);
  check_equal(chttp_server_put(&server, "/bucket/copied%20key", s3_operations_copy, &probe),
              SALTS_OK);
  check_equal(chttp_server_start(&server), SALTS_OK);
  check_equal(chttp_server_port(&server, &port), SALTS_OK);
  check_equal(
      s3_test_endpoint(port, connection_uri, sizeof(connection_uri), authority, sizeof(authority)),
      SALTS_OK);
  check_equal(chttp_client_init(&http_client, &http_config), SALTS_OK);
  config = (s3_client_config){.size = sizeof(config),
                              .connection_uri = connection_uri,
                              .authority = authority,
                              .region = "eu-west-1",
                              .addressing_style = S3_ADDRESSING_PATH,
                              .protocol = protocol,
                              .credentials = s3_credentials_provider_static(&credentials),
                              .clock = s3_operations_clock,
                              .timeout_ms = S3_TEST_TIMEOUT_MS};
  check_equal(s3_client_init(&client, &http_client, &config), SALTS_OK);

  check_equal(s3_presign_url(&client, S3_METHOD_GET, "bucket", "key one", NULL, 0u, 60u,
                             &presigned_url, &error),
              SALTS_OK);
  check_contains(presigned_url, "http://127.0.0.1:");
  check_contains(presigned_url, "/bucket/key%20one?X-Amz-Algorithm=AWS4-HMAC-SHA256&");
  check_contains(presigned_url, "X-Amz-Expires=60");
  check_contains(presigned_url, "X-Amz-Signature=");
  s3_string_free(presigned_url);
  presigned_url = NULL;

  check_equal(s3_create_bucket(&client, "new-bucket", &response, &error), SALTS_OK);
  s3_response_destroy(&response);
  check_equal(s3_head_bucket(&client, "new-bucket", &response, &error), SALTS_OK);
  s3_response_destroy(&response);
  check_equal(s3_delete_bucket(&client, "new-bucket", &response, &error), SALTS_OK);
  s3_response_destroy(&response);
  check_equal(s3_list_buckets(&client, &buckets, &response, &error), SALTS_OK);
  check_equal(buckets.count, (size_t)2u);
  check_equal(buckets.items[0].name, "alpha");
  check_equal(buckets.items[0].creation_date, "2026-09-01T00:00:00Z");
  check_equal(buckets.items[1].name, "a&b");
  s3_bucket_list_destroy(&buckets);
  s3_response_destroy(&response);

  check_equal(
      s3_put_object(&client, "bucket", "key one", "payload", 7u, "text/plain", &response, &error),
      SALTS_OK);
  check_equal(chttp_response_header(&response.http, "etag"), "\"put-etag\"");
  s3_response_destroy(&response);
  check_equal(s3_get_object(&client, "bucket", "key one", &response, &error), SALTS_OK);
  check_equal(response.http.body, "payload", 7u);
  s3_response_destroy(&response);
  check_equal(s3_head_object(&client, "bucket", "key one", &response, &error), SALTS_OK);
  check_equal(response.http.body_size, (size_t)0u);
  check_equal(chttp_response_header(&response.http, "etag"), "\"head-etag\"");
  s3_response_destroy(&response);
  check_equal(s3_delete_object(&client, "bucket", "key one", &response, &error), SALTS_OK);
  check_equal(response.http.status_code, 204u);
  s3_response_destroy(&response);

  check_equal(
      s3_copy_object(&client, "source", "source key", "bucket", "copied key", &response, &error),
      SALTS_OK);
  s3_response_destroy(&response);

  check_equal(s3_list_objects(&client, "bucket", &list_options, &list, &response, &error),
              SALTS_OK);
  check_equal(list.count, (size_t)2u);
  check_equal(list.items[0].key, "a.txt");
  check_equal(list.items[0].last_modified, "2026-09-03T01:02:03.000Z");
  check_equal(list.items[0].etag, "\"a\"");
  check_equal(list.items[0].size, UINT64_C(3));
  check_equal(list.items[0].storage_class, "STANDARD");
  check_equal(list.items[1].key, "b.txt");
  check_equal(list.items[1].size, UINT64_C(4));
  check_null(list.items[1].storage_class);
  check_equal(list.is_truncated, 1);
  check_equal(list.next_continuation_token, "next token");
  s3_object_list_destroy(&list);
  s3_response_destroy(&response);

  check_equal(probe.puts, (size_t)1u);
  check_equal(probe.gets, (size_t)1u);
  check_equal(probe.heads, (size_t)1u);
  check_equal(probe.deletes, (size_t)1u);
  check_equal(probe.lists, (size_t)1u);
  check_equal(probe.bucket_creates, (size_t)1u);
  check_equal(probe.bucket_heads, (size_t)1u);
  check_equal(probe.bucket_deletes, (size_t)1u);
  check_equal(probe.bucket_lists, (size_t)1u);
  check_equal(probe.copies, (size_t)1u);
  check_equal(s3_client_destroy(&client), SALTS_OK);
  check_equal(chttp_client_destroy(&http_client, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_stop(&server, S3_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_destroy(&server), SALTS_OK);
}

spec("S3 bucket and object operations") {
  it("uses the object convenience API over HTTP/1.1") { s3_operations_run(CHTTP_HTTP_1_1); }
  it("uses the object convenience API over HTTP/2") { s3_operations_run(CHTTP_HTTP_2); }
}
