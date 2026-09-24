#include <chttp_service/service.h>

#include <salts/error_codes.h>

#include <cserde/cserde.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_service_impl chttp_service_impl;

typedef struct chttp_service_method_record {
  chttp_service_impl *owner;
  const DataBindHttpMethodPlan *plan;
  char *route;
  chttp_method method;
  chttp_service_invoke_fn invoke;
  void *user;
} chttp_service_method_record;

struct chttp_service_impl {
  chttp_service_method_record *methods;
  size_t method_capacity;
  size_t method_count;

  char *scalar_scratch;
  size_t scalar_capacity;

  unsigned char *response_scratch;
  size_t response_scratch_bytes;

  unsigned char *native_workspace;
  DataBindNativeOptions native_options;
};

typedef struct chttp_service_scalar_reader {
  cserde_token token;
  int emitted;
} chttp_service_scalar_reader;

typedef struct chttp_service_http_provider {
  chttp_service_impl *service;
  const chttp_server_request_view *request;

  chttp_service_scalar_reader scalar;

  DataBindNativeDiagnostic native_diagnostic;
  cserde_writer writer;

  size_t staged_body_size;
  const char *staged_content_type;
  int output_started;
  int output_written;
  int output_committed;
  int error_output;
} chttp_service_http_provider;

static void chttp_service_error_set(
    DataBindError *error, DataBindStatus status, const char *message) {
  if (error == NULL) return;
  if (error->size == 0u) error->size = sizeof(*error);
  error->code = status;
  error->line = -1;
  error->column = -1;
  error->path[0] = '\0';
  snprintf(error->message, sizeof(error->message), "%s",
           message != NULL ? message : "");
}

static int chttp_service_native_scalar_kind(const cmeta_data_desc *data) {
  if (data == NULL) return 0;
  switch (data->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
  case CMETA_DATA_ENUM:
    return 1;
  default:
    return 0;
  }
}

static cserde_status chttp_service_scalar_next(
    void *context, cserde_token *out) {
  chttp_service_scalar_reader *reader =
      (chttp_service_scalar_reader *)context;
  if (reader == NULL || out == NULL) return CSERDE_INVALID_ARGUMENT;
  if (reader->emitted) return CSERDE_DONE;
  *out = reader->token;
  reader->emitted = 1;
  return CSERDE_OK;
}

static const cserde_reader_ops CHTTP_SERVICE_SCALAR_READER_OPS = {
    offsetof(cserde_reader_ops, next) + sizeof(cserde_reader_next_fn),
    CSERDE_READER_OPS_ABI_VERSION,
    chttp_service_scalar_next};

static int chttp_service_hex(unsigned char value) {
  if (value >= (unsigned char)'0' && value <= (unsigned char)'9')
    return (int)(value - (unsigned char)'0');
  if (value >= (unsigned char)'a' && value <= (unsigned char)'f')
    return 10 + (int)(value - (unsigned char)'a');
  if (value >= (unsigned char)'A' && value <= (unsigned char)'F')
    return 10 + (int)(value - (unsigned char)'A');
  return -1;
}

static DataBindStatus chttp_service_decode_component(
    chttp_service_http_provider *provider,
    const char *source,
    size_t source_size,
    int plus_is_space,
    const char **out_text,
    size_t *out_size,
    DataBindError *error) {
  size_t src = 0u;
  size_t dst = 0u;
  char *buffer;

  if (provider == NULL || provider->service == NULL || source == NULL ||
      out_text == NULL || out_size == NULL)
    return DATA_BIND_ERR_INVALID_ARG;
  if (source_size > provider->service->scalar_capacity) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_LIMIT,
        "HTTP binding value exceeds configured bound");
    return DATA_BIND_ERR_LIMIT;
  }

  buffer = provider->service->scalar_scratch;
  while (src < source_size) {
    unsigned char value = (unsigned char)source[src++];
    if (value == (unsigned char)'%') {
      int high;
      int low;
      if (src + 1u >= source_size) {
        chttp_service_error_set(
            error, DATA_BIND_ERR_PARSE, "Truncated HTTP percent escape");
        return DATA_BIND_ERR_PARSE;
      }
      high = chttp_service_hex((unsigned char)source[src]);
      low = chttp_service_hex((unsigned char)source[src + 1u]);
      if (high < 0 || low < 0) {
        chttp_service_error_set(
            error, DATA_BIND_ERR_PARSE, "Malformed HTTP percent escape");
        return DATA_BIND_ERR_PARSE;
      }
      value = (unsigned char)((high << 4) | low);
      src += 2u;
    } else if (plus_is_space && value == (unsigned char)'+') {
      value = (unsigned char)' ';
    }
    if (dst == provider->service->scalar_capacity) {
      chttp_service_error_set(
          error, DATA_BIND_ERR_LIMIT,
          "HTTP binding value exceeds configured bound");
      return DATA_BIND_ERR_LIMIT;
    }
    buffer[dst++] = (char)value;
  }

  buffer[dst] = '\0';
  *out_text = buffer;
  *out_size = dst;
  return DATA_BIND_OK;
}

static DataBindStatus chttp_service_query_value(
    chttp_service_http_provider *provider,
    const char *name,
    const char **out_text,
    size_t *out_size,
    DataBindError *error) {
  const char *query;
  const char *cursor;
  size_t name_size;

  if (provider == NULL || provider->request == NULL || name == NULL ||
      out_text == NULL || out_size == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  *out_text = NULL;
  *out_size = 0u;
  query = strchr(provider->request->target, '?');
  if (query == NULL) return DATA_BIND_OK;

  name_size = strlen(name);
  cursor = query + 1u;
  while (*cursor != '\0' && *cursor != '#') {
    const char *pair_end = strchr(cursor, '&');
    const char *equals;
    size_t key_size;
    if (pair_end == NULL) pair_end = cursor + strlen(cursor);
    equals = memchr(cursor, '=', (size_t)(pair_end - cursor));
    key_size = equals != NULL ? (size_t)(equals - cursor)
                              : (size_t)(pair_end - cursor);
    if (key_size == name_size && memcmp(cursor, name, key_size) == 0) {
      const char *value = equals != NULL ? equals + 1u : pair_end;
      return chttp_service_decode_component(
          provider, value, (size_t)(pair_end - value), 1,
          out_text, out_size, error);
    }
    if (*pair_end == '\0') break;
    cursor = pair_end + 1u;
  }
  return DATA_BIND_OK;
}

static DataBindStatus chttp_service_cookie_value(
    chttp_service_http_provider *provider,
    const char *name,
    const char **out_text,
    size_t *out_size) {
  const char *cookie;
  const char *cursor;
  size_t name_size;

  if (provider == NULL || provider->request == NULL || name == NULL ||
      out_text == NULL || out_size == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  *out_text = NULL;
  *out_size = 0u;
  cookie = chttp_server_request_header(provider->request, "Cookie");
  if (cookie == NULL) return DATA_BIND_OK;

  name_size = strlen(name);
  cursor = cookie;
  while (*cursor != '\0') {
    const char *end;
    const char *equals;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ';') ++cursor;
    end = strchr(cursor, ';');
    if (end == NULL) end = cursor + strlen(cursor);
    equals = memchr(cursor, '=', (size_t)(end - cursor));
    if (equals != NULL) {
      const char *key_end = equals;
      const char *value = equals + 1u;
      const char *value_end = end;
      while (key_end > cursor &&
             (key_end[-1] == ' ' || key_end[-1] == '\t'))
        --key_end;
      while (value < end && (*value == ' ' || *value == '\t')) ++value;
      while (value_end > value &&
             (value_end[-1] == ' ' || value_end[-1] == '\t'))
        --value_end;
      if ((size_t)(key_end - cursor) == name_size &&
          memcmp(cursor, name, name_size) == 0) {
        *out_text = value;
        *out_size = (size_t)(value_end - value);
        return DATA_BIND_OK;
      }
    }
    if (*end == '\0') break;
    cursor = end + 1u;
  }
  return DATA_BIND_OK;
}

static DataBindStatus chttp_service_scalar_reader_init(
    chttp_service_http_provider *provider,
    const DataBindBindingPlanEntry *entry,
    const char *text,
    size_t text_size,
    cserde_reader *reader,
    DataBindError *error) {
  char *end = NULL;

  if (provider == NULL || entry == NULL || entry->data == NULL ||
      text == NULL || reader == NULL)
    return DATA_BIND_ERR_INVALID_ARG;
  if (text_size > provider->service->scalar_capacity) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_LIMIT,
        "HTTP binding value exceeds configured bound");
    return DATA_BIND_ERR_LIMIT;
  }

  provider->scalar = (chttp_service_scalar_reader){0};
  errno = 0;

  switch (entry->data->kind) {
  case CMETA_DATA_BOOL:
    provider->scalar.token.kind = CSERDE_BOOL;
    if ((text_size == 4u && memcmp(text, "true", 4u) == 0) ||
        (text_size == 1u && text[0] == '1'))
      provider->scalar.token.value.boolean = true;
    else if ((text_size == 5u && memcmp(text, "false", 5u) == 0) ||
             (text_size == 1u && text[0] == '0'))
      provider->scalar.token.value.boolean = false;
    else {
      chttp_service_error_set(
          error, DATA_BIND_ERR_PARSE,
          "Invalid boolean HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    break;

  case CMETA_DATA_SINT: {
    long long value;
    if (text_size > provider->service->scalar_capacity)
      return DATA_BIND_ERR_LIMIT;
    memmove(provider->service->scalar_scratch, text, text_size);
    provider->service->scalar_scratch[text_size] = '\0';
    value = strtoll(provider->service->scalar_scratch, &end, 10);
    if (errno != 0 || end == provider->service->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(
          error, DATA_BIND_ERR_PARSE,
          "Invalid signed HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    provider->scalar.token.kind = CSERDE_SINT;
    provider->scalar.token.value.sint = (int64_t)value;
    break;
  }

  case CMETA_DATA_UINT: {
    unsigned long long value;
    if (text_size == 0u || text[0] == '-' ||
        text_size > provider->service->scalar_capacity) {
      chttp_service_error_set(
          error, DATA_BIND_ERR_PARSE,
          "Invalid unsigned HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    memmove(provider->service->scalar_scratch, text, text_size);
    provider->service->scalar_scratch[text_size] = '\0';
    value = strtoull(provider->service->scalar_scratch, &end, 10);
    if (errno != 0 || end == provider->service->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(
          error, DATA_BIND_ERR_PARSE,
          "Invalid unsigned HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    provider->scalar.token.kind = CSERDE_UINT;
    provider->scalar.token.value.uint = (uint64_t)value;
    break;
  }

  case CMETA_DATA_FLOAT: {
    double value;
    if (text_size > provider->service->scalar_capacity)
      return DATA_BIND_ERR_LIMIT;
    memmove(provider->service->scalar_scratch, text, text_size);
    provider->service->scalar_scratch[text_size] = '\0';
    value = strtod(provider->service->scalar_scratch, &end);
    if (errno != 0 || end == provider->service->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(
          error, DATA_BIND_ERR_PARSE,
          "Invalid floating HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    provider->scalar.token.kind = CSERDE_FLOAT;
    provider->scalar.token.value.floating = value;
    break;
  }

  case CMETA_DATA_STRING:
  case CMETA_DATA_ENUM:
    provider->scalar.token.kind = CSERDE_STRING;
    provider->scalar.token.value.slice.data =
        (const unsigned char *)text;
    provider->scalar.token.value.slice.size = text_size;
    provider->scalar.token.value.slice.lifetime = CSERDE_VIEW_STABLE;
    break;

  case CMETA_DATA_BYTES:
    provider->scalar.token.kind = CSERDE_BYTES;
    provider->scalar.token.value.slice.data =
        (const unsigned char *)text;
    provider->scalar.token.value.slice.size = text_size;
    provider->scalar.token.value.slice.lifetime = CSERDE_VIEW_STABLE;
    break;

  default:
    chttp_service_error_set(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "HTTP scalar binding requires a scalar/string/bytes/enum value");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  return cserde_reader_init(
             reader, &CHTTP_SERVICE_SCALAR_READER_OPS, &provider->scalar) ==
         CSERDE_OK
             ? DATA_BIND_OK
             : DATA_BIND_ERR_RUNTIME;
}

static DataBindStatus chttp_service_http_open_input(
    void *context,
    const DataBindBindingPlanEntry *entry,
    cserde_reader *reader,
    DataBindBindingValueState *state,
    DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  const char *text = NULL;
  size_t text_size = 0u;
  DataBindStatus status;

  if (provider == NULL || provider->request == NULL || entry == NULL ||
      reader == NULL || state == NULL || entry->address.space == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  *state = DATA_BIND_VALUE_STATE_ABSENT;

  if (strcmp(entry->address.space, "http.body") == 0) {
    if (provider->request->body == NULL || provider->request->body_size == 0u)
      return DATA_BIND_OK;
    chttp_service_error_set(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "Structured HTTP body binding requires a generated FormatPlan");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  if (strcmp(entry->address.space, "http.path") == 0) {
    const char *raw =
        chttp_server_request_param(provider->request, entry->address.name);
    if (raw == NULL) return DATA_BIND_OK;
    status = chttp_service_decode_component(
        provider, raw, strlen(raw), 0, &text, &text_size, error);
    if (status != DATA_BIND_OK) return status;
  } else if (strcmp(entry->address.space, "http.query") == 0) {
    status = chttp_service_query_value(
        provider, entry->address.name, &text, &text_size, error);
    if (status != DATA_BIND_OK || text == NULL) return status;
  } else if (strcmp(entry->address.space, "http.header") == 0) {
    text = chttp_server_request_header(
        provider->request, entry->address.name);
    if (text == NULL) return DATA_BIND_OK;
    text_size = strlen(text);
  } else if (strcmp(entry->address.space, "http.cookie") == 0) {
    status = chttp_service_cookie_value(
        provider, entry->address.name, &text, &text_size);
    if (status != DATA_BIND_OK || text == NULL) return status;
  } else {
    chttp_service_error_set(
        error, DATA_BIND_ERR_SCHEMA,
        "Unsupported HTTP MethodPlan ingress address space");
    return DATA_BIND_ERR_SCHEMA;
  }

  status = chttp_service_scalar_reader_init(
      provider, entry, text, text_size, reader, error);
  if (status == DATA_BIND_OK) *state = DATA_BIND_VALUE_STATE_VALUE;
  return status;
}

static cserde_status chttp_service_output_token(
    void *context, const cserde_token *token) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  unsigned char *output;
  size_t capacity;
  int length = -1;

  if (provider == NULL || provider->service == NULL || token == NULL)
    return CSERDE_INVALID_ARGUMENT;
  if (provider->output_written) return CSERDE_UNSUPPORTED;

  output = provider->service->response_scratch;
  capacity = provider->service->response_scratch_bytes;

  switch (token->kind) {
  case CSERDE_BOOL: {
    const char *text = token->value.boolean ? "true" : "false";
    size_t size = token->value.boolean ? 4u : 5u;
    if (size > capacity) return CSERDE_LIMIT_EXCEEDED;
    memcpy(output, text, size);
    provider->staged_body_size = size;
    provider->staged_content_type = "text/plain";
    break;
  }
  case CSERDE_SINT:
    length = snprintf(
        (char *)output, capacity, "%" PRId64, token->value.sint);
    break;
  case CSERDE_UINT:
    length = snprintf(
        (char *)output, capacity, "%" PRIu64, token->value.uint);
    break;
  case CSERDE_FLOAT:
    length = snprintf(
        (char *)output, capacity, "%.17g", token->value.floating);
    break;
  case CSERDE_STRING:
  case CSERDE_BYTES:
    if (token->value.slice.size > capacity)
      return CSERDE_LIMIT_EXCEEDED;
    if (token->value.slice.size != 0u &&
        token->value.slice.data == NULL)
      return CSERDE_INVALID_TOKEN;
    if (token->value.slice.size != 0u)
      memcpy(output, token->value.slice.data, token->value.slice.size);
    provider->staged_body_size = token->value.slice.size;
    provider->staged_content_type =
        token->kind == CSERDE_BYTES
            ? "application/octet-stream"
            : "text/plain";
    break;
  case CSERDE_NULL:
    if (capacity < 4u) return CSERDE_LIMIT_EXCEEDED;
    memcpy(output, "null", 4u);
    provider->staged_body_size = 4u;
    provider->staged_content_type = "application/json";
    break;
  default:
    return CSERDE_UNSUPPORTED;
  }

  if (length >= 0) {
    if ((size_t)length >= capacity) return CSERDE_LIMIT_EXCEEDED;
    provider->staged_body_size = (size_t)length;
    provider->staged_content_type = "text/plain";
  }

  provider->output_written = 1;
  return CSERDE_OK;
}

static cserde_status chttp_service_output_finish(void *context) {
  (void)context;
  return CSERDE_OK;
}

static const cserde_writer_ops CHTTP_SERVICE_SCALAR_WRITER_OPS = {
    sizeof(cserde_writer_ops),
    CSERDE_WRITER_OPS_ABI_VERSION,
    chttp_service_output_token,
    chttp_service_output_finish};

static DataBindStatus chttp_service_http_begin_output(
    void *context, DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  (void)error;
  if (provider == NULL || provider->service == NULL)
    return DATA_BIND_ERR_INVALID_ARG;
  provider->native_diagnostic =
      (DataBindNativeDiagnostic)DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  provider->writer = (cserde_writer){0};
  provider->staged_body_size = 0u;
  provider->staged_content_type = "text/plain";
  provider->output_started = 1;
  provider->output_written = 0;
  provider->output_committed = 0;
  provider->error_output = 0;
  return DATA_BIND_OK;
}

static DataBindStatus chttp_service_http_write_output(
    void *context,
    const DataBindBindingPlanEntry *entry,
    DataBindBindingValueState state,
    const void *value,
    size_t value_bytes,
    DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  DataBindStatus status;

  if (provider == NULL || provider->service == NULL || entry == NULL ||
      !provider->output_started)
    return DATA_BIND_ERR_INVALID_ARG;

  if (entry->address.binding_class == DATA_BIND_BINDING_ERROR) {
    if (provider->output_written) {
      chttp_service_error_set(
          error, DATA_BIND_ERR_TYPE_MISMATCH,
          "Only one HTTP typed error may be published");
      return DATA_BIND_ERR_TYPE_MISMATCH;
    }
    provider->error_output = 1;
    provider->output_written = 1;
    provider->staged_body_size = 0u;
    provider->staged_content_type = "text/plain";
    return DATA_BIND_OK;
  }

  if (entry->address.space == NULL ||
      strcmp(entry->address.space, "http.response.body") != 0) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "Response headers/structured output require a generated FormatPlan");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  if (provider->output_written) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "First CHttp::Service slice admits one scalar response body value");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  if (state == DATA_BIND_VALUE_STATE_NULL) {
    cserde_token token = {0};
    token.kind = CSERDE_NULL;
    return chttp_service_output_token(provider, &token) == CSERDE_OK
               ? DATA_BIND_OK
               : DATA_BIND_ERR_LIMIT;
  }

  if (state != DATA_BIND_VALUE_STATE_VALUE || value == NULL ||
      !chttp_service_native_scalar_kind(entry->data)) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "CHttp::Service response requires one scalar/text value");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  if (cserde_writer_init(
          &provider->writer, &CHTTP_SERVICE_SCALAR_WRITER_OPS,
          provider) != CSERDE_OK) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Could not initialize HTTP scalar response writer");
    return DATA_BIND_ERR_RUNTIME;
  }

  status = data_bind_native_encode(
      &provider->service->native_options,
      entry->data, value, value_bytes,
      &provider->writer, &provider->native_diagnostic);
  if (status == DATA_BIND_OK &&
      cserde_writer_finish(&provider->writer) != CSERDE_OK) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "HTTP scalar response writer could not finish");
    return DATA_BIND_ERR_RUNTIME;
  }
  if (status != DATA_BIND_OK && error != NULL)
    *error = provider->native_diagnostic.error;
  return status;
}

static DataBindStatus chttp_service_http_commit_output(
    void *context, DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  (void)error;
  if (provider == NULL || !provider->output_started)
    return DATA_BIND_ERR_INVALID_ARG;
  provider->output_committed = 1;
  return DATA_BIND_OK;
}

static void chttp_service_http_abort_output(void *context) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  if (provider == NULL) return;
  provider->staged_body_size = 0u;
  provider->staged_content_type = "text/plain";
  provider->output_started = 0;
  provider->output_written = 0;
  provider->output_committed = 0;
  provider->error_output = 0;
  provider->writer = (cserde_writer){0};
}

static DataBindBindingProvider chttp_service_provider(
    chttp_service_http_provider *state) {
  DataBindBindingProvider provider = DATA_BIND_BINDING_PROVIDER_INIT;
  provider.context = state;
  provider.open_input = chttp_service_http_open_input;
  provider.begin_output = chttp_service_http_begin_output;
  provider.write_output = chttp_service_http_write_output;
  provider.commit_output = chttp_service_http_commit_output;
  provider.abort_output = chttp_service_http_abort_output;
  return provider;
}

static int chttp_service_method_from_text(
    const char *method, chttp_method *out) {
  if (method == NULL || out == NULL) return SALTS_EINVAL;
  if (strcmp(method, "GET") == 0)
    *out = CHTTP_METHOD_GET;
  else if (strcmp(method, "HEAD") == 0)
    *out = CHTTP_METHOD_HEAD;
  else if (strcmp(method, "POST") == 0)
    *out = CHTTP_METHOD_POST;
  else if (strcmp(method, "PUT") == 0)
    *out = CHTTP_METHOD_PUT;
  else if (strcmp(method, "DELETE") == 0)
    *out = CHTTP_METHOD_DELETE;
  else if (strcmp(method, "PATCH") == 0)
    *out = CHTTP_METHOD_PATCH;
  else if (strcmp(method, "OPTIONS") == 0)
    *out = CHTTP_METHOD_OPTIONS;
  else
    return SALTS_ENOTSUP;
  return SALTS_OK;
}

static int chttp_service_lower_route(const char *source, char **out_route) {
  size_t length;
  char *route;
  size_t src = 0u;
  size_t dst = 0u;

  if (source == NULL || out_route == NULL || source[0] != '/')
    return SALTS_EINVAL;
  *out_route = NULL;
  length = strlen(source);
  route = (char *)malloc(length + 1u);
  if (route == NULL) return SALTS_ENOMEM;

  while (src < length) {
    if (source[src] == '{') {
      size_t end = src + 1u;
      if (src == 0u || source[src - 1u] != '/') {
        free(route);
        return SALTS_EINVAL;
      }
      while (end < length && source[end] != '}') ++end;
      if (end == length || end == src + 1u ||
          (end + 1u < length && source[end + 1u] != '/')) {
        free(route);
        return SALTS_EINVAL;
      }
      route[dst++] = ':';
      ++src;
      while (src < end) route[dst++] = source[src++];
      ++src;
    } else {
      route[dst++] = source[src++];
    }
  }

  route[dst] = '\0';
  *out_route = route;
  return SALTS_OK;
}

static int chttp_service_plan_supported(
    const DataBindHttpMethodPlan *plan) {
  const DataBindBindingPlan *binding;
  size_t i;
  size_t body_outputs = 0u;

  if (plan == NULL ||
      data_bind_http_method_plan_context_flags(plan) !=
          DATA_BIND_HTTP_CONTEXT_NONE)
    return 0;
  binding = data_bind_http_method_plan_binding(plan);
  if (binding == NULL) return 0;

  for (i = 0u; i < data_bind_binding_plan_ingress_count(binding); ++i) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "http.body") == 0 ||
        (strcmp(entry.address.space, "http.path") != 0 &&
         strcmp(entry.address.space, "http.query") != 0 &&
         strcmp(entry.address.space, "http.header") != 0 &&
         strcmp(entry.address.space, "http.cookie") != 0) ||
        !chttp_service_native_scalar_kind(entry.data))
      return 0;
  }

  for (i = 0u; i < data_bind_binding_plan_egress_count(binding); ++i) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_egress_at(binding, i, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "http.response.body") != 0 ||
        !chttp_service_native_scalar_kind(entry.data))
      return 0;
    ++body_outputs;
  }

  return body_outputs <= 1u;
}

static int chttp_service_http_handler(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_service_method_record *record =
      (chttp_service_method_record *)user;
  chttp_service_http_provider state;
  DataBindBindingProvider provider;
  DataBindBindingOutcome outcome = DATA_BIND_BINDING_OUTCOME_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  int status;
  int http_status;

  if (record == NULL || record->owner == NULL || record->plan == NULL ||
      record->invoke == NULL || request == NULL || response == NULL)
    return SALTS_EINVAL;

  state = (chttp_service_http_provider){
      .service = record->owner,
      .request = request,
      .staged_content_type = "text/plain"};
  provider = chttp_service_provider(&state);

  status = record->invoke(
      record->user, record->plan, &provider,
      &record->owner->native_options, &outcome, &diagnostic);
  if (status != SALTS_OK) {
    if (status == SALTS_EINVAL)
      return chttp_server_reply(
          response, 400u, "text/plain", "Bad Request", 11u);
    return chttp_server_reply(
        response, 500u, "text/plain", "Internal Server Error", 21u);
  }

  if (!data_bind_http_method_plan_status_for_outcome(
          record->plan, &outcome, &http_status)) {
    return chttp_server_reply(
        response, 500u, "text/plain", "Internal Server Error", 21u);
  }

  if (!state.output_committed)
    return chttp_server_reply(
        response, 500u, "text/plain", "Internal Server Error", 21u);

  return chttp_server_reply(
      response, (unsigned int)http_status,
      state.staged_content_type != NULL
          ? state.staged_content_type
          : "text/plain",
      state.staged_body_size != 0u
          ? record->owner->response_scratch
          : NULL,
      state.staged_body_size);
}

int chttp_service_init(
    chttp_service *service, const chttp_service_config *config) {
  chttp_service_impl *impl;

  if (service == NULL || config == NULL ||
      config->size < sizeof(*config) ||
      config->method_capacity == 0u ||
      config->max_binding_value_bytes == 0u ||
      config->max_response_body_bytes == 0u ||
      config->native_workspace_bytes == 0u ||
      config->native_max_depth == 0u ||
      config->native_max_items == 0u)
    return SALTS_EINVAL;
  if (service->impl != NULL) return SALTS_EALREADY;
  if (config->max_binding_value_bytes == SIZE_MAX ||
      config->max_response_body_bytes == SIZE_MAX)
    return SALTS_ERANGE;

  impl = (chttp_service_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->methods = (chttp_service_method_record *)calloc(
      config->method_capacity, sizeof(*impl->methods));
  impl->scalar_scratch =
      (char *)malloc(config->max_binding_value_bytes + 1u);
  impl->response_scratch =
      (unsigned char *)malloc(config->max_response_body_bytes);
  impl->native_workspace =
      (unsigned char *)malloc(config->native_workspace_bytes);
  if (impl->methods == NULL || impl->scalar_scratch == NULL ||
      impl->response_scratch == NULL || impl->native_workspace == NULL) {
    free(impl->native_workspace);
    free(impl->response_scratch);
    free(impl->scalar_scratch);
    free(impl->methods);
    free(impl);
    return SALTS_ENOMEM;
  }

  impl->method_capacity = config->method_capacity;
  impl->scalar_capacity = config->max_binding_value_bytes;
  impl->response_scratch_bytes = config->max_response_body_bytes;
  impl->native_options =
      (DataBindNativeOptions)DATA_BIND_NATIVE_OPTIONS_INIT;
  impl->native_options.workspace = impl->native_workspace;
  impl->native_options.workspace_bytes = config->native_workspace_bytes;
  impl->native_options.max_depth = config->native_max_depth;
  impl->native_options.max_items = config->native_max_items;
  impl->native_options.max_owned_bytes = config->native_max_owned_bytes;

  service->impl = impl;
  return SALTS_OK;
}

int chttp_service_mount_http(
    chttp_service *service,
    chttp_server *server,
    const chttp_service_http_mount *mount) {
  chttp_service_impl *impl;
  chttp_service_method_record *record;
  chttp_server_route_options route_options;
  const char *method_text;
  const char *route_text;
  char *route = NULL;
  chttp_method method;
  int status;

  if (service == NULL || service->impl == NULL || server == NULL ||
      mount == NULL || mount->size < sizeof(*mount) ||
      mount->plan == NULL || mount->invoke == NULL ||
      (mount->middleware_count != 0u && mount->middleware == NULL))
    return SALTS_EINVAL;
  if (!chttp_service_plan_supported(mount->plan))
    return SALTS_ENOTSUP;

  impl = (chttp_service_impl *)service->impl;
  if (impl->method_count == impl->method_capacity) return SALTS_ENOBUFS;

  method_text = data_bind_http_method_plan_method(mount->plan);
  route_text = data_bind_http_method_plan_route(mount->plan);
  status = chttp_service_method_from_text(method_text, &method);
  if (status != SALTS_OK) return status;
  status = chttp_service_lower_route(route_text, &route);
  if (status != SALTS_OK) return status;

  record = &impl->methods[impl->method_count];
  memset(record, 0, sizeof(*record));
  record->owner = impl;
  record->plan = mount->plan;
  record->route = route;
  record->method = method;
  record->invoke = mount->invoke;
  record->user = mount->user;

  route_options = (chttp_server_route_options){
      .method = method,
      .path = record->route,
      .middleware = mount->middleware,
      .middleware_count = mount->middleware_count,
      .handler = chttp_service_http_handler,
      .user = record};

  status = chttp_server_route_with(server, &route_options);
  if (status != SALTS_OK) {
    free(record->route);
    memset(record, 0, sizeof(*record));
    return status;
  }

  ++impl->method_count;
  return SALTS_OK;
}

int chttp_service_destroy(chttp_service *service) {
  chttp_service_impl *impl;
  size_t i;

  if (service == NULL) return SALTS_EINVAL;
  impl = (chttp_service_impl *)service->impl;
  if (impl == NULL) return SALTS_OK;

  for (i = 0u; i < impl->method_count; ++i)
    free(impl->methods[i].route);
  free(impl->native_workspace);
  free(impl->response_scratch);
  free(impl->scalar_scratch);
  free(impl->methods);
  free(impl);
  service->impl = NULL;
  return SALTS_OK;
}
