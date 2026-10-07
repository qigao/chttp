#include <chttp_service/service.h>

#include <salts/error_codes.h>
#include <cmeta_buffer.h>

#include <cserde/cserde.h>
#include <cflow/plan.h>
#include <cmeta/data.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_service_impl chttp_service_impl;

typedef struct chttp_service_method_record {
  chttp_service_impl *owner;
  const DataBindHttpMethodPlan *method_plan;
  const DataBindBindingPlan *binding;
  const DataBindServiceNativeBinding *native_binding;
  const DataBindNativeExecution *execution;
  chttp_service_execution_mode execution_mode;
  cflow_executor *executor;
  cflow_plan cflow_plan;

  salts_component_plugin_scope component_scope;
  chttp_service_component_operation component_operation;

  size_t request_bytes;
  size_t response_bytes;
  size_t error_bytes;
  size_t param_count;
  _Atomic size_t deferred_in_flight;
} chttp_service_method_record;

struct chttp_service_impl {
  chttp_service_method_record *methods;
  size_t method_capacity;
  size_t method_count;

  size_t scalar_capacity;
  size_t native_workspace_bytes;
  DataBindNativeOptions native_options;

  size_t response_capacity;
  size_t max_call_frame_bytes;
};

typedef struct chttp_service_invocation {
  chttp_service_method_record *record;

  unsigned char *request_storage;
  unsigned char *response_storage;
  unsigned char *error_storage;

  void *params[3];
  size_t param_bytes[3];
  DataBindBindingCallFrame frame;

  char *scalar_scratch;
  unsigned char *native_workspace;
  DataBindNativeOptions native_options;
  chttp_server_deferred deferred;
  int frame_live;
  int deferred_method_held;
} chttp_service_invocation;

typedef struct chttp_service_scalar_reader {
  cserde_token token;
  int emitted;
} chttp_service_scalar_reader;

typedef struct chttp_service_http_provider {
  chttp_service_impl *service;
  chttp_service_invocation *invocation;
  const chttp_server_request_view *request;
  chttp_service_scalar_reader scalar;

  cserde_writer writer;
  size_t writer_tokens;
  mem_buffer_t *output_buffer;

  int output_attempted;
  int output_active;
  int output_published;
  size_t staged_body_size;
  const char *staged_content_type;
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

static int chttp_service_scalar_kind(const cmeta_data_desc *data) {
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
    const char *source, size_t source_size, int plus_is_space,
    const char **out_text, size_t *out_size, DataBindError *error) {
  size_t src = 0u;
  size_t dst = 0u;
  char *buffer;

  if (provider == NULL || provider->service == NULL ||
      provider->invocation == NULL ||
      provider->invocation->scalar_scratch == NULL || source == NULL ||
      out_text == NULL || out_size == NULL)
    return DATA_BIND_ERR_INVALID_ARG;
  if (source_size > provider->service->scalar_capacity) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_LIMIT,
        "HTTP binding value exceeds configured bound");
    return DATA_BIND_ERR_LIMIT;
  }

  buffer = provider->invocation->scalar_scratch;
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
    chttp_service_http_provider *provider, const char *name,
    const char **out_text, size_t *out_size, DataBindError *error) {
  const char *query;
  const char *cursor;

  if (provider == NULL || provider->request == NULL || name == NULL ||
      out_text == NULL || out_size == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  query = strchr(provider->request->target, '?');
  if (query == NULL) {
    *out_text = NULL;
    *out_size = 0u;
    return DATA_BIND_OK;
  }

  cursor = query + 1u;
  while (*cursor != '\0' && *cursor != '#') {
    const char *pair_end = strchr(cursor, '&');
    const char *equals;
    size_t key_size;
    size_t value_size;
    if (pair_end == NULL) pair_end = cursor + strlen(cursor);
    equals = memchr(cursor, '=', (size_t)(pair_end - cursor));
    key_size = equals != NULL ? (size_t)(equals - cursor)
                              : (size_t)(pair_end - cursor);
    if (strlen(name) == key_size && memcmp(cursor, name, key_size) == 0) {
      const char *value = equals != NULL ? equals + 1u : pair_end;
      value_size = (size_t)(pair_end - value);
      return chttp_service_decode_component(
          provider, value, value_size, 1, out_text, out_size, error);
    }
    if (*pair_end == '\0') break;
    cursor = pair_end + 1u;
  }

  *out_text = NULL;
  *out_size = 0u;
  return DATA_BIND_OK;
}

static DataBindStatus chttp_service_cookie_value(
    chttp_service_http_provider *provider, const char *name,
    const char **out_text, size_t *out_size) {
  const char *cookie;
  const char *cursor;
  size_t name_size;

  if (provider == NULL || provider->request == NULL || name == NULL ||
      out_text == NULL || out_size == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  cookie = chttp_server_request_header(provider->request, "Cookie");
  if (cookie == NULL) {
    *out_text = NULL;
    *out_size = 0u;
    return DATA_BIND_OK;
  }

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
      while (key_end > cursor &&
             (key_end[-1] == ' ' || key_end[-1] == '\t'))
        --key_end;
      while (value < end && (*value == ' ' || *value == '\t')) ++value;
      if ((size_t)(key_end - cursor) == name_size &&
          memcmp(cursor, name, name_size) == 0) {
        const char *value_end = end;
        while (value_end > value &&
               (value_end[-1] == ' ' || value_end[-1] == '\t'))
          --value_end;
        *out_text = value;
        *out_size = (size_t)(value_end - value);
        return DATA_BIND_OK;
      }
    }
    if (*end == '\0') break;
    cursor = end + 1u;
  }

  *out_text = NULL;
  *out_size = 0u;
  return DATA_BIND_OK;
}

static DataBindStatus chttp_service_scalar_reader_init(
    chttp_service_http_provider *provider,
    const DataBindBindingPlanEntry *entry,
    const char *text, size_t text_size,
    cserde_reader *reader, DataBindError *error) {
  char *end = NULL;

  if (provider == NULL || provider->service == NULL ||
      provider->invocation == NULL ||
      provider->invocation->scalar_scratch == NULL || entry == NULL ||
      entry->data == NULL || text == NULL || reader == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

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
          error, DATA_BIND_ERR_PARSE, "Invalid boolean HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    break;

  case CMETA_DATA_SINT: {
    long long value;
    if (text_size > provider->service->scalar_capacity)
      return DATA_BIND_ERR_LIMIT;
    memmove(provider->invocation->scalar_scratch, text, text_size);
    provider->invocation->scalar_scratch[text_size] = '\0';
    value = strtoll(provider->invocation->scalar_scratch, &end, 10);
    if (errno != 0 || end == provider->invocation->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(
          error, DATA_BIND_ERR_PARSE, "Invalid signed HTTP binding value");
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
          error, DATA_BIND_ERR_PARSE, "Invalid unsigned HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    memmove(provider->invocation->scalar_scratch, text, text_size);
    provider->invocation->scalar_scratch[text_size] = '\0';
    value = strtoull(provider->invocation->scalar_scratch, &end, 10);
    if (errno != 0 || end == provider->invocation->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(
          error, DATA_BIND_ERR_PARSE, "Invalid unsigned HTTP binding value");
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
    memmove(provider->invocation->scalar_scratch, text, text_size);
    provider->invocation->scalar_scratch[text_size] = '\0';
    value = strtod(provider->invocation->scalar_scratch, &end);
    if (errno != 0 || end == provider->invocation->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(
          error, DATA_BIND_ERR_PARSE, "Invalid floating HTTP binding value");
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
        "Phase-1 HTTP scalar binding requires scalar/string/bytes/enum data");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  return cserde_reader_init(
             reader, &CHTTP_SERVICE_SCALAR_READER_OPS,
             &provider->scalar) == CSERDE_OK
             ? DATA_BIND_OK
             : DATA_BIND_ERR_RUNTIME;
}

static DataBindStatus chttp_service_http_open_input(
    void *context, const DataBindBindingPlanEntry *entry,
    cserde_reader *reader, DataBindBindingValueState *state,
    DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  const char *text = NULL;
  size_t text_size = 0u;
  DataBindStatus status;

  if (provider == NULL || entry == NULL || reader == NULL ||
      state == NULL || entry->address.space == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  *state = DATA_BIND_VALUE_STATE_ABSENT;

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
  } else if (strcmp(entry->address.space, "http.body") == 0) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Structured HTTP body binding requires shared DataBind FormatPlan support");
    return DATA_BIND_ERR_RUNTIME;
  } else {
    chttp_service_error_set(
        error, DATA_BIND_ERR_SCHEMA,
        "Unsupported generated HTTP BindingPlan address space");
    return DATA_BIND_ERR_SCHEMA;
  }

  status = chttp_service_scalar_reader_init(
      provider, entry, text, text_size, reader, error);
  if (status == DATA_BIND_OK) *state = DATA_BIND_VALUE_STATE_VALUE;
  return status;
}

static void chttp_service_http_release_output(
    chttp_service_http_provider *provider) {
  if (provider == NULL) return;
  if (provider->output_buffer != NULL) {
    mem_buffer_release(provider->output_buffer);
    provider->output_buffer = NULL;
  }
  provider->staged_body_size = 0u;
}

static cserde_status chttp_service_http_output_buffer(
    chttp_service_http_provider *provider, size_t required,
    char **out_data, size_t *out_capacity) {
  mem_buffer_t *buffer;
  if (provider == NULL || provider->service == NULL ||
      out_data == NULL || out_capacity == NULL)
    return CSERDE_INVALID_ARGUMENT;
  if (provider->output_buffer != NULL)
    return CSERDE_INVALID_STATE;
  if (required > provider->service->response_capacity)
    return CSERDE_LIMIT_EXCEEDED;
  if (required == 0u) {
    *out_data = NULL;
    *out_capacity = 0u;
    return CSERDE_OK;
  }
  buffer = mem_get_buffer(mem_global(), required);
  if (buffer == NULL) return CSERDE_SINK_ERROR;
  provider->output_buffer = buffer;
  *out_data = mem_buffer_data(buffer);
  *out_capacity = mem_buffer_capacity(buffer);
  if (*out_data == NULL || *out_capacity < required) {
    chttp_service_http_release_output(provider);
    return CSERDE_SINK_ERROR;
  }
  return CSERDE_OK;
}

static cserde_status chttp_service_scalar_write(
    void *context, const cserde_token *token) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  char *buffer = NULL;
  size_t capacity = 0u;
  size_t required = 0u;
  int written;
  cserde_status buffer_status;

  if (provider == NULL || provider->service == NULL || token == NULL)
    return CSERDE_INVALID_ARGUMENT;
  if (provider->writer_tokens != 0u) return CSERDE_INVALID_STATE;

  switch (token->kind) {
  case CSERDE_NULL:
    required = 4u;
    break;
  case CSERDE_BOOL:
    required = token->value.boolean ? 4u : 5u;
    break;
  case CSERDE_SINT:
  case CSERDE_UINT:
    required = provider->service->response_capacity < 32u
                   ? provider->service->response_capacity
                   : 32u;
    break;
  case CSERDE_FLOAT:
    required = provider->service->response_capacity < 64u
                   ? provider->service->response_capacity
                   : 64u;
    break;
  case CSERDE_STRING:
  case CSERDE_BYTES:
    required = token->value.slice.size;
    break;
  default:
    return CSERDE_UNSUPPORTED;
  }

  buffer_status = chttp_service_http_output_buffer(
      provider, required, &buffer, &capacity);
  if (buffer_status != CSERDE_OK) return buffer_status;

  switch (token->kind) {
  case CSERDE_NULL:
    memcpy(buffer, "null", 4u);
    provider->staged_body_size = 4u;
    provider->staged_content_type = "text/plain";
    break;
  case CSERDE_BOOL:
    if (token->value.boolean) {
      memcpy(buffer, "true", 4u);
      provider->staged_body_size = 4u;
    } else {
      memcpy(buffer, "false", 5u);
      provider->staged_body_size = 5u;
    }
    provider->staged_content_type = "text/plain";
    break;
  case CSERDE_SINT:
    written = snprintf(
        buffer, capacity, "%" PRId64, token->value.sint);
    if (written < 0 || (size_t)written >= capacity ||
        (size_t)written > provider->service->response_capacity)
      return CSERDE_LIMIT_EXCEEDED;
    provider->staged_body_size = (size_t)written;
    provider->staged_content_type = "text/plain";
    break;
  case CSERDE_UINT:
    written = snprintf(
        buffer, capacity, "%" PRIu64, token->value.uint);
    if (written < 0 || (size_t)written >= capacity ||
        (size_t)written > provider->service->response_capacity)
      return CSERDE_LIMIT_EXCEEDED;
    provider->staged_body_size = (size_t)written;
    provider->staged_content_type = "text/plain";
    break;
  case CSERDE_FLOAT:
    written = snprintf(
        buffer, capacity, "%.17g", token->value.floating);
    if (written < 0 || (size_t)written >= capacity ||
        (size_t)written > provider->service->response_capacity)
      return CSERDE_LIMIT_EXCEEDED;
    provider->staged_body_size = (size_t)written;
    provider->staged_content_type = "text/plain";
    break;
  case CSERDE_STRING:
    if (token->value.slice.size > capacity) return CSERDE_LIMIT_EXCEEDED;
    if (token->value.slice.size != 0u)
      memcpy(buffer, token->value.slice.data, token->value.slice.size);
    provider->staged_body_size = token->value.slice.size;
    provider->staged_content_type = "text/plain";
    break;
  case CSERDE_BYTES:
    if (token->value.slice.size > capacity) return CSERDE_LIMIT_EXCEEDED;
    if (token->value.slice.size != 0u)
      memcpy(buffer, token->value.slice.data, token->value.slice.size);
    provider->staged_body_size = token->value.slice.size;
    provider->staged_content_type = "application/octet-stream";
    break;
  default:
    return CSERDE_UNSUPPORTED;
  }

  provider->writer_tokens = 1u;
  if (provider->output_buffer != NULL)
    mem_set_used(provider->output_buffer, provider->staged_body_size);
  return CSERDE_OK;
}

static cserde_status chttp_service_scalar_finish(void *context) {
  (void)context;
  return CSERDE_OK;
}

static const cserde_writer_ops CHTTP_SERVICE_SCALAR_WRITER_OPS = {
    sizeof(cserde_writer_ops),
    CSERDE_WRITER_OPS_ABI_VERSION,
    chttp_service_scalar_write,
    chttp_service_scalar_finish};

static DataBindStatus chttp_service_http_begin_output(
    void *context, DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  (void)error;
  if (provider == NULL || provider->service == NULL)
    return DATA_BIND_ERR_INVALID_ARG;
  chttp_service_http_release_output(provider);
  provider->output_attempted = 1;
  provider->output_active = 1;
  provider->output_published = 0;
  provider->staged_body_size = 0u;
  provider->staged_content_type = "text/plain";
  provider->writer_tokens = 0u;
  provider->writer = (cserde_writer){0};
  return DATA_BIND_OK;
}

static DataBindStatus chttp_service_http_write_output(
    void *context, const DataBindBindingPlanEntry *entry,
    DataBindBindingValueState state, const void *value, size_t value_bytes,
    DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  DataBindNativeDiagnostic diagnostic =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  DataBindStatus status;

  if (provider == NULL || provider->service == NULL || entry == NULL ||
      !provider->output_active)
    return DATA_BIND_ERR_INVALID_ARG;
  if (provider->writer_tokens != 0u) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Phase-1 HTTP egress admits one scalar response value");
    return DATA_BIND_ERR_RUNTIME;
  }

  if (entry->address.binding_class != DATA_BIND_BINDING_ERROR) {
    if (entry->address.space == NULL ||
        strcmp(entry->address.space, "http.response.body") != 0) {
      chttp_service_error_set(
          error, DATA_BIND_ERR_RUNTIME,
          "Phase-1 HTTP egress does not support response headers/structured output");
      return DATA_BIND_ERR_RUNTIME;
    }
  }

  if (state != DATA_BIND_VALUE_STATE_NULL &&
      (state != DATA_BIND_VALUE_STATE_VALUE || value == NULL)) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "HTTP scalar egress requires VALUE or NULL state");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  if (cserde_writer_init(
          &provider->writer, &CHTTP_SERVICE_SCALAR_WRITER_OPS,
          provider) != CSERDE_OK) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Could not initialize HTTP scalar writer");
    return DATA_BIND_ERR_RUNTIME;
  }

  if (state == DATA_BIND_VALUE_STATE_NULL) {
    const cserde_token token = {.kind = CSERDE_NULL};
    const cserde_status write_status =
        cserde_writer_write(&provider->writer, &token);
    if (write_status != CSERDE_OK) {
      chttp_service_http_release_output(provider);
      chttp_service_error_set(
          error,
          write_status == CSERDE_LIMIT_EXCEEDED
              ? DATA_BIND_ERR_LIMIT
              : DATA_BIND_ERR_RUNTIME,
          "Could not encode HTTP null response");
      return write_status == CSERDE_LIMIT_EXCEEDED
                 ? DATA_BIND_ERR_LIMIT
                 : DATA_BIND_ERR_RUNTIME;
    }
    return DATA_BIND_OK;
  }

  if (provider->invocation == NULL)
    return DATA_BIND_ERR_INVALID_ARG;
  status = data_bind_native_encode(
      &provider->invocation->native_options, entry->data,
      value, value_bytes, &provider->writer, &diagnostic);
  if (status != DATA_BIND_OK) {
    chttp_service_http_release_output(provider);
    if (error != NULL) *error = diagnostic.error;
  }
  return status;
}

static DataBindStatus chttp_service_http_commit_output(
    void *context, DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  if (provider == NULL || !provider->output_active)
    return DATA_BIND_ERR_INVALID_ARG;
  if (provider->writer_tokens != 0u &&
      cserde_writer_finish(&provider->writer) != CSERDE_OK) {
    chttp_service_http_release_output(provider);
    provider->output_active = 0;
    provider->output_published = 0;
    chttp_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Could not finish HTTP response writer");
    return DATA_BIND_ERR_RUNTIME;
  }
  provider->output_active = 0;
  provider->output_published = 1;
  return DATA_BIND_OK;
}

static void chttp_service_http_abort_output(void *context) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  if (provider == NULL) return;
  chttp_service_http_release_output(provider);
  provider->output_active = 0;
  provider->output_published = 0;
  provider->writer_tokens = 0u;
  provider->writer = (cserde_writer){0};
}

static int chttp_service_method_from_text(
    const char *method, chttp_method *out) {
  if (method == NULL || out == NULL) return SALTS_EINVAL;
  if (strcmp(method, "GET") == 0) *out = CHTTP_METHOD_GET;
  else if (strcmp(method, "HEAD") == 0) *out = CHTTP_METHOD_HEAD;
  else if (strcmp(method, "POST") == 0) *out = CHTTP_METHOD_POST;
  else if (strcmp(method, "PUT") == 0) *out = CHTTP_METHOD_PUT;
  else if (strcmp(method, "DELETE") == 0) *out = CHTTP_METHOD_DELETE;
  else if (strcmp(method, "PATCH") == 0) *out = CHTTP_METHOD_PATCH;
  else if (strcmp(method, "OPTIONS") == 0) *out = CHTTP_METHOD_OPTIONS;
  else return SALTS_ENOTSUP;
  return SALTS_OK;
}

static int chttp_service_lower_route(
    const char *source, char **out_route) {
  size_t length;
  char *route;
  size_t src = 0u;
  size_t dst = 0u;

  if (source == NULL || out_route == NULL || source[0] != '/')
    return SALTS_EINVAL;
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
      if (end == length || end == src + 1u) {
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

static int chttp_service_native_type_valid(
    const DataBindNativeTypeBinding *binding, size_t *out_bytes) {
  if (binding == NULL || out_bytes == NULL ||
      binding->size < sizeof(*binding) ||
      binding->abi_version != DATA_BIND_NATIVE_BINDING_ABI_VERSION ||
      binding->data == NULL || !cmeta_data_desc_valid(binding->data) ||
      binding->data->storage_type == NULL ||
      binding->data->storage_type->size == 0u)
    return 0;
  *out_bytes = binding->data->storage_type->size;
  return 1;
}

static int chttp_service_add_size(
    size_t *total, size_t value, size_t limit) {
  if (total == NULL || value > SIZE_MAX - *total) return 0;
  *total += value;
  return *total <= limit;
}

static int chttp_service_function_semantics_admit(
    const cmeta_function_desc *function, size_t expected_params) {
  const cmeta_param_flags request_flags =
      (cmeta_param_flags)(CMETA_PARAM_IN | CMETA_PARAM_BORROWED);
  const cmeta_param_flags output_flags =
      (cmeta_param_flags)(CMETA_PARAM_OUT | CMETA_PARAM_BORROWED);

  if (!cmeta_function_desc_valid(function) ||
      function->result_flags != (cmeta_result_flags)CMETA_RESULT_VALUE ||
      function->param_count != expected_params ||
      function->params == NULL ||
      function->params[0].flags != request_flags ||
      function->params[1].flags != output_flags)
    return 0;

  if (expected_params == 3u)
    return function->params[2].flags == output_flags;
  return expected_params == 2u;
}

static int chttp_service_capability_admit(
    const DataBindHttpMethodPlan *method_plan,
    const DataBindServiceNativeBinding *native,
    const cmeta_function_desc *capability_function,
    const cmeta_function_abi_desc *capability_abi,
    int allow_typed_errors,
    size_t max_frame_bytes,
    size_t *request_bytes,
    size_t *response_bytes,
    size_t *error_bytes,
    size_t *param_count) {
  const DataBindBindingPlan *binding;
  const cmeta_function_desc *function;
  size_t expected_params;
  size_t total = 0u;
  size_t i;

  if (method_plan == NULL || native == NULL ||
      capability_function == NULL || capability_abi == NULL ||
      request_bytes == NULL || response_bytes == NULL ||
      error_bytes == NULL || param_count == NULL ||
      native->size < sizeof(*native) ||
      native->abi_version != DATA_BIND_BINDING_PLAN_ABI_VERSION ||
      !cmeta_function_desc_valid(native->function) ||
      !cmeta_function_desc_valid(capability_function) ||
      !cmeta_function_abi_desc_valid(capability_abi) ||
      !chttp_service_native_type_valid(native->request, request_bytes) ||
      !chttp_service_native_type_valid(native->response, response_bytes))
    return SALTS_EINVAL;

  binding = data_bind_http_method_plan_binding(method_plan);
  function = binding != NULL ? data_bind_binding_plan_function(binding) : NULL;
  if (function == NULL ||
      !cmeta_function_desc_equal(function, native->function) ||
      !cmeta_function_desc_equal(function, capability_function))
    return SALTS_EINVAL;

  if ((function->effects & CMETA_EFFECT_ASYNC) != 0u)
    return SALTS_ENOTSUP;

  if (data_bind_binding_plan_error_count(binding) != native->error_count)
    return SALTS_EINVAL;
  if (!allow_typed_errors && native->error_count != 0u)
    return SALTS_ENOTSUP;
  for (i = 0u; i < native->error_count; ++i) {
    const char *plan_error = data_bind_binding_plan_error_at(binding, i);
    if (plan_error == NULL || native->errors == NULL ||
        native->errors[i].idl_type_name == NULL ||
        strcmp(plan_error, native->errors[i].idl_type_name) != 0)
      return SALTS_EINVAL;
  }

  {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    size_t count = data_bind_binding_plan_ingress_count(binding);
    for (i = 0u; i < count; ++i) {
      size_t bytes;
      entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
          entry.data == NULL || entry.data->storage_type == NULL ||
          entry.function_param_index != 0u)
        return SALTS_ENOTSUP;
      bytes = entry.data->storage_type->size;
      if (entry.native_offset > *request_bytes ||
          bytes > *request_bytes - entry.native_offset)
        return SALTS_EINVAL;
    }
    count = data_bind_binding_plan_egress_count(binding);
    for (i = 0u; i < count; ++i) {
      size_t bytes;
      entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      if (!data_bind_binding_plan_egress_at(binding, i, &entry) ||
          entry.data == NULL || entry.data->storage_type == NULL ||
          entry.target_is_return || entry.function_param_index != 1u)
        return SALTS_ENOTSUP;
      bytes = entry.data->storage_type->size;
      if (entry.native_offset > *response_bytes ||
          bytes > *response_bytes - entry.native_offset)
        return SALTS_EINVAL;
    }
  }

  expected_params = native->error_count == 0u ? 2u : 3u;
  if (!chttp_service_function_semantics_admit(function, expected_params))
    return SALTS_ENOTSUP;
  if (capability_abi->param_count != expected_params ||
      capability_abi->return_carrier != CMETA_ABI_SCALAR ||
      cmeta_function_param_abi(capability_abi, 0u) !=
          CMETA_ABI_OBJECT_POINTER ||
      cmeta_function_param_abi(capability_abi, 1u) !=
          CMETA_ABI_OBJECT_POINTER)
    return SALTS_ENOTSUP;

  *error_bytes = 0u;
  if (native->error_count == 0u) {
    if (native->errors != NULL || native->error_param_index != SIZE_MAX ||
        native->error_envelope_bytes != 0u ||
        native->error_kind_bytes != 0u)
      return SALTS_EINVAL;
  } else {
    if (native->errors == NULL || native->error_param_index != 2u ||
        native->error_envelope_bytes == 0u ||
        native->error_kind_bytes != sizeof(uint32_t) ||
        native->error_kind_bytes > native->error_envelope_bytes ||
        native->error_kind_offset >
            native->error_envelope_bytes - native->error_kind_bytes ||
        cmeta_function_param_abi(capability_abi, 2u) !=
            CMETA_ABI_OBJECT_POINTER)
      return SALTS_ENOTSUP;
    for (i = 0u; i < native->error_count; ++i) {
      if (native->errors[i].size < sizeof(DataBindNativeErrorBinding) ||
          native->errors[i].idl_type_name == NULL ||
          native->errors[i].data_resolver == NULL ||
          native->errors[i].kind_value != (uint32_t)(i + 1u) ||
          native->errors[i].payload_offset >= native->error_envelope_bytes)
        return SALTS_EINVAL;
    }
    *error_bytes = native->error_envelope_bytes;
  }

  if (max_frame_bytes == 0u ||
      !chttp_service_add_size(&total, *request_bytes, max_frame_bytes) ||
      !chttp_service_add_size(&total, *response_bytes, max_frame_bytes) ||
      !chttp_service_add_size(&total, *error_bytes, max_frame_bytes))
    return SALTS_EMSGSIZE;

  *param_count = expected_params;
  return SALTS_OK;
}

static int chttp_service_direct_admit(
    const DataBindHttpMethodPlan *method_plan,
    const DataBindServiceNativeBinding *native,
    const DataBindNativeExecution *execution,
    size_t max_frame_bytes,
    size_t *request_bytes,
    size_t *response_bytes,
    size_t *error_bytes,
    size_t *param_count) {
  if (!data_bind_native_execution_valid(execution))
    return SALTS_EINVAL;
  return chttp_service_capability_admit(
      method_plan, native, execution->function, execution->abi, 1,
      max_frame_bytes, request_bytes, response_bytes, error_bytes,
      param_count);
}

static int chttp_service_cflow_admit(
    const DataBindHttpMethodPlan *method_plan,
    const DataBindServiceNativeBinding *native,
    const cflow_function_typed_adapter_projection *projection,
    size_t max_frame_bytes,
    size_t *request_bytes,
    size_t *response_bytes,
    size_t *error_bytes,
    size_t *param_count) {
  int status;
  if (!cflow_function_typed_adapter_projection_valid(projection) ||
      native == NULL || native->request == NULL || native->response == NULL ||
      native->request->data == NULL || native->response->data == NULL ||
      native->request->data->storage_type == NULL ||
      native->response->data->storage_type == NULL ||
      !cmeta_type_equal(
          projection->input_type, native->request->data->storage_type) ||
      !cmeta_type_equal(
          projection->output_type, native->response->data->storage_type))
    return SALTS_EINVAL;
  if (native->request->presence_count != 0u ||
      native->request->null_count != 0u ||
      native->response->presence_count != 0u ||
      native->response->null_count != 0u)
    return SALTS_ENOTSUP;

  status = chttp_service_capability_admit(
      method_plan, native, projection->function, projection->abi, 0,
      max_frame_bytes, request_bytes, response_bytes, error_bytes,
      param_count);
  if (status != SALTS_OK) return status;
  return *param_count == 2u && *error_bytes == 0u
             ? SALTS_OK
             : SALTS_ENOTSUP;
}

static int chttp_service_component_resolve(
    chttp_service_method_record *record,
    salts_component_plugin_runtime *runtime,
    const char *component_id) {
  salts_component_service service = {0};
  chttp_service_operation_provider provider =
      chttp_service_operation_provider_bind(NULL, NULL);
  chttp_service_component_operation operation =
      (chttp_service_component_operation)CHTTP_SERVICE_COMPONENT_OPERATION_INIT;

  if (record == NULL || runtime == NULL ||
      component_id == NULL || component_id[0] == '\0')
    return SALTS_EINVAL;

  if (salts_component_plugin_scope_acquire(
          runtime, &record->component_scope) !=
      SALTS_COMPONENT_PLUGIN_OK)
    return SALTS_EINVAL;

  if (salts_component_plugin_scope_find_service_from(
          &record->component_scope,
          component_id,
          chttp_service_operation_provider_interface(),
          &service) != SALTS_COMPONENT_PLUGIN_OK)
    return SALTS_EINVAL;

  if (chttp_service_operation_provider_borrow_from_object(
          service.object, service.interfaces, &provider) != CMETA_OK ||
      !chttp_service_operation_provider_valid(&provider) ||
      !chttp_service_operation_provider_get_operation(
          &provider, &operation) ||
      !chttp_service_component_operation_valid(&operation))
    return SALTS_EINVAL;

  record->component_operation = operation;
  record->native_binding = &record->component_operation.native;
  record->execution = &record->component_operation.execution;
  return SALTS_OK;
}

static void chttp_service_method_release(
    chttp_service_method_record *record) {
  if (record == NULL) return;
  cflow_plan_destroy(&record->cflow_plan);
  if (record->component_scope.live &&
      salts_component_plugin_scope_release(
          &record->component_scope) != SALTS_COMPONENT_PLUGIN_OK)
    abort();
  memset(record, 0, sizeof(*record));
}

static void chttp_service_invocation_release(
    chttp_service_invocation *invocation) {
  const DataBindServiceNativeBinding *native;
  if (invocation == NULL) return;
  native = invocation->record != NULL
               ? invocation->record->native_binding
               : NULL;
  if (invocation->frame_live && native != NULL) {
    if (invocation->error_storage != NULL) {
      DataBindError error = DATA_BIND_ERROR_INIT;
      (void)data_bind_service_native_error_restore_zero(
          native, invocation->error_storage,
          invocation->record != NULL
              ? invocation->record->error_bytes
              : 0u,
          &error);
    }
    if (native->response != NULL && native->response->data != NULL &&
        invocation->response_storage != NULL)
      (void)cmeta_data_value_restore_zero(
          native->response->data, invocation->response_storage);
    if (native->request != NULL && native->request->data != NULL &&
        invocation->request_storage != NULL)
      (void)cmeta_data_value_restore_zero(
          native->request->data, invocation->request_storage);
  }
  free(invocation->native_workspace);
  free(invocation->scalar_scratch);
  free(invocation->error_storage);
  free(invocation->response_storage);
  free(invocation->request_storage);
  memset(invocation, 0, sizeof(*invocation));
}

static void chttp_service_invocation_finalize(void *user) {
  chttp_service_invocation *invocation =
      (chttp_service_invocation *)user;
  chttp_service_method_record *record;
  int held;
  if (invocation == NULL) return;
  record = invocation->record;
  held = invocation->deferred_method_held;
  chttp_service_invocation_release(invocation);
  if (held && record != NULL)
    (void)atomic_fetch_sub_explicit(
        &record->deferred_in_flight, 1u, memory_order_release);
  free(invocation);
}

static int chttp_service_invocation_init(
    chttp_service_method_record *record,
    chttp_service_invocation *invocation) {
  chttp_service_impl *service;

  if (record == NULL || record->owner == NULL || invocation == NULL ||
      record->request_bytes == 0u || record->response_bytes == 0u ||
      record->param_count < 2u || record->param_count > 3u)
    return SALTS_EINVAL;

  service = record->owner;
  *invocation = (chttp_service_invocation){0};
  invocation->record = record;
  invocation->request_storage =
      (unsigned char *)calloc(1u, record->request_bytes);
  invocation->response_storage =
      (unsigned char *)calloc(1u, record->response_bytes);
  if (record->error_bytes != 0u)
    invocation->error_storage =
        (unsigned char *)calloc(1u, record->error_bytes);
  invocation->scalar_scratch =
      (char *)malloc(service->scalar_capacity + 1u);
  invocation->native_workspace =
      (unsigned char *)malloc(service->native_workspace_bytes);

  if (invocation->request_storage == NULL ||
      invocation->response_storage == NULL ||
      (record->error_bytes != 0u && invocation->error_storage == NULL) ||
      invocation->scalar_scratch == NULL ||
      invocation->native_workspace == NULL) {
    chttp_service_invocation_release(invocation);
    return SALTS_ENOMEM;
  }

  invocation->params[0] = invocation->request_storage;
  invocation->params[1] = invocation->response_storage;
  invocation->param_bytes[0] = record->request_bytes;
  invocation->param_bytes[1] = record->response_bytes;
  if (record->param_count == 3u) {
    invocation->params[2] = invocation->error_storage;
    invocation->param_bytes[2] = record->error_bytes;
  }

  invocation->frame =
      (DataBindBindingCallFrame)DATA_BIND_BINDING_CALL_FRAME_INIT;
  invocation->frame.request = invocation->request_storage;
  invocation->frame.request_bytes = record->request_bytes;
  invocation->frame.params = invocation->params;
  invocation->frame.param_bytes = invocation->param_bytes;
  invocation->frame.param_count = record->param_count;

  invocation->native_options = service->native_options;
  invocation->native_options.workspace = invocation->native_workspace;
  invocation->native_options.workspace_bytes = service->native_workspace_bytes;
  return SALTS_OK;
}

static int chttp_service_plan_supported(
    const DataBindHttpMethodPlan *plan) {
  const DataBindBindingPlan *binding;
  size_t i;
  size_t egress_count;
  DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;

  if (plan == NULL ||
      data_bind_http_method_plan_context_flags(plan) !=
          DATA_BIND_HTTP_CONTEXT_NONE)
    return 0;
  binding = data_bind_http_method_plan_binding(plan);
  if (binding == NULL) return 0;

  for (i = 0u; i < data_bind_binding_plan_ingress_count(binding); ++i) {
    entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
        entry.address.space == NULL || !chttp_service_scalar_kind(entry.data))
      return 0;
    if (strcmp(entry.address.space, "http.path") != 0 &&
        strcmp(entry.address.space, "http.query") != 0 &&
        strcmp(entry.address.space, "http.header") != 0 &&
        strcmp(entry.address.space, "http.cookie") != 0)
      return 0;
  }

  egress_count = data_bind_binding_plan_egress_count(binding);
  if (egress_count > 1u) return 0;
  if (egress_count == 1u) {
    entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_egress_at(binding, 0u, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "http.response.body") != 0 ||
        !chttp_service_scalar_kind(entry.data))
      return 0;
  }
  return 1;
}

typedef struct chttp_service_http_failure_response {
  unsigned int status_code;
  const char *body;
  size_t body_size;
} chttp_service_http_failure_response;

static chttp_service_http_failure_response
chttp_service_http_ingress_failure(DataBindStatus status) {
  switch (status) {
  case DATA_BIND_ERR_INVALID_ARG:
  case DATA_BIND_ERR_PARSE:
  case DATA_BIND_ERR_TYPE_NOT_FOUND:
  case DATA_BIND_ERR_TYPE_MISMATCH:
    return (chttp_service_http_failure_response){
        400u, "Binding Error", sizeof("Binding Error") - 1u};

  case DATA_BIND_ERR_VALIDATION:
    return (chttp_service_http_failure_response){
        422u, "Validation Error", sizeof("Validation Error") - 1u};

  case DATA_BIND_ERR_LIMIT:
    return (chttp_service_http_failure_response){
        413u, "Binding Limit Error", sizeof("Binding Limit Error") - 1u};

  default:
    return (chttp_service_http_failure_response){
        500u, "Internal Server Error",
        sizeof("Internal Server Error") - 1u};
  }
}

static int chttp_service_http_failure_reply(
    chttp_server_response *response,
    chttp_service_http_failure_response failure) {
  return chttp_server_reply(
      response, failure.status_code, "text/plain",
      failure.body, failure.body_size);
}

static void chttp_service_deferred_cancel_task(void *user) {
  chttp_service_invocation *invocation =
      (chttp_service_invocation *)user;
  if (invocation == NULL || invocation->deferred.impl == NULL)
    return;
  (void)chttp_server_deferred_cancel(&invocation->deferred);
}

static int chttp_service_deferred_text_reply(
    chttp_service_invocation *invocation,
    unsigned int status_code,
    const char *body) {
  chttp_server_deferred_response reply;
  int status;
  if (invocation == NULL || invocation->deferred.impl == NULL ||
      body == NULL)
    return SALTS_EINVAL;
  reply = (chttp_server_deferred_response){
      sizeof(chttp_server_deferred_response),
      status_code,
      "text/plain",
      NULL,
      0u,
      body,
      strlen(body)};
  status = chttp_server_deferred_reply(&invocation->deferred, &reply);
  if (status != SALTS_OK && invocation->deferred.impl != NULL)
    (void)chttp_server_deferred_cancel(&invocation->deferred);
  return status;
}

static void chttp_service_deferred_publish(
    chttp_service_invocation *invocation,
    const DataBindBindingCallFrame *frame,
    int native_status) {
  chttp_service_method_record *record;
  chttp_service_http_provider provider_context;
  DataBindBindingProvider provider = DATA_BIND_BINDING_PROVIDER_INIT;
  DataBindBindingOutcome outcome = DATA_BIND_BINDING_OUTCOME_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  DataBindStatus bind_status;
  int http_status = 500;
  int terminal_status;

  if (invocation == NULL || invocation->record == NULL || frame == NULL)
    return;
  record = invocation->record;

  provider_context = (chttp_service_http_provider){
      .service = record->owner,
      .invocation = invocation,
      .request = NULL,
      .staged_content_type = "text/plain"};
  provider.context = &provider_context;
  provider.begin_output = chttp_service_http_begin_output;
  provider.write_output = chttp_service_http_write_output;
  provider.commit_output = chttp_service_http_commit_output;
  provider.abort_output = chttp_service_http_abort_output;

  bind_status = data_bind_binding_plan_write_outcome(
      record->binding, &provider, frame, native_status,
      &outcome, &diagnostic);
  if (bind_status != DATA_BIND_OK) {
    (void)chttp_service_deferred_text_reply(
        invocation, 500u, "Internal Server Error");
    chttp_service_http_release_output(&provider_context);
    return;
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_NATIVE_STATUS) {
    (void)chttp_service_deferred_text_reply(
        invocation, 500u, "Application Error");
    chttp_service_http_release_output(&provider_context);
    return;
  }

  if (!data_bind_http_method_plan_status_for_outcome(
          record->method_plan, &outcome, &http_status)) {
    (void)chttp_service_deferred_text_reply(
        invocation, 500u, "Internal Server Error");
    chttp_service_http_release_output(&provider_context);
    return;
  }

  terminal_status = chttp_server_deferred_reply_buffer(
      &invocation->deferred,
      (unsigned int)http_status,
      provider_context.output_published
          ? provider_context.staged_content_type
          : "text/plain",
      NULL,
      0u,
      provider_context.output_published
          ? provider_context.output_buffer
          : NULL);
  if (terminal_status != SALTS_OK &&
      invocation->deferred.impl != NULL)
    (void)chttp_server_deferred_cancel(&invocation->deferred);
  chttp_service_http_release_output(&provider_context);
}

static void chttp_service_deferred_direct_run(void *user) {
  chttp_service_invocation *invocation =
      (chttp_service_invocation *)user;
  chttp_service_method_record *record;
  int native_status = 0;

  if (invocation == NULL || invocation->record == NULL)
    return;
  record = invocation->record;
  if (record->execution == NULL ||
      !record->execution->invoke(
          record->execution->context, &native_status,
          invocation->params, record->param_count)) {
    (void)chttp_service_deferred_text_reply(
        invocation, 500u, "Internal Server Error");
    return;
  }
  chttp_service_deferred_publish(
      invocation, &invocation->frame, native_status);
}

static void chttp_service_deferred_cflow_run(void *user) {
  chttp_service_invocation *invocation =
      (chttp_service_invocation *)user;
  chttp_service_method_record *record;
  cflow_result result = {0};
  DataBindBindingCallFrame frame;
  void *params[3];
  size_t param_bytes[3];

  if (invocation == NULL || invocation->record == NULL)
    return;
  record = invocation->record;

  if (!cflow_plan_eval_array(
          &record->cflow_plan, invocation->request_storage, 1u, &result) ||
      result.count != 1u || result.data == NULL ||
      !cmeta_type_equal(result.type, record->cflow_plan.output_type)) {
    cflow_result_destroy(&result);
    (void)chttp_service_deferred_text_reply(
        invocation, 500u, "Internal Server Error");
    return;
  }

  memcpy(params, invocation->params, sizeof(params));
  memcpy(param_bytes, invocation->param_bytes, sizeof(param_bytes));
  params[1] = result.data;
  param_bytes[1] = record->cflow_plan.output_type->size;
  frame = invocation->frame;
  frame.params = params;
  frame.param_bytes = param_bytes;

  chttp_service_deferred_publish(invocation, &frame, 0);
  cflow_result_destroy(&result);
}

static int chttp_service_http_execute_deferred(
    chttp_service_method_record *record,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_service_invocation *invocation;
  chttp_service_http_provider provider_context;
  DataBindBindingProvider provider = DATA_BIND_BINDING_PROVIDER_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  cflow_executor_task task;
  cflow_admission_status admission;
  DataBindStatus bind_status;
  int status;

  if (record == NULL || record->owner == NULL ||
      record->method_plan == NULL || record->binding == NULL ||
      record->native_binding == NULL || record->executor == NULL ||
      ((record->execution_mode ==
            CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT ||
        record->execution_mode ==
            CHTTP_SERVICE_EXECUTION_DEFERRED_COMPONENT) &&
       record->execution == NULL) ||
      (record->execution_mode ==
           CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW &&
       record->cflow_plan.impl == NULL) ||
      request == NULL || response == NULL)
    return SALTS_EINVAL;

  invocation =
      (chttp_service_invocation *)calloc(1u, sizeof(*invocation));
  if (invocation == NULL) return SALTS_ENOMEM;
  status = chttp_service_invocation_init(record, invocation);
  if (status != SALTS_OK) {
    free(invocation);
    return status;
  }

  provider_context = (chttp_service_http_provider){
      .service = record->owner,
      .invocation = invocation,
      .request = request,
      .staged_content_type = "text/plain"};
  provider.context = &provider_context;
  provider.open_input = chttp_service_http_open_input;

  bind_status = data_bind_binding_plan_bind_inputs(
      record->binding, &provider, &invocation->native_options,
      &invocation->frame, &diagnostic);
  if (bind_status == DATA_BIND_OK)
    invocation->frame_live = 1;
  if (bind_status != DATA_BIND_OK) {
    const chttp_service_http_failure_response failure =
        chttp_service_http_ingress_failure(bind_status);
    status = chttp_service_http_failure_reply(response, failure);
    chttp_service_invocation_finalize(invocation);
    return status;
  }

  status = chttp_server_response_defer(
      response, &invocation->deferred);
  if (status != SALTS_OK) {
    chttp_service_invocation_finalize(invocation);
    return status;
  }

  /*
   * From this point the deferred task may outlive the HTTP callback. Hold the
   * method/mount domain lifetime explicitly until task.finalize, which runs
   * canonical native-value teardown before dropping this count.
   */
  (void)atomic_fetch_add_explicit(
      &record->deferred_in_flight, 1u, memory_order_acq_rel);
  invocation->deferred_method_held = 1;
  task = (cflow_executor_task){
      .run = record->execution_mode ==
                     CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW
                 ? chttp_service_deferred_cflow_run
                 : chttp_service_deferred_direct_run,
      .cancel = chttp_service_deferred_cancel_task,
      .finalize = chttp_service_invocation_finalize,
      .user = invocation};
  admission = cflow_executor_try_post_task(record->executor, &task);
  if (admission == CFLOW_ADMISSION_ACCEPTED)
    return SALTS_OK;

  /*
   * Executor admission is deliberately non-blocking. The response has already
   * been generation-safely deferred, so fail it explicitly rather than block
   * the HTTP owner thread waiting for worker capacity.
   */
  (void)chttp_service_deferred_text_reply(
      invocation, 503u, "Service Unavailable");
  chttp_service_invocation_finalize(invocation);
  return SALTS_OK;
}

static int chttp_service_http_execute(
    chttp_service_method_record *record,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_service_invocation invocation = {0};
  chttp_service_http_provider provider_context;
  DataBindBindingProvider provider = DATA_BIND_BINDING_PROVIDER_INIT;
  DataBindBindingOutcome outcome = DATA_BIND_BINDING_OUTCOME_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  const DataBindBindingPlan *binding;
  DataBindStatus bind_status;
  int native_status = 0;
  int http_status = 500;
  int result;

  if (record == NULL || record->owner == NULL ||
      record->method_plan == NULL || record->binding == NULL ||
      record->native_binding == NULL || record->execution == NULL ||
      request == NULL || response == NULL)
    return SALTS_EINVAL;

  result = chttp_service_invocation_init(record, &invocation);
  if (result != SALTS_OK) return result;

  provider_context = (chttp_service_http_provider){
      .service = record->owner,
      .invocation = &invocation,
      .request = request,
      .staged_content_type = "text/plain"};

  provider.context = &provider_context;
  provider.open_input = chttp_service_http_open_input;
  provider.begin_output = chttp_service_http_begin_output;
  provider.write_output = chttp_service_http_write_output;
  provider.commit_output = chttp_service_http_commit_output;
  provider.abort_output = chttp_service_http_abort_output;

  binding = record->binding;
  bind_status = data_bind_binding_plan_bind_inputs(
      binding, &provider, &invocation.native_options,
      &invocation.frame, &diagnostic);
  if (bind_status == DATA_BIND_OK)
    invocation.frame_live = 1;

  if (bind_status != DATA_BIND_OK) {
    const chttp_service_http_failure_response failure =
        chttp_service_http_ingress_failure(bind_status);
    result = chttp_service_http_failure_reply(response, failure);
    goto cleanup;
  }

  if (!record->execution->invoke(
          record->execution->context, &native_status,
          invocation.params, record->param_count))
    bind_status = DATA_BIND_ERR_RUNTIME;

  if (bind_status == DATA_BIND_OK)
    bind_status = data_bind_binding_plan_write_outcome(
        binding, &provider, &invocation.frame, native_status,
        &outcome, &diagnostic);

  if (bind_status != DATA_BIND_OK) {
    result = chttp_server_reply(
        response, 500u, "text/plain",
        "Internal Server Error",
        sizeof("Internal Server Error") - 1u);
    goto cleanup;
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_NATIVE_STATUS) {
    result = chttp_server_reply(
        response, 500u, "text/plain",
        "Application Error",
        sizeof("Application Error") - 1u);
    goto cleanup;
  }

  if (!data_bind_http_method_plan_status_for_outcome(
          record->method_plan, &outcome, &http_status)) {
    result = chttp_server_reply(
        response, 500u, "text/plain", "Internal Server Error", 21u);
    goto cleanup;
  }

  if (!provider_context.output_published) {
    result = chttp_server_reply(
        response, (unsigned int)http_status, "text/plain", NULL, 0u);
    goto cleanup;
  }

  result = chttp_server_reply_buffer(
      response, (unsigned int)http_status,
      provider_context.staged_content_type,
      provider_context.output_buffer);

cleanup:
  chttp_service_http_release_output(&provider_context);
  chttp_service_invocation_release(&invocation);
  return result;
}

static int chttp_service_http_handler(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_service_method_record *record =
      (chttp_service_method_record *)user;
  if (record == NULL || record->owner == NULL)
    return SALTS_EINVAL;
  if (record->execution_mode ==
          CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT ||
      record->execution_mode ==
          CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW ||
      record->execution_mode ==
          CHTTP_SERVICE_EXECUTION_DEFERRED_COMPONENT)
    return chttp_service_http_execute_deferred(
        record, request, response);
  return chttp_service_http_execute(record, request, response);
}

int chttp_service_init(
    chttp_service *service, const chttp_service_config *config) {
  chttp_service_impl *impl;

  if (service == NULL || config == NULL ||
      config->size < sizeof(*config) ||
      config->method_capacity == 0u ||
      config->max_binding_value_bytes == 0u ||
      config->max_response_body_bytes == 0u ||
      config->max_call_frame_bytes == 0u ||
      config->native_workspace_bytes == 0u ||
      config->native_max_depth == 0u ||
      config->native_max_items == 0u)
    return SALTS_EINVAL;
  if (service->impl != NULL) return SALTS_EALREADY;
  if (config->max_binding_value_bytes == SIZE_MAX)
    return SALTS_ERANGE;

  impl = (chttp_service_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->methods = (chttp_service_method_record *)calloc(
      config->method_capacity, sizeof(*impl->methods));
  if (impl->methods == NULL) {
    free(impl);
    return SALTS_ENOMEM;
  }

  impl->method_capacity = config->method_capacity;
  impl->scalar_capacity = config->max_binding_value_bytes;
  impl->native_workspace_bytes = config->native_workspace_bytes;
  impl->response_capacity = config->max_response_body_bytes;
  impl->max_call_frame_bytes = config->max_call_frame_bytes;
  impl->native_options =
      (DataBindNativeOptions)DATA_BIND_NATIVE_OPTIONS_INIT;
  impl->native_options.workspace = NULL;
  impl->native_options.workspace_bytes = config->native_workspace_bytes;
  impl->native_options.max_depth = config->native_max_depth;
  impl->native_options.max_items = config->native_max_items;
  impl->native_options.max_owned_bytes = config->native_max_owned_bytes;

  service->impl = impl;
  return SALTS_OK;
}

int chttp_service_mount_http(
    chttp_service *service, chttp_server *server,
    const chttp_service_http_mount *mount) {
  chttp_service_impl *impl;
  chttp_service_method_record *record;
  chttp_server_route_options route_options = {0};
  chttp_method method;
  const char *method_text;
  const char *route_text;
  const DataBindServiceNativeBinding *native = NULL;
  const DataBindNativeExecution *execution = NULL;
  char *route = NULL;
  size_t request_bytes = 0u;
  size_t response_bytes = 0u;
  size_t error_bytes = 0u;
  size_t param_count = 0u;
  const DataBindBindingPlan *binding;
  int status;
  int component_mode;

  if (service == NULL || service->impl == NULL || server == NULL ||
      mount == NULL || mount->size < sizeof(*mount) ||
      mount->method_plan == NULL)
    return SALTS_EINVAL;

  component_mode =
      mount->execution_mode == CHTTP_SERVICE_EXECUTION_DEFERRED_COMPONENT;
  if (mount->execution_mode != CHTTP_SERVICE_EXECUTION_INLINE_DIRECT &&
      mount->execution_mode != CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT &&
      mount->execution_mode != CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW &&
      !component_mode)
    return SALTS_EINVAL;

  if (component_mode) {
    if (mount->native_binding != NULL || mount->execution != NULL ||
        mount->cflow_projection != NULL ||
        mount->executor == NULL ||
        !cflow_executor_valid(mount->executor) ||
        mount->component_runtime == NULL ||
        mount->component_id == NULL ||
        mount->component_id[0] == '\0')
      return SALTS_EINVAL;
  } else {
    if (mount->component_runtime != NULL ||
        mount->component_id != NULL ||
        mount->native_binding == NULL)
      return SALTS_EINVAL;
    if (mount->execution_mode == CHTTP_SERVICE_EXECUTION_INLINE_DIRECT &&
        (mount->execution == NULL || mount->executor != NULL ||
         mount->cflow_projection != NULL))
      return SALTS_EINVAL;
    if (mount->execution_mode == CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT &&
        (mount->execution == NULL || mount->executor == NULL ||
         !cflow_executor_valid(mount->executor) ||
         mount->cflow_projection != NULL))
      return SALTS_EINVAL;
    if (mount->execution_mode == CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW &&
        (mount->execution != NULL || mount->executor == NULL ||
         !cflow_executor_valid(mount->executor) ||
         mount->cflow_projection == NULL))
      return SALTS_EINVAL;
  }

  if (!chttp_service_plan_supported(mount->method_plan))
    return SALTS_ENOTSUP;

  impl = (chttp_service_impl *)service->impl;
  if (impl->method_count == impl->method_capacity)
    return SALTS_ENOBUFS;

  record = &impl->methods[impl->method_count];
  memset(record, 0, sizeof(*record));
  atomic_init(&record->deferred_in_flight, 0u);
  record->owner = impl;
  record->method_plan = mount->method_plan;
  record->execution_mode = mount->execution_mode;
  record->executor = mount->executor;

  if (component_mode) {
    status = chttp_service_component_resolve(
        record, mount->component_runtime, mount->component_id);
    if (status != SALTS_OK) {
      chttp_service_method_release(record);
      return status;
    }
    native = record->native_binding;
    execution = record->execution;
  } else {
    native = mount->native_binding;
    execution = mount->execution;
    record->native_binding = native;
    record->execution = execution;
  }

  if (mount->execution_mode == CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW)
    status = chttp_service_cflow_admit(
        mount->method_plan, native, mount->cflow_projection,
        impl->max_call_frame_bytes,
        &request_bytes, &response_bytes, &error_bytes, &param_count);
  else
    status = chttp_service_direct_admit(
        mount->method_plan, native, execution,
        impl->max_call_frame_bytes,
        &request_bytes, &response_bytes, &error_bytes, &param_count);
  if (status != SALTS_OK) {
    chttp_service_method_release(record);
    return status;
  }

  binding = data_bind_http_method_plan_binding(mount->method_plan);
  if (binding == NULL) {
    chttp_service_method_release(record);
    return SALTS_EINVAL;
  }
  record->binding = binding;
  record->request_bytes = request_bytes;
  record->response_bytes = response_bytes;
  record->error_bytes = error_bytes;
  record->param_count = param_count;

  method_text = data_bind_http_method_plan_method(mount->method_plan);
  route_text = data_bind_http_method_plan_route(mount->method_plan);
  status = chttp_service_method_from_text(method_text, &method);
  if (status != SALTS_OK) {
    chttp_service_method_release(record);
    return status;
  }
  status = chttp_service_lower_route(route_text, &route);
  if (status != SALTS_OK) {
    chttp_service_method_release(record);
    return status;
  }

  if (mount->execution_mode == CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW) {
    cflow_graph graph = {0};
    cflow_graph_init(&graph, mount->cflow_projection->input_type);
    if (!cflow_graph_add_function_typed_adapter_projection(
            &graph, mount->cflow_projection) ||
        !cflow_plan_compile_surface(&record->cflow_plan, &graph, NULL)) {
      cflow_graph_destroy(&graph);
      chttp_service_method_release(record);
      free(route);
      return SALTS_ENOTSUP;
    }
    cflow_graph_destroy(&graph);
  }

  route_options.method = method;
  route_options.path = route;
  route_options.middleware = mount->middleware;
  route_options.middleware_count = mount->middleware_count;
  route_options.handler = chttp_service_http_handler;
  route_options.user = record;

  status = chttp_server_route_with(server, &route_options);
  free(route);
  if (status != SALTS_OK) {
    chttp_service_method_release(record);
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

  /*
   * Destruction is all-or-nothing. A stopped/destroyed Server prevents new
   * route admission; any accepted deferred task must finish/cancel and run its
   * finalizer before method storage, cached borrowed descriptors/execution, a
   * CFlow Plan, or a mount-owned Component generation scope may be released.
   */
  for (i = 0u; i < impl->method_count; ++i) {
    if (atomic_load_explicit(
            &impl->methods[i].deferred_in_flight,
            memory_order_acquire) != 0u)
      return SALTS_EBUSY;
  }

  for (i = 0u; i < impl->method_count; ++i)
    chttp_service_method_release(&impl->methods[i]);
  free(impl->methods);
  free(impl);
  service->impl = NULL;
  return SALTS_OK;
}
