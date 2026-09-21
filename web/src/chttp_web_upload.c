#include <chttp_web/web.h>

#include <salts/error_codes.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
  CHTTP_WEB_UPLOAD_IDLE = 0,
  CHTTP_WEB_UPLOAD_STREAMING = 1,
  CHTTP_WEB_UPLOAD_BODY_COMPLETE = 2,
  CHTTP_WEB_UPLOAD_COMMITTED = 3,
  CHTTP_WEB_UPLOAD_ABORTED = 4
};

static chttp_web_status chttp_web_upload_error(
    chttp_web_error *error,
    chttp_web_status status,
    int native_status,
    const char *message) {
  if (error != NULL) {
    error->status = status;
    error->native_status = native_status;
    error->offset = 0u;
    error->template_name[0] = '\0';
    if (message != NULL)
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
    else
      error->message[0] = '\0';
  }
  return status;
}

static int chttp_web_upload_callbacks_valid(
    const chttp_web_upload_callbacks *callbacks) {
  return callbacks != NULL &&
         callbacks->size >= sizeof(*callbacks) &&
         callbacks->begin != NULL &&
         callbacks->part_begin != NULL &&
         callbacks->part_data != NULL &&
         callbacks->part_end != NULL &&
         callbacks->commit != NULL &&
         callbacks->abort != NULL;
}

static int chttp_web_upload_valid(
    const chttp_web_upload_request *upload) {
  return upload != NULL &&
         upload->size >= sizeof(*upload) &&
         chttp_web_upload_callbacks_valid(&upload->callbacks);
}

static int chttp_web_upload_name_is_csrf(
    const chttp_web_multipart_part *part) {
  static const char name[] = CHTTP_WEB_CSRF_FORM_FIELD;
  return part != NULL &&
         part->name.data != NULL &&
         part->name.size == sizeof(name) - 1u &&
         memcmp(part->name.data, name, sizeof(name) - 1u) == 0;
}

static void chttp_web_upload_abort_once(
    chttp_web_upload_request *upload,
    chttp_web_status status,
    int native_status) {
  if (!chttp_web_upload_valid(upload) ||
      upload->state == CHTTP_WEB_UPLOAD_ABORTED ||
      upload->state == CHTTP_WEB_UPLOAD_COMMITTED)
    return;

  upload->terminal_status =
      status == CHTTP_WEB_OK ? CHTTP_WEB_UPLOAD : status;
  upload->terminal_native_status = native_status;
  upload->state = CHTTP_WEB_UPLOAD_ABORTED;
  if (upload->begun)
    upload->callbacks.abort(
        upload->user,
        upload->terminal_status,
        upload->terminal_native_status);
}

static int chttp_web_upload_part_begin(
    void *user,
    const chttp_web_multipart_part *part) {
  chttp_web_upload_request *upload =
      (chttp_web_upload_request *)user;

  if (!chttp_web_upload_valid(upload) ||
      upload->state != CHTTP_WEB_UPLOAD_STREAMING ||
      part == NULL)
    return SALTS_EINVAL;

  upload->csrf_part = false;
  if (chttp_web_upload_name_is_csrf(part)) {
    if (part->has_filename || upload->csrf_count != 0u)
      return SALTS_EPROTO;
    upload->csrf_size = 0u;
    upload->csrf_token[0] = '\0';
    upload->csrf_part = true;
    return SALTS_OK;
  }

  return upload->callbacks.part_begin(upload->user, part);
}

static int chttp_web_upload_part_data(
    void *user,
    const void *data,
    size_t size) {
  chttp_web_upload_request *upload =
      (chttp_web_upload_request *)user;

  if (!chttp_web_upload_valid(upload) ||
      upload->state != CHTTP_WEB_UPLOAD_STREAMING ||
      (size != 0u && data == NULL))
    return SALTS_EINVAL;

  if (!upload->csrf_part)
    return upload->callbacks.part_data(upload->user, data, size);

  if (size > CHTTP_WEB_CSRF_TOKEN_BYTES - upload->csrf_size)
    return SALTS_EMSGSIZE;
  if (size != 0u)
    memcpy(upload->csrf_token + upload->csrf_size, data, size);
  upload->csrf_size += size;
  upload->csrf_token[upload->csrf_size] = '\0';
  return SALTS_OK;
}

static int chttp_web_upload_part_end(void *user) {
  chttp_web_upload_request *upload =
      (chttp_web_upload_request *)user;

  if (!chttp_web_upload_valid(upload) ||
      upload->state != CHTTP_WEB_UPLOAD_STREAMING)
    return SALTS_EINVAL;

  if (upload->csrf_part) {
    ++upload->csrf_count;
    upload->csrf_part = false;
    return SALTS_OK;
  }

  return upload->callbacks.part_end(upload->user);
}

static int chttp_web_upload_native_status(
    chttp_web_status status,
    const chttp_web_error *error) {
  if (error != NULL && error->native_status != 0)
    return error->native_status;
  if (status == CHTTP_WEB_CAPACITY)
    return SALTS_ENOBUFS;
  if (status == CHTTP_WEB_INVALID_ARGUMENT)
    return SALTS_EINVAL;
  return SALTS_EPROTO;
}

static int chttp_web_upload_sink_write(
    void *user,
    const void *data,
    size_t size) {
  chttp_web_upload_request *upload =
      (chttp_web_upload_request *)user;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;

  if (!chttp_web_upload_valid(upload) ||
      upload->state != CHTTP_WEB_UPLOAD_STREAMING)
    return SALTS_EINVAL;

  status = chttp_web_multipart_feed(
      &upload->multipart, data, size, &error);
  if (status == CHTTP_WEB_OK) return SALTS_OK;

  upload->terminal_status = status;
  upload->terminal_native_status = error.native_status;
  return chttp_web_upload_native_status(status, &error);
}

chttp_web_status chttp_web_upload_open(
    chttp_web_upload_request *upload,
    const chttp_server_request_view *request,
    const chttp_web_multipart_limits *limits,
    const chttp_web_upload_callbacks *callbacks,
    void *user,
    chttp_body_sink *out_sink,
    chttp_web_error *error) {
  const char *content_type;
  const chttp_web_multipart_callbacks multipart_callbacks = {
      .size = sizeof(chttp_web_multipart_callbacks),
      .part_begin = chttp_web_upload_part_begin,
      .part_data = chttp_web_upload_part_data,
      .part_end = chttp_web_upload_part_end};
  chttp_web_status status;
  int native_status;

  if (upload == NULL || request == NULL || limits == NULL ||
      !chttp_web_upload_callbacks_valid(callbacks) ||
      out_sink == NULL || upload->size != 0u)
    return chttp_web_upload_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "upload open arguments or request state are invalid");

  *out_sink = (chttp_body_sink){0};
  content_type = chttp_server_request_header(request, "Content-Type");
  if (content_type == NULL)
    return chttp_web_upload_error(
        error, CHTTP_WEB_MULTIPART, 0,
        "multipart upload requires Content-Type");

  *upload = (chttp_web_upload_request)CHTTP_WEB_UPLOAD_REQUEST_INIT;
  upload->size = sizeof(*upload);
  upload->callbacks = *callbacks;
  upload->user = user;
  upload->terminal_status = CHTTP_WEB_OK;

  status = chttp_web_multipart_init(
      &upload->multipart,
      content_type,
      limits,
      &multipart_callbacks,
      upload,
      error);
  if (status != CHTTP_WEB_OK) {
    *upload = (chttp_web_upload_request)CHTTP_WEB_UPLOAD_REQUEST_INIT;
    return status;
  }

  native_status = upload->callbacks.begin(upload->user, request);
  if (native_status != SALTS_OK) {
    *upload = (chttp_web_upload_request)CHTTP_WEB_UPLOAD_REQUEST_INIT;
    return chttp_web_upload_error(
        error, CHTTP_WEB_UPLOAD, native_status,
        "application upload staging begin failed");
  }

  upload->begun = true;
  upload->state = CHTTP_WEB_UPLOAD_STREAMING;
  *out_sink = (chttp_body_sink){
      .write = chttp_web_upload_sink_write,
      .user = upload};
  return chttp_web_upload_error(
      error, CHTTP_WEB_OK, 0, NULL);
}

void chttp_web_upload_close(
    chttp_web_upload_request *upload,
    int status) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status web_status;

  if (!chttp_web_upload_valid(upload) ||
      upload->state != CHTTP_WEB_UPLOAD_STREAMING)
    return;

  if (status != SALTS_OK) {
    web_status = upload->terminal_status != CHTTP_WEB_OK
        ? upload->terminal_status
        : CHTTP_WEB_SERVER;
    chttp_web_upload_abort_once(
        upload,
        web_status,
        upload->terminal_native_status != 0
            ? upload->terminal_native_status
            : status);
    return;
  }

  if (upload->terminal_status != CHTTP_WEB_OK) {
    chttp_web_upload_abort_once(
        upload,
        upload->terminal_status,
        upload->terminal_native_status);
    return;
  }

  web_status = chttp_web_multipart_finish(
      &upload->multipart, &error);
  if (web_status != CHTTP_WEB_OK) {
    chttp_web_upload_abort_once(
        upload, web_status, error.native_status);
    return;
  }

  upload->terminal_status = CHTTP_WEB_OK;
  upload->terminal_native_status = 0;
  upload->state = CHTTP_WEB_UPLOAD_BODY_COMPLETE;
}

static chttp_web_status chttp_web_upload_aborted_status(
    const chttp_web_upload_request *upload,
    chttp_web_error *error) {
  const chttp_web_status status =
      upload->terminal_status != CHTTP_WEB_OK
          ? upload->terminal_status
          : CHTTP_WEB_UPLOAD;
  return chttp_web_upload_error(
      error,
      status,
      upload->terminal_native_status,
      "upload transaction was aborted before commit");
}

chttp_web_status chttp_web_upload_finalize(
    chttp_web_upload_request *upload,
    const chttp_server_request_view *request,
    const chttp_web_validation *validation,
    chttp_web_error *error) {
  const char *header = NULL;
  const void *candidate = NULL;
  size_t candidate_size = 0u;
  chttp_web_status status;
  int native_status;

  if (!chttp_web_upload_valid(upload) || request == NULL ||
      !request->body_streamed ||
      request->body_sink_user != upload)
    return chttp_web_upload_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "upload finalizer does not match the streamed request");

  if (upload->state == CHTTP_WEB_UPLOAD_ABORTED)
    return chttp_web_upload_aborted_status(upload, error);
  if (upload->state != CHTTP_WEB_UPLOAD_BODY_COMPLETE)
    return chttp_web_upload_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "upload body is not ready for terminal validation");

  if (chttp_web_request_is_htmx(request))
    header = chttp_server_request_header(
        request, CHTTP_WEB_CSRF_HEADER);
  if (header != NULL) {
    candidate = header;
    candidate_size = strlen(header);
  } else if (upload->csrf_count == 1u) {
    candidate = upload->csrf_token;
    candidate_size = upload->csrf_size;
  }

  status = chttp_web_csrf_validate_token(
      request, candidate, candidate_size, error);
  if (status != CHTTP_WEB_OK) {
    chttp_web_upload_abort_once(
        upload, status,
        error != NULL ? error->native_status : 0);
    return status;
  }

  if (validation != NULL) {
    if (validation->size < sizeof(*validation)) {
      chttp_web_upload_abort_once(
          upload, CHTTP_WEB_INVALID_ARGUMENT, 0);
      return chttp_web_upload_error(
          error, CHTTP_WEB_INVALID_ARGUMENT, 0,
          "upload validation state is invalid");
    }
    if (!validation->valid) {
      chttp_web_upload_abort_once(
          upload, CHTTP_WEB_VALIDATION, 0);
      return chttp_web_upload_error(
          error, CHTTP_WEB_VALIDATION, 0,
          "upload application validation rejected the request");
    }
  }

  native_status = upload->callbacks.commit(upload->user);
  if (native_status != SALTS_OK) {
    chttp_web_upload_abort_once(
        upload, CHTTP_WEB_UPLOAD, native_status);
    return chttp_web_upload_error(
        error, CHTTP_WEB_UPLOAD, native_status,
        "application upload commit failed");
  }

  upload->terminal_status = CHTTP_WEB_OK;
  upload->terminal_native_status = 0;
  upload->state = CHTTP_WEB_UPLOAD_COMMITTED;
  return chttp_web_upload_error(
      error, CHTTP_WEB_OK, 0, NULL);
}

chttp_web_status chttp_web_upload_abort(
    chttp_web_upload_request *upload,
    chttp_web_status status,
    int native_status,
    chttp_web_error *error) {
  if (!chttp_web_upload_valid(upload) ||
      status == CHTTP_WEB_OK ||
      upload->state == CHTTP_WEB_UPLOAD_IDLE ||
      upload->state == CHTTP_WEB_UPLOAD_COMMITTED)
    return chttp_web_upload_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "upload abort state or status is invalid");

  if (upload->state != CHTTP_WEB_UPLOAD_ABORTED)
    chttp_web_upload_abort_once(upload, status, native_status);
  return chttp_web_upload_error(
      error, CHTTP_WEB_OK, 0, NULL);
}

chttp_web_status chttp_web_upload_reset(
    chttp_web_upload_request *upload,
    chttp_web_error *error) {
  if (!chttp_web_upload_valid(upload) ||
      (upload->state != CHTTP_WEB_UPLOAD_COMMITTED &&
       upload->state != CHTTP_WEB_UPLOAD_ABORTED))
    return chttp_web_upload_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "upload reset requires committed or aborted state");

  *upload = (chttp_web_upload_request)CHTTP_WEB_UPLOAD_REQUEST_INIT;
  return chttp_web_upload_error(
      error, CHTTP_WEB_OK, 0, NULL);
}

void *chttp_web_upload_user(
    const chttp_web_upload_request *upload) {
  return chttp_web_upload_valid(upload) ? upload->user : NULL;
}
