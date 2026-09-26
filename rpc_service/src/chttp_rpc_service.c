#include <chttp_rpc_service/service.h>

#include <salts/error_codes.h>

#include <cserde/cserde.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CHTTP_RPC_INVALID_PARAMS = -32602,
  CHTTP_RPC_NATIVE_STATUS = -32000
};

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
  unsigned char *native_workspace;
  DataBindNativeOptions native_options;
};

typedef struct chttp_rpc_service_provider {
  chttp_rpc_service_impl *service;
  const DataBindRpcMethodPlan *method_plan;
  const crpc_server_request_view *request;
  crpc_server_response *response;

  crpc_server_param_reader param_reader;
  int param_reader_open;

  int output_attempted;
  int output_active;
  int output_published;
  int response_completed;

  const DataBindBindingPlanEntry *staged_entry;
  DataBindBindingValueState staged_state;
  const void *staged_value;
  size_t staged_value_bytes;
  int staged_rpc_code;
} chttp_rpc_service_provider;

static void chttp_rpc_service_error_set(
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

static void chttp_rpc_service_close_param(
    chttp_rpc_service_provider *provider) {
  if (provider == NULL || !provider->param_reader_open) return;
  crpc_server_request_param_close(&provider->param_reader);
  provider->param_reader_open = 0;
}

static DataBindStatus chttp_rpc_service_open_input(
    void *context, const DataBindBindingPlanEntry *entry,
    cserde_reader *reader, DataBindBindingValueState *state,
    DataBindError *error) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  cserde_token first = {0};
  int present = 0;
  int rpc_status;

  if (provider == NULL || provider->request == NULL ||
      entry == NULL || reader == NULL || state == NULL ||
      entry->address.space == NULL ||
      strcmp(entry->address.space, "rpc.params") != 0)
    return DATA_BIND_ERR_INVALID_ARG;

  chttp_rpc_service_close_param(provider);
  provider->param_reader =
      (crpc_server_param_reader)CRPC_SERVER_PARAM_READER_INIT;
  *state = DATA_BIND_VALUE_STATE_ABSENT;

  rpc_status = crpc_server_request_param_open(
      provider->request, entry->address.name, entry->address.ordinal,
      &provider->param_reader, &present);
  if (rpc_status != SALTS_OK) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_PARSE,
        "JSON-RPC params selection failed");
    return DATA_BIND_ERR_PARSE;
  }
  if (!present) return DATA_BIND_OK;
  provider->param_reader_open = 1;

  if (cserde_reader_next(provider->param_reader.reader, &first) !=
      CSERDE_OK) {
    chttp_rpc_service_close_param(provider);
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_PARSE,
        "Selected JSON-RPC param has no value");
    return DATA_BIND_ERR_PARSE;
  }

  if (first.kind == CSERDE_NULL) {
    if (cserde_reader_next(provider->param_reader.reader, &first) !=
        CSERDE_DONE) {
      chttp_rpc_service_close_param(provider);
      chttp_rpc_service_error_set(
          error, DATA_BIND_ERR_PARSE,
          "Selected JSON-RPC null param has trailing tokens");
      return DATA_BIND_ERR_PARSE;
    }
    chttp_rpc_service_close_param(provider);
    *state = DATA_BIND_VALUE_STATE_NULL;
    return DATA_BIND_OK;
  }

  chttp_rpc_service_close_param(provider);
  provider->param_reader =
      (crpc_server_param_reader)CRPC_SERVER_PARAM_READER_INIT;
  present = 0;
  rpc_status = crpc_server_request_param_open(
      provider->request, entry->address.name, entry->address.ordinal,
      &provider->param_reader, &present);
  if (rpc_status != SALTS_OK || !present ||
      provider->param_reader.reader == NULL) {
    chttp_rpc_service_close_param(provider);
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Could not reopen selected JSON-RPC param");
    return DATA_BIND_ERR_RUNTIME;
  }

  provider->param_reader_open = 1;
  *reader = *provider->param_reader.reader;
  *state = DATA_BIND_VALUE_STATE_VALUE;
  return DATA_BIND_OK;
}

static DataBindStatus chttp_rpc_service_begin_output(
    void *context, DataBindError *error) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  (void)error;
  if (provider == NULL || provider->response == NULL ||
      provider->method_plan == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  provider->output_attempted = 1;
  provider->output_active = 1;
  provider->output_published = 0;
  provider->staged_entry = NULL;
  provider->staged_state = DATA_BIND_VALUE_STATE_ABSENT;
  provider->staged_value = NULL;
  provider->staged_value_bytes = 0u;
  provider->staged_rpc_code = 0;
  return DATA_BIND_OK;
}

static DataBindStatus chttp_rpc_service_write_output(
    void *context, const DataBindBindingPlanEntry *entry,
    DataBindBindingValueState state, const void *value, size_t value_bytes,
    DataBindError *error) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;

  if (provider == NULL || entry == NULL || !provider->output_active)
    return DATA_BIND_ERR_INVALID_ARG;
  if (provider->staged_entry != NULL) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "Phase-1 RPC MethodPlan admits one result/error root");
    return DATA_BIND_ERR_RUNTIME;
  }

  if (entry->address.binding_class != DATA_BIND_BINDING_ERROR) {
    if (entry->address.space == NULL ||
        strcmp(entry->address.space, "rpc.result") != 0) {
      chttp_rpc_service_error_set(
          error, DATA_BIND_ERR_SCHEMA,
          "Unsupported generated RPC egress address");
      return DATA_BIND_ERR_SCHEMA;
    }
  }

  if (state != DATA_BIND_VALUE_STATE_VALUE &&
      state != DATA_BIND_VALUE_STATE_NULL) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "RPC result/error requires VALUE or NULL state");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }
  if (state == DATA_BIND_VALUE_STATE_VALUE &&
      (value == NULL || entry->data == NULL)) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_INVALID_ARG,
        "RPC native result staging is unavailable");
    return DATA_BIND_ERR_INVALID_ARG;
  }

  provider->staged_entry = entry;
  provider->staged_state = state;
  provider->staged_value = value;
  provider->staged_value_bytes = value_bytes;
  return DATA_BIND_OK;
}

static cserde_status chttp_rpc_service_encode_staged(
    void *user, cserde_writer *writer) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)user;
  DataBindNativeDiagnostic diagnostic =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  DataBindStatus status;

  if (provider == NULL || writer == NULL ||
      provider->staged_entry == NULL)
    return CSERDE_INVALID_ARGUMENT;

  if (provider->staged_state == DATA_BIND_VALUE_STATE_NULL) {
    cserde_token token = {.kind = CSERDE_NULL};
    return cserde_writer_write(writer, &token);
  }

  status = data_bind_native_encode(
      &provider->service->native_options,
      provider->staged_entry->data,
      provider->staged_value,
      provider->staged_value_bytes,
      writer, &diagnostic);
  if (status == DATA_BIND_OK) return CSERDE_OK;
  if (diagnostic.source_status != CSERDE_OK)
    return diagnostic.source_status;
  return CSERDE_CALLBACK_ERROR;
}

static DataBindStatus chttp_rpc_service_commit_output(
    void *context, DataBindError *error) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  int status;

  if (provider == NULL || !provider->output_active ||
      provider->response == NULL || provider->method_plan == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  if (provider->staged_entry == NULL) {
    status = crpc_server_response_result(
        provider->response, NULL, NULL);
  } else if (provider->staged_entry->address.binding_class ==
             DATA_BIND_BINDING_ERROR) {
    DataBindRpcErrorMapping mapping =
        (DataBindRpcErrorMapping)DATA_BIND_RPC_ERROR_MAPPING_INIT;
    size_t index = provider->staged_entry->address.ordinal;
    if (index == SIZE_MAX ||
        !data_bind_rpc_method_plan_error_at(
            provider->method_plan, index, &mapping)) {
      chttp_rpc_service_error_set(
          error, DATA_BIND_ERR_SCHEMA,
          "Typed Service error has no RPC mapping");
      return DATA_BIND_ERR_SCHEMA;
    }
    provider->staged_rpc_code = mapping.code;
    status = crpc_server_response_error(
        provider->response, mapping.code,
        mapping.error_type != NULL ? mapping.error_type : "Service error",
        chttp_rpc_service_encode_staged, provider);
  } else {
    provider->staged_rpc_code = 0;
    status = crpc_server_response_result(
        provider->response, chttp_rpc_service_encode_staged, provider);
  }

  if (status != SALTS_OK) {
    chttp_rpc_service_error_set(
        error, DATA_BIND_ERR_RUNTIME,
        "CRPC response publication failed");
    return DATA_BIND_ERR_RUNTIME;
  }

  provider->output_active = 0;
  provider->output_published = 1;
  provider->response_completed = 1;
  return DATA_BIND_OK;
}

static void chttp_rpc_service_abort_output(void *context) {
  chttp_rpc_service_provider *provider =
      (chttp_rpc_service_provider *)context;
  if (provider == NULL) return;
  provider->output_active = 0;
  if (!provider->response_completed)
    provider->output_published = 0;
  provider->staged_entry = NULL;
  provider->staged_value = NULL;
  provider->staged_value_bytes = 0u;
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
  cserde_token token;
  if (status == NULL || writer == NULL) return CSERDE_INVALID_ARGUMENT;
  token = (cserde_token){.kind = CSERDE_SINT};
  token.value.sint = (int64_t)*status;
  return cserde_writer_write(writer, &token);
}

static int chttp_rpc_service_plan_supported(
    const DataBindRpcMethodPlan *plan) {
  const DataBindBindingPlan *binding;
  DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
  size_t i;
  size_t egress_count;

  if (plan == NULL ||
      data_bind_rpc_method_plan_wire_method(plan) == NULL)
    return 0;
  binding = data_bind_rpc_method_plan_binding(plan);
  if (binding == NULL) return 0;

  for (i = 0u; i < data_bind_binding_plan_ingress_count(binding); ++i) {
    entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "rpc.params") != 0 ||
        entry.address.name == NULL)
      return 0;
  }

  egress_count = data_bind_binding_plan_egress_count(binding);
  if (egress_count > 1u) return 0;
  if (egress_count == 1u) {
    entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_egress_at(binding, 0u, &entry) ||
        entry.address.space == NULL ||
        strcmp(entry.address.space, "rpc.result") != 0)
      return 0;
  }
  return 1;
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
  DataBindStatus bind_status;
  int mapped_code = 0;
  int status;

  if (record == NULL || record->owner == NULL ||
      record->method_plan == NULL || record->invoke == NULL ||
      request == NULL || response == NULL)
    return SALTS_EINVAL;

  provider_context = (chttp_rpc_service_provider){
      .service = record->owner,
      .method_plan = record->method_plan,
      .request = request,
      .response = response,
      .param_reader =
          (crpc_server_param_reader)CRPC_SERVER_PARAM_READER_INIT};

  provider.context = &provider_context;
  provider.open_input = chttp_rpc_service_open_input;
  provider.begin_output = chttp_rpc_service_begin_output;
  provider.write_output = chttp_rpc_service_write_output;
  provider.commit_output = chttp_rpc_service_commit_output;
  provider.abort_output = chttp_rpc_service_abort_output;

  binding = data_bind_rpc_method_plan_binding(record->method_plan);
  bind_status = record->invoke(
      record->user, binding, &provider,
      &record->owner->native_options, &outcome, &diagnostic);
  chttp_rpc_service_close_param(&provider_context);

  if (bind_status != DATA_BIND_OK) {
    if (!provider_context.output_attempted &&
        chttp_rpc_service_input_failure(bind_status))
      return crpc_server_response_error(
          response, CHTTP_RPC_INVALID_PARAMS,
          "Invalid params", NULL, NULL);
    return SALTS_EPROTO;
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_NATIVE_STATUS) {
    if (provider_context.response_completed)
      return SALTS_EPROTO;
    status = crpc_server_response_error(
        response, CHTTP_RPC_NATIVE_STATUS, "Native status",
        chttp_rpc_service_encode_native_status,
        &outcome.native_status);
    return status;
  }

  if (!data_bind_rpc_method_plan_code_for_outcome(
          record->method_plan, &outcome, &mapped_code))
    return SALTS_EPROTO;
  if (!provider_context.response_completed ||
      !provider_context.output_published ||
      provider_context.staged_rpc_code != mapped_code)
    return SALTS_EPROTO;

  return SALTS_OK;
}

int chttp_rpc_service_init(
    chttp_rpc_service *service,
    const chttp_rpc_service_config *config) {
  chttp_rpc_service_impl *impl;

  if (service == NULL || config == NULL ||
      config->size < sizeof(*config) ||
      config->method_capacity == 0u ||
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
    chttp_rpc_service *service, crpc_server *server,
    const chttp_rpc_service_mount *mount) {
  chttp_rpc_service_impl *impl;
  chttp_rpc_service_method_record *record;
  const char *wire_method;
  crpc_method method;
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
  if (wire_method == NULL || wire_method[0] == '\0')
    return SALTS_EINVAL;

  record = &impl->methods[impl->method_count];
  *record = (chttp_rpc_service_method_record){
      .owner = impl,
      .method_plan = mount->method_plan,
      .invoke = mount->invoke,
      .user = mount->user};

  method = (crpc_method){.name = wire_method};
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
  free(impl->methods);
  free(impl);
  service->impl = NULL;
  return SALTS_OK;
}
