#include <chttp_rpc_service/service.h>

#include <salts/error_codes.h>

#include <cserde/cserde.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_rpc_service_impl chttp_rpc_service_impl;

typedef struct chttp_rpc_service_method_record {
  chttp_rpc_service_impl *owner;
  const DataBindRpcMethodPlan *method_plan;
  chttp_rpc_service_exact_fn invoke;
  void *user;
} chttp_rpc_service_method_record;

struct chttp_rpc_service_impl {
  chttp_rpc_service_method_record *methods;
  size_t method_capacity;
  size_t method_count;

  unsigned char *output_scratch;
  size_t output_capacity;

  unsigned char *native_workspace;
  DataBindNativeOptions native_options;
};

typedef struct chttp_rpc_service_provider {
  chttp_rpc_service_impl *service;
  const crpc_server_request_view *request;
  crpc_server_param_reader param_reader;

  cserde_writer writer;
  cserde_token token;
  int token_valid;

  int output_active;
  int output_published;
  int output_is_error;
} chttp_rpc_service_provider;

static void chttp_rpc_service_error_set(
    DataBindError *error, DataBindStatus status, const char *message) {
  if (error == NULL) return;
  if (error->size == 0u) error->size = sizeof(*error);
  error->code = status;
  error->line = -1;
  error->column = -1;
  error->path[0] = '\0';
  if (message == NULL) message = "";
  {
    size_t length = strlen(message);
    if (length >= sizeof(error->message))
      length = sizeof(error->message) - 1u;
    memcpy(error->message, message, length);
    error->message[length] = '\0';
  }
}

static int chttp_rpc_service_scalar_kind(const cmeta_data_desc *data) {
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

static DataBindStatus chttp_rpc_service_param_status(int status) {
  if (status == SALTS_ENOMEM) return DATA_BIND_ERR_OOM;
  if (status == SALTS_EPROTO) return DATA_BIND_ERR_PARSE;
  if (status == SALTS_EINVAL || status == SALTS_EALREADY)
    return DATA_BIND_ERR_SCHEMA;
  return DATA_BIND_ERR_RUNTIME;
}

static DataBindStatus chttp_rpc_service_open_input(
    void *context, const DataBindBindingPlanEntry *entry,
    cserde_reader *reader, DataBindBindingValueState *state,
    DataBindError *error) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  int present = 0;
  int status;

  if (provider == NULL || provider->request == NULL ||
      entry == NULL || reader == NULL || state == NULL ||
      entry->address.space == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  crpc_server_request_param_close(&provider->param_reader);
  provider->param_reader =
      (crpc_server_param_reader)CRPC_SERVER_PARAM_READER_INIT;
  *state = DATA_BIND_VALUE_STATE_ABSENT;

  if (strcmp(entry->address.space, "rpc.params") != 0) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_SCHEMA,
        "Generated RPC BindingPlan exposed an unsupported ingress space");
    return DATA_BIND_ERR_SCHEMA;
  }

  status = crpc_server_request_param_open(
      provider->request, entry->address.name, entry->address.ordinal,
      &provider->param_reader, &present);
  if (status != SALTS_OK) {
    DataBindStatus mapped = chttp_rpc_service_param_status(status);
    chttp_rpc_service_error_set(
        error, mapped,
        status == SALTS_EPROTO
            ? "Malformed or duplicate JSON-RPC params selection"
            : "RPC params selection failed");
    return mapped;
  }
  if (!present) return DATA_BIND_OK;
  if (provider->param_reader.reader == NULL) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "RPC params selector returned a missing reader");
    return DATA_BIND_ERR_RUNTIME;
  }

  *reader = *provider->param_reader.reader;
  *state = DATA_BIND_VALUE_STATE_VALUE;
  return DATA_BIND_OK;
}

static cserde_status chttp_rpc_service_capture_token(
    void *context, const cserde_token *token) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;

  if (provider == NULL || provider->service == NULL || token == NULL)
    return CSERDE_INVALID_ARGUMENT;
  if (provider->token_valid) return CSERDE_INVALID_STATE;

  provider->token = *token;
  if (token->kind == CSERDE_STRING || token->kind == CSERDE_BYTES) {
    if (token->value.slice.size > provider->service->output_capacity)
      return CSERDE_LIMIT_EXCEEDED;
    if (token->value.slice.size != 0u) {
      if (token->value.slice.data == NULL) return CSERDE_INVALID_TOKEN;
      memcpy(
          provider->service->output_scratch,
          token->value.slice.data,
          token->value.slice.size);
    }
    provider->token.value.slice.data =
        provider->service->output_scratch;
    provider->token.value.slice.lifetime = CSERDE_VIEW_STABLE;
  } else if (token->kind == CSERDE_ARRAY_BEGIN ||
             token->kind == CSERDE_ARRAY_END ||
             token->kind == CSERDE_MAP_BEGIN ||
             token->kind == CSERDE_MAP_END) {
    return CSERDE_UNSUPPORTED;
  }

  provider->token_valid = 1;
  return CSERDE_OK;
}

static cserde_status chttp_rpc_service_capture_finish(void *context) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  return provider != NULL ? CSERDE_OK : CSERDE_INVALID_ARGUMENT;
}

static const cserde_writer_ops CHTTP_RPC_SERVICE_CAPTURE_OPS = {
    sizeof(cserde_writer_ops),
    CSERDE_WRITER_OPS_ABI_VERSION,
    chttp_rpc_service_capture_token,
    chttp_rpc_service_capture_finish};

static DataBindStatus chttp_rpc_service_begin_output(
    void *context, DataBindError *error) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  (void)error;
  if (provider == NULL || provider->service == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  provider->writer = (cserde_writer){0};
  provider->token = (cserde_token){0};
  provider->token_valid = 0;
  provider->output_active = 1;
  provider->output_published = 0;
  provider->output_is_error = 0;
  return DATA_BIND_OK;
}

static DataBindStatus chttp_rpc_service_write_output(
    void *context, const DataBindBindingPlanEntry *entry,
    DataBindBindingValueState state, const void *value, size_t value_bytes,
    DataBindError *error) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  DataBindNativeDiagnostic diagnostic =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  DataBindStatus status;

  if (provider == NULL || provider->service == NULL || entry == NULL ||
      !provider->output_active)
    return DATA_BIND_ERR_INVALID_ARG;
  if (provider->token_valid) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Phase-1 RPC egress admits one scalar result/error value");
    return DATA_BIND_ERR_RUNTIME;
  }

  if (entry->address.binding_class == DATA_BIND_BINDING_ERROR) {
    provider->output_is_error = 1;
  } else {
    if (entry->address.space == NULL ||
        strcmp(entry->address.space, "rpc.result") != 0) {
      chttp_rpc_service_error_set(
          error, DATA_BIND_ERR_SCHEMA,
          "Generated RPC BindingPlan exposed an unsupported egress space");
      return DATA_BIND_ERR_SCHEMA;
    }
    provider->output_is_error = 0;
  }

  if (state == DATA_BIND_VALUE_STATE_NULL) {
    provider->token = (cserde_token){.kind = CSERDE_NULL};
    provider->token_valid = 1;
    return DATA_BIND_OK;
  }
  if (state != DATA_BIND_VALUE_STATE_VALUE || value == NULL ||
      !chttp_rpc_service_scalar_kind(entry->data)) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "Phase-1 RPC egress requires one scalar VALUE/NULL payload");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  if (cserde_writer_init(
          &provider->writer, &CHTTP_RPC_SERVICE_CAPTURE_OPS,
          provider) != CSERDE_OK) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Could not initialize RPC scalar staging writer");
    return DATA_BIND_ERR_RUNTIME;
  }

  status = data_bind_native_encode(
      &provider->service->native_options, entry->data,
      value, value_bytes, &provider->writer, &diagnostic);
  if (status != DATA_BIND_OK && error != NULL)
    *error = diagnostic.error;
  return status;
}

static DataBindStatus chttp_rpc_service_commit_output(
    void *context, DataBindError *error) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  (void)error;
  if (provider == NULL || !provider->output_active)
    return DATA_BIND_ERR_INVALID_ARG;
  provider->output_active = 0;
  provider->output_published = 1;
  return DATA_BIND_OK;
}

static void chttp_rpc_service_abort_output(void *context) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  if (provider == NULL) return;
  provider->output_active = 0;
  provider->output_published = 0;
  provider->output_is_error = 0;
  provider->token_valid = 0;
  provider->writer = (cserde_writer){0};
  provider->token = (cserde_token){0};
}

static cserde_status chttp_rpc_service_encode_staged(
    void *user, cserde_writer *writer) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)user;
  if (provider == NULL || writer == NULL || !provider->token_valid)
    return CSERDE_INVALID_ARGUMENT;
  return cserde_writer_write(writer, &provider->token);
}

static int chttp_rpc_service_input_failure(DataBindStatus status) {
  return status == DATA_BIND_ERR_INVALID_ARG ||
         status == DATA_BIND_ERR_PARSE ||
         status == DATA_BIND_ERR_TYPE_NOT_FOUND ||
         status == DATA_BIND_ERR_TYPE_MISMATCH ||
         status == DATA_BIND_ERR_LIMIT ||
         status == DATA_BIND_ERR_VALIDATION;
}

static int chttp_rpc_service_handler(
    void *user, const crpc_server_request_view *request,
    crpc_server_response *response) {
  chttp_rpc_service_method_record *record =
      (chttp_rpc_service_method_record *)user;
  chttp_rpc_service_provider provider_context;
  DataBindBindingProvider provider = DATA_BIND_BINDING_PROVIDER_INIT;
  DataBindBindingOutcome outcome = DATA_BIND_BINDING_OUTCOME_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  const DataBindBindingPlan *binding;
  DataBindStatus status;
  int code = 0;
  int result;

  if (record == NULL || record->owner == NULL ||
      record->method_plan == NULL || record->invoke == NULL ||
      request == NULL || response == NULL)
    return SALTS_EINVAL;

  provider_context = (chttp_rpc_service_provider){
      .service = record->owner,
      .request = request,
      .param_reader = CRPC_SERVER_PARAM_READER_INIT};

  provider.context = &provider_context;
  provider.open_input = chttp_rpc_service_open_input;
  provider.begin_output = chttp_rpc_service_begin_output;
  provider.write_output = chttp_rpc_service_write_output;
  provider.commit_output = chttp_rpc_service_commit_output;
  provider.abort_output = chttp_rpc_service_abort_output;

  binding = data_bind_rpc_method_plan_binding(record->method_plan);
  status = record->invoke(
      record->user, binding, &provider,
      &record->owner->native_options, &outcome, &diagnostic);

  crpc_server_request_param_close(&provider_context.param_reader);

  if (status != DATA_BIND_OK) {
    if (chttp_rpc_service_input_failure(status))
      return crpc_server_response_error(
          response, -32602, "Invalid params", NULL, NULL);
    return SALTS_EPROTO;
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_SUCCESS) {
    if (!provider_context.output_published ||
        provider_context.output_is_error)
      return SALTS_EPROTO;
    return crpc_server_response_result(
        response,
        provider_context.token_valid
            ? chttp_rpc_service_encode_staged
            : NULL,
        provider_context.token_valid ? &provider_context : NULL);
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_TYPED_ERROR) {
    if (!provider_context.output_published ||
        !provider_context.output_is_error ||
        !data_bind_rpc_method_plan_code_for_outcome(
            record->method_plan, &outcome, &code))
      return SALTS_EPROTO;
    result = crpc_server_response_error(
        response, (int64_t)code,
        outcome.typed_error != NULL ? outcome.typed_error : "Service error",
        provider_context.token_valid
            ? chttp_rpc_service_encode_staged
            : NULL,
        provider_context.token_valid ? &provider_context : NULL);
    return result;
  }

  /*
   * Native/business status deliberately has no implicit RPC-code mapping in
   * DataBindRpcMethodPlan. Until an explicit application policy is mounted,
   * fail closed and let CRPC emit its ordinary internal-error envelope.
   */
  return SALTS_EPROTO;
}

static int chttp_rpc_service_plan_supported(
    const DataBindRpcMethodPlan *plan) {
  const DataBindBindingPlan *binding;
  size_t i;
  DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;

  if (plan == NULL ||
      data_bind_rpc_method_plan_wire_method(plan) == NULL ||
      data_bind_rpc_method_plan_wire_method(plan)[0] == '\0')
    return 0;

  binding = data_bind_rpc_method_plan_binding(plan);
  if (binding == NULL) return 0;

  for (i = 0u; i < data_bind_binding_plan_ingress_count(binding); ++i) {
    entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "rpc.params") != 0)
      return 0;
  }

  if (data_bind_binding_plan_egress_count(binding) > 1u)
    return 0;
  if (data_bind_binding_plan_egress_count(binding) == 1u) {
    entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_egress_at(binding, 0u, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "rpc.result") != 0 ||
        !chttp_rpc_service_scalar_kind(entry.data))
      return 0;
  }

  return 1;
}

int chttp_rpc_service_init(
    chttp_rpc_service *service,
    const chttp_rpc_service_config *config) {
  chttp_rpc_service_impl *impl;

  if (service == NULL || config == NULL ||
      config->size < sizeof(*config) ||
      config->method_capacity == 0u ||
      config->max_output_value_bytes == 0u ||
      config->native_workspace_bytes == 0u ||
      config->native_max_depth == 0u ||
      config->native_max_items == 0u)
    return SALTS_EINVAL;
  if (service->impl != NULL) return SALTS_EALREADY;

  impl = (chttp_rpc_service_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->methods = (chttp_rpc_service_method_record *)calloc(
      config->method_capacity, sizeof(*impl->methods));
  impl->output_scratch =
      (unsigned char *)malloc(config->max_output_value_bytes);
  impl->native_workspace =
      (unsigned char *)malloc(config->native_workspace_bytes);
  if (impl->methods == NULL || impl->output_scratch == NULL ||
      impl->native_workspace == NULL) {
    free(impl->native_workspace);
    free(impl->output_scratch);
    free(impl->methods);
    free(impl);
    return SALTS_ENOMEM;
  }

  impl->method_capacity = config->method_capacity;
  impl->output_capacity = config->max_output_value_bytes;
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

int chttp_rpc_service_mount(
    chttp_rpc_service *service,
    crpc_server *server,
    const chttp_rpc_service_mount *mount) {
  chttp_rpc_service_impl *impl;
  chttp_rpc_service_method_record *record;
  crpc_method method = {0};
  const char *wire_method;
  int status;

  if (service == NULL || service->impl == NULL || server == NULL ||
      mount == NULL || mount->size < sizeof(*mount) ||
      mount->target == NULL || mount->target[0] == '\0' ||
      mount->method_plan == NULL || mount->invoke == NULL)
    return SALTS_EINVAL;
  if (!chttp_rpc_service_plan_supported(mount->method_plan))
    return SALTS_ENOTSUP;

  impl = (chttp_rpc_service_impl *)service->impl;
  if (impl->method_count == impl->method_capacity)
    return SALTS_ENOBUFS;

  wire_method =
      data_bind_rpc_method_plan_wire_method(mount->method_plan);
  method.name = wire_method;
  method.service = NULL;
  method.callable = NULL;

  record = &impl->methods[impl->method_count];
  *record = (chttp_rpc_service_method_record){
      .owner = impl,
      .method_plan = mount->method_plan,
      .invoke = mount->invoke,
      .user = mount->user};

  status = crpc_server_register(
      server, mount->target, &method,
      chttp_rpc_service_handler, record);
  if (status != SALTS_OK) {
    memset(record, 0, sizeof(*record));
    return status;
  }

  ++impl->method_count;
  return SALTS_OK;
}

int chttp_rpc_service_destroy(chttp_rpc_service *service) {
  chttp_rpc_service_impl *impl;
  if (service == NULL) return SALTS_EINVAL;
  impl = (chttp_rpc_service_impl *)service->impl;
  if (impl == NULL) return SALTS_OK;
  free(impl->native_workspace);
  free(impl->output_scratch);
  free(impl->methods);
  free(impl);
  service->impl = NULL;
  return SALTS_OK;
}
