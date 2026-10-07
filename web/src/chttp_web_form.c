#include <chttp_web/web.h>

#include <stdio.h>
#include <string.h>

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
