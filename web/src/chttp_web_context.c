#include <chttp_web/web.h>

#include <jinja_cmeta.h>
#include <salts/error_codes.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(chttp_web_sequence_view) == sizeof(JINJA_CMETA_SEQUENCE_VIEW),
               "CHttp::Web sequence bridge must match Jinja sequence view size");
_Static_assert(_Alignof(chttp_web_sequence_view) == _Alignof(JINJA_CMETA_SEQUENCE_VIEW),
               "CHttp::Web sequence bridge must match Jinja sequence view alignment");
_Static_assert(offsetof(chttp_web_sequence_view, data) ==
                   offsetof(JINJA_CMETA_SEQUENCE_VIEW, data),
               "sequence data offset mismatch");
_Static_assert(offsetof(chttp_web_sequence_view, count) ==
                   offsetof(JINJA_CMETA_SEQUENCE_VIEW, count),
               "sequence count offset mismatch");
_Static_assert(offsetof(chttp_web_sequence_view, stride) ==
                   offsetof(JINJA_CMETA_SEQUENCE_VIEW, stride),
               "sequence stride offset mismatch");
_Static_assert(offsetof(chttp_web_sequence_view, element) ==
                   offsetof(JINJA_CMETA_SEQUENCE_VIEW, element),
               "sequence element offset mismatch");

static chttp_web_status chttp_web_context_fail(
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

static bool chttp_web_string_is_zero(const void *object) {
  const chttp_web_string_view *value =
      (const chttp_web_string_view *)object;
  return value != NULL && value->data == NULL && value->size == 0u;
}

static cmeta_status chttp_web_string_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
  chttp_web_string_view *value = (chttp_web_string_view *)object;
  if (value == NULL || (size != 0u && data == NULL))
    return CMETA_INVALID_ARGUMENT;
  if (size > max_bytes) return CMETA_CAPACITY_EXCEEDED;
  if (!chttp_web_string_is_zero(value)) return CMETA_INVALID_ARGUMENT;
  value->data = (const char *)data;
  value->size = size;
  return CMETA_OK;
}

static void chttp_web_string_restore_zero(void *object) {
  chttp_web_string_view *value = (chttp_web_string_view *)object;
  if (value != NULL) *value = (chttp_web_string_view){0};
}

static cmeta_status chttp_web_string_read(
    const void *object, const unsigned char **out_data, size_t *out_size) {
  const chttp_web_string_view *value =
      (const chttp_web_string_view *)object;
  if (value == NULL || out_data == NULL || out_size == NULL ||
      (value->size != 0u && value->data == NULL))
    return CMETA_INVALID_ARGUMENT;
  *out_data = (const unsigned char *)value->data;
  *out_size = value->size;
  return CMETA_OK;
}

static cmeta_status chttp_web_string_init_zero(void *object) {
  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  *(chttp_web_string_view *)object = (chttp_web_string_view){0};
  return CMETA_OK;
}

static void chttp_web_string_move(void *destination, void *source) {
  chttp_web_string_view *to = (chttp_web_string_view *)destination;
  chttp_web_string_view *from = (chttp_web_string_view *)source;
  if (to == NULL || from == NULL) return;
  *to = *from;
  *from = (chttp_web_string_view){0};
}

static const cmeta_type_identity CHTTP_WEB_STRING_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.StringView.v1");

static const cmeta_type_desc CHTTP_WEB_STRING_TYPE = {
    "chttp_web_string_view",
    sizeof(chttp_web_string_view),
    _Alignof(chttp_web_string_view),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_WEB_STRING_ID};

static const cmeta_data_buffer_shape CHTTP_WEB_STRING_SHAPE = {
    CMETA_DATA_BUFFER_BORROWED};

static const cmeta_data_buffer_ops CHTTP_WEB_STRING_OPS = {
    .struct_size = sizeof(cmeta_data_buffer_ops),
    .abi_version = CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    .storage_type = &CHTTP_WEB_STRING_TYPE,
    .ownership = CMETA_DATA_BUFFER_BORROWED,
    .is_zero = chttp_web_string_is_zero,
    .assign = chttp_web_string_assign,
    .restore_zero = chttp_web_string_restore_zero,
    .read = chttp_web_string_read,
    .init_zero = chttp_web_string_init_zero,
    .move = chttp_web_string_move};

static const cmeta_data_desc CHTTP_WEB_STRING_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "chttp.web.StringView.data.v1",
    .display_name = "CHttp Web borrowed string",
    .kind = CMETA_DATA_STRING,
    .storage_type = &CHTTP_WEB_STRING_TYPE,
    .shape = &CHTTP_WEB_STRING_SHAPE,
    .buffer_ops = &CHTTP_WEB_STRING_OPS};

static const cmeta_type_identity CHTTP_WEB_SEQUENCE_ID =
    CMETA_TYPE_ID_ATOM_INIT("salts-utils.jinja-cmeta.SequenceView.v1");

static const cmeta_type_desc CHTTP_WEB_SEQUENCE_TYPE = {
    "JINJA_CMETA_SEQUENCE_VIEW",
    sizeof(chttp_web_sequence_view),
    _Alignof(chttp_web_sequence_view),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_WEB_SEQUENCE_ID};

static const unsigned char CHTTP_WEB_SEQUENCE_SHAPE = 1u;

static const cmeta_data_desc CHTTP_WEB_SEQUENCE_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "salts-utils.jinja-cmeta.SequenceView.data.v1",
    .display_name = "Jinja sequence view",
    .kind = CMETA_DATA_CUSTOM,
    .storage_type = &CHTTP_WEB_SEQUENCE_TYPE,
    .shape = &CHTTP_WEB_SEQUENCE_SHAPE};

static const cmeta_type_identity CHTTP_WEB_VALIDATION_ERROR_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.ValidationError.v1");

static const cmeta_type_desc CHTTP_WEB_VALIDATION_ERROR_TYPE = {
    "chttp_web_validation_error",
    sizeof(chttp_web_validation_error),
    _Alignof(chttp_web_validation_error),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_WEB_VALIDATION_ERROR_ID};

static const cmeta_field_desc CHTTP_WEB_VALIDATION_ERROR_LAYOUT_FIELDS[] = {
    {"field", "chttp_web_string_view",
     offsetof(chttp_web_validation_error, field),
     sizeof(((chttp_web_validation_error *)0)->field),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"message", "chttp_web_string_view",
     offsetof(chttp_web_validation_error, message),
     sizeof(((chttp_web_validation_error *)0)->message),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"global", "bool",
     offsetof(chttp_web_validation_error, global),
     sizeof(((chttp_web_validation_error *)0)->global),
     _Alignof(bool), NULL, NULL}};

static const cmeta_struct_desc CHTTP_WEB_VALIDATION_ERROR_LAYOUT = {
    "chttp_web_validation_error",
    sizeof(chttp_web_validation_error),
    _Alignof(chttp_web_validation_error),
    CHTTP_WEB_VALIDATION_ERROR_LAYOUT_FIELDS,
    3u};

static const cmeta_data_field_desc CHTTP_WEB_VALIDATION_ERROR_FIELDS[] = {
    {"chttp.web.ValidationError.field", "field",
     offsetof(chttp_web_validation_error, field), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.ValidationError.message", "message",
     offsetof(chttp_web_validation_error, message), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.ValidationError.global", "global",
     offsetof(chttp_web_validation_error, global), &cmeta_data_bool}};

static const cmeta_data_struct_shape CHTTP_WEB_VALIDATION_ERROR_SHAPE = {
    &CHTTP_WEB_VALIDATION_ERROR_LAYOUT,
    CHTTP_WEB_VALIDATION_ERROR_FIELDS,
    3u};

static const cmeta_data_desc CHTTP_WEB_VALIDATION_ERROR_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "chttp.web.ValidationError.data.v1",
    .display_name = "CHttp Web validation error",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &CHTTP_WEB_VALIDATION_ERROR_TYPE,
    .shape = &CHTTP_WEB_VALIDATION_ERROR_SHAPE};

static const cmeta_type_identity CHTTP_WEB_VALIDATION_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.Validation.v1");

static const cmeta_type_desc CHTTP_WEB_VALIDATION_TYPE = {
    "chttp_web_validation",
    sizeof(chttp_web_validation),
    _Alignof(chttp_web_validation),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_WEB_VALIDATION_ID};

static const cmeta_field_desc CHTTP_WEB_VALIDATION_LAYOUT_FIELDS[] = {
    {"valid", "bool",
     offsetof(chttp_web_validation, valid),
     sizeof(((chttp_web_validation *)0)->valid),
     _Alignof(bool), NULL, NULL},
    {"errors", "chttp_web_sequence_view",
     offsetof(chttp_web_validation, errors),
     sizeof(((chttp_web_validation *)0)->errors),
     _Alignof(chttp_web_sequence_view), NULL, NULL}};

static const cmeta_struct_desc CHTTP_WEB_VALIDATION_LAYOUT = {
    "chttp_web_validation",
    sizeof(chttp_web_validation),
    _Alignof(chttp_web_validation),
    CHTTP_WEB_VALIDATION_LAYOUT_FIELDS,
    2u};

static const cmeta_data_field_desc CHTTP_WEB_VALIDATION_FIELDS[] = {
    {"chttp.web.Validation.valid", "valid",
     offsetof(chttp_web_validation, valid), &cmeta_data_bool},
    {"chttp.web.Validation.errors", "errors",
     offsetof(chttp_web_validation, errors), &CHTTP_WEB_SEQUENCE_DATA}};

static const cmeta_data_struct_shape CHTTP_WEB_VALIDATION_SHAPE = {
    &CHTTP_WEB_VALIDATION_LAYOUT,
    CHTTP_WEB_VALIDATION_FIELDS,
    2u};

static const cmeta_data_desc CHTTP_WEB_VALIDATION_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "chttp.web.Validation.data.v1",
    .display_name = "CHttp Web validation result",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &CHTTP_WEB_VALIDATION_TYPE,
    .shape = &CHTTP_WEB_VALIDATION_SHAPE};

static const cmeta_type_identity CHTTP_WEB_NAMED_VALUE_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.NamedValue.v1");

static const cmeta_type_desc CHTTP_WEB_NAMED_VALUE_TYPE = {
    "chttp_web_named_value",
    sizeof(chttp_web_named_value),
    _Alignof(chttp_web_named_value),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_WEB_NAMED_VALUE_ID};

static const cmeta_field_desc CHTTP_WEB_NAMED_VALUE_LAYOUT_FIELDS[] = {
    {"name", "chttp_web_string_view",
     offsetof(chttp_web_named_value, name),
     sizeof(((chttp_web_named_value *)0)->name),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"value", "chttp_web_string_view",
     offsetof(chttp_web_named_value, value),
     sizeof(((chttp_web_named_value *)0)->value),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"present", "bool",
     offsetof(chttp_web_named_value, present),
     sizeof(((chttp_web_named_value *)0)->present),
     _Alignof(bool), NULL, NULL}};

static const cmeta_struct_desc CHTTP_WEB_NAMED_VALUE_LAYOUT = {
    "chttp_web_named_value",
    sizeof(chttp_web_named_value),
    _Alignof(chttp_web_named_value),
    CHTTP_WEB_NAMED_VALUE_LAYOUT_FIELDS,
    3u};

static const cmeta_data_field_desc CHTTP_WEB_NAMED_VALUE_FIELDS[] = {
    {"chttp.web.NamedValue.name", "name",
     offsetof(chttp_web_named_value, name), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.NamedValue.value", "value",
     offsetof(chttp_web_named_value, value), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.NamedValue.present", "present",
     offsetof(chttp_web_named_value, present), &cmeta_data_bool}};

static const cmeta_data_struct_shape CHTTP_WEB_NAMED_VALUE_SHAPE = {
    &CHTTP_WEB_NAMED_VALUE_LAYOUT,
    CHTTP_WEB_NAMED_VALUE_FIELDS,
    3u};

static const cmeta_data_desc CHTTP_WEB_NAMED_VALUE_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "chttp.web.NamedValue.data.v1",
    .display_name = "CHttp Web named value",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &CHTTP_WEB_NAMED_VALUE_TYPE,
    .shape = &CHTTP_WEB_NAMED_VALUE_SHAPE};

static const cmeta_type_identity CHTTP_WEB_PRINCIPAL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.Principal.v1");

static const cmeta_type_desc CHTTP_WEB_PRINCIPAL_TYPE = {
    "chttp_web_principal",
    sizeof(chttp_web_principal),
    _Alignof(chttp_web_principal),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_WEB_PRINCIPAL_ID};

static const cmeta_field_desc CHTTP_WEB_PRINCIPAL_LAYOUT_FIELDS[] = {
    {"authenticated", "bool",
     offsetof(chttp_web_principal, authenticated),
     sizeof(((chttp_web_principal *)0)->authenticated),
     _Alignof(bool), NULL, NULL},
    {"subject", "chttp_web_string_view",
     offsetof(chttp_web_principal, subject),
     sizeof(((chttp_web_principal *)0)->subject),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"role", "chttp_web_string_view",
     offsetof(chttp_web_principal, role),
     sizeof(((chttp_web_principal *)0)->role),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"display_name", "chttp_web_string_view",
     offsetof(chttp_web_principal, display_name),
     sizeof(((chttp_web_principal *)0)->display_name),
     _Alignof(chttp_web_string_view), NULL, NULL}};

static const cmeta_struct_desc CHTTP_WEB_PRINCIPAL_LAYOUT = {
    "chttp_web_principal",
    sizeof(chttp_web_principal),
    _Alignof(chttp_web_principal),
    CHTTP_WEB_PRINCIPAL_LAYOUT_FIELDS,
    4u};

static const cmeta_data_field_desc CHTTP_WEB_PRINCIPAL_FIELDS[] = {
    {"chttp.web.Principal.authenticated", "authenticated",
     offsetof(chttp_web_principal, authenticated), &cmeta_data_bool},
    {"chttp.web.Principal.subject", "subject",
     offsetof(chttp_web_principal, subject), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.Principal.role", "role",
     offsetof(chttp_web_principal, role), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.Principal.display_name", "display_name",
     offsetof(chttp_web_principal, display_name), &CHTTP_WEB_STRING_DATA}};

static const cmeta_data_struct_shape CHTTP_WEB_PRINCIPAL_SHAPE = {
    &CHTTP_WEB_PRINCIPAL_LAYOUT,
    CHTTP_WEB_PRINCIPAL_FIELDS,
    4u};

static const cmeta_data_desc CHTTP_WEB_PRINCIPAL_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "chttp.web.Principal.data.v1",
    .display_name = "CHttp Web browser principal",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &CHTTP_WEB_PRINCIPAL_TYPE,
    .shape = &CHTTP_WEB_PRINCIPAL_SHAPE};

static const cmeta_type_identity CHTTP_WEB_REQUEST_CONTEXT_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.RequestContext.v1");

static const cmeta_type_desc CHTTP_WEB_REQUEST_CONTEXT_TYPE = {
    "chttp_web_request_context",
    sizeof(chttp_web_request_context),
    _Alignof(chttp_web_request_context),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_WEB_REQUEST_CONTEXT_ID};

static const cmeta_field_desc CHTTP_WEB_REQUEST_CONTEXT_LAYOUT_FIELDS[] = {
    {"method", "chttp_web_string_view",
     offsetof(chttp_web_request_context, method),
     sizeof(((chttp_web_request_context *)0)->method),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"target", "chttp_web_string_view",
     offsetof(chttp_web_request_context, target),
     sizeof(((chttp_web_request_context *)0)->target),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"path", "chttp_web_string_view",
     offsetof(chttp_web_request_context, path),
     sizeof(((chttp_web_request_context *)0)->path),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"htmx", "bool",
     offsetof(chttp_web_request_context, htmx),
     sizeof(((chttp_web_request_context *)0)->htmx),
     _Alignof(bool), NULL, NULL},
    {"session_available", "bool",
     offsetof(chttp_web_request_context, session_available),
     sizeof(((chttp_web_request_context *)0)->session_available),
     _Alignof(bool), NULL, NULL},
    {"csrf_token", "chttp_web_string_view",
     offsetof(chttp_web_request_context, csrf_token),
     sizeof(((chttp_web_request_context *)0)->csrf_token),
     _Alignof(chttp_web_string_view), NULL, NULL},
    {"csrf_available", "bool",
     offsetof(chttp_web_request_context, csrf_available),
     sizeof(((chttp_web_request_context *)0)->csrf_available),
     _Alignof(bool), NULL, NULL},
    {"params", "chttp_web_sequence_view",
     offsetof(chttp_web_request_context, params),
     sizeof(((chttp_web_request_context *)0)->params),
     _Alignof(chttp_web_sequence_view), NULL, NULL},
    {"headers", "chttp_web_sequence_view",
     offsetof(chttp_web_request_context, headers),
     sizeof(((chttp_web_request_context *)0)->headers),
     _Alignof(chttp_web_sequence_view), NULL, NULL},
    {"session", "chttp_web_sequence_view",
     offsetof(chttp_web_request_context, session),
     sizeof(((chttp_web_request_context *)0)->session),
     _Alignof(chttp_web_sequence_view), NULL, NULL}};

static const cmeta_struct_desc CHTTP_WEB_REQUEST_CONTEXT_LAYOUT = {
    "chttp_web_request_context",
    sizeof(chttp_web_request_context),
    _Alignof(chttp_web_request_context),
    CHTTP_WEB_REQUEST_CONTEXT_LAYOUT_FIELDS,
    10u};

static const cmeta_data_field_desc CHTTP_WEB_REQUEST_CONTEXT_FIELDS[] = {
    {"chttp.web.RequestContext.method", "method",
     offsetof(chttp_web_request_context, method), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.RequestContext.target", "target",
     offsetof(chttp_web_request_context, target), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.RequestContext.path", "path",
     offsetof(chttp_web_request_context, path), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.RequestContext.htmx", "htmx",
     offsetof(chttp_web_request_context, htmx), &cmeta_data_bool},
    {"chttp.web.RequestContext.session_available", "session_available",
     offsetof(chttp_web_request_context, session_available), &cmeta_data_bool},
    {"chttp.web.RequestContext.csrf_token", "csrf_token",
     offsetof(chttp_web_request_context, csrf_token), &CHTTP_WEB_STRING_DATA},
    {"chttp.web.RequestContext.csrf_available", "csrf_available",
     offsetof(chttp_web_request_context, csrf_available), &cmeta_data_bool},
    {"chttp.web.RequestContext.params", "params",
     offsetof(chttp_web_request_context, params), &CHTTP_WEB_SEQUENCE_DATA},
    {"chttp.web.RequestContext.headers", "headers",
     offsetof(chttp_web_request_context, headers), &CHTTP_WEB_SEQUENCE_DATA},
    {"chttp.web.RequestContext.session", "session",
     offsetof(chttp_web_request_context, session), &CHTTP_WEB_SEQUENCE_DATA}};

static const cmeta_data_struct_shape CHTTP_WEB_REQUEST_CONTEXT_SHAPE = {
    &CHTTP_WEB_REQUEST_CONTEXT_LAYOUT,
    CHTTP_WEB_REQUEST_CONTEXT_FIELDS,
    10u};

static const cmeta_data_desc CHTTP_WEB_REQUEST_CONTEXT_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "chttp.web.RequestContext.data.v1",
    .display_name = "CHttp Web request context",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &CHTTP_WEB_REQUEST_CONTEXT_TYPE,
    .shape = &CHTTP_WEB_REQUEST_CONTEXT_SHAPE};

static chttp_web_string_view chttp_web_string_from_cstr(const char *value) {
  if (value == NULL) return (chttp_web_string_view){NULL, 0u};
  return (chttp_web_string_view){value, strlen(value)};
}

static const char *chttp_web_method_name(chttp_method method) {
  switch (method) {
  case CHTTP_METHOD_GET: return "GET";
  case CHTTP_METHOD_HEAD: return "HEAD";
  case CHTTP_METHOD_POST: return "POST";
  case CHTTP_METHOD_PUT: return "PUT";
  case CHTTP_METHOD_DELETE: return "DELETE";
  case CHTTP_METHOD_PATCH: return "PATCH";
  case CHTTP_METHOD_OPTIONS: return "OPTIONS";
  case CHTTP_METHOD_CONNECT: return "CONNECT";
  default: return NULL;
  }
}

static void chttp_web_named_value_set(
    chttp_web_named_value *out, const char *name, const char *value) {
  static const char empty[] = "";
  out->name = chttp_web_string_from_cstr(name);
  out->present = value != NULL;
  out->value = chttp_web_string_from_cstr(value != NULL ? value : empty);
}

bool chttp_web_request_is_htmx(const chttp_server_request_view *request) {
  const char *value;
  if (request == NULL) return false;
  value = chttp_server_request_header(request, "HX-Request");
  return value != NULL && strcmp(value, "true") == 0;
}

chttp_web_status chttp_web_request_context_init(
    chttp_web_request_context *context,
    const chttp_server_request_view *request,
    const chttp_web_request_context_options *options,
    chttp_web_error *error) {
  const char *method;
  size_t i;

  if (context != NULL) *context = (chttp_web_request_context){0};
  if (context == NULL || request == NULL || options == NULL ||
      options->size < sizeof(*options) || request->target == NULL ||
      request->path == NULL ||
      (request->header_count != 0u && request->headers == NULL) ||
      (request->param_count != 0u && request->params == NULL) ||
      (options->header_name_count != 0u && options->header_names == NULL) ||
      (options->session_key_count != 0u && options->session_keys == NULL))
    return chttp_web_context_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "request context arguments are invalid");

  method = chttp_web_method_name(request->method);
  if (method == NULL)
    return chttp_web_context_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "request method is invalid");

  if (request->param_count > options->param_capacity ||
      options->header_name_count > options->header_capacity ||
      options->session_key_count > options->session_capacity)
    return chttp_web_context_fail(
        error, CHTTP_WEB_CAPACITY, 0,
        "request context storage is too small");

  if ((request->param_count != 0u && options->param_storage == NULL) ||
      (options->header_name_count != 0u && options->header_storage == NULL) ||
      (options->session_key_count != 0u && options->session_storage == NULL))
    return chttp_web_context_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "request context storage pointer is missing");

  for (i = 0u; i < request->param_count; ++i) {
    const chttp_server_param *param = &request->params[i];
    if (param->name == NULL || param->value == NULL)
      return chttp_web_context_fail(
          error, CHTTP_WEB_INVALID_ARGUMENT, 0,
          "route parameter is invalid");
    chttp_web_named_value_set(
        &options->param_storage[i], param->name, param->value);
  }

  for (i = 0u; i < options->header_name_count; ++i) {
    const char *name = options->header_names[i];
    const char *value;
    if (name == NULL || name[0] == '\0')
      return chttp_web_context_fail(
          error, CHTTP_WEB_INVALID_ARGUMENT, 0,
          "selected header name is invalid");
    value = chttp_server_request_header(request, name);
    chttp_web_named_value_set(&options->header_storage[i], name, value);
  }

  for (i = 0u; i < options->session_key_count; ++i) {
    const char *key = options->session_keys[i];
    const char *value;
    if (key == NULL || key[0] == '\0')
      return chttp_web_context_fail(
          error, CHTTP_WEB_INVALID_ARGUMENT, 0,
          "selected session key is invalid");
    value = request->session != NULL
                ? chttp_session_get(request->session, key)
                : NULL;
    chttp_web_named_value_set(&options->session_storage[i], key, value);
  }

  context->method = chttp_web_string_from_cstr(method);
  context->target = chttp_web_string_from_cstr(request->target);
  context->path = chttp_web_string_from_cstr(request->path);
  context->htmx = chttp_web_request_is_htmx(request);
  context->session_available = request->session != NULL;
  {
    const char *csrf = chttp_web_csrf_token(request->session);
    context->csrf_token = chttp_web_string_from_cstr(csrf);
    context->csrf_available = csrf != NULL;
  }
  context->params = (chttp_web_sequence_view){
      options->param_storage, request->param_count,
      sizeof(chttp_web_named_value), &CHTTP_WEB_NAMED_VALUE_DATA};
  context->headers = (chttp_web_sequence_view){
      options->header_storage, options->header_name_count,
      sizeof(chttp_web_named_value), &CHTTP_WEB_NAMED_VALUE_DATA};
  context->session = (chttp_web_sequence_view){
      options->session_storage, options->session_key_count,
      sizeof(chttp_web_named_value), &CHTTP_WEB_NAMED_VALUE_DATA};

  return chttp_web_context_fail(error, CHTTP_WEB_OK, 0, NULL);
}

const cmeta_data_desc *chttp_web_principal_data(void) {
  return &CHTTP_WEB_PRINCIPAL_DATA;
}

const cmeta_data_desc *chttp_web_validation_error_data(void) {
  return &CHTTP_WEB_VALIDATION_ERROR_DATA;
}

const cmeta_data_desc *chttp_web_validation_data(void) {
  return &CHTTP_WEB_VALIDATION_DATA;
}

const cmeta_data_desc *chttp_web_request_context_data(void) {
  return &CHTTP_WEB_REQUEST_CONTEXT_DATA;
}

static chttp_web_status chttp_web_set_header(
    chttp_server_response *response, const char *name, const char *value,
    chttp_web_error *error) {
  int status;
  if (response == NULL || name == NULL || value == NULL || value[0] == '\0')
    return chttp_web_context_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "response header arguments are invalid");
  status = chttp_server_response_set_header(response, name, value);
  if (status != SALTS_OK)
    return chttp_web_context_fail(
        error, CHTTP_WEB_SERVER, status,
        "CHTTP rejected the response header");
  return chttp_web_context_fail(error, CHTTP_WEB_OK, 0, NULL);
}

chttp_web_status chttp_web_redirect(
    chttp_server_response *response, unsigned int status_code,
    const char *location, chttp_web_error *error) {
  chttp_web_status status;
  int server_status;

  if (status_code != 301u && status_code != 302u && status_code != 303u &&
      status_code != 307u && status_code != 308u)
    return chttp_web_context_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "redirect status must be 301, 302, 303, 307, or 308");

  status = chttp_web_set_header(response, "Location", location, error);
  if (status != CHTTP_WEB_OK) return status;

  server_status = chttp_server_reply(response, status_code, NULL, NULL, 0u);
  if (server_status != SALTS_OK)
    return chttp_web_context_fail(
        error, CHTTP_WEB_SERVER, server_status,
        "CHTTP rejected the redirect response");
  return chttp_web_context_fail(error, CHTTP_WEB_OK, 0, NULL);
}

chttp_web_status chttp_web_hx_redirect(
    chttp_server_response *response, const char *location,
    chttp_web_error *error) {
  return chttp_web_set_header(response, "HX-Redirect", location, error);
}

chttp_web_status chttp_web_hx_trigger(
    chttp_server_response *response, const char *trigger,
    chttp_web_error *error) {
  return chttp_web_set_header(response, "HX-Trigger", trigger, error);
}

chttp_web_status chttp_web_hx_retarget(
    chttp_server_response *response, const char *selector,
    chttp_web_error *error) {
  return chttp_web_set_header(response, "HX-Retarget", selector, error);
}

chttp_web_status chttp_web_render_error(
    chttp_web_renderer *renderer,
    chttp_server_response *response,
    unsigned int status_code,
    const char *template_name,
    const cmeta_data_desc *model_desc,
    const void *model,
    chttp_web_error *error) {
  if (status_code < 400u || status_code > 599u)
    return chttp_web_context_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0,
        "HTML error status must be in the 400..599 range");
  return chttp_web_render_response(
      renderer, response, template_name, model_desc, model,
      status_code, NULL, error);
}
