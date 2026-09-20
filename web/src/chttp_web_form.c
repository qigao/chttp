#include <chttp_web/web.h>

#include <data_bind.h>
#include <tbe_typed.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct chttp_web_json_writer {
  char *data;
  size_t size;
  size_t capacity;
} chttp_web_json_writer;

static chttp_web_status chttp_web_form_fail(
    chttp_web_error *error, chttp_web_status status, int native_status,
    size_t offset, const char *message) {
  if (error != NULL) {
    error->status = status;
    error->native_status = native_status;
    error->offset = offset;
    error->template_name[0] = '\0';
    if (message != NULL)
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
    else
      error->message[0] = '\0';
  }
  return status;
}

static int chttp_web_hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
  return -1;
}

static chttp_web_status chttp_web_form_decode_component(
    const unsigned char *input, size_t begin, size_t end,
    const chttp_web_form_parse_options *options,
    size_t *used, chttp_web_string_view *out, chttp_web_error *error) {
  const size_t start = *used;
  size_t i = begin;
  while (i < end) {
    unsigned char value = input[i];
    if (value == '+') {
      value = ' ';
      ++i;
    } else if (value == '%') {
      int high;
      int low;
      if (i + 2u >= end)
        return chttp_web_form_fail(
            error, CHTTP_WEB_FORM, 0, i,
            "form percent escape is truncated");
      high = chttp_web_hex_value(input[i + 1u]);
      low = chttp_web_hex_value(input[i + 2u]);
      if (high < 0 || low < 0)
        return chttp_web_form_fail(
            error, CHTTP_WEB_FORM, 0, i,
            "form percent escape is invalid");
      value = (unsigned char)((high << 4) | low);
      i += 3u;
    } else {
      ++i;
    }

    if (*used >= options->max_decoded_bytes ||
        *used >= options->byte_capacity)
      return chttp_web_form_fail(
          error, CHTTP_WEB_CAPACITY, 0, i,
          "decoded form bytes exceed configured storage");
    options->byte_storage[*used] = (char)value;
    ++(*used);
  }

  *out = (chttp_web_string_view){
      options->byte_storage + start, *used - start};
  return CHTTP_WEB_OK;
}

chttp_web_status chttp_web_form_parse(
    const void *data, size_t data_size,
    const chttp_web_form_parse_options *options,
    chttp_web_form *out_form, chttp_web_error *error) {
  const unsigned char *input = (const unsigned char *)data;
  size_t position = 0u;
  size_t count = 0u;
  size_t used = 0u;

  if (out_form != NULL) *out_form = (chttp_web_form){0};
  if (out_form == NULL || options == NULL ||
      options->size < sizeof(*options) ||
      (data_size != 0u && data == NULL))
    return chttp_web_form_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "form parser arguments are invalid");

  if (data_size > options->max_input_bytes)
    return chttp_web_form_fail(
        error, CHTTP_WEB_CAPACITY, 0, data_size,
        "form input exceeds configured byte limit");
  if (data_size == 0u)
    return chttp_web_form_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);

  if (options->max_pairs == 0u || options->max_decoded_bytes == 0u ||
      options->pair_storage == NULL || options->pair_capacity == 0u ||
      options->byte_storage == NULL || options->byte_capacity == 0u)
    return chttp_web_form_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "non-empty form requires bounded caller storage");

  while (position < data_size) {
    size_t end = position;
    size_t equals;
    chttp_web_form_pair pair = {0};
    chttp_web_status status;

    while (end < data_size && input[end] != '&') ++end;
    if (end == position)
      return chttp_web_form_fail(
          error, CHTTP_WEB_FORM, 0, position,
          "empty form segment is invalid");
    if (count >= options->max_pairs || count >= options->pair_capacity)
      return chttp_web_form_fail(
          error, CHTTP_WEB_CAPACITY, 0, position,
          "form pair count exceeds configured storage");

    equals = position;
    while (equals < end && input[equals] != '=') ++equals;
    if (equals == position)
      return chttp_web_form_fail(
          error, CHTTP_WEB_FORM, 0, position,
          "form field name must not be empty");

    status = chttp_web_form_decode_component(
        input, position, equals, options, &used, &pair.name, error);
    if (status != CHTTP_WEB_OK) return status;
    status = chttp_web_form_decode_component(
        input, equals < end ? equals + 1u : end, end,
        options, &used, &pair.value, error);
    if (status != CHTTP_WEB_OK) return status;

    options->pair_storage[count++] = pair;
    if (end == data_size) break;
    position = end + 1u;
    if (position == data_size)
      return chttp_web_form_fail(
          error, CHTTP_WEB_FORM, 0, end,
          "trailing form separator is invalid");
  }

  out_form->pairs = options->pair_storage;
  out_form->pair_count = count;
  out_form->decoded_bytes = used;
  return chttp_web_form_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
}

static int chttp_web_form_name_equal(
    chttp_web_string_view value, const char *name) {
  size_t size;
  if (name == NULL) return 0;
  size = strlen(name);
  return value.size == size &&
         (size == 0u || memcmp(value.data, name, size) == 0);
}

size_t chttp_web_form_count(const chttp_web_form *form, const char *name) {
  size_t result = 0u;
  size_t i;
  if (form == NULL || name == NULL ||
      (form->pair_count != 0u && form->pairs == NULL))
    return 0u;
  for (i = 0u; i < form->pair_count; ++i)
    if (chttp_web_form_name_equal(form->pairs[i].name, name)) ++result;
  return result;
}

const chttp_web_form_pair *chttp_web_form_get(
    const chttp_web_form *form, const char *name, size_t occurrence) {
  size_t seen = 0u;
  size_t i;
  if (form == NULL || name == NULL ||
      (form->pair_count != 0u && form->pairs == NULL))
    return NULL;
  for (i = 0u; i < form->pair_count; ++i) {
    if (!chttp_web_form_name_equal(form->pairs[i].name, name)) continue;
    if (seen++ == occurrence) return &form->pairs[i];
  }
  return NULL;
}

static int chttp_web_json_write(
    chttp_web_json_writer *writer, const void *data, size_t size) {
  if (writer == NULL || (size != 0u && data == NULL)) return 0;
  if (size > writer->capacity - writer->size) return 0;
  if (size != 0u) memcpy(writer->data + writer->size, data, size);
  writer->size += size;
  return 1;
}

static int chttp_web_json_char(chttp_web_json_writer *writer, char value) {
  return chttp_web_json_write(writer, &value, 1u);
}

static int chttp_web_json_string(
    chttp_web_json_writer *writer, chttp_web_string_view value) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  if (!chttp_web_json_char(writer, '"')) return 0;
  for (i = 0u; i < value.size; ++i) {
    const unsigned char c = (unsigned char)value.data[i];
    if (c == '"' || c == '\\') {
      const char escaped[2] = {'\\', (char)c};
      if (!chttp_web_json_write(writer, escaped, sizeof(escaped))) return 0;
    } else if (c < 0x20u) {
      const char escaped[6] = {
          '\\', 'u', '0', '0', hex[(c >> 4) & 0x0fu], hex[c & 0x0fu]};
      if (!chttp_web_json_write(writer, escaped, sizeof(escaped))) return 0;
    } else if (!chttp_web_json_char(writer, (char)c)) {
      return 0;
    }
  }
  return chttp_web_json_char(writer, '"');
}

static int chttp_web_json_integer_valid(
    chttp_web_string_view value, int allow_negative) {
  size_t i = 0u;
  if (value.size == 0u) return 0;
  if (value.data[i] == '-') {
    if (!allow_negative || ++i == value.size) return 0;
  }
  if (value.data[i] == '0')
    return i + 1u == value.size;
  if (value.data[i] < '1' || value.data[i] > '9') return 0;
  for (++i; i < value.size; ++i)
    if (value.data[i] < '0' || value.data[i] > '9') return 0;
  return 1;
}

static int chttp_web_json_number_valid(chttp_web_string_view value) {
  size_t i = 0u;
  if (value.size == 0u) return 0;
  if (value.data[i] == '-' && ++i == value.size) return 0;

  if (value.data[i] == '0') {
    ++i;
  } else {
    if (value.data[i] < '1' || value.data[i] > '9') return 0;
    while (++i < value.size &&
           value.data[i] >= '0' && value.data[i] <= '9') {}
  }

  if (i < value.size && value.data[i] == '.') {
    if (++i == value.size ||
        value.data[i] < '0' || value.data[i] > '9')
      return 0;
    while (++i < value.size &&
           value.data[i] >= '0' && value.data[i] <= '9') {}
  }

  if (i < value.size &&
      (value.data[i] == 'e' || value.data[i] == 'E')) {
    if (++i == value.size) return 0;
    if ((value.data[i] == '+' || value.data[i] == '-') &&
        ++i == value.size)
      return 0;
    if (value.data[i] < '0' || value.data[i] > '9') return 0;
    while (++i < value.size &&
           value.data[i] >= '0' && value.data[i] <= '9') {}
  }
  return i == value.size;
}

static int chttp_web_json_value(
    chttp_web_json_writer *writer, TbeTypedKind kind,
    chttp_web_string_view value) {
  switch (kind) {
  case TBE_TYPED_STRING:
  case TBE_TYPED_ENUM:
  case TBE_TYPED_UUID:
    return chttp_web_json_string(writer, value);
  case TBE_TYPED_BOOL:
    if ((value.size == 4u && memcmp(value.data, "true", 4u) == 0) ||
        (value.size == 5u && memcmp(value.data, "false", 5u) == 0))
      return chttp_web_json_write(writer, value.data, value.size);
    return -1;
  case TBE_TYPED_I8:
  case TBE_TYPED_I16:
  case TBE_TYPED_I32:
  case TBE_TYPED_I64:
    if (!chttp_web_json_integer_valid(value, 1)) return -1;
    return chttp_web_json_write(writer, value.data, value.size);
  case TBE_TYPED_U8:
  case TBE_TYPED_U16:
  case TBE_TYPED_U32:
  case TBE_TYPED_U64:
    if (!chttp_web_json_integer_valid(value, 0)) return -1;
    return chttp_web_json_write(writer, value.data, value.size);
  case TBE_TYPED_F32:
  case TBE_TYPED_F64:
    if (!chttp_web_json_number_valid(value)) return -1;
    return chttp_web_json_write(writer, value.data, value.size);
  default:
    return -2;
  }
}

static const TbeTypedField *chttp_web_form_field(
    const TbeTypedType *type, chttp_web_string_view name) {
  size_t i;
  if (type == NULL || type->fields == NULL) return NULL;
  for (i = 0u; i < type->field_count; ++i) {
    const TbeTypedField *field = &type->fields[i];
    if (field->name != NULL &&
        chttp_web_form_name_equal(name, field->name))
      return field;
  }
  return NULL;
}

static chttp_web_status chttp_web_form_build_json(
    const chttp_web_form *form, const TbeTypedType *type,
    const chttp_web_form_bind_options *options,
    size_t *out_size, chttp_web_error *error) {
  chttp_web_json_writer writer;
  size_t i;
  int first = 1;

  if (out_size != NULL) *out_size = 0u;
  if (form == NULL || type == NULL || options == NULL ||
      options->size < sizeof(*options) || out_size == NULL ||
      options->json_storage == NULL || options->json_capacity == 0u ||
      (form->pair_count != 0u && form->pairs == NULL))
    return chttp_web_form_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "form binding arguments are invalid");

  for (i = 0u; i < form->pair_count; ++i)
    if (chttp_web_form_field(type, form->pairs[i].name) == NULL)
      return chttp_web_form_fail(
          error, CHTTP_WEB_BIND, 0, i,
          "form contains a field absent from the typed descriptor");

  writer = (chttp_web_json_writer){
      options->json_storage, 0u, options->json_capacity};
  if (!chttp_web_json_char(&writer, '{'))
    return chttp_web_form_fail(
        error, CHTTP_WEB_CAPACITY, 0, 0u,
        "form JSON bridge exceeds caller storage");

  for (i = 0u; i < type->field_count; ++i) {
    const TbeTypedField *field = &type->fields[i];
    const size_t count = field->name != NULL
        ? chttp_web_form_count(form, field->name) : 0u;
    chttp_web_string_view field_name;
    size_t occurrence;

    if (count == 0u) continue;
    if (!first && !chttp_web_json_char(&writer, ','))
      goto capacity;
    first = 0;

    field_name = (chttp_web_string_view){
        field->name, strlen(field->name)};
    if (!chttp_web_json_string(&writer, field_name) ||
        !chttp_web_json_char(&writer, ':'))
      goto capacity;

    if (field->kind == TBE_TYPED_LIST ||
        field->kind == TBE_TYPED_SET ||
        field->kind == TBE_TYPED_FIXED_ARRAY) {
      if (field->element_kind == TBE_TYPED_OBJECT ||
          field->element_kind == TBE_TYPED_LIST ||
          field->element_kind == TBE_TYPED_SET ||
          field->element_kind == TBE_TYPED_MAP ||
          field->element_kind == TBE_TYPED_FIXED_ARRAY ||
          field->element_kind == TBE_TYPED_BYTES ||
          field->element_kind == TBE_TYPED_FIXED_BYTES)
        return chttp_web_form_fail(
            error, CHTTP_WEB_BIND, 0, i,
            "flat form collection element type is unsupported");
      if (field->kind == TBE_TYPED_FIXED_ARRAY &&
          count != field->fixed_count)
        return chttp_web_form_fail(
            error, CHTTP_WEB_BIND, 0, i,
            "fixed-array form field has the wrong item count");
      if (!chttp_web_json_char(&writer, '[')) goto capacity;
      for (occurrence = 0u; occurrence < count; ++occurrence) {
        const chttp_web_form_pair *pair =
            chttp_web_form_get(form, field->name, occurrence);
        const int wrote = chttp_web_json_value(
            &writer, field->element_kind, pair->value);
        if (wrote == -1)
          return chttp_web_form_fail(
              error, CHTTP_WEB_BIND, 0, i,
              "form collection value is not valid for its typed field");
        if (wrote == -2)
          return chttp_web_form_fail(
              error, CHTTP_WEB_BIND, 0, i,
              "flat form collection element type is unsupported");
        if (wrote == 0) goto capacity;
        if (occurrence + 1u < count &&
            !chttp_web_json_char(&writer, ','))
          goto capacity;
      }
      if (!chttp_web_json_char(&writer, ']')) goto capacity;
    } else {
      const chttp_web_form_pair *pair;
      int wrote;
      if (count != 1u)
        return chttp_web_form_fail(
            error, CHTTP_WEB_BIND, 0, i,
            "scalar form field must appear exactly once");
      if (field->kind == TBE_TYPED_OBJECT ||
          field->kind == TBE_TYPED_MAP ||
          field->kind == TBE_TYPED_BYTES ||
          field->kind == TBE_TYPED_FIXED_BYTES)
        return chttp_web_form_fail(
            error, CHTTP_WEB_BIND, 0, i,
            "flat form field type is unsupported");
      pair = chttp_web_form_get(form, field->name, 0u);
      wrote = chttp_web_json_value(&writer, field->kind, pair->value);
      if (wrote == -1)
        return chttp_web_form_fail(
            error, CHTTP_WEB_BIND, 0, i,
            "form value is not valid for its typed field");
      if (wrote == -2)
        return chttp_web_form_fail(
            error, CHTTP_WEB_BIND, 0, i,
            "flat form field type is unsupported");
      if (wrote == 0) goto capacity;
    }
  }

  if (!chttp_web_json_char(&writer, '}')) goto capacity;
  *out_size = writer.size;
  return chttp_web_form_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);

capacity:
  return chttp_web_form_fail(
      error, CHTTP_WEB_CAPACITY, 0, writer.size,
      "form JSON bridge exceeds caller storage");
}

static chttp_web_status chttp_web_form_databind_error(
    chttp_web_error *error, DataBindStatus status,
    const DataBindError *data_bind_error) {
  chttp_web_status mapped = CHTTP_WEB_BIND;
  if (status == DATA_BIND_ERR_INVALID_ARG)
    mapped = CHTTP_WEB_INVALID_ARGUMENT;
  else if (status == DATA_BIND_ERR_OOM)
    mapped = CHTTP_WEB_OUT_OF_MEMORY;
  else if (status == DATA_BIND_ERR_LIMIT ||
           status == DATA_BIND_ERR_BUFFER_TOO_SMALL)
    mapped = CHTTP_WEB_CAPACITY;

  return chttp_web_form_fail(
      error, mapped, (int)status, 0u,
      data_bind_error != NULL && data_bind_error->message[0] != '\0'
          ? data_bind_error->message
          : "DataBind rejected the form model");
}

chttp_web_status chttp_web_form_bind_typed(
    const chttp_web_form *form, DataBind *codec, const char *type_name,
    const TbeTypedType *type, void *destination,
    const chttp_web_form_bind_options *options, chttp_web_error *error) {
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  DataBindStatus status;
  size_t json_size = 0u;
  chttp_web_status web_status;

  if (codec == NULL || type_name == NULL || type_name[0] == '\0' ||
      type == NULL || destination == NULL)
    return chttp_web_form_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "typed form binding arguments are invalid");

  status = tbe_typed_validate_descriptor(type, &bind_error);
  if (status != DATA_BIND_OK)
    return chttp_web_form_databind_error(error, status, &bind_error);

  web_status = chttp_web_form_build_json(
      form, type, options, &json_size, error);
  if (web_status != CHTTP_WEB_OK) return web_status;

  bind_error = (DataBindError)DATA_BIND_ERROR_INIT;
  status = tbe_typed_parse_ex(
      codec, type_name, type, DATA_BIND_FORMAT_JSON,
      options->json_storage, json_size, 0u, destination, &bind_error);
  if (status != DATA_BIND_OK)
    return chttp_web_form_databind_error(error, status, &bind_error);
  return chttp_web_form_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
}

chttp_web_status chttp_web_form_bind_descriptor(
    const chttp_web_form *form, DataBind *codec, const char *type_name,
    const TbeTypedDescriptor *descriptor, void *destination,
    const chttp_web_form_bind_options *options, chttp_web_error *error) {
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  DataBindStatus status;
  size_t json_size = 0u;
  chttp_web_status web_status;

  if (codec == NULL || type_name == NULL || type_name[0] == '\0' ||
      descriptor == NULL || destination == NULL)
    return chttp_web_form_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "descriptor form binding arguments are invalid");

  status = tbe_typed_descriptor_validate(descriptor, &bind_error);
  if (status != DATA_BIND_OK)
    return chttp_web_form_databind_error(error, status, &bind_error);
  if (descriptor->overlay == NULL)
    return chttp_web_form_fail(
        error, CHTTP_WEB_BIND, 0, 0u,
        "descriptor has no DataBind overlay");

  web_status = chttp_web_form_build_json(
      form, descriptor->overlay, options, &json_size, error);
  if (web_status != CHTTP_WEB_OK) return web_status;

  bind_error = (DataBindError)DATA_BIND_ERROR_INIT;
  status = tbe_typed_descriptor_parse(
      codec, type_name, descriptor, DATA_BIND_FORMAT_JSON,
      options->json_storage, json_size, 0u, destination, &bind_error);
  if (status != DATA_BIND_OK)
    return chttp_web_form_databind_error(error, status, &bind_error);
  return chttp_web_form_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
}
