#include "chttp_document.h"
#include <salts/error_codes.h>
#include <string.h>

/* FormatPlan owns XML schema admission. Native token emission additionally
 * excludes BYTES, including below records/collections: the XML writer has no
 * binary representation. Bound descriptor traversal before route publication. */
static int xml_native_shape(const cmeta_data_desc *data, size_t depth) {
  if (data == NULL || depth > DATA_BIND_FORMAT_CURSOR_MAX_DEPTH) return 0;
  switch (data->kind) {
  case CMETA_DATA_BOOL: case CMETA_DATA_SINT: case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT: case CMETA_DATA_STRING: case CMETA_DATA_ENUM:
    return 1;
  case CMETA_DATA_STRUCT: {
    const cmeta_data_struct_shape *shape = data->shape;
    if (shape == NULL) return 0;
    for (size_t i = 0u; i < shape->field_count; ++i) {
      const cmeta_data_field_desc *field = cmeta_data_struct_field(shape, i);
      if (field == NULL || !xml_native_shape(field->value, depth + 1u)) return 0;
    }
    return 1;
  }
  case CMETA_DATA_SEQUENCE: case CMETA_DATA_SET:
    return xml_native_shape(cmeta_data_collection_element_data(data), depth + 1u);
  default:
    return 0;
  }
}

static int states_equal(const DataBindNativeStateBinding *left, size_t left_count,
    const DataBindNativeStateBinding *right, size_t right_count) {
  if (left_count != right_count ||
      (left_count != 0u && (left == NULL || right == NULL))) return 0;
  for (size_t i = 0u; i < left_count; ++i) {
    int found = 0;
    if (left[i].size < sizeof(*left) || left[i].field_name == NULL) return 0;
    for (size_t j = 0u; j < right_count; ++j) {
      if (right[j].size >= sizeof(*right) && right[j].field_name != NULL &&
          strcmp(left[i].field_name, right[j].field_name) == 0 &&
          left[i].byte_offset == right[j].byte_offset && left[i].bit == right[j].bit) {
        found = 1;
        break;
      }
    }
    if (!found) return 0;
  }
  return 1;
}

int chttp_document_message_admit(const DataBindMessagePlan *message,
    const DataBindNativeTypeBinding *expected,
    const chttp_document_plan *document, size_t field_count) {
  const DataBindNativeTypeBinding *actual = data_bind_message_plan_native_binding(message);
  if (actual == NULL || expected == NULL || actual->idl_type_name == NULL ||
      expected->idl_type_name == NULL || document->root_name == NULL ||
      strcmp(actual->idl_type_name, expected->idl_type_name) != 0 ||
      strcmp(actual->idl_type_name, document->root_name) != 0 ||
      !cmeta_data_desc_equal(actual->data, expected->data) ||
      data_bind_message_plan_field_count(message) != field_count ||
      !states_equal(actual->presence, actual->presence_count,
          expected->presence, expected->presence_count) ||
      !states_equal(actual->nulls, actual->null_count,
          expected->nulls, expected->null_count))
    return SALTS_EINVAL;
  return SALTS_OK;
}

int chttp_document_input_admit(const DataBindTransportPlan *transport,
    const DataBindBindingPlan *binding, chttp_document_plan *out) {
  DataBindTransportPlanInfo info = DATA_BIND_TRANSPORT_PLAN_INFO_INIT;
  DataBindFormatPlanInfo format = DATA_BIND_FORMAT_PLAN_INFO_INIT;
  if (!data_bind_transport_plan_info(transport, &info) ||
      !data_bind_format_plan_info(info.ingress, &format) || binding == NULL ||
      (format.format != DATA_BIND_FORMAT_JSON && format.format != DATA_BIND_FORMAT_XML))
    return SALTS_ENOTSUP;
  for (size_t i = 0u; i < data_bind_binding_plan_ingress_count(binding); ++i) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_ingress_at(binding, i, &entry) ||
        entry.address.space == NULL || strcmp(entry.address.space, "http.body") != 0 ||
        entry.schema_field == NULL || entry.address.name == NULL ||
        strcmp(entry.schema_field, entry.address.name) != 0 ||
        (format.format == DATA_BIND_FORMAT_XML && !xml_native_shape(entry.data, 1u)))
      return SALTS_ENOTSUP;
  }
  *out = (chttp_document_plan){info.ingress, format.format, format.type_name};
  return SALTS_OK;
}

int chttp_document_admit(const DataBindTransportPlan *transport,
    const DataBindBindingPlan *binding, const char *space,
    int allow_xml, chttp_document_plan *out) {
  DataBindTransportPlanInfo info = DATA_BIND_TRANSPORT_PLAN_INFO_INIT;
  if (!data_bind_transport_plan_info(transport, &info)) return SALTS_ENOTSUP;
  return chttp_document_format_admit(info.egress, binding, space, allow_xml, out);
}

int chttp_document_format_admit(const DataBindFormatPlan *plan,
    const DataBindBindingPlan *binding, const char *space,
    int allow_xml, chttp_document_plan *out) {
  DataBindFormatPlanInfo format = DATA_BIND_FORMAT_PLAN_INFO_INIT;
  if (!data_bind_format_plan_info(plan, &format) || binding == NULL ||
      data_bind_binding_plan_error_count(binding) != 0u ||
      (format.format != DATA_BIND_FORMAT_JSON &&
       (!allow_xml || format.format != DATA_BIND_FORMAT_XML)))
    return SALTS_ENOTSUP;
  for (size_t i = 0; i < data_bind_binding_plan_egress_count(binding); ++i) {
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    if (!data_bind_binding_plan_egress_at(binding, i, &entry) ||
        entry.address.space == NULL || strcmp(entry.address.space, space) != 0 ||
        entry.schema_field == NULL || entry.address.name == NULL ||
        strcmp(entry.schema_field, entry.address.name) != 0 ||
        (format.format == DATA_BIND_FORMAT_XML &&
            (format.has_nullable || !xml_native_shape(entry.data, 1u))))
      return SALTS_ENOTSUP;
  }
  *out = (chttp_document_plan){plan, format.format, format.type_name};
  return SALTS_OK;
}

static cserde_status bounded_write(void *context, const cserde_token *token) {
  chttp_document_writer *document = context;
  size_t bytes = sizeof(*token);
  if (token->kind == CSERDE_STRING || token->kind == CSERDE_BYTES) {
    if (token->value.slice.size > SIZE_MAX - bytes) return CSERDE_LIMIT_EXCEEDED;
    bytes += token->value.slice.size;
  }
  if (document->remaining_tokens == 0u || bytes > document->remaining_bytes)
    return CSERDE_LIMIT_EXCEEDED;
  document->remaining_bytes -= bytes;
  --document->remaining_tokens;
  return cserde_writer_write(data_bind_format_canonical_writer_writer(
      &document->canonical), token);
}

static cserde_status bounded_finish(void *context) {
  chttp_document_writer *document = context;
  return cserde_writer_finish(data_bind_format_canonical_writer_writer(
      &document->canonical));
}

static const cserde_writer_ops BOUNDED_OPS = {
    sizeof(cserde_writer_ops), CSERDE_WRITER_OPS_ABI_VERSION,
    bounded_write, bounded_finish};

static DataBindStatus document_status(cserde_status status) {
  if (status == CSERDE_OK) return DATA_BIND_OK;
  return status == CSERDE_LIMIT_EXCEEDED ? DATA_BIND_ERR_LIMIT : DATA_BIND_ERR_RUNTIME;
}

DataBindStatus chttp_document_begin(chttp_document_writer *document,
    const chttp_document_plan *plan, cserde_writer *target,
    const DataBindNativeOptions *options, size_t capacity, DataBindError *error) {
  *document = (chttp_document_writer){
      .canonical = DATA_BIND_FORMAT_CANONICAL_WRITER_INIT,
      .cursor = DATA_BIND_FORMAT_CURSOR_INIT,
      .options = options, .remaining_bytes = capacity,
      .remaining_tokens = options->max_items};
  DataBindStatus status = data_bind_format_canonical_writer_init_recursive(
      plan->format_plan, target, &document->canonical, &document->cursor, error);
  if (status != DATA_BIND_OK) return status;
  if (cserde_writer_init(&document->bounded, &BOUNDED_OPS, document) != CSERDE_OK)
    return DATA_BIND_ERR_RUNTIME;
  return document_status(cserde_writer_write(&document->bounded,
      &(const cserde_token){.kind = CSERDE_MAP_BEGIN}));
}

DataBindStatus chttp_document_field(chttp_document_writer *document,
    const DataBindBindingPlanEntry *entry, DataBindBindingValueState state,
    const void *value, size_t bytes, DataBindError *error) {
  if (state == DATA_BIND_VALUE_STATE_ABSENT) return DATA_BIND_OK;
  if (entry->address.binding_class == DATA_BIND_BINDING_ERROR ||
      (state != DATA_BIND_VALUE_STATE_VALUE && state != DATA_BIND_VALUE_STATE_NULL))
    return DATA_BIND_ERR_TYPE_MISMATCH;
  cserde_token key = {.kind = CSERDE_STRING,
      .value.slice = {(const unsigned char *)entry->schema_field,
          strlen(entry->schema_field), CSERDE_VIEW_STABLE}};
  DataBindStatus status = document_status(cserde_writer_write(&document->bounded, &key));
  if (status != DATA_BIND_OK) return status;
  if (state == DATA_BIND_VALUE_STATE_NULL)
    return document_status(cserde_writer_write(&document->bounded,
        &(const cserde_token){.kind = CSERDE_NULL}));
  DataBindNativeDiagnostic diagnostic = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  status = data_bind_native_encode(document->options, entry->data, value, bytes,
      &document->bounded, &diagnostic);
  if (status != DATA_BIND_OK && error != NULL) *error = diagnostic.error;
  return status;
}

DataBindStatus chttp_document_finish(chttp_document_writer *document) {
  DataBindStatus status = document_status(cserde_writer_write(&document->bounded,
      &(const cserde_token){.kind = CSERDE_MAP_END}));
  return status == DATA_BIND_OK
      ? document_status(cserde_writer_finish(&document->bounded)) : status;
}
