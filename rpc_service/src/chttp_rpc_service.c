#include <chttp_rpc_service/service.h>

#include <salts/error_codes.h>
#include <cmeta_buffer.h>

#include <cserde/cserde.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_rpc_service_impl chttp_rpc_service_impl;

typedef struct chttp_rpc_service_method_record {
  chttp_rpc_service_impl *owner;
  const DataBindRpcMethodPlan *method_plan;
  const DataBindBindingPlan *binding;
  const DataBindServiceNativeBinding *native_binding;
  const DataBindNativeExecution *execution;

  unsigned char *request_storage;
  size_t request_bytes;
  unsigned char *response_storage;
  size_t response_bytes;
  unsigned char *error_storage;
  size_t error_bytes;

  void *params[3];
  size_t param_bytes[3];
  size_t param_count;
  DataBindBindingCallFrame frame;
} chttp_rpc_service_method_record;

struct chttp_rpc_service_impl {
  chttp_rpc_service_method_record *methods;
  size_t method_capacity;
  size_t method_count;

  size_t output_capacity;
  size_t max_call_frame_bytes;

  unsigned char *native_workspace;
  DataBindNativeOptions native_options;
};

typedef struct chttp_rpc_service_provider {
  chttp_rpc_service_impl *service;
  const crpc_server_request_view *request;
  crpc_server_param_reader param_reader;

  cserde_writer writer;
  cserde_token token;
  mem_buffer_t *token_storage;
  int writer_started;
  int token_valid;

  int output_attempted;
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

static void chttp_rpc_service_release_token_storage(
    chttp_rpc_service_provider *provider) {
  if (provider == NULL || provider->token_storage == NULL) return;
  mem_buffer_release(provider->token_storage);
  provider->token_storage = NULL;
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
    const size_t size = token->value.slice.size;
    if (size > provider->service->output_capacity)
      return CSERDE_LIMIT_EXCEEDED;
    if (size != 0u && token->value.slice.data == NULL)
      return CSERDE_INVALID_TOKEN;
    provider->token_storage =
        mem_get_buffer(mem_global(), size == 0u ? 1u : size);
    if (provider->token_storage == NULL) return CSERDE_SINK_ERROR;
    if (size != 0u)
      memcpy(mem_buffer_data(provider->token_storage),
             token->value.slice.data, size);
    mem_set_used(provider->token_storage, size);
    provider->token.value.slice.data =
        (const unsigned char *)mem_buffer_const_data(provider->token_storage);
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

  chttp_rpc_service_release_token_storage(provider);
  provider->writer = (cserde_writer){0};
  provider->token = (cserde_token){0};
  provider->writer_started = 0;
  provider->token_valid = 0;
  provider->output_attempted = 1;
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
  provider->writer_started = 1;

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
  if (provider == NULL || !provider->output_active)
    return DATA_BIND_ERR_INVALID_ARG;
  if (provider->writer_started &&
      cserde_writer_finish(&provider->writer) != CSERDE_OK) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Could not finish RPC scalar staging writer");
    provider->output_active = 0;
    provider->output_published = 0;
    chttp_rpc_service_release_token_storage(provider);
    return DATA_BIND_ERR_RUNTIME;
  }
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
  provider->writer_started = 0;
  provider->writer = (cserde_writer){0};
  provider->token = (cserde_token){0};
  chttp_rpc_service_release_token_storage(provider);
}

static cserde_status chttp_rpc_service_encode_staged(
    void *user, cserde_writer *writer) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)user;
  if (provider == NULL || writer == NULL || !provider->token_valid)
    return CSERDE_INVALID_ARGUMENT;
  return cserde_writer_write(writer, &provider->token);
}

static int chttp_rpc_service_native_type_valid(
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

static int chttp_rpc_service_add_size(
    size_t *total, size_t value, size_t limit) {
  if (total == NULL || value > SIZE_MAX - *total) return 0;
  *total += value;
  return *total <= limit;
}

static int chttp_rpc_service_function_semantics_admit(
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

static int chttp_rpc_service_execution_admit(
    const DataBindRpcMethodPlan *method_plan,
    const DataBindServiceNativeBinding *native,
    const DataBindNativeExecution *execution,
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

  if (method_plan == NULL || native == NULL || execution == NULL ||
      request_bytes == NULL || response_bytes == NULL ||
      error_bytes == NULL || param_count == NULL ||
      native->size < sizeof(*native) ||
      native->abi_version != DATA_BIND_BINDING_PLAN_ABI_VERSION ||
      !cmeta_function_desc_valid(native->function) ||
      !data_bind_native_execution_valid(execution) ||
      !chttp_rpc_service_native_type_valid(native->request, request_bytes) ||
      !chttp_rpc_service_native_type_valid(native->response, response_bytes))
    return SALTS_EINVAL;

  binding = data_bind_rpc_method_plan_binding(method_plan);
  function = binding != NULL ? data_bind_binding_plan_function(binding) : NULL;
  if (function == NULL ||
      !cmeta_function_desc_equal(function, native->function) ||
      !cmeta_function_desc_equal(function, execution->function))
    return SALTS_EINVAL;

  if ((function->effects & CMETA_EFFECT_ASYNC) != 0u)
    return SALTS_ENOTSUP;

  if (data_bind_binding_plan_error_count(binding) != native->error_count)
    return SALTS_EINVAL;
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
          entry.function_param_index != 0u ||
          entry.address.space == NULL ||
          strcmp(entry.address.space, "rpc.params") != 0 ||
          !chttp_rpc_service_scalar_kind(entry.data))
        return SALTS_ENOTSUP;
      bytes = entry.data->storage_type->size;
      if (entry.native_offset > *request_bytes ||
          bytes > *request_bytes - entry.native_offset)
        return SALTS_EINVAL;
    }

    count = data_bind_binding_plan_egress_count(binding);
    if (count > 1u) return SALTS_ENOTSUP;
    for (i = 0u; i < count; ++i) {
      size_t bytes;
      entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      if (!data_bind_binding_plan_egress_at(binding, i, &entry) ||
          entry.data == NULL || entry.data->storage_type == NULL ||
          entry.target_is_return || entry.function_param_index != 1u ||
          entry.address.space == NULL ||
          strcmp(entry.address.space, "rpc.result") != 0 ||
          !chttp_rpc_service_scalar_kind(entry.data))
        return SALTS_ENOTSUP;
      bytes = entry.data->storage_type->size;
      if (entry.native_offset > *response_bytes ||
          bytes > *response_bytes - entry.native_offset)
        return SALTS_EINVAL;
    }
  }

  expected_params = native->error_count == 0u ? 2u : 3u;
  if (!chttp_rpc_service_function_semantics_admit(function, expected_params))
    return SALTS_ENOTSUP;
  if (execution->abi->param_count != expected_params ||
      execution->abi->return_carrier != CMETA_ABI_SCALAR ||
      cmeta_function_param_abi(execution->abi, 0u) !=
          CMETA_ABI_OBJECT_POINTER ||
      cmeta_function_param_abi(execution->abi, 1u) !=
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
        cmeta_function_param_abi(execution->abi, 2u) !=
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
      !chttp_rpc_service_add_size(&total, *request_bytes, max_frame_bytes) ||
      !chttp_rpc_service_add_size(&total, *response_bytes, max_frame_bytes) ||
      !chttp_rpc_service_add_size(&total, *error_bytes, max_frame_bytes))
    return SALTS_EMSGSIZE;

  *param_count = expected_params;
  return SALTS_OK;
}

static void chttp_rpc_service_method_release(
    chttp_rpc_service_method_record *record) {
  if (record == NULL) return;
  free(record->error_storage);
  free(record->response_storage);
  free(record->request_storage);
  memset(record, 0, sizeof(*record));
}

static int chttp_rpc_service_input_failure(DataBindStatus status) {
  return status == DATA_BIND_ERR_INVALID_ARG ||
         status == DATA_BIND_ERR_PARSE ||
         status == DATA_BIND_ERR_TYPE_NOT_FOUND ||
         status == DATA_BIND_ERR_TYPE_MISMATCH ||
         status == DATA_BIND_ERR_LIMIT ||
         status == DATA_BIND_ERR_VALIDATION;
}

static cserde_status chttp_rpc_service_encode_native_status(
    void *user, cserde_writer *writer) {
  const int *status = (const int *)user;
  if (status == NULL || writer == NULL) return CSERDE_INVALID_ARGUMENT;
  return cserde_writer_write(
      writer,
      &(const cserde_token){.kind = CSERDE_SINT,
                           .value.sint = (int64_t)*status});
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
  DataBindStatus status;
  int native_status = 0;
  int code = 0;
  int result;

  if (record == NULL || record->owner == NULL ||
      record->method_plan == NULL || record->binding == NULL ||
      record->native_binding == NULL || record->execution == NULL ||
      record->request_storage == NULL || record->response_storage == NULL ||
      request == NULL || response == NULL)
    return SALTS_EINVAL;

  memset(record->request_storage, 0, record->request_bytes);
  memset(record->response_storage, 0, record->response_bytes);
  if (record->error_storage != NULL)
    memset(record->error_storage, 0, record->error_bytes);

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

  status = data_bind_binding_plan_bind_inputs(
      record->binding, &provider, &record->owner->native_options,
      &record->frame, &diagnostic);

  if (status == DATA_BIND_OK &&
      !record->execution->invoke(
          record->execution->context, &native_status,
          record->params, record->param_count))
    status = DATA_BIND_ERR_RUNTIME;

  if (status == DATA_BIND_OK)
    status = data_bind_binding_plan_write_outcome(
        record->binding, &provider, &record->frame, native_status,
        &outcome, &diagnostic);

  crpc_server_request_param_close(&provider_context.param_reader);

  if (status != DATA_BIND_OK) {
    chttp_rpc_service_release_token_storage(&provider_context);
    if (!provider_context.output_attempted &&
        chttp_rpc_service_input_failure(status))
      return crpc_server_response_error(
          response, -32602, "Invalid params", NULL, NULL);
    return SALTS_EPROTO;
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_SUCCESS) {
    if (!provider_context.output_published ||
        provider_context.output_is_error) {
      chttp_rpc_service_release_token_storage(&provider_context);
      return SALTS_EPROTO;
    }
    result = crpc_server_response_result(
        response,
        provider_context.token_valid
            ? chttp_rpc_service_encode_staged
            : NULL,
        provider_context.token_valid ? &provider_context : NULL);
    chttp_rpc_service_release_token_storage(&provider_context);
    return result;
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_TYPED_ERROR) {
    if (!provider_context.output_published ||
        !provider_context.output_is_error ||
        !data_bind_rpc_method_plan_code_for_outcome(
            record->method_plan, &outcome, &code)) {
      chttp_rpc_service_release_token_storage(&provider_context);
      return SALTS_EPROTO;
    }
    result = crpc_server_response_error(
        response, (int64_t)code,
        outcome.typed_error != NULL ? outcome.typed_error : "Service error",
        provider_context.token_valid
            ? chttp_rpc_service_encode_staged
            : NULL,
        provider_context.token_valid ? &provider_context : NULL);
    chttp_rpc_service_release_token_storage(&provider_context);
    return result;
  }

  chttp_rpc_service_release_token_storage(&provider_context);
  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_NATIVE_STATUS)
    return crpc_server_response_error(
        response, -32000, "Native status",
        chttp_rpc_service_encode_native_status, &native_status);

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
      config->max_call_frame_bytes == 0u ||
      config->native_workspace_bytes == 0u ||
      config->native_max_depth == 0u ||
      config->native_max_items == 0u)
    return SALTS_EINVAL;
  if (service->impl != NULL) return SALTS_EALREADY;

  impl = (chttp_rpc_service_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->methods = (chttp_rpc_service_method_record *)calloc(
      config->method_capacity, sizeof(*impl->methods));
  impl->native_workspace =
      (unsigned char *)malloc(config->native_workspace_bytes);
  if (impl->methods == NULL || impl->native_workspace == NULL) {
    free(impl->native_workspace);
    free(impl->methods);
    free(impl);
    return SALTS_ENOMEM;
  }

  impl->method_capacity = config->method_capacity;
  impl->output_capacity = config->max_output_value_bytes;
  impl->max_call_frame_bytes = config->max_call_frame_bytes;
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
    const chttp_rpc_service_mount_options *mount) {
  chttp_rpc_service_impl *impl;
  chttp_rpc_service_method_record *record;
  crpc_method method = {0};
  const char *wire_method;
  const DataBindBindingPlan *binding;
  size_t request_bytes = 0u;
  size_t response_bytes = 0u;
  size_t error_bytes = 0u;
  size_t param_count = 0u;
  int status;

  if (service == NULL || service->impl == NULL || server == NULL ||
      mount == NULL || mount->size < sizeof(*mount) ||
      mount->target == NULL || mount->target[0] == '\0' ||
      mount->method_plan == NULL || mount->native_binding == NULL ||
      mount->execution == NULL)
    return SALTS_EINVAL;
  if (!chttp_rpc_service_plan_supported(mount->method_plan))
    return SALTS_ENOTSUP;

  impl = (chttp_rpc_service_impl *)service->impl;
  if (impl->method_count == impl->method_capacity)
    return SALTS_ENOBUFS;

  status = chttp_rpc_service_execution_admit(
      mount->method_plan, mount->native_binding, mount->execution,
      impl->max_call_frame_bytes, &request_bytes, &response_bytes,
      &error_bytes, &param_count);
  if (status != SALTS_OK) return status;

  binding = data_bind_rpc_method_plan_binding(mount->method_plan);
  if (binding == NULL) return SALTS_EINVAL;

  wire_method =
      data_bind_rpc_method_plan_wire_method(mount->method_plan);
  method.name = wire_method;
  method.service = NULL;
  method.callable = NULL;

  record = &impl->methods[impl->method_count];
  *record = (chttp_rpc_service_method_record){
      .owner = impl,
      .method_plan = mount->method_plan,
      .binding = binding,
      .native_binding = mount->native_binding,
      .execution = mount->execution,
      .request_bytes = request_bytes,
      .response_bytes = response_bytes,
      .error_bytes = error_bytes,
      .param_count = param_count,
      .frame = (DataBindBindingCallFrame)DATA_BIND_BINDING_CALL_FRAME_INIT};

  record->request_storage =
      (unsigned char *)calloc(1u, record->request_bytes);
  record->response_storage =
      (unsigned char *)calloc(1u, record->response_bytes);
  if (record->error_bytes != 0u)
    record->error_storage =
        (unsigned char *)calloc(1u, record->error_bytes);
  if (record->request_storage == NULL ||
      record->response_storage == NULL ||
      (record->error_bytes != 0u && record->error_storage == NULL)) {
    chttp_rpc_service_method_release(record);
    return SALTS_ENOMEM;
  }

  record->params[0] = record->request_storage;
  record->params[1] = record->response_storage;
  record->param_bytes[0] = record->request_bytes;
  record->param_bytes[1] = record->response_bytes;
  if (record->param_count == 3u) {
    record->params[2] = record->error_storage;
    record->param_bytes[2] = record->error_bytes;
  }
  record->frame.request = record->request_storage;
  record->frame.request_bytes = record->request_bytes;
  record->frame.params = record->params;
  record->frame.param_bytes = record->param_bytes;
  record->frame.param_count = record->param_count;

  status = crpc_server_register(
      server, mount->target, &method,
      chttp_rpc_service_handler, record);
  if (status != SALTS_OK) {
    chttp_rpc_service_method_release(record);
    return status;
  }

  ++impl->method_count;
  return SALTS_OK;
}

int chttp_rpc_service_destroy(chttp_rpc_service *service) {
  chttp_rpc_service_impl *impl;
  size_t i;
  if (service == NULL) return SALTS_EINVAL;
  impl = (chttp_rpc_service_impl *)service->impl;
  if (impl == NULL) return SALTS_OK;
  for (i = 0u; i < impl->method_count; ++i)
    chttp_rpc_service_method_release(&impl->methods[i]);
  free(impl->native_workspace);
  free(impl->methods);
  free(impl);
  service->impl = NULL;
  return SALTS_OK;
}
