#include <chttp_web/web.h>

#include <stddef.h>
#include <string.h>

typedef struct probe {
  size_t begins;
  size_t parts;
  size_t bytes;
  size_t commits;
  size_t aborts;
} probe;

static int part_begin(void *user, const chttp_web_multipart_part *part) {
  probe *value = (probe *)user;
  if (value == NULL || part == NULL) return -1;
  ++value->parts;
  return 0;
}

static int part_data(void *user, const void *data, size_t size) {
  probe *value = (probe *)user;
  if (value == NULL || (size != 0u && data == NULL)) return -1;
  value->bytes += size;
  return 0;
}

static int part_end(void *user) {
  return user != NULL ? 0 : -1;
}

static int upload_begin(void *user, const chttp_server_request_view *request) {
  probe *value = (probe *)user;
  if (value == NULL || request == NULL) return -1;
  ++value->begins;
  return 0;
}

static int upload_commit(void *user) {
  probe *value = (probe *)user;
  if (value == NULL) return -1;
  ++value->commits;
  return 0;
}

static void upload_abort(void *user, chttp_web_status status, int native_status) {
  probe *value = (probe *)user;
  (void)status;
  (void)native_status;
  if (value != NULL) ++value->aborts;
}

int main(void) {
  static const char body[] =
      "--AaB03x\r\n"
      "Content-Disposition: form-data; name=\"value\"\r\n\r\n"
      "ok\r\n"
      "--AaB03x--\r\n";
  static const chttp_header headers[] = {
      {"Content-Type", "multipart/form-data; boundary=AaB03x"}};
  chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
  chttp_web_validation_error validation_errors[2];
  char validation_bytes[128];
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_multipart_parser parser = CHTTP_WEB_MULTIPART_PARSER_INIT;
  chttp_web_multipart_limits limits = CHTTP_WEB_MULTIPART_LIMITS_INIT;
  chttp_web_multipart_callbacks multipart_callbacks =
      CHTTP_WEB_MULTIPART_CALLBACKS_INIT;
  chttp_web_upload_request upload = CHTTP_WEB_UPLOAD_REQUEST_INIT;
  chttp_web_upload_callbacks upload_callbacks = CHTTP_WEB_UPLOAD_CALLBACKS_INIT;
  chttp_server_request_view request = {
      .http_major = 1u,
      .http_minor = 1u,
      .method = CHTTP_METHOD_GET,
      .target = "/installed-web",
      .path = "/installed-web",
      .headers = headers,
      .header_count = 1u};
  chttp_body_sink sink = {0};
  probe multipart_probe = {0};
  probe upload_probe = {0};
  chttp_web_principal principal = CHTTP_WEB_PRINCIPAL_INIT;
  chttp_web_principal_input input = {0};

  if (chttp_web_validation_init(&validation, validation_errors, 2u,
          validation_bytes, sizeof(validation_bytes), &error) != CHTTP_WEB_OK)
    return 1;
  if (chttp_web_validation_add_field(&validation, "name", "required", &error) !=
      CHTTP_WEB_OK)
    return 2;
  if (validation.valid ||
      chttp_web_validation_field_count(&validation, "name") != 1u ||
      chttp_web_validation_field_get(&validation, "name", 0u) == NULL ||
      chttp_web_validation_data() == NULL ||
      chttp_web_validation_error_data() == NULL)
    return 3;
  if (chttp_web_validation_reset(&validation, &error) != CHTTP_WEB_OK ||
      !validation.valid)
    return 4;

  multipart_callbacks.part_begin = part_begin;
  multipart_callbacks.part_data = part_data;
  multipart_callbacks.part_end = part_end;
  if (chttp_web_multipart_init(&parser, headers[0].value, &limits,
          &multipart_callbacks, &multipart_probe, &error) != CHTTP_WEB_OK)
    return 5;
  if (chttp_web_multipart_feed(&parser, body, sizeof(body) - 1u, &error) !=
      CHTTP_WEB_OK)
    return 6;
  if (chttp_web_multipart_finish(&parser, &error) != CHTTP_WEB_OK ||
      multipart_probe.parts != 1u || multipart_probe.bytes != 2u)
    return 7;
  if (chttp_web_multipart_reset(&parser, &error) != CHTTP_WEB_OK)
    return 8;

  upload_callbacks.begin = upload_begin;
  upload_callbacks.part_begin = part_begin;
  upload_callbacks.part_data = part_data;
  upload_callbacks.part_end = part_end;
  upload_callbacks.commit = upload_commit;
  upload_callbacks.abort = upload_abort;
  if (chttp_web_upload_open(&upload, &request, &limits, &upload_callbacks,
          &upload_probe, &sink, &error) != CHTTP_WEB_OK)
    return 9;
  if (sink.write == NULL || sink.user != &upload ||
      sink.write(sink.user, body, sizeof(body) - 1u) != 0)
    return 10;
  chttp_web_upload_close(&upload, 0);
  request.body_streamed = 1;
  request.body_sink_user = &upload;
  if (chttp_web_upload_finalize(&upload, &request, &validation, &error) !=
      CHTTP_WEB_OK)
    return 11;
  if (upload_probe.begins != 1u || upload_probe.parts != 1u ||
      upload_probe.bytes != 2u || upload_probe.commits != 1u ||
      upload_probe.aborts != 0u || chttp_web_upload_user(&upload) != &upload_probe)
    return 12;
  if (chttp_web_upload_reset(&upload, &error) != CHTTP_WEB_OK)
    return 13;

  /* Link and exercise every new principal entry point through the installed
     Web target. The live authenticated lifecycle remains the HTTP test's job. */
  if (chttp_web_principal_get(&request, &principal, &error) != CHTTP_WEB_OK ||
      principal.authenticated || chttp_web_principal_data() == NULL)
    return 14;
  request.method = CHTTP_METHOD_POST;
  input.size = sizeof(input);
  input.subject = "installed-probe";
  if (chttp_web_principal_sign_in(&request, NULL, &input, NULL, &error) !=
      CHTTP_WEB_INVALID_ARGUMENT)
    return 15;
  if (chttp_web_principal_sign_out(&request, NULL, &error) !=
      CHTTP_WEB_INVALID_ARGUMENT)
    return 16;
  return 0;
}
