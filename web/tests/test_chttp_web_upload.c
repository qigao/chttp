#include <chttp_web/web.h>

#include "tinytest.h"

#include <salts/error_codes.h>

#include <stddef.h>
#include <string.h>

enum {
  UPLOAD_TEST_DATA_BYTES = 256,
  UPLOAD_TEST_NAME_BYTES = 64
};

typedef struct upload_test_probe {
  size_t begin_calls;
  size_t part_begin_calls;
  size_t part_data_calls;
  size_t part_end_calls;
  size_t commit_calls;
  size_t abort_calls;
  char part_name[UPLOAD_TEST_NAME_BYTES];
  unsigned char data[UPLOAD_TEST_DATA_BYTES];
  size_t data_size;
  chttp_web_status abort_status;
  int abort_native_status;
  int fail_data;
  int fail_commit;
} upload_test_probe;

static int upload_test_begin(
    void *user, const chttp_server_request_view *request) {
  upload_test_probe *probe = (upload_test_probe *)user;
  if (probe == NULL || request == NULL ||
      request->path == NULL || strcmp(request->path, "/upload") != 0)
    return SALTS_EINVAL;
  ++probe->begin_calls;
  return SALTS_OK;
}

static int upload_test_part_begin(
    void *user, const chttp_web_multipart_part *part) {
  upload_test_probe *probe = (upload_test_probe *)user;
  if (probe == NULL || part == NULL ||
      part->name.data == NULL ||
      part->name.size >= sizeof(probe->part_name))
    return SALTS_EINVAL;
  memcpy(probe->part_name, part->name.data, part->name.size);
  probe->part_name[part->name.size] = '\0';
  ++probe->part_begin_calls;
  return SALTS_OK;
}

static int upload_test_part_data(
    void *user, const void *data, size_t size) {
  upload_test_probe *probe = (upload_test_probe *)user;
  if (probe == NULL || (size != 0u && data == NULL))
    return SALTS_EINVAL;
  ++probe->part_data_calls;
  if (probe->fail_data) return SALTS_EIO;
  if (size > sizeof(probe->data) - probe->data_size)
    return SALTS_ENOBUFS;
  if (size != 0u)
    memcpy(probe->data + probe->data_size, data, size);
  probe->data_size += size;
  return SALTS_OK;
}

static int upload_test_part_end(void *user) {
  upload_test_probe *probe = (upload_test_probe *)user;
  if (probe == NULL) return SALTS_EINVAL;
  ++probe->part_end_calls;
  return SALTS_OK;
}

static int upload_test_commit(void *user) {
  upload_test_probe *probe = (upload_test_probe *)user;
  if (probe == NULL) return SALTS_EINVAL;
  ++probe->commit_calls;
  return probe->fail_commit ? SALTS_EIO : SALTS_OK;
}

static void upload_test_abort(
    void *user, chttp_web_status status, int native_status) {
  upload_test_probe *probe = (upload_test_probe *)user;
  if (probe == NULL) return;
  ++probe->abort_calls;
  probe->abort_status = status;
  probe->abort_native_status = native_status;
}

static chttp_web_upload_callbacks upload_test_callbacks(void) {
  const chttp_web_upload_callbacks callbacks = {
      .size = sizeof(chttp_web_upload_callbacks),
      .begin = upload_test_begin,
      .part_begin = upload_test_part_begin,
      .part_data = upload_test_part_data,
      .part_end = upload_test_part_end,
      .commit = upload_test_commit,
      .abort = upload_test_abort};
  return callbacks;
}

static chttp_web_multipart_limits upload_test_limits(void) {
  chttp_web_multipart_limits limits =
      (chttp_web_multipart_limits)CHTTP_WEB_MULTIPART_LIMITS_INIT;
  limits.max_parts = 8u;
  limits.max_header_count = 8u;
  limits.max_header_bytes = 1024u;
  limits.max_name_bytes = 32u;
  limits.max_filename_bytes = 96u;
  limits.max_content_type_bytes = 64u;
  limits.max_field_bytes = 128u;
  limits.max_total_bytes = 4096u;
  return limits;
}

static chttp_server_request_view upload_test_request(
    chttp_method method,
    const chttp_header *headers,
    size_t header_count) {
  return (chttp_server_request_view){
      .http_major = 1u,
      .http_minor = 1u,
      .method = method,
      .target = "/upload",
      .path = "/upload",
      .headers = headers,
      .header_count = header_count,
      .protocol_keep_alive = 1};
}

static chttp_web_status upload_test_open(
    chttp_web_upload_request *upload,
    upload_test_probe *probe,
    chttp_server_request_view *request,
    chttp_body_sink *sink,
    chttp_web_error *error) {
  static const chttp_header headers[] = {
      {"Content-Type", "multipart/form-data; boundary=AaB03x"}};
  chttp_web_multipart_limits limits = upload_test_limits();
  chttp_web_upload_callbacks callbacks = upload_test_callbacks();
  *request = upload_test_request(
      CHTTP_METHOD_GET, headers, sizeof(headers) / sizeof(headers[0]));
  return chttp_web_upload_open(
      upload, request, &limits, &callbacks, probe, sink, error);
}

static int upload_test_feed(
    chttp_body_sink *sink,
    const char *body,
    size_t body_size,
    size_t chunk_size) {
  size_t position = 0u;
  while (position < body_size) {
    size_t size = chunk_size;
    int status;
    if (size > body_size - position) size = body_size - position;
    status = sink->write(sink->user, body + position, size);
    if (status != SALTS_OK) return status;
    position += size;
  }
  return SALTS_OK;
}

spec("CHttp::Web streaming upload transaction") {
  it("stages multipart parts, consumes reserved csrf, and commits once") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"_csrf\"\r\n\r\n"
        "token\r\n"
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "payload\r\n"
        "--AaB03x--\r\n";
    chttp_web_upload_request upload = CHTTP_WEB_UPLOAD_REQUEST_INIT;
    upload_test_probe probe = {0};
    chttp_server_request_view request = {0};
    chttp_body_sink sink = {0};
    chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        upload_test_open(
            &upload, &probe, &request, &sink, &error),
        CHTTP_WEB_OK);
    check_equal(probe.begin_calls, (size_t)1u);
    check_not_null(sink.write);
    check_true(sink.user == &upload);

    check_equal(
        upload_test_feed(
            &sink, body, sizeof(body) - 1u, 3u),
        SALTS_OK);
    chttp_web_upload_close(&upload, SALTS_OK);

    request.body_streamed = 1;
    request.body_sink_user = &upload;
    check_equal(
        chttp_web_validation_init(
            &validation, NULL, 0u, NULL, 0u, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_upload_finalize(
            &upload, &request, &validation, &error),
        CHTTP_WEB_OK);

    check_equal(probe.part_begin_calls, (size_t)1u);
    check_equal(probe.part_end_calls, (size_t)1u);
    check_equal(probe.part_name, "file");
    check_equal(probe.data_size, (size_t)7u);
    check_equal(probe.data, "payload", 7u);
    check_equal(probe.commit_calls, (size_t)1u);
    check_equal(probe.abort_calls, (size_t)0u);
    check_true(chttp_web_upload_user(&upload) == &probe);

    check_equal(chttp_web_upload_reset(&upload, &error), CHTTP_WEB_OK);
    check_equal(upload.size, (size_t)0u);
  }

  it("aborts once when structured validation rejects the request") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"value\"\r\n\r\n"
        "ok\r\n"
        "--AaB03x--\r\n";
    chttp_web_upload_request upload = CHTTP_WEB_UPLOAD_REQUEST_INIT;
    upload_test_probe probe = {0};
    chttp_server_request_view request = {0};
    chttp_body_sink sink = {0};
    chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
    chttp_web_validation_error validation_errors[1];
    char validation_bytes[32];
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        upload_test_open(
            &upload, &probe, &request, &sink, &error),
        CHTTP_WEB_OK);
    check_equal(
        upload_test_feed(
            &sink, body, sizeof(body) - 1u, 2u),
        SALTS_OK);
    chttp_web_upload_close(&upload, SALTS_OK);

    check_equal(
        chttp_web_validation_init(
            &validation,
            validation_errors,
            1u,
            validation_bytes,
            sizeof(validation_bytes),
            &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_validation_add_global(
            &validation, "rejected", &error),
        CHTTP_WEB_OK);

    request.body_streamed = 1;
    request.body_sink_user = &upload;
    check_equal(
        chttp_web_upload_finalize(
            &upload, &request, &validation, &error),
        CHTTP_WEB_VALIDATION);
    check_equal(probe.commit_calls, (size_t)0u);
    check_equal(probe.abort_calls, (size_t)1u);
    check_equal(probe.abort_status, CHTTP_WEB_VALIDATION);

    check_equal(
        chttp_web_upload_finalize(
            &upload, &request, &validation, &error),
        CHTTP_WEB_VALIDATION);
    check_equal(probe.abort_calls, (size_t)1u);
  }

  it("aborts transport and parser callback failures exactly once") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"value\"\r\n\r\n"
        "payload\r\n"
        "--AaB03x--\r\n";
    chttp_web_upload_request upload = CHTTP_WEB_UPLOAD_REQUEST_INIT;
    upload_test_probe probe = {0};
    chttp_server_request_view request = {0};
    chttp_body_sink sink = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        upload_test_open(
            &upload, &probe, &request, &sink, &error),
        CHTTP_WEB_OK);
    chttp_web_upload_close(&upload, SALTS_ECANCELED);
    check_equal(probe.abort_calls, (size_t)1u);
    check_equal(probe.abort_status, CHTTP_WEB_SERVER);
    check_equal(probe.abort_native_status, SALTS_ECANCELED);
    check_equal(
        chttp_web_upload_abort(
            &upload, CHTTP_WEB_UPLOAD, SALTS_EIO, &error),
        CHTTP_WEB_OK);
    check_equal(probe.abort_calls, (size_t)1u);

    check_equal(chttp_web_upload_reset(&upload, &error), CHTTP_WEB_OK);
    memset(&probe, 0, sizeof(probe));
    probe.fail_data = 1;
    check_equal(
        upload_test_open(
            &upload, &probe, &request, &sink, &error),
        CHTTP_WEB_OK);
    check_equal(
        upload_test_feed(
            &sink, body, sizeof(body) - 1u, sizeof(body) - 1u),
        SALTS_EIO);
    chttp_web_upload_close(&upload, SALTS_EIO);
    check_equal(probe.abort_calls, (size_t)1u);
    check_equal(probe.abort_status, CHTTP_WEB_MULTIPART);
    check_equal(probe.abort_native_status, SALTS_EIO);
  }

  it("fails closed on repeated csrf fields without staging them") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"_csrf\"\r\n\r\n"
        "one\r\n"
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"_csrf\"\r\n\r\n"
        "two\r\n"
        "--AaB03x--\r\n";
    chttp_web_upload_request upload = CHTTP_WEB_UPLOAD_REQUEST_INIT;
    upload_test_probe probe = {0};
    chttp_server_request_view request = {0};
    chttp_body_sink sink = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    int status;

    check_equal(
        upload_test_open(
            &upload, &probe, &request, &sink, &error),
        CHTTP_WEB_OK);
    status = upload_test_feed(
        &sink, body, sizeof(body) - 1u, 1u);
    check_equal(status, SALTS_EPROTO);
    chttp_web_upload_close(&upload, status);

    check_equal(probe.part_begin_calls, (size_t)0u);
    check_equal(probe.part_data_calls, (size_t)0u);
    check_equal(probe.part_end_calls, (size_t)0u);
    check_equal(probe.commit_calls, (size_t)0u);
    check_equal(probe.abort_calls, (size_t)1u);
    check_equal(probe.abort_status, CHTTP_WEB_MULTIPART);
  }

  it("aborts exactly once when application commit fails") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"value\"\r\n\r\n"
        "ok\r\n"
        "--AaB03x--\r\n";
    chttp_web_upload_request upload = CHTTP_WEB_UPLOAD_REQUEST_INIT;
    upload_test_probe probe = {.fail_commit = 1};
    chttp_server_request_view request = {0};
    chttp_body_sink sink = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        upload_test_open(
            &upload, &probe, &request, &sink, &error),
        CHTTP_WEB_OK);
    check_equal(
        upload_test_feed(
            &sink, body, sizeof(body) - 1u, 5u),
        SALTS_OK);
    chttp_web_upload_close(&upload, SALTS_OK);
    request.body_streamed = 1;
    request.body_sink_user = &upload;

    check_equal(
        chttp_web_upload_finalize(
            &upload, &request, NULL, &error),
        CHTTP_WEB_UPLOAD);
    check_equal(error.native_status, SALTS_EIO);
    check_equal(probe.commit_calls, (size_t)1u);
    check_equal(probe.abort_calls, (size_t)1u);
    check_equal(probe.abort_status, CHTTP_WEB_UPLOAD);
    check_equal(probe.abort_native_status, SALTS_EIO);
  }
}
