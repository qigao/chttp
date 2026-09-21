#include <chttp_web/web.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static chttp_web_status chttp_web_validation_fail(
    chttp_web_error *error,
    chttp_web_status status,
    size_t offset,
    const char *message) {
  if (error != NULL) {
    error->status = status;
    error->native_status = 0;
    error->offset = offset;
    error->template_name[0] = '\0';
    if (message != NULL)
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
    else
      error->message[0] = '\0';
  }
  return status;
}

static int chttp_web_validation_state_valid(
    const chttp_web_validation *validation) {
  if (validation == NULL ||
      validation->size < sizeof(*validation) ||
      validation->errors.count > validation->error_capacity ||
      validation->errors.data != validation->error_storage ||
      validation->errors.stride != sizeof(chttp_web_validation_error) ||
      validation->errors.element != chttp_web_validation_error_data() ||
      validation->byte_used > validation->byte_capacity)
    return 0;
  if (validation->error_capacity != 0u &&
      validation->error_storage == NULL)
    return 0;
  if (validation->byte_capacity != 0u &&
      validation->byte_storage == NULL)
    return 0;
  return 1;
}

chttp_web_status chttp_web_validation_init(
    chttp_web_validation *validation,
    chttp_web_validation_error *error_storage,
    size_t error_capacity,
    char *byte_storage,
    size_t byte_capacity,
    chttp_web_error *error) {
  if (validation == NULL ||
      (error_capacity != 0u && error_storage == NULL) ||
      (byte_capacity != 0u && byte_storage == NULL))
    return chttp_web_validation_fail(
        error,
        CHTTP_WEB_INVALID_ARGUMENT,
        0u,
        "validation storage arguments are invalid");

  *validation = (chttp_web_validation){
      .size = sizeof(chttp_web_validation),
      .valid = true,
      .errors = {
          .data = error_storage,
          .count = 0u,
          .stride = sizeof(chttp_web_validation_error),
          .element = chttp_web_validation_error_data()},
      .error_storage = error_storage,
      .error_capacity = error_capacity,
      .byte_storage = byte_storage,
      .byte_capacity = byte_capacity,
      .byte_used = 0u};

  return chttp_web_validation_fail(
      error, CHTTP_WEB_OK, 0u, NULL);
}

chttp_web_status chttp_web_validation_reset(
    chttp_web_validation *validation,
    chttp_web_error *error) {
  if (!chttp_web_validation_state_valid(validation))
    return chttp_web_validation_fail(
        error,
        CHTTP_WEB_INVALID_ARGUMENT,
        0u,
        "validation state is not initialized");

  validation->valid = true;
  validation->errors.count = 0u;
  validation->byte_used = 0u;
  return chttp_web_validation_fail(
      error, CHTTP_WEB_OK, 0u, NULL);
}

static chttp_web_status chttp_web_validation_add(
    chttp_web_validation *validation,
    const char *field,
    const char *message,
    bool global,
    chttp_web_error *error) {
  size_t field_size = 0u;
  size_t message_size;
  size_t required;
  size_t field_offset;
  size_t message_offset;
  chttp_web_validation_error value;

  if (!chttp_web_validation_state_valid(validation) ||
      message == NULL ||
      message[0] == '\0' ||
      (!global && (field == NULL || field[0] == '\0')))
    return chttp_web_validation_fail(
        error,
        CHTTP_WEB_INVALID_ARGUMENT,
        0u,
        "validation error arguments are invalid");

  if (!global) field_size = strlen(field);
  message_size = strlen(message);
  if (field_size > SIZE_MAX - message_size)
    return chttp_web_validation_fail(
        error,
        CHTTP_WEB_CAPACITY,
        validation->errors.count,
        "validation error bytes overflow size_t");
  required = field_size + message_size;

  if (validation->errors.count >= validation->error_capacity)
    return chttp_web_validation_fail(
        error,
        CHTTP_WEB_CAPACITY,
        validation->errors.count,
        "validation error count exceeds caller storage");
  if (required > validation->byte_capacity - validation->byte_used)
    return chttp_web_validation_fail(
        error,
        CHTTP_WEB_CAPACITY,
        validation->byte_used,
        "validation error bytes exceed caller storage");

  field_offset = validation->byte_used;
  message_offset = field_offset + field_size;
  if (field_size != 0u)
    memcpy(validation->byte_storage + field_offset, field, field_size);
  if (message_size != 0u)
    memcpy(validation->byte_storage + message_offset, message, message_size);

  value = (chttp_web_validation_error){
      .field = global
          ? (chttp_web_string_view){NULL, 0u}
          : (chttp_web_string_view){
                validation->byte_storage + field_offset, field_size},
      .message = (chttp_web_string_view){
          validation->byte_storage + message_offset, message_size},
      .global = global};

  validation->error_storage[validation->errors.count] = value;
  ++validation->errors.count;
  validation->byte_used += required;
  validation->valid = false;

  return chttp_web_validation_fail(
      error, CHTTP_WEB_OK, 0u, NULL);
}

chttp_web_status chttp_web_validation_add_field(
    chttp_web_validation *validation,
    const char *field,
    const char *message,
    chttp_web_error *error) {
  return chttp_web_validation_add(
      validation, field, message, false, error);
}

chttp_web_status chttp_web_validation_add_global(
    chttp_web_validation *validation,
    const char *message,
    chttp_web_error *error) {
  return chttp_web_validation_add(
      validation, NULL, message, true, error);
}

static int chttp_web_validation_field_equal(
    chttp_web_string_view value,
    const char *field) {
  const size_t field_size = field != NULL ? strlen(field) : 0u;
  return field != NULL &&
         value.size == field_size &&
         (field_size == 0u ||
          memcmp(value.data, field, field_size) == 0);
}

size_t chttp_web_validation_field_count(
    const chttp_web_validation *validation,
    const char *field) {
  size_t count = 0u;
  size_t i;
  if (!chttp_web_validation_state_valid(validation) ||
      field == NULL ||
      field[0] == '\0')
    return 0u;

  for (i = 0u; i < validation->errors.count; ++i) {
    const chttp_web_validation_error *item =
        &validation->error_storage[i];
    if (!item->global &&
        chttp_web_validation_field_equal(item->field, field))
      ++count;
  }
  return count;
}

const chttp_web_validation_error *chttp_web_validation_field_get(
    const chttp_web_validation *validation,
    const char *field,
    size_t occurrence) {
  size_t seen = 0u;
  size_t i;
  if (!chttp_web_validation_state_valid(validation) ||
      field == NULL ||
      field[0] == '\0')
    return NULL;

  for (i = 0u; i < validation->errors.count; ++i) {
    const chttp_web_validation_error *item =
        &validation->error_storage[i];
    if (item->global ||
        !chttp_web_validation_field_equal(item->field, field))
      continue;
    if (seen++ == occurrence) return item;
  }
  return NULL;
}

size_t chttp_web_validation_global_count(
    const chttp_web_validation *validation) {
  size_t count = 0u;
  size_t i;
  if (!chttp_web_validation_state_valid(validation)) return 0u;
  for (i = 0u; i < validation->errors.count; ++i)
    if (validation->error_storage[i].global) ++count;
  return count;
}
