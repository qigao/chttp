#include <chttp_rpc_service/service.h>

#include <string.h>

typedef enum chttp_rpc_service_params_mode {
  CHTTP_RPC_SERVICE_PARAMS_NONE = 0,
  CHTTP_RPC_SERVICE_PARAMS_OBJECT,
  CHTTP_RPC_SERVICE_PARAMS_ARRAY
} chttp_rpc_service_params_mode;

typedef struct chttp_rpc_service_client_encode_context {
  const DataBindBindingPlan *binding;
  const DataBindNativeOptions *native_options;
  const unsigned char *request;
  size_t request_bytes;
  chttp_rpc_service_params_mode mode;
  DataBindStatus bind_status;
} chttp_rpc_service_client_encode_context;

static int chttp_rpc_service_client_scalar_kind(const cmeta_data_desc *data) {
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

static int chttp_rpc_service_client_native_type_valid(
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

static int chttp_rpc_service_client_bit(
    const unsigned char *base, size_t bytes,
    size_t offset, unsigned bit, int *out) {
  if (base == NULL || out == NULL || offset >= bytes || bit >= 8u)
    return 0;
  *out = (base[offset] & (unsigned char)(1u << bit)) != 0u;
  return 1;
}

static int chttp_rpc_service_client_entry_state(
    const chttp_rpc_service_client_encode_context *context,
    const DataBindBindingPlanEntry *entry,
    DataBindBindingValueState *out_state,
    const void **out_value,
    size_t *out_bytes) {
  int present = 1;
  int is_null = 0;
  size_t bytes;

  if (context == NULL || entry == NULL || out_state == NULL ||
      out_value == NULL || out_bytes == NULL || entry->data == NULL ||
      entry->data->storage_type == NULL)
    return 0;

  bytes = entry->data->storage_type->size;
  if (entry->native_offset > context->request_bytes ||
      bytes > context->request_bytes - entry->native_offset)
    return 0;

  if (entry->has_presence &&
      !chttp_rpc_service_client_bit(
          context->request, context->request_bytes,
          entry->presence_offset, entry->presence_bit, &present))
    return 0;
  if (entry->has_null &&
      !chttp_rpc_service_client_bit(
          context->request, context->request_bytes,
          entry->null_offset, entry->null_bit, &is_null))
    return 0;
  if (!present && is_null) return 0;

  if (!present) {
    *out_state = DATA_BIND_VALUE_STATE_ABSENT;
    *out_value = NULL;
    *out_bytes = 0u;
    return 1;
  }
  if (is_null) {
    if (!entry->nullable) return 0;
    *out_state = DATA_BIND_VALUE_STATE_NULL;
    *out_value = NULL;
    *out_bytes = 0u;
    return 1;
  }

  *out_state = DATA_BIND_VALUE_STATE_VALUE;
  *out_value = context->request + entry->native_offset;
  *out_bytes = bytes;
  return 1;
}

static cserde_status chttp_rpc_service_client_bind_status(
    DataBindStatus status) {
  switch (status) {
  case DATA_BIND_OK:
    return CSERDE_OK;
  case DATA_BIND_ERR_LIMIT:
  case DATA_BIND_ERR_BUFFER_TOO_SMALL:
    return CSERDE_LIMIT_EXCEEDED;
  case DATA_BIND_ERR_INVALID_ARG:
  case DATA_BIND_ERR_PARSE:
  case DATA_BIND_ERR_SCHEMA:
  case DATA_BIND_ERR_TYPE_NOT_FOUND:
  case DATA_BIND_ERR_TYPE_MISMATCH:
  case DATA_BIND_ERR_VALIDATION:
    return CSERDE_INVALID_TOKEN;
  default:
    return CSERDE_SINK_ERROR;
  }
}

static cserde_status chttp_rpc_service_client_write_token(
    cserde_writer *writer, cserde_token token) {
  return cserde_writer_write(writer, &token);
}

static cserde_status chttp_rpc_service_client_encode_value(
    chttp_rpc_service_client_encode_context *context,
    const DataBindBindingPlanEntry *entry,
    cserde_writer *writer) {
  DataBindBindingValueState state = DATA_BIND_VALUE_STATE_ABSENT;
  DataBindNativeDiagnostic diagnostic = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  const void *value = NULL;
  size_t value_bytes = 0u;
  DataBindStatus status;

  if (!chttp_rpc_service_client_entry_state(
          context, entry, &state, &value, &value_bytes)) {
    context->bind_status = DATA_BIND_ERR_SCHEMA;
    return CSERDE_INVALID_TOKEN;
  }
  if (state == DATA_BIND_VALUE_STATE_ABSENT)
    return CSERDE_INVALID_STATE;
  if (state == DATA_BIND_VALUE_STATE_NULL)
    return chttp_rpc_service_client_write_token(
        writer, (cserde_token){.kind = CSERDE_NULL});

  status = data_bind_native_encode(
      context->native_options, entry->data,
      value, value_bytes, writer, &diagnostic);
  if (status != DATA_BIND_OK) context->bind_status = status;
  return chttp_rpc_service_client_bind_status(status);
}

static int chttp_rpc_service_client_entry_at_ordinal(
    const DataBindBindingPlan *binding, size_t ordinal,
    DataBindBindingPlanEntry *out) {
  size_t i;
  size_t count = data_bind_binding_plan_ingress_count(binding);
  for (i = 0u; i < count; ++i) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry))
      return 0;
    if (entry.address.ordinal == ordinal) {
      *out = entry;
      return 1;
    }
  }
  return 0;
}

static cserde_status chttp_rpc_service_client_encode_params(
    void *user, cserde_writer *writer) {
  chttp_rpc_service_client_encode_context *context =
      (chttp_rpc_service_client_encode_context *)user;
  size_t count;
  size_t i;
  cserde_status status;

  if (context == NULL || context->binding == NULL || writer == NULL)
    return CSERDE_INVALID_ARGUMENT;
  context->bind_status = DATA_BIND_OK;
  count = data_bind_binding_plan_ingress_count(context->binding);

  if (context->mode == CHTTP_RPC_SERVICE_PARAMS_OBJECT) {
    status = chttp_rpc_service_client_write_token(
        writer, (cserde_token){.kind = CSERDE_MAP_BEGIN});
    for (i = 0u; status == CSERDE_OK && i < count; ++i) {
      DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      DataBindBindingValueState state = DATA_BIND_VALUE_STATE_ABSENT;
      const void *value = NULL;
      size_t value_bytes = 0u;
      if (!data_bind_binding_plan_ingress_at(context->binding, i, &entry) ||
          !chttp_rpc_service_client_entry_state(
              context, &entry, &state, &value, &value_bytes)) {
        context->bind_status = DATA_BIND_ERR_SCHEMA;
        return CSERDE_INVALID_TOKEN;
      }
      if (state == DATA_BIND_VALUE_STATE_ABSENT) continue;
      status = chttp_rpc_service_client_write_token(
          writer,
          (cserde_token){
              .kind = CSERDE_STRING,
              .value.slice = {
                  (const unsigned char *)entry.address.name,
                  strlen(entry.address.name), CSERDE_VIEW_STABLE}});
      if (status == CSERDE_OK)
        status = chttp_rpc_service_client_encode_value(
            context, &entry, writer);
    }
    if (status == CSERDE_OK)
      status = chttp_rpc_service_client_write_token(
          writer, (cserde_token){.kind = CSERDE_MAP_END});
    return status;
  }

  if (context->mode == CHTTP_RPC_SERVICE_PARAMS_ARRAY) {
    size_t length = count;
    while (length != 0u) {
      DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      DataBindBindingValueState state = DATA_BIND_VALUE_STATE_ABSENT;
      const void *value = NULL;
      size_t value_bytes = 0u;
      if (!chttp_rpc_service_client_entry_at_ordinal(
              context->binding, length - 1u, &entry) ||
          !chttp_rpc_service_client_entry_state(
              context, &entry, &state, &value, &value_bytes)) {
        context->bind_status = DATA_BIND_ERR_SCHEMA;
        return CSERDE_INVALID_TOKEN;
      }
      if (state != DATA_BIND_VALUE_STATE_ABSENT) break;
      --length;
    }

    status = chttp_rpc_service_client_write_token(
        writer, (cserde_token){.kind = CSERDE_ARRAY_BEGIN});
    for (i = 0u; status == CSERDE_OK && i < length; ++i) {
      DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      DataBindBindingValueState state = DATA_BIND_VALUE_STATE_ABSENT;
      const void *value = NULL;
      size_t value_bytes = 0u;
      if (!chttp_rpc_service_client_entry_at_ordinal(
              context->binding, i, &entry) ||
          !chttp_rpc_service_client_entry_state(
              context, &entry, &state, &value, &value_bytes)) {
        context->bind_status = DATA_BIND_ERR_SCHEMA;
        return CSERDE_INVALID_TOKEN;
      }
      if (state == DATA_BIND_VALUE_STATE_ABSENT) {
        context->bind_status = DATA_BIND_ERR_TYPE_NOT_FOUND;
        return CSERDE_INVALID_TOKEN;
      }
      status = chttp_rpc_service_client_encode_value(
          context, &entry, writer);
    }
    if (status == CSERDE_OK)
      status = chttp_rpc_service_client_write_token(
          writer, (cserde_token){.kind = CSERDE_ARRAY_END});
    return status;
  }

  return CSERDE_INVALID_ARGUMENT;
}

static int chttp_rpc_service_client_plan_admit(
    const DataBindRpcMethodPlan *method_plan,
    const DataBindServiceNativeBinding *native,
    size_t request_bytes, size_t response_bytes,
    chttp_rpc_service_params_mode *out_mode) {
  const DataBindBindingPlan *binding;
  const cmeta_function_desc *function;
  size_t native_request_bytes = 0u;
  size_t native_response_bytes = 0u;
  size_t ingress_count;
  size_t egress_count;
  size_t i;
  int saw_ordinal = 0;
  int saw_name_only = 0;

  if (method_plan == NULL || native == NULL || out_mode == NULL ||
      native->size < sizeof(*native) ||
      native->abi_version != DATA_BIND_BINDING_PLAN_ABI_VERSION ||
      !cmeta_function_desc_valid(native->function) ||
      !chttp_rpc_service_client_native_type_valid(
          native->request, &native_request_bytes) ||
      !chttp_rpc_service_client_native_type_valid(
          native->response, &native_response_bytes) ||
      request_bytes < native_request_bytes ||
      response_bytes < native_response_bytes)
    return SALTS_EINVAL;

  binding = data_bind_rpc_method_plan_binding(method_plan);
  function = binding != NULL ? data_bind_binding_plan_function(binding) : NULL;
  if (binding == NULL || function == NULL ||
      !cmeta_function_desc_equal(function, native->function) ||
      data_bind_rpc_method_plan_wire_method(method_plan) == NULL ||
      data_bind_rpc_method_plan_wire_method(method_plan)[0] == '\0')
    return SALTS_EINVAL;

  if (function->param_count != (native->error_count == 0u ? 2u : 3u))
    return SALTS_ENOTSUP;

  if (data_bind_binding_plan_error_count(binding) != native->error_count)
    return SALTS_EINVAL;

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
            native->error_envelope_bytes - native->error_kind_bytes)
      return SALTS_ENOTSUP;
  }

  for (i = 0u; i < native->error_count; ++i) {
    DataBindRpcErrorMapping mapping = DATA_BIND_RPC_ERROR_MAPPING_INIT;
    const char *plan_error = data_bind_binding_plan_error_at(binding, i);
    size_t j;
    const cmeta_data_desc *error_data = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    if (native->errors == NULL || plan_error == NULL ||
        native->errors[i].size < sizeof(DataBindNativeErrorBinding) ||
        native->errors[i].idl_type_name == NULL ||
        strcmp(plan_error, native->errors[i].idl_type_name) != 0 ||
        native->errors[i].data_resolver == NULL ||
        native->errors[i].kind_value != (uint32_t)(i + 1u) ||
        !data_bind_rpc_method_plan_error_at(method_plan, i, &mapping))
      return SALTS_EINVAL;
    if (native->errors[i].data_resolver(&error_data, &error) != DATA_BIND_OK ||
        error_data == NULL || !cmeta_data_desc_valid(error_data) ||
        error_data->storage_type == NULL ||
        native->errors[i].payload_offset >
            native->error_envelope_bytes ||
        error_data->storage_type->size >
            native->error_envelope_bytes - native->errors[i].payload_offset ||
        !chttp_rpc_service_client_scalar_kind(error_data))
      return SALTS_ENOTSUP;
    for (j = 0u; j < i; ++j) {
      DataBindRpcErrorMapping previous = DATA_BIND_RPC_ERROR_MAPPING_INIT;
      if (!data_bind_rpc_method_plan_error_at(method_plan, j, &previous))
        return SALTS_EINVAL;
      if (previous.code == mapping.code) return SALTS_ENOTSUP;
    }
  }

  ingress_count = data_bind_binding_plan_ingress_count(binding);
  for (i = 0u; i < ingress_count; ++i) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    size_t bytes;
    size_t j;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "rpc.params") != 0 ||
        entry.address.binding_class != DATA_BIND_BINDING_VALUE ||
        entry.address.name == NULL || entry.address.name[0] == '\0' ||
        entry.function_param_index != 0u || entry.parameter_indirect ||
        entry.target_is_return || !chttp_rpc_service_client_scalar_kind(entry.data) ||
        entry.data->storage_type == NULL)
      return SALTS_ENOTSUP;
    bytes = entry.data->storage_type->size;
    if (entry.native_offset > native_request_bytes ||
        bytes > native_request_bytes - entry.native_offset ||
        (entry.has_presence &&
         (entry.presence_offset >= native_request_bytes ||
          entry.presence_bit >= 8u)) ||
        (entry.has_null &&
         (entry.null_offset >= native_request_bytes ||
          entry.null_bit >= 8u)))
      return SALTS_EINVAL;

    if (entry.address.ordinal == SIZE_MAX) {
      saw_name_only = 1;
      for (j = 0u; j < i; ++j) {
        DataBindBindingPlanEntry previous =
            DATA_BIND_BINDING_PLAN_ENTRY_INIT;
        if (!data_bind_binding_plan_ingress_at(binding, j, &previous))
          return SALTS_EINVAL;
        if (previous.address.ordinal == SIZE_MAX &&
            previous.address.name != NULL &&
            strcmp(previous.address.name, entry.address.name) == 0)
          return SALTS_ENOTSUP;
      }
    } else {
      saw_ordinal = 1;
      if (entry.address.ordinal >= ingress_count) return SALTS_ENOTSUP;
      for (j = 0u; j < i; ++j) {
        DataBindBindingPlanEntry previous = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
        if (!data_bind_binding_plan_ingress_at(binding, j, &previous))
          return SALTS_EINVAL;
        if (previous.address.ordinal == entry.address.ordinal)
          return SALTS_ENOTSUP;
      }
    }
  }
  if (saw_ordinal && saw_name_only) return SALTS_ENOTSUP;

  egress_count = data_bind_binding_plan_egress_count(binding);
  if (egress_count > 1u) return SALTS_ENOTSUP;
  if (egress_count == 1u) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    size_t bytes;
    if (!data_bind_binding_plan_egress_at(binding, 0u, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "rpc.result") != 0 ||
        entry.address.binding_class != DATA_BIND_BINDING_RESULT ||
        entry.target_is_return || entry.function_param_index != 1u ||
        !chttp_rpc_service_client_scalar_kind(entry.data) ||
        entry.data->storage_type == NULL)
      return SALTS_ENOTSUP;
    bytes = entry.data->storage_type->size;
    if (entry.native_offset > native_response_bytes ||
        bytes > native_response_bytes - entry.native_offset ||
        (entry.has_presence &&
         (entry.presence_offset >= native_response_bytes ||
          entry.presence_bit >= 8u)) ||
        (entry.has_null &&
         (entry.null_offset >= native_response_bytes ||
          entry.null_bit >= 8u)))
      return SALTS_EINVAL;
  }

  *out_mode = ingress_count == 0u
                  ? CHTTP_RPC_SERVICE_PARAMS_NONE
                  : saw_ordinal ? CHTTP_RPC_SERVICE_PARAMS_ARRAY
                                : CHTTP_RPC_SERVICE_PARAMS_OBJECT;
  return SALTS_OK;
}

static int chttp_rpc_service_client_bind_status_to_salts(
    DataBindStatus status) {
  switch (status) {
  case DATA_BIND_OK:
    return SALTS_OK;
  case DATA_BIND_ERR_OOM:
    return SALTS_ENOMEM;
  case DATA_BIND_ERR_LIMIT:
  case DATA_BIND_ERR_BUFFER_TOO_SMALL:
    return SALTS_EMSGSIZE;
  case DATA_BIND_ERR_INVALID_ARG:
  case DATA_BIND_ERR_SCHEMA:
  case DATA_BIND_ERR_TYPE_NOT_FOUND:
  case DATA_BIND_ERR_TYPE_MISMATCH:
  case DATA_BIND_ERR_VALIDATION:
    return SALTS_EINVAL;
  default:
    return SALTS_EPROTO;
  }
}

static int chttp_rpc_service_client_decode_success(
    const chttp_rpc_service_client_call_options *options,
    const crpc_response *response) {
  const DataBindBindingPlan *binding =
      data_bind_rpc_method_plan_binding(options->method_plan);
  const size_t count = data_bind_binding_plan_egress_count(binding);
  DataBindNativeDiagnostic diagnostic = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  DataBindStatus status;

  if (count == 0u) return SALTS_OK;
  if (response->value.result == NULL) return SALTS_EPROTO;

  {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    unsigned char *destination;
    size_t bytes;
    if (!data_bind_binding_plan_egress_at(binding, 0u, &entry) ||
        entry.data == NULL || entry.data->storage_type == NULL)
      return SALTS_EPROTO;
    bytes = entry.data->storage_type->size;
    destination = (unsigned char *)options->response + entry.native_offset;
    status = data_bind_native_decode(
        options->native_options, entry.data, response->value.result,
        destination, bytes, &diagnostic);
    if (status != DATA_BIND_OK)
      return chttp_rpc_service_client_bind_status_to_salts(status);
    if (entry.has_presence) {
      unsigned char *presence =
          (unsigned char *)options->response + entry.presence_offset;
      if (entry.presence_bit >= 8u) return SALTS_EPROTO;
      *presence |= (unsigned char)(1u << entry.presence_bit);
    }
    if (entry.has_null) {
      unsigned char *nulls =
          (unsigned char *)options->response + entry.null_offset;
      if (entry.null_bit >= 8u) return SALTS_EPROTO;
      *nulls &= (unsigned char)~(1u << entry.null_bit);
    }
  }
  return SALTS_OK;
}

static int chttp_rpc_service_client_typed_error_index(
    const DataBindRpcMethodPlan *plan, int64_t code, size_t *out_index) {
  size_t i;
  size_t count = data_bind_rpc_method_plan_error_count(plan);
  if (out_index == NULL) return 0;
  for (i = 0u; i < count; ++i) {
    DataBindRpcErrorMapping mapping = DATA_BIND_RPC_ERROR_MAPPING_INIT;
    if (!data_bind_rpc_method_plan_error_at(plan, i, &mapping))
      return 0;
    if ((int64_t)mapping.code == code) {
      *out_index = i;
      return 1;
    }
  }
  return 0;
}

static int chttp_rpc_service_client_decode_typed_error(
    const chttp_rpc_service_client_call_options *options,
    const crpc_response *response, size_t index) {
  const DataBindNativeErrorBinding *error_binding;
  const cmeta_data_desc *payload = NULL;
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  DataBindNativeDiagnostic diagnostic = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  size_t payload_bytes;
  unsigned char *payload_storage;
  uint32_t kind;
  DataBindStatus status;

  if (options->typed_error == NULL ||
      options->typed_error_bytes < options->native_binding->error_envelope_bytes ||
      options->native_binding->errors == NULL ||
      index >= options->native_binding->error_count)
    return SALTS_EMSGSIZE;

  error_binding = &options->native_binding->errors[index];
  status = error_binding->data_resolver(&payload, &bind_error);
  if (status != DATA_BIND_OK || payload == NULL ||
      !cmeta_data_desc_valid(payload) || payload->storage_type == NULL)
    return SALTS_EPROTO;
  payload_bytes = payload->storage_type->size;
  if (error_binding->payload_offset >
          options->native_binding->error_envelope_bytes ||
      payload_bytes >
          options->native_binding->error_envelope_bytes -
              error_binding->payload_offset)
    return SALTS_EPROTO;
  if (response->value.remote_error.data == NULL) return SALTS_EPROTO;

  memset(options->typed_error, 0, options->typed_error_bytes);
  payload_storage =
      (unsigned char *)options->typed_error + error_binding->payload_offset;
  status = data_bind_native_init(
      options->native_options, payload, payload_storage,
      payload_bytes, &diagnostic);
  if (status != DATA_BIND_OK)
    return chttp_rpc_service_client_bind_status_to_salts(status);
  status = data_bind_native_decode(
      options->native_options, payload,
      response->value.remote_error.data,
      payload_storage, payload_bytes, &diagnostic);
  if (status != DATA_BIND_OK) {
    (void)data_bind_native_clear(
        options->native_options, payload,
        payload_storage, payload_bytes, &diagnostic);
    memset(options->typed_error, 0, options->typed_error_bytes);
    return chttp_rpc_service_client_bind_status_to_salts(status);
  }

  kind = error_binding->kind_value;
  memcpy((unsigned char *)options->typed_error +
             options->native_binding->error_kind_offset,
         &kind, sizeof(kind));
  return SALTS_OK;
}

int chttp_rpc_service_client_call(
    crpc_client *client,
    const chttp_rpc_service_client_call_options *options,
    chttp_rpc_service_client_outcome *outcome,
    crpc_error *out_error) {
  const DataBindBindingPlan *binding;
  chttp_rpc_service_client_encode_context encode = {0};
  chttp_rpc_service_params_mode mode = CHTTP_RPC_SERVICE_PARAMS_NONE;
  crpc_options rpc_options = {0};
  crpc_response rpc_response = {0};
  crpc_method method = {0};
  DataBindNativeDiagnostic diagnostic = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  size_t native_request_bytes = 0u;
  size_t native_response_bytes = 0u;
  size_t typed_index = SIZE_MAX;
  DataBindStatus bind_status;
  int status;

  if (outcome == NULL || out_error == NULL)
    return SALTS_EINVAL;
  *outcome =
      (chttp_rpc_service_client_outcome)
          CHTTP_RPC_SERVICE_CLIENT_OUTCOME_INIT;
  *out_error = (crpc_error){0};

  if (client == NULL || options == NULL ||
      options->size < sizeof(*options) ||
      options->method_plan == NULL || options->native_binding == NULL ||
      options->native_options == NULL || options->request == NULL ||
      options->response == NULL || options->connection_uri == NULL ||
      options->authority == NULL || options->target == NULL)
    return SALTS_EINVAL;

  if (!chttp_rpc_service_client_native_type_valid(
          options->native_binding->request, &native_request_bytes) ||
      !chttp_rpc_service_client_native_type_valid(
          options->native_binding->response, &native_response_bytes))
    return SALTS_EINVAL;

  status = chttp_rpc_service_client_plan_admit(
      options->method_plan, options->native_binding,
      options->request_bytes, options->response_bytes, &mode);
  if (status != SALTS_OK) return status;

  if (options->native_binding->error_count != 0u &&
      (options->typed_error == NULL ||
       options->typed_error_bytes <
           options->native_binding->error_envelope_bytes))
    return SALTS_EMSGSIZE;

  memset(options->response, 0, options->response_bytes);
  bind_status = data_bind_native_init(
      options->native_options, options->native_binding->response->data,
      options->response, native_response_bytes, &diagnostic);
  if (bind_status != DATA_BIND_OK)
    return chttp_rpc_service_client_bind_status_to_salts(bind_status);
  if (options->typed_error != NULL && options->typed_error_bytes != 0u)
    memset(options->typed_error, 0, options->typed_error_bytes);

  binding = data_bind_rpc_method_plan_binding(options->method_plan);
  encode.binding = binding;
  encode.native_options = options->native_options;
  encode.request = (const unsigned char *)options->request;
  encode.request_bytes = options->request_bytes;
  encode.mode = mode;
  encode.bind_status = DATA_BIND_OK;

  method.name = data_bind_rpc_method_plan_wire_method(options->method_plan);
  method.service = NULL;
  method.callable = NULL;
  rpc_options = (crpc_options){
      .connection_uri = options->connection_uri,
      .authority = options->authority,
      .target = options->target,
      .method = method,
      .request_id = options->request_id,
      .metadata = options->metadata,
      .metadata_count = options->metadata_count,
      .deadline_ms = options->deadline_ms,
      .encode_params =
          mode == CHTTP_RPC_SERVICE_PARAMS_NONE
              ? NULL
              : chttp_rpc_service_client_encode_params,
      .params_user =
          mode == CHTTP_RPC_SERVICE_PARAMS_NONE ? NULL : &encode,
      .tls = options->tls,
      .protocol = options->protocol};

  status = crpc_request_reply(
      client, &rpc_options, &rpc_response, out_error);
  if (status != SALTS_OK) {
    if (encode.bind_status != DATA_BIND_OK) {
      status = chttp_rpc_service_client_bind_status_to_salts(
          encode.bind_status);
      *out_error = (crpc_error){
          .status = status, .stage = "rpc-service-encode"};
    }
    goto fail;
  }

  if (rpc_response.kind == CRPC_RESPONSE_RESULT) {
    status = chttp_rpc_service_client_decode_success(
        options, &rpc_response);
    if (status != SALTS_OK) {
      *out_error = (crpc_error){
          .status = status,
          .http_status = rpc_response.http_status,
          .stage = "rpc-service-result"};
      goto fail;
    }
    outcome->kind = CHTTP_RPC_SERVICE_CLIENT_SUCCESS;
    crpc_response_destroy(&rpc_response);
    return SALTS_OK;
  }

  if (rpc_response.kind != CRPC_RESPONSE_REMOTE_ERROR) {
    status = SALTS_EPROTO;
    *out_error = (crpc_error){
        .status = status,
        .http_status = rpc_response.http_status,
        .stage = "rpc-service-envelope"};
    goto fail;
  }

  outcome->remote_code = rpc_response.value.remote_error.code;
  if (chttp_rpc_service_client_typed_error_index(
          options->method_plan, outcome->remote_code, &typed_index)) {
    status = chttp_rpc_service_client_decode_typed_error(
        options, &rpc_response, typed_index);
    if (status != SALTS_OK) {
      *out_error = (crpc_error){
          .status = status,
          .http_status = rpc_response.http_status,
          .stage = "rpc-service-typed-error"};
      goto fail;
    }
    outcome->kind = CHTTP_RPC_SERVICE_CLIENT_TYPED_ERROR;
    outcome->typed_error_index = typed_index;
    outcome->typed_error =
        data_bind_binding_plan_error_at(binding, typed_index);
  } else {
    outcome->kind = CHTTP_RPC_SERVICE_CLIENT_REMOTE_ERROR;
  }

  crpc_response_destroy(&rpc_response);
  return SALTS_OK;

fail:
  crpc_response_destroy(&rpc_response);
  (void)data_bind_native_clear(
      options->native_options, options->native_binding->response->data,
      options->response, native_response_bytes, &diagnostic);
  return status;
}
