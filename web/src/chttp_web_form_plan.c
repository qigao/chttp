#include <chttp_web/web.h>

#include <data_bind_method_plan.h>

#include <cserde/reader.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_web_form_plan_reader {
  cserde_token token;
  int emitted;
} chttp_web_form_plan_reader;

typedef struct chttp_web_form_plan_provider {
  const chttp_web_form *form;
  const chttp_web_form_plan_options *options;
  chttp_web_form_plan_reader scalar;
} chttp_web_form_plan_provider;

static chttp_web_status chttp_web_form_plan_fail(
    chttp_web_error *error, chttp_web_status status, int native_status,
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

static void chttp_web_form_plan_bind_error(
    DataBindError *error, DataBindStatus status, const char *message) {
  if (error == NULL) return;
  if (error->size == 0u) error->size = sizeof(*error);
  error->code = status;
  error->line = -1;
  error->column = -1;
  error->path[0] = '\0';
  (void)snprintf(
      error->message, sizeof(error->message), "%s",
      message != NULL ? message : "");
}

static cserde_status chttp_web_form_plan_next(
    void *context, cserde_token *out) {
  chttp_web_form_plan_reader *reader =
      (chttp_web_form_plan_reader *)context;
  if (reader == NULL || out == NULL) return CSERDE_INVALID_ARGUMENT;
  if (reader->emitted) return CSERDE_DONE;
  *out = reader->token;
  reader->emitted = 1;
  return CSERDE_OK;
}

static const cserde_reader_ops CHTTP_WEB_FORM_PLAN_READER_OPS = {
    offsetof(cserde_reader_ops, next) + sizeof(cserde_reader_next_fn),
    CSERDE_READER_OPS_ABI_VERSION,
    chttp_web_form_plan_next};

static int chttp_web_form_plan_view_name(
    chttp_web_string_view value, const char *name) {
  size_t size;
  if (name == NULL) return 0;
  size = strlen(name);
  return value.size == size &&
         (size == 0u || memcmp(value.data, name, size) == 0);
}

static DataBindStatus chttp_web_form_plan_scalar_reader(
    chttp_web_form_plan_provider *provider,
    const DataBindBindingPlanEntry *entry,
    chttp_web_string_view value,
    cserde_reader *reader,
    DataBindError *error) {
  char *end = NULL;
  char *scratch;

  if (provider == NULL || provider->options == NULL ||
      entry == NULL || entry->data == NULL || reader == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  provider->scalar = (chttp_web_form_plan_reader){0};
  scratch = provider->options->scalar_storage;

  switch (entry->data->kind) {
  case CMETA_DATA_BOOL:
    provider->scalar.token.kind = CSERDE_BOOL;
    if ((value.size == 4u && memcmp(value.data, "true", 4u) == 0) ||
        (value.size == 1u && value.data[0] == '1'))
      provider->scalar.token.value.boolean = true;
    else if ((value.size == 5u && memcmp(value.data, "false", 5u) == 0) ||
             (value.size == 1u && value.data[0] == '0'))
      provider->scalar.token.value.boolean = false;
    else {
      chttp_web_form_plan_bind_error(
          error, DATA_BIND_ERR_PARSE, "invalid boolean form value");
      return DATA_BIND_ERR_PARSE;
    }
    break;

  case CMETA_DATA_SINT: {
    long long parsed;
    if (value.size == 0u || scratch == NULL ||
        value.size >= provider->options->scalar_capacity ||
        memchr(value.data, '\0', value.size) != NULL) {
      chttp_web_form_plan_bind_error(
          error, DATA_BIND_ERR_PARSE, "invalid signed integer form value");
      return DATA_BIND_ERR_PARSE;
    }
    memcpy(scratch, value.data, value.size);
    scratch[value.size] = '\0';
    errno = 0;
    parsed = strtoll(scratch, &end, 10);
    if (errno != 0 || end != scratch + value.size) {
      chttp_web_form_plan_bind_error(
          error, DATA_BIND_ERR_PARSE, "invalid signed integer form value");
      return DATA_BIND_ERR_PARSE;
    }
    provider->scalar.token.kind = CSERDE_SINT;
    provider->scalar.token.value.sint = (int64_t)parsed;
    break;
  }

  case CMETA_DATA_UINT: {
    unsigned long long parsed;
    if (value.size == 0u || value.data[0] == '-' || scratch == NULL ||
        value.size >= provider->options->scalar_capacity ||
        memchr(value.data, '\0', value.size) != NULL) {
      chttp_web_form_plan_bind_error(
          error, DATA_BIND_ERR_PARSE, "invalid unsigned integer form value");
      return DATA_BIND_ERR_PARSE;
    }
    memcpy(scratch, value.data, value.size);
    scratch[value.size] = '\0';
    errno = 0;
    parsed = strtoull(scratch, &end, 10);
    if (errno != 0 || end != scratch + value.size) {
      chttp_web_form_plan_bind_error(
          error, DATA_BIND_ERR_PARSE, "invalid unsigned integer form value");
      return DATA_BIND_ERR_PARSE;
    }
    provider->scalar.token.kind = CSERDE_UINT;
    provider->scalar.token.value.uint = (uint64_t)parsed;
    break;
  }

  case CMETA_DATA_FLOAT: {
    double parsed;
    if (value.size == 0u || scratch == NULL ||
        value.size >= provider->options->scalar_capacity ||
        memchr(value.data, '\0', value.size) != NULL) {
      chttp_web_form_plan_bind_error(
          error, DATA_BIND_ERR_PARSE, "invalid floating form value");
      return DATA_BIND_ERR_PARSE;
    }
    memcpy(scratch, value.data, value.size);
    scratch[value.size] = '\0';
    errno = 0;
    parsed = strtod(scratch, &end);
    if (errno != 0 || end != scratch + value.size) {
      chttp_web_form_plan_bind_error(
          error, DATA_BIND_ERR_PARSE, "invalid floating form value");
      return DATA_BIND_ERR_PARSE;
    }
    provider->scalar.token.kind = CSERDE_FLOAT;
    provider->scalar.token.value.floating = parsed;
    break;
  }

  case CMETA_DATA_STRING:
  case CMETA_DATA_ENUM:
    provider->scalar.token.kind = CSERDE_STRING;
    provider->scalar.token.value.slice.data =
        (const unsigned char *)value.data;
    provider->scalar.token.value.slice.size = value.size;
    provider->scalar.token.value.slice.lifetime = CSERDE_VIEW_STABLE;
    break;

  case CMETA_DATA_BYTES:
    provider->scalar.token.kind = CSERDE_BYTES;
    provider->scalar.token.value.slice.data =
        (const unsigned char *)value.data;
    provider->scalar.token.value.slice.size = value.size;
    provider->scalar.token.value.slice.lifetime = CSERDE_VIEW_STABLE;
    break;

  default:
    chttp_web_form_plan_bind_error(
        error, DATA_BIND_ERR_TYPE_MISMATCH,
        "generated form field type is unsupported");
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  return cserde_reader_init(
             reader, &CHTTP_WEB_FORM_PLAN_READER_OPS,
             &provider->scalar) == CSERDE_OK
             ? DATA_BIND_OK
             : DATA_BIND_ERR_RUNTIME;
}

static DataBindStatus chttp_web_form_plan_open_input(
    void *context, const DataBindBindingPlanEntry *entry,
    cserde_reader *reader, DataBindBindingValueState *state,
    DataBindError *error) {
  chttp_web_form_plan_provider *provider =
      (chttp_web_form_plan_provider *)context;
  const chttp_web_form_pair *pair;
  size_t count;

  if (provider == NULL || provider->form == NULL || entry == NULL ||
      entry->address.name == NULL || entry->address.name[0] == '\0' ||
      reader == NULL || state == NULL)
    return DATA_BIND_ERR_INVALID_ARG;

  *state = DATA_BIND_VALUE_STATE_ABSENT;
  count = chttp_web_form_count(provider->form, entry->address.name);
  if (count == 0u) return DATA_BIND_OK;
  if (count != 1u) {
    chttp_web_form_plan_bind_error(
        error, DATA_BIND_ERR_PARSE,
        "scalar generated form field occurs more than once");
    return DATA_BIND_ERR_PARSE;
  }

  pair = chttp_web_form_get(provider->form, entry->address.name, 0u);
  if (pair == NULL) return DATA_BIND_ERR_RUNTIME;

  {
    const DataBindStatus status = chttp_web_form_plan_scalar_reader(
        provider, entry, pair->value, reader, error);
    if (status == DATA_BIND_OK) *state = DATA_BIND_VALUE_STATE_VALUE;
    return status;
  }
}

static const char *chttp_web_form_plan_wire_name(
    const DataBindBindingPlan *binding, const char *schema_field) {
  size_t i;
  const size_t count = data_bind_binding_plan_ingress_count(binding);
  if (schema_field == NULL || schema_field[0] == '\0') return NULL;
  for (i = 0u; i < count; ++i) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
        entry.schema_field == NULL || entry.address.name == NULL)
      continue;
    if (strcmp(entry.schema_field, schema_field) == 0)
      return entry.address.name;
  }
  return NULL;
}

static chttp_web_status chttp_web_form_plan_validate_projection(
    const chttp_web_form *form, const DataBindBindingPlan *binding,
    chttp_web_validation *validation, chttp_web_error *error) {
  const size_t ingress_count = data_bind_binding_plan_ingress_count(binding);
  size_t i;
  size_t j;

  for (i = 0u; i < ingress_count; ++i) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
        entry.address.name == NULL || entry.address.name[0] == '\0')
      return chttp_web_form_plan_fail(
          error, CHTTP_WEB_BIND, DATA_BIND_ERR_SCHEMA,
          "generated form binding has no wire name");

    for (j = i + 1u; j < ingress_count; ++j) {
      DataBindBindingPlanEntry other = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      if (!data_bind_binding_plan_ingress_at(binding, j, &other) ||
          other.address.name == NULL)
        return chttp_web_form_plan_fail(
            error, CHTTP_WEB_BIND, DATA_BIND_ERR_SCHEMA,
            "generated form binding is malformed");
      if (strcmp(entry.address.name, other.address.name) == 0) {
        if (validation != NULL)
          (void)chttp_web_validation_add_global(
              validation, "generated form field names are ambiguous", NULL);
        return chttp_web_form_plan_fail(
            error, CHTTP_WEB_BIND, DATA_BIND_ERR_SCHEMA,
            "generated form field names are ambiguous");
      }
    }
  }

  for (i = 0u; i < form->pair_count; ++i) {
    size_t matches = 0u;
    for (j = 0u; j < ingress_count; ++j) {
      DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      if (!data_bind_binding_plan_ingress_at(binding, j, &entry) ||
          entry.address.name == NULL)
        return chttp_web_form_plan_fail(
            error, CHTTP_WEB_BIND, DATA_BIND_ERR_SCHEMA,
            "generated form binding is malformed");
      if (chttp_web_form_plan_view_name(
              form->pairs[i].name, entry.address.name))
        ++matches;
    }
    if (matches != 1u) {
      if (validation != NULL)
        (void)chttp_web_validation_add_global(
            validation,
            matches == 0u ? "form contains an unknown field"
                          : "form field mapping is ambiguous",
            NULL);
      return chttp_web_form_plan_fail(
          error, CHTTP_WEB_BIND, DATA_BIND_ERR_SCHEMA,
          matches == 0u ? "form contains an unknown generated field"
                        : "form field mapping is ambiguous");
    }
  }

  return CHTTP_WEB_OK;
}

static chttp_web_status chttp_web_form_plan_map_status(
    DataBindStatus status) {
  if (status == DATA_BIND_ERR_INVALID_ARG) return CHTTP_WEB_INVALID_ARGUMENT;
  if (status == DATA_BIND_ERR_OOM) return CHTTP_WEB_OUT_OF_MEMORY;
  if (status == DATA_BIND_ERR_LIMIT ||
      status == DATA_BIND_ERR_BUFFER_TOO_SMALL)
    return CHTTP_WEB_CAPACITY;
  return CHTTP_WEB_BIND;
}

chttp_web_status chttp_web_form_bind_method_plan(
    const chttp_web_form *form,
    const DataBindHttpMethodPlan *method_plan,
    const DataBindNativeOptions *native_options,
    DataBindBindingCallFrame *frame,
    const chttp_web_form_plan_options *options,
    chttp_web_validation *validation,
    DataBindBindingPlanDiagnostic *diagnostic,
    chttp_web_error *error) {
  DataBindBindingPlanDiagnostic local_diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  DataBindBindingPlanDiagnostic *active_diagnostic =
      diagnostic != NULL ? diagnostic : &local_diagnostic;
  const DataBindBindingPlan *binding;
  DataBindBindingProvider provider = DATA_BIND_BINDING_PROVIDER_INIT;
  chttp_web_form_plan_provider provider_context;
  DataBindStatus status;
  chttp_web_status web_status;

  if (form == NULL || method_plan == NULL || native_options == NULL ||
      frame == NULL || options == NULL ||
      options->size < sizeof(*options) ||
      options->scalar_storage == NULL || options->scalar_capacity < 2u ||
      (form->pair_count != 0u && form->pairs == NULL))
    return chttp_web_form_plan_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, DATA_BIND_ERR_INVALID_ARG,
        "generated form binding arguments are invalid");

  if (diagnostic != NULL)
    *diagnostic =
        (DataBindBindingPlanDiagnostic)DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;

  if (validation != NULL) {
    web_status = chttp_web_validation_reset(validation, error);
    if (web_status != CHTTP_WEB_OK) return web_status;
  }

  binding = data_bind_http_method_plan_binding(method_plan);
  if (binding == NULL)
    return chttp_web_form_plan_fail(
        error, CHTTP_WEB_BIND, DATA_BIND_ERR_SCHEMA,
        "HTTP MethodPlan has no canonical BindingPlan");

  web_status = chttp_web_form_plan_validate_projection(
      form, binding, validation, error);
  if (web_status != CHTTP_WEB_OK) return web_status;

  provider_context = (chttp_web_form_plan_provider){
      .form = form,
      .options = options};
  provider.context = &provider_context;
  provider.open_input = chttp_web_form_plan_open_input;

  status = data_bind_binding_plan_bind_inputs(
      binding, &provider, native_options, frame, active_diagnostic);
  if (status == DATA_BIND_OK)
    return chttp_web_form_plan_fail(error, CHTTP_WEB_OK, 0, NULL);

  if (validation != NULL) {
    const char *message =
        active_diagnostic->message[0] != '\0'
            ? active_diagnostic->message
            : "generated form binding failed";
    const char *wire_name = chttp_web_form_plan_wire_name(
        binding, active_diagnostic->schema_field);
    chttp_web_status validation_status;
    if (wire_name != NULL)
      validation_status = chttp_web_validation_add_field(
          validation, wire_name, message, NULL);
    else
      validation_status = chttp_web_validation_add_global(
          validation, message, NULL);
    if (validation_status != CHTTP_WEB_OK)
      return chttp_web_form_plan_fail(
          error, validation_status, (int)status,
          "generated form validation output exceeded caller storage");
  }

  return chttp_web_form_plan_fail(
      error, chttp_web_form_plan_map_status(status), (int)status,
      active_diagnostic->message[0] != '\0'
          ? active_diagnostic->message
          : "generated BindingPlan rejected the form");
}
