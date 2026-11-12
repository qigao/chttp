#ifndef CHTTP_NATIVE_JSON_H
#define CHTTP_NATIVE_JSON_H
/* Private worker output adapter. All schema work is confined to mount. */
#include <data_bind_message_plan.h>
#include <data_bind_method_plan.h>
#include <data_bind_json_provider.h>
#include <data_bind_native.h>
#include <memory/buffer.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct chttp_native_json_plan {
  DataBindFormatPlan *format;
  const char *name; /* Method-owned native metadata, never codec storage. */
} chttp_native_json_plan;
typedef struct chttp_native_json_plans {
  chttp_native_json_plan value;
  chttp_native_json_plan *errors;
  size_t error_count;
} chttp_native_json_plans;

static inline DataBindStatus chttp_native_json_error(DataBindError *error, DataBindStatus status,
    const char *message) {
  if (error) { error->code = status; snprintf(error->message, sizeof(error->message), "%s", message); }
  return status;
}
static inline void chttp_native_json_plans_clear(chttp_native_json_plans *plans) {
  data_bind_format_plan_free(plans->value.format);
  for (size_t i = 0u; i < plans->error_count; ++i) data_bind_format_plan_free(plans->errors[i].format);
  free(plans->errors); *plans = (chttp_native_json_plans){0};
}
static inline DataBindStatus chttp_native_json_plan_compile(DataBind *codec, const char *name,
    const cmeta_data_desc *data, chttp_native_json_plan *out, DataBindError *error) {
  if (!name || !data || data->kind != CMETA_DATA_STRUCT)
    return chttp_native_json_error(error, DATA_BIND_ERR_SCHEMA, "Structured output requires a declared record");
  DataBindFormatPlan *format = NULL;
  DataBindStatus status = data_bind_format_plan_compile(codec, name, DATA_BIND_FORMAT_JSON, &format, error);
  if (status != DATA_BIND_OK) return status;
  DataBindFormatPlanInfo info = DATA_BIND_FORMAT_PLAN_INFO_INIT;
  if (!data_bind_format_plan_info(format, &info) || info.has_optional || info.has_nullable) {
    data_bind_format_plan_free(format);
    return chttp_native_json_error(error, DATA_BIND_ERR_SCHEMA, "Structured output needs explicit presence/null native bindings");
  }
  DataBindNativeTypeBinding native = DATA_BIND_NATIVE_TYPE_BINDING_INIT(name, data);
  DataBindMessagePlan *message = NULL;
  DataBindMessagePlanDiagnostic diagnostic = DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;
  status = data_bind_message_plan_compile(codec, name, &native, &message, &diagnostic);
  data_bind_message_plan_free(message);
  if (status != DATA_BIND_OK) {
    data_bind_format_plan_free(format);
    return chttp_native_json_error(error, status, diagnostic.message);
  }
  *out = (chttp_native_json_plan){format, name};
  return DATA_BIND_OK;
}
static inline DataBindStatus chttp_native_json_plans_compile(DataBind *codec,
    const DataBindBindingPlan *binding, const DataBindServiceNativeBinding *native,
    const DataBindTransportPlan *transport, chttp_native_json_plans *out, DataBindError *error) {
  if (!codec || !binding || !native || !native->response)
    return chttp_native_json_error(error, DATA_BIND_ERR_INVALID_ARG, "Worker output requires its MethodPlan codec");
  DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
  const size_t count = data_bind_binding_plan_egress_count(binding);
  if (count > 1u) return chttp_native_json_error(error, DATA_BIND_ERR_SCHEMA, "One projected output value is required");
  if (count && !data_bind_binding_plan_egress_at(binding, 0u, &entry))
    return chttp_native_json_error(error, DATA_BIND_ERR_SCHEMA, "Missing projected output entry");
  const bool structured = count && entry.data && entry.data->kind == CMETA_DATA_STRUCT;
  if (!structured && !native->error_count) return DATA_BIND_OK;
  DataBindTransportPlanInfo transport_info = DATA_BIND_TRANSPORT_PLAN_INFO_INIT;
  DataBindFormatPlanInfo format_info = DATA_BIND_FORMAT_PLAN_INFO_INIT;
  if (!data_bind_transport_plan_info(transport, &transport_info) ||
      !data_bind_format_plan_info(transport_info.egress, &format_info) || format_info.format != DATA_BIND_FORMAT_JSON)
    return chttp_native_json_error(error, DATA_BIND_ERR_SCHEMA, "Structured worker output requires the declared JSON format");
  DataBindStatus status = DATA_BIND_OK;
  if (structured) {
    if (entry.has_presence || entry.nullable)
      return chttp_native_json_error(error, DATA_BIND_ERR_SCHEMA, "Structured projected value must be required and non-nullable");
    DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;
    bool found = false;
    const size_t fields = data_bind_schema_field_count(codec, native->response->idl_type_name);
    for (size_t i = 0u; i < fields; ++i) {
      if (!data_bind_schema_field_at(codec, native->response->idl_type_name, i, &field))
        return chttp_native_json_error(error, DATA_BIND_ERR_SCHEMA, "Missing output schema field");
      if (strcmp(field.name, entry.schema_field) == 0) { found = true; break; }
    }
    if (!found || field.is_collection || field.is_map || field.is_group)
      return chttp_native_json_error(error, DATA_BIND_ERR_SCHEMA, "Projected record schema is unavailable");
    /* The FormatPlan owns a copy of field.type. Do not retain codec strings. */
    status = chttp_native_json_plan_compile(codec, field.type, entry.data, &out->value, error);
    if (status != DATA_BIND_OK) return status;
    out->value.name = NULL;
  }
  if (native->error_count > SIZE_MAX / sizeof(*out->errors)) {
    status = DATA_BIND_ERR_LIMIT; goto failed;
  }
  if (native->error_count) {
    out->errors = calloc(native->error_count, sizeof(*out->errors));
    if (!out->errors) { status = DATA_BIND_ERR_OOM; goto failed; }
    out->error_count = native->error_count;
  }
  for (size_t i = 0u; i < native->error_count; ++i) {
    const cmeta_data_desc *data = NULL;
    status = native->errors[i].data_resolver(&data, error);
    if (status != DATA_BIND_OK) goto failed;
    status = chttp_native_json_plan_compile(codec, native->errors[i].idl_type_name, data, &out->errors[i], error);
    if (status != DATA_BIND_OK) goto failed;
  }
  return DATA_BIND_OK;
failed:
  chttp_native_json_plans_clear(out);
  return status;
}
static inline const DataBindFormatPlan *chttp_native_json_plan_for(const chttp_native_json_plans *plans,
    const DataBindBindingPlanEntry *entry) {
  if (entry->address.binding_class != DATA_BIND_BINDING_ERROR) return plans->value.format;
  for (size_t i = 0u; i < plans->error_count; ++i)
    if (strcmp(plans->errors[i].name, entry->schema_field) == 0) return plans->errors[i].format;
  return NULL;
}

typedef struct chttp_native_json_sink {
  mem_buffer_t *buffer;
  size_t size, limit;
  bool overflow;
} chttp_native_json_sink;
static inline int chttp_native_json_write(const void *data, size_t size, void *user) {
  chttp_native_json_sink *sink = user;
  if (size > sink->limit - sink->size) { sink->overflow = true; return -1; }
  if (size) memcpy((unsigned char *)mem_buffer_data(sink->buffer) + sink->size, data, size);
  sink->size += size;
  return 0;
}
static inline DataBindStatus chttp_native_json_encode(const DataBindFormatPlan *plan,
    const DataBindNativeOptions *options, const cmeta_data_desc *data, const void *value, size_t bytes,
    size_t limit, mem_buffer_t **out, DataBindError *error) {
  if (!plan || !options || !limit || !out || *out)
    return chttp_native_json_error(error, DATA_BIND_ERR_INVALID_ARG, "Invalid structured output transaction");
  chttp_native_json_sink sink = {.buffer = mem_get_buffer(mem_global(), limit), .limit = limit};
  if (!sink.buffer) return chttp_native_json_error(error, DATA_BIND_ERR_OOM, "Cannot allocate bounded JSON output");
  DataBindFormatWriter writer = DATA_BIND_FORMAT_WRITER_INIT;
  DataBindFormatCanonicalWriter canonical = DATA_BIND_FORMAT_CANONICAL_WRITER_INIT;
  DataBindStatus status = data_bind_format_writer_open(data_bind_json_format_provider(),
      chttp_native_json_write, &sink, options->max_depth, &writer, error);
  if (status == DATA_BIND_OK)
    status = data_bind_format_canonical_writer_init(plan, writer.writer, &canonical, error);
  if (status == DATA_BIND_OK) {
    DataBindNativeDiagnostic diagnostic = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
    status = data_bind_native_encode(options, data, value, bytes,
        data_bind_format_canonical_writer_writer(&canonical), &diagnostic);
    if (status != DATA_BIND_OK && error) *error = diagnostic.error;
  }
  if (status == DATA_BIND_OK && cserde_writer_finish(data_bind_format_canonical_writer_writer(&canonical)) != CSERDE_OK)
    status = chttp_native_json_error(error, DATA_BIND_ERR_RUNTIME, "Incomplete structured output");
  DataBindError close_error = DATA_BIND_ERROR_INIT;
  const DataBindStatus closed = data_bind_format_writer_close(&writer, &close_error);
  if (status == DATA_BIND_OK && closed != DATA_BIND_OK) { status = closed; if (error) *error = close_error; }
  if (sink.overflow) status = chttp_native_json_error(error, DATA_BIND_ERR_LIMIT, "Structured output byte limit exceeded");
  if (status != DATA_BIND_OK) { mem_buffer_release(sink.buffer); return status; }
  mem_set_used(sink.buffer, sink.size); *out = sink.buffer;
  return DATA_BIND_OK;
}
#endif
