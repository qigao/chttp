#include <chttp_web/web.h>

#include <salts/error_codes.h>
#include <vstr.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct chttp_web_sse_writer {
  char *data;
  size_t capacity;
  size_t size;
} chttp_web_sse_writer;

static chttp_web_status chttp_web_sse_fail(
    chttp_web_error *error,
    chttp_web_status status,
    int native_status,
    const char *message) {
  if (error != NULL) {
    *error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    error->status = status;
    error->native_status = native_status;
    if (message != NULL)
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return status;
}

static int chttp_web_sse_view_valid(chttp_web_string_view value) {
  if (value.size != 0u && value.data == NULL) return 0;
  return vstr_utf8_invalid_offset(
             vstr_from_buf(value.data, value.size)) == VSTR_NPOS;
}

static int chttp_web_sse_single_line_valid(
    chttp_web_string_view value, int reject_nul) {
  if (!chttp_web_sse_view_valid(value)) return 0;
  for (size_t index = 0u; index < value.size; ++index) {
    const unsigned char byte = (unsigned char)value.data[index];
    if (byte == (unsigned char)'\r' || byte == (unsigned char)'\n' ||
        (reject_nul && byte == 0u))
      return 0;
  }
  return 1;
}

static int chttp_web_sse_event_valid(const chttp_web_sse_event *event) {
  if (event == NULL || event->size != sizeof(*event) ||
      (!event->has_event && !event->has_data &&
       !event->has_id && !event->has_retry))
    return 0;
  if (event->has_event &&
      !chttp_web_sse_single_line_valid(event->event, 1))
    return 0;
  if (event->has_id &&
      !chttp_web_sse_single_line_valid(event->id, 1))
    return 0;
  if (event->has_data && !chttp_web_sse_view_valid(event->data))
    return 0;
  return 1;
}

static int chttp_web_sse_append(
    chttp_web_sse_writer *writer, const void *data, size_t size) {
  if (writer == NULL || (size != 0u && data == NULL) ||
      size > SIZE_MAX - writer->size)
    return 0;
  if (writer->data != NULL) {
    if (writer->size > writer->capacity ||
        size > writer->capacity - writer->size)
      return 0;
    if (size != 0u)
      memcpy(writer->data + writer->size, data, size);
  }
  writer->size += size;
  return 1;
}

static int chttp_web_sse_append_field(
    chttp_web_sse_writer *writer,
    const char *name,
    chttp_web_string_view value) {
  const size_t name_size = strlen(name);
  if (!chttp_web_sse_append(writer, name, name_size) ||
      !chttp_web_sse_append(writer, ":", 1u))
    return 0;
  if (value.size != 0u &&
      (!chttp_web_sse_append(writer, " ", 1u) ||
       !chttp_web_sse_append(writer, value.data, value.size)))
    return 0;
  return chttp_web_sse_append(writer, "\n", 1u);
}

static int chttp_web_sse_append_data_line(
    chttp_web_sse_writer *writer, const char *data, size_t size) {
  const chttp_web_string_view line = {data, size};
  return chttp_web_sse_append_field(writer, "data", line);
}

static int chttp_web_sse_append_data(
    chttp_web_sse_writer *writer, chttp_web_string_view data) {
  size_t start = 0u;
  size_t cursor = 0u;

  if (data.size == 0u)
    return chttp_web_sse_append_data_line(writer, NULL, 0u);

  for (;;) {
    while (cursor < data.size &&
           data.data[cursor] != '\r' && data.data[cursor] != '\n')
      ++cursor;

    if (!chttp_web_sse_append_data_line(
            writer, data.data + start, cursor - start))
      return 0;

    if (cursor == data.size) return 1;

    if (data.data[cursor] == '\r' &&
        cursor + 1u < data.size &&
        data.data[cursor + 1u] == '\n')
      cursor += 2u;
    else
      ++cursor;

    start = cursor;
    if (start == data.size)
      return chttp_web_sse_append_data_line(writer, NULL, 0u);
  }
}

static int chttp_web_sse_format_core(
    const chttp_web_sse_event *event, chttp_web_sse_writer *writer) {
  char retry[48];
  int retry_size;

  if (event->has_event &&
      !chttp_web_sse_append_field(writer, "event", event->event))
    return 0;
  if (event->has_id &&
      !chttp_web_sse_append_field(writer, "id", event->id))
    return 0;
  if (event->has_retry) {
    retry_size = snprintf(
        retry, sizeof(retry), "retry: %" PRIu64 "\n", event->retry_ms);
    if (retry_size < 0 || (size_t)retry_size >= sizeof(retry) ||
        !chttp_web_sse_append(writer, retry, (size_t)retry_size))
      return 0;
  }
  if (event->has_data &&
      !chttp_web_sse_append_data(writer, event->data))
    return 0;
  return chttp_web_sse_append(writer, "\n", 1u);
}

chttp_web_status chttp_web_sse_format_event(
    const chttp_web_sse_event *event,
    char *buffer,
    size_t buffer_capacity,
    size_t *out_size,
    chttp_web_error *error) {
  chttp_web_sse_writer measure = {0};
  chttp_web_sse_writer output;

  if (out_size != NULL) *out_size = 0u;
  if (!chttp_web_sse_event_valid(event) || buffer == NULL ||
      buffer_capacity == 0u || out_size == NULL)
    return chttp_web_sse_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL,
        "invalid SSE event or output buffer");

  if (!chttp_web_sse_format_core(event, &measure))
    return chttp_web_sse_fail(
        error, CHTTP_WEB_CAPACITY, SALTS_EMSGSIZE,
        "SSE event size overflow");
  if (measure.size > buffer_capacity)
    return chttp_web_sse_fail(
        error, CHTTP_WEB_CAPACITY, SALTS_EMSGSIZE,
        "SSE event exceeds output capacity");

  output = (chttp_web_sse_writer){buffer, buffer_capacity, 0u};
  if (!chttp_web_sse_format_core(event, &output) ||
      output.size != measure.size)
    return chttp_web_sse_fail(
        error, CHTTP_WEB_CAPACITY, SALTS_EMSGSIZE,
        "SSE event formatting exceeded output capacity");

  *out_size = output.size;
  return chttp_web_sse_fail(error, CHTTP_WEB_OK, SALTS_OK, NULL);
}

chttp_web_status chttp_web_sse_stream_init(
    chttp_web_sse_stream *stream,
    chttp_web_sse_next_fn next,
    chttp_web_sse_close_fn close,
    void *user,
    char *scratch,
    size_t scratch_capacity,
    chttp_web_error *error) {
  if (stream == NULL || next == NULL || scratch == NULL ||
      scratch_capacity == 0u)
    return chttp_web_sse_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL,
        "invalid SSE stream configuration");

  *stream = (chttp_web_sse_stream)CHTTP_WEB_SSE_STREAM_INIT;
  stream->next = next;
  stream->close = close;
  stream->user = user;
  stream->scratch = scratch;
  stream->scratch_capacity = scratch_capacity;
  return chttp_web_sse_fail(error, CHTTP_WEB_OK, SALTS_OK, NULL);
}

static int chttp_web_sse_source_read(
    void *user, void *buffer, size_t capacity, size_t *out_size) {
  chttp_web_sse_stream *stream = (chttp_web_sse_stream *)user;

  if (out_size != NULL) *out_size = 0u;
  if (stream == NULL || stream->size != sizeof(*stream) ||
      buffer == NULL || capacity == 0u || out_size == NULL ||
      !stream->active || stream->next == NULL ||
      stream->scratch == NULL || stream->scratch_capacity == 0u)
    return SALTS_EINVAL;

  while (stream->buffered_offset == stream->buffered_size) {
    chttp_web_sse_event event =
        (chttp_web_sse_event)CHTTP_WEB_SSE_EVENT_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    size_t size = 0u;
    int status;
    chttp_web_status formatted;

    stream->buffered_offset = 0u;
    stream->buffered_size = 0u;
    if (stream->eof) return SALTS_OK;

    status = stream->next(stream->user, &event);
    if (status == SALTS_ENOENT) {
      stream->eof = true;
      return SALTS_OK;
    }
    if (status != SALTS_OK) return status;

    formatted = chttp_web_sse_format_event(
        &event, stream->scratch, stream->scratch_capacity, &size, &error);
    if (formatted == CHTTP_WEB_CAPACITY) return SALTS_EMSGSIZE;
    if (formatted != CHTTP_WEB_OK) return SALTS_EINVAL;
    stream->buffered_size = size;
  }

  {
    const size_t remaining =
        stream->buffered_size - stream->buffered_offset;
    const size_t copied = remaining < capacity ? remaining : capacity;
    memcpy(buffer, stream->scratch + stream->buffered_offset, copied);
    stream->buffered_offset += copied;
    *out_size = copied;
  }
  return SALTS_OK;
}

static void chttp_web_sse_source_cleanup(void *user, int status) {
  chttp_web_sse_stream *stream = (chttp_web_sse_stream *)user;
  chttp_web_sse_close_fn close;
  void *close_user;

  if (stream == NULL || stream->size != sizeof(*stream)) return;
  close = stream->close;
  close_user = stream->user;
  stream->active = false;
  stream->terminal_status = status;
  stream->buffered_offset = 0u;
  stream->buffered_size = 0u;
  if (close != NULL) close(close_user, status);
}

chttp_web_status chttp_web_sse_response(
    chttp_server_response *response,
    chttp_web_sse_stream *stream,
    chttp_web_error *error) {
  chttp_body_source source;
  int status;

  if (response == NULL || stream == NULL ||
      stream->size != sizeof(*stream) || stream->active ||
      stream->next == NULL || stream->scratch == NULL ||
      stream->scratch_capacity == 0u)
    return chttp_web_sse_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL,
        "invalid SSE response state");

  status = chttp_server_response_set_header(
      response, "Cache-Control", "no-cache");
  if (status != SALTS_OK)
    return chttp_web_sse_fail(
        error, CHTTP_WEB_SERVER, status,
        "CHTTP rejected the SSE cache policy");

  stream->buffered_offset = 0u;
  stream->buffered_size = 0u;
  stream->terminal_status = SALTS_OK;
  stream->eof = false;
  stream->active = true;

  source = (chttp_body_source){
      .read = chttp_web_sse_source_read,
      .user = stream,
      .content_length = 0u,
      .content_length_known = 0};
  status = chttp_server_response_source_with_cleanup(
      response, 200u, "text/event-stream; charset=utf-8",
      &source, chttp_web_sse_source_cleanup, stream);
  if (status != SALTS_OK) {
    stream->active = false;
    return chttp_web_sse_fail(
        error, CHTTP_WEB_SERVER, status,
        "CHTTP rejected the SSE response source");
  }

  return chttp_web_sse_fail(error, CHTTP_WEB_OK, SALTS_OK, NULL);
}
