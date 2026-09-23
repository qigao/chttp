#include <chttp_service/service.h>

#include <data_bind_format_provider.h>
#include <data_bind_json_provider.h>

#include <salts/error_codes.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_service_impl chttp_service_impl;

typedef struct chttp_service_method_record {
  chttp_service_impl *owner;
  DataBindBindingPlan *plan;
  char *route;
  chttp_method method;
  chttp_service_executor_fn execute;
  void *user;
} chttp_service_method_record;

struct chttp_service_impl {
  DataBind *contract;
  chttp_service_method_record *methods;
  size_t method_capacity;
  size_t method_count;
  char *scalar_scratch;
  size_t scalar_capacity;
  unsigned char *native_workspace;
  DataBindNativeOptions native_options;
  size_t max_json_depth;
};

typedef struct chttp_service_scalar_reader {
  cserde_token token;
  int emitted;
} chttp_service_scalar_reader;

typedef struct chttp_service_http_provider {
  chttp_service_impl *service;
  const chttp_server_request_view *request;
  chttp_service_scalar_reader scalar;
  DataBindFormatReader body_reader;
  int body_reader_open;
} chttp_service_http_provider;

static void chttp_service_error_set(DataBindError *error,
                                    DataBindStatus status,
                                    const char *message) {
  if (error == NULL) return;
  if (error->size == 0u) error->size = sizeof(*error);
  error->code = status;
  error->line = -1;
  error->column = -1;
  error->path[0] = '\0';
  snprintf(error->message, sizeof(error->message), "%s",
           message != NULL ? message : "");
}

static cserde_status chttp_service_scalar_next(void *context,
                                                cserde_token *out) {
  chttp_service_scalar_reader *reader =
      (chttp_service_scalar_reader *)context;
  if (reader == NULL || out == NULL) return CSERDE_INVALID_ARGUMENT;
  if (reader->emitted) return CSERDE_DONE;
  *out = reader->token;
  reader->emitted = 1;
  return CSERDE_OK;
}

static const cserde_reader_ops CHTTP_SERVICE_SCALAR_OPS = {
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

  if (provider == NULL || source == NULL || out_text == NULL ||
      out_size == NULL)
    return DATA_BIND_ERR_INVALID_ARG;
  if (source_size > provider->service->scalar_capacity) {
    chttp_service_error_set(error, DATA_BIND_ERR_LIMIT,
                            "HTTP binding value exceeds configured bound");
    return DATA_BIND_ERR_LIMIT;
  }

  buffer = provider->service->scalar_scratch;
  while (src < source_size) {
    unsigned char value = (unsigned char)source[src++];
    if (value == (unsigned char)'%' && src + 1u < source_size) {
      int high = chttp_service_hex((unsigned char)source[src]);
      int low = chttp_service_hex((unsigned char)source[src + 1u]);
      if (high < 0 || low < 0) {
        chttp_service_error_set(error, DATA_BIND_ERR_PARSE,
                                "Malformed HTTP percent escape");
        return DATA_BIND_ERR_PARSE;
      }
      value = (unsigned char)((high << 4) | low);
      src += 2u;
    } else if (value == (unsigned char)'%' ) {
      chttp_service_error_set(error, DATA_BIND_ERR_PARSE,
                              "Truncated HTTP percent escape");
      return DATA_BIND_ERR_PARSE;
    } else if (plus_is_space && value == (unsigned char)'+') {
      value = (unsigned char)' ';
    }
    if (dst == provider->service->scalar_capacity) {
      chttp_service_error_set(error, DATA_BIND_ERR_LIMIT,
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
    key_size =
        equals != NULL ? (size_t)(equals - cursor)
                       : (size_t)(pair_end - cursor);
    if (strlen(name) == key_size && memcmp(cursor, name, key_size) == 0) {
      const char *value =
          equals != NULL ? equals + 1u : pair_end;
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
    const char *text,
    size_t text_size,
    cserde_reader *reader,
    DataBindError *error) {
  char *end = NULL;

  if (provider == NULL || entry == NULL || entry->data == NULL ||
      text == NULL || reader == NULL)
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
      chttp_service_error_set(error, DATA_BIND_ERR_PARSE,
                              "Invalid boolean HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    break;

  case CMETA_DATA_SINT: {
    long long value;
    if (text_size > provider->service->scalar_capacity)
      return DATA_BIND_ERR_LIMIT;
    memcpy(provider->service->scalar_scratch, text, text_size);
    provider->service->scalar_scratch[text_size] = '\0';
    value = strtoll(provider->service->scalar_scratch, &end, 10);
    if (errno != 0 || end == provider->service->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(error, DATA_BIND_ERR_PARSE,
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
      chttp_service_error_set(error, DATA_BIND_ERR_PARSE,
                              "Invalid unsigned HTTP binding value");
      return DATA_BIND_ERR_PARSE;
    }
    memcpy(provider->service->scalar_scratch, text, text_size);
    provider->service->scalar_scratch[text_size] = '\0';
    value = strtoull(provider->service->scalar_scratch, &end, 10);
    if (errno != 0 || end == provider->service->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(error, DATA_BIND_ERR_PARSE,
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
    memcpy(provider->service->scalar_scratch, text, text_size);
    provider->service->scalar_scratch[text_size] = '\0';
    value = strtod(provider->service->scalar_scratch, &end);
    if (errno != 0 || end == provider->service->scalar_scratch ||
        end == NULL || *end != '\0') {
      chttp_service_error_set(error, DATA_BIND_ERR_PARSE,
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
        "HTTP scalar binding requires scalar/string/bytes/enum native data");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  return cserde_reader_init(
             reader, &CHTTP_SERVICE_SCALAR_OPS, &provider->scalar) == CSERDE_OK
             ? DATA_BIND_OK
             : DATA_BIND_ERR_RUNTIME;
}

static DataBindStatus chttp_service_http_open_input(
    void *context,
    const DataBindBindingPlanEntry *entry,
    cserde_reader *reader,
    int *present,
    DataBindError *error) {
  chttp_service_http_provider *provider =
      (chttp_service_http_provider *)context;
  const char *text = NULL;
  size_t text_size = 0u;
  DataBindStatus status;

  if (provider == NULL || entry == NULL || reader == NULL ||
      present == NULL || entry->address.space == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  *present = 0;

  if (strcmp(entry->address.space, "http.body") == 0) {
    if (provider->request->body_streamed) {
      chttp_service_error_set(
          error, DATA_BIND_ERR_RUNTIME,
          "Typed Service body binding does not accept streamed request bodies");
      return DATA_BIND_ERR_RUNTIME;
    }
    if (provider->request->body == NULL || provider->request->body_size == 0u)
      return DATA_BIND_OK;
    if (provider->body_reader_open) {
      chttp_service_error_set(error, DATA_BIND_ERR_RUNTIME,
                              "HTTP body binding opened more than once");
      return DATA_BIND_ERR_RUNTIME;
    }

    status = data_bind_format_reader_open(
        data_bind_json_format_provider(),
        (const char *)provider->request->body,
        provider->request->body_size,
        provider->service->max_json_depth,
        &provider->body_reader,
        error);
    if (status != DATA_BIND_OK) return status;
    provider->body_reader_open = 1;
    *reader = *provider->body_reader.reader;
    *present = 1;
    return DATA_BIND_OK;
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
    if (status != DATA_BIND_OK) return status;
    if (text == NULL) return DATA_BIND_OK;
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
    chttp_service_error_set(error, DATA_BIND_ERR_SCHEMA,
                            "Unsupported HTTP BindingPlan address space");
    return DATA_BIND_ERR_SCHEMA;
  }

  status = chttp_service_scalar_reader_init(
      provider, entry, text, text_size, reader, error);
  if (status == DATA_BIND_OK) *present = 1;
  return status;
}

static DataBindStatus chttp_service_http_project(
    void *context,
    const DataBindServiceOperation *operation,
    const DataBindSchemaField *field,
    DataBindBindingDirection direction,
    DataBindBindingAddress *out,
    DataBindError *error) {
  (void)context;
  (void)operation;

  if (field == NULL || out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  *out = (DataBindBindingAddress)DATA_BIND_BINDING_ADDRESS_INIT;

  if (direction == DATA_BIND_BINDING_EGRESS) {
    out->binding_class = DATA_BIND_BINDING_RESULT;
    out->space = "http.result";
    out->name = field->name;
    return DATA_BIND_OK;
  }

  if (field->binding_kind == NULL) {
    chttp_service_error_set(
        error, DATA_BIND_ERR_SCHEMA,
        "HTTP Service request field has no HTTP binding projection");
    return DATA_BIND_ERR_SCHEMA;
  }

  out->name =
      field->binding_name != NULL ? field->binding_name : field->name;

  if (strcmp(field->binding_kind, "path") == 0) {
    out->binding_class = DATA_BIND_BINDING_VALUE;
    out->space = "http.path";
  } else if (strcmp(field->binding_kind, "query") == 0) {
    out->binding_class = DATA_BIND_BINDING_VALUE;
    out->space = "http.query";
  } else if (strcmp(field->binding_kind, "header") == 0) {
    out->binding_class = DATA_BIND_BINDING_METADATA;
    out->space = "http.header";
  } else if (strcmp(field->binding_kind, "cookie") == 0) {
    out->binding_class = DATA_BIND_BINDING_METADATA;
    out->space = "http.cookie";
  } else if (strcmp(field->binding_kind, "body") == 0) {
    out->binding_class = DATA_BIND_BINDING_PAYLOAD;
    out->space = "http.body";
  } else {
    chttp_service_error_set(error, DATA_BIND_ERR_SCHEMA,
                            "Unknown HTTP Service field binding");
    return DATA_BIND_ERR_SCHEMA;
  }

  return DATA_BIND_OK;
}

static int chttp_service_method_from_text(const char *method,
                                          chttp_method *out) {
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

static int chttp_service_lower_route(const char *source, char **out_route) {
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

static int chttp_service_result_valid(const chttp_service_result *result) {
  return result != NULL && result->size >= sizeof(*result) &&
         result->status_code >= 100u && result->status_code <= 599u &&
         (result->body_size == 0u || result->body != NULL);
}

static int chttp_service_http_handler(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_service_method_record *record =
      (chttp_service_method_record *)user;
  chttp_service_http_provider provider_context;
  DataBindBindingProvider provider = DATA_BIND_BINDING_PROVIDER_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  chttp_service_context context = CHTTP_SERVICE_CONTEXT_INIT;
  chttp_service_result result = CHTTP_SERVICE_RESULT_INIT;
  int status;

  if (record == NULL || record->owner == NULL || request == NULL ||
      response == NULL)
    return SALTS_EINVAL;

  provider_context = (chttp_service_http_provider){
      .service = record->owner,
      .request = request,
      .body_reader = DATA_BIND_FORMAT_READER_INIT};

  provider.context = &provider_context;
  provider.open_input = chttp_service_http_open_input;

  context.http = request;
  status = record->execute(
      record->user, record->plan, &provider,
      &record->owner->native_options, &context,
      &result, &diagnostic);

  if (provider_context.body_reader_open)
    (void)data_bind_format_reader_close(&provider_context.body_reader);

  if (status != SALTS_OK) {
    if (diagnostic.status != DATA_BIND_OK)
      return chttp_server_reply(
          response, 400u, "text/plain", "Bad Request", 11u);
    return chttp_server_reply(
        response, 500u, "text/plain", "Internal Server Error", 21u);
  }

  if (!chttp_service_result_valid(&result))
    return chttp_server_reply(
        response, 500u, "text/plain", "Internal Server Error", 21u);

  return chttp_server_reply(
      response, result.status_code, result.content_type,
      result.body, result.body_size);
}

int chttp_service_init(chttp_service *service,
                       const chttp_service_config *config) {
  chttp_service_impl *impl;

  if (service == NULL || config == NULL ||
      config->size < sizeof(*config) || config->contract == NULL ||
      config->method_capacity == 0u ||
      config->max_binding_value_bytes == 0u ||
      config->max_json_depth == 0u ||
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
  impl->scalar_scratch =
      (char *)malloc(config->max_binding_value_bytes + 1u);
  impl->native_workspace =
      (unsigned char *)malloc(config->native_workspace_bytes);
  if (impl->methods == NULL || impl->scalar_scratch == NULL ||
      impl->native_workspace == NULL) {
    free(impl->native_workspace);
    free(impl->scalar_scratch);
    free(impl->methods);
    free(impl);
    return SALTS_ENOMEM;
  }

  impl->contract = config->contract;
  impl->method_capacity = config->method_capacity;
  impl->scalar_capacity = config->max_binding_value_bytes;
  impl->max_json_depth = config->max_json_depth;
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

int chttp_service_register_http(
    chttp_service *service,
    chttp_server *server,
    const chttp_service_http_method *method) {
  chttp_service_impl *impl;
  chttp_service_method_record *record;
  DataBindServiceOperation operation = DATA_BIND_SERVICE_OPERATION_INIT;
  DataBindBindingProjection projection =
      DATA_BIND_BINDING_PROJECTION_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  chttp_server_route_options route_options;
  DataBindStatus bind_status;
  chttp_method http_method;
  char *route = NULL;
  int status;

  if (service == NULL || service->impl == NULL || server == NULL ||
      method == NULL || method->size < sizeof(*method) ||
      method->service == NULL || method->operation == NULL ||
      method->native == NULL || method->execute == NULL)
    return SALTS_EINVAL;

  impl = (chttp_service_impl *)service->impl;
  if (impl->method_count == impl->method_capacity) return SALTS_ENOBUFS;

  if (!data_bind_service_operation_find(
          impl->contract, method->service, method->operation, &operation) ||
      !operation.has_http || operation.http_method == NULL ||
      operation.http_path == NULL)
    return SALTS_ENOENT;

  status = chttp_service_method_from_text(operation.http_method, &http_method);
  if (status != SALTS_OK) return status;
  status = chttp_service_lower_route(operation.http_path, &route);
  if (status != SALTS_OK) return status;

  projection.id = "http";
  projection.project_field = chttp_service_http_project;

  bind_status = data_bind_binding_plan_compile_service(
      impl->contract, method->service, method->operation,
      &projection, method->native, &record, &diagnostic);
  if (bind_status != DATA_BIND_OK) {
    free(route);
    return SALTS_EINVAL;
  }

  record = &impl->methods[impl->method_count];
  memset(record, 0, sizeof(*record));
  record->owner = impl;
  record->route = route;
  record->method = http_method;
  record->execute = method->execute;
  record->user = method->user;

  /* Recompile into the stable record after the earlier admission probe. */
  bind_status = data_bind_binding_plan_compile_service(
      impl->contract, method->service, method->operation,
      &projection, method->native, &record->plan, &diagnostic);
  if (bind_status != DATA_BIND_OK) {
    free(route);
    memset(record, 0, sizeof(*record));
    return SALTS_EINVAL;
  }

  route_options = (chttp_server_route_options){
      .method = http_method,
      .path = record->route,
      .middleware = method->middleware,
      .middleware_count = method->middleware_count,
      .handler = chttp_service_http_handler,
      .user = record};

  status = chttp_server_route_with(server, &route_options);
  if (status != SALTS_OK) {
    data_bind_binding_plan_free(record->plan);
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

  for (i = 0u; i < impl->method_count; ++i) {
    data_bind_binding_plan_free(impl->methods[i].plan);
    free(impl->methods[i].route);
  }

  free(impl->native_workspace);
  free(impl->scalar_scratch);
  free(impl->methods);
  free(impl);
  service->impl = NULL;
  return SALTS_OK;
}
