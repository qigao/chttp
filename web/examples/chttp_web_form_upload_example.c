#include <chttp_web/web.h>

#include <cmeta/struct.h>
#include <salts/error_codes.h>
#include <vstr.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WEB_FORMS_TIMEOUT_MS = 5000,
  WEB_FORMS_CONNECTIONS = 8,
  WEB_FORMS_COMMANDS = 32,
  WEB_FORMS_HEADERS = 20,
  WEB_FORMS_HEADER_BYTES = 4096,
  WEB_FORMS_BODY_BYTES = 64 * 1024,
  WEB_FORMS_SEND_BYTES = 128 * 1024,
  WEB_FORMS_COMMAND_BUFFER_BYTES = 256 * 1024,
  WEB_FORMS_BUFFER_CAPACITY_BYTES = 2 * 1024 * 1024,
  WEB_FORMS_PATH_BYTES = 1024,
  WEB_FORMS_PROFILE_NAME_BYTES = 64,
  WEB_FORMS_PROFILE_NOTE_BYTES = 256,
  WEB_FORMS_FORM_PAIRS = 8,
  WEB_FORMS_FORM_BYTES = 2048,
  WEB_FORMS_UPLOAD_SLOTS = 4,
  WEB_FORMS_UPLOAD_LABEL_BYTES = 96,
  WEB_FORMS_UPLOAD_FILENAME_BYTES = 192,
  WEB_FORMS_UPLOAD_FILE_BYTES = 4096,
  WEB_FORMS_VALIDATION_ERRORS = 4,
  WEB_FORMS_VALIDATION_BYTES = 512,
  WEB_FORMS_FLASH_BYTES = 256
};

typedef struct web_forms_model {
  chttp_web_request_context request;
  chttp_web_validation validation;
  vstr name;
  vstr note;
  vstr flash;
  vstr upload_label;
  vstr upload_filename;
} web_forms_model;

struct web_forms_app;

typedef enum web_forms_upload_part_kind {
  WEB_FORMS_UPLOAD_PART_NONE = 0,
  WEB_FORMS_UPLOAD_PART_LABEL,
  WEB_FORMS_UPLOAD_PART_FILE
} web_forms_upload_part_kind;

typedef struct web_forms_upload_slot {
  chttp_web_upload_request upload;
  struct web_forms_app *app;
  bool in_use;
  web_forms_upload_part_kind current_part;
  char label[WEB_FORMS_UPLOAD_LABEL_BYTES];
  size_t label_size;
  char filename[WEB_FORMS_UPLOAD_FILENAME_BYTES];
  size_t filename_size;
  unsigned char file[WEB_FORMS_UPLOAD_FILE_BYTES];
  size_t file_size;
} web_forms_upload_slot;

typedef struct web_forms_app {
  chttp_web_renderer renderer;
  cmeta_data_field_desc model_fields[7];
  cmeta_data_struct_shape model_shape;
  cmeta_data_desc model_desc;

  char profile_name[WEB_FORMS_PROFILE_NAME_BYTES];
  char profile_note[WEB_FORMS_PROFILE_NOTE_BYTES];

  char upload_label[WEB_FORMS_UPLOAD_LABEL_BYTES];
  char upload_filename[WEB_FORMS_UPLOAD_FILENAME_BYTES];
  unsigned char upload_file[WEB_FORMS_UPLOAD_FILE_BYTES];
  size_t upload_file_size;

  web_forms_upload_slot upload_slots[WEB_FORMS_UPLOAD_SLOTS];
} web_forms_app;

static const cmeta_type_identity WEB_FORMS_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.forms.example.model");

static const cmeta_type_desc WEB_FORMS_MODEL_TYPE = {
    "web_forms_model",
    sizeof(web_forms_model),
    _Alignof(web_forms_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_FORMS_MODEL_ID};

static const cmeta_field_desc WEB_FORMS_MODEL_LAYOUT_FIELDS[] = {
    {"request", "chttp_web_request_context",
     offsetof(web_forms_model, request),
     sizeof(((web_forms_model *)0)->request),
     _Alignof(chttp_web_request_context), NULL, NULL},
    {"validation", "chttp_web_validation",
     offsetof(web_forms_model, validation),
     sizeof(((web_forms_model *)0)->validation),
     _Alignof(chttp_web_validation), NULL, NULL},
    {"name", "vstr", offsetof(web_forms_model, name),
     sizeof(((web_forms_model *)0)->name), _Alignof(vstr), NULL, NULL},
    {"note", "vstr", offsetof(web_forms_model, note),
     sizeof(((web_forms_model *)0)->note), _Alignof(vstr), NULL, NULL},
    {"flash", "vstr", offsetof(web_forms_model, flash),
     sizeof(((web_forms_model *)0)->flash), _Alignof(vstr), NULL, NULL},
    {"upload_label", "vstr", offsetof(web_forms_model, upload_label),
     sizeof(((web_forms_model *)0)->upload_label), _Alignof(vstr), NULL, NULL},
    {"upload_filename", "vstr", offsetof(web_forms_model, upload_filename),
     sizeof(((web_forms_model *)0)->upload_filename), _Alignof(vstr), NULL, NULL}};

static const cmeta_struct_desc WEB_FORMS_MODEL_LAYOUT = {
    "web_forms_model",
    sizeof(web_forms_model),
    _Alignof(web_forms_model),
    WEB_FORMS_MODEL_LAYOUT_FIELDS,
    7u};

static cmeta_data_desc web_forms_model_desc(
    cmeta_data_field_desc fields[7],
    cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.forms.example.model.request",
      "request",
      offsetof(web_forms_model, request),
      chttp_web_request_context_data()};
  fields[1] = (cmeta_data_field_desc){
      "chttp.web.forms.example.model.validation",
      "validation",
      offsetof(web_forms_model, validation),
      chttp_web_validation_data()};
  fields[2] = (cmeta_data_field_desc){
      "chttp.web.forms.example.model.name",
      "name",
      offsetof(web_forms_model, name),
      chttp_web_vstr_cmeta_data()};
  fields[3] = (cmeta_data_field_desc){
      "chttp.web.forms.example.model.note",
      "note",
      offsetof(web_forms_model, note),
      chttp_web_vstr_cmeta_data()};
  fields[4] = (cmeta_data_field_desc){
      "chttp.web.forms.example.model.flash",
      "flash",
      offsetof(web_forms_model, flash),
      chttp_web_vstr_cmeta_data()};
  fields[5] = (cmeta_data_field_desc){
      "chttp.web.forms.example.model.upload_label",
      "upload_label",
      offsetof(web_forms_model, upload_label),
      chttp_web_vstr_cmeta_data()};
  fields[6] = (cmeta_data_field_desc){
      "chttp.web.forms.example.model.upload_filename",
      "upload_filename",
      offsetof(web_forms_model, upload_filename),
      chttp_web_vstr_cmeta_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_FORMS_MODEL_LAYOUT, fields, 7u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.forms.example.model.data",
      .display_name = "CHttp Web forms example model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_FORMS_MODEL_TYPE,
      .shape = shape};
}

static native_io_backend_kind web_forms_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_server_config web_forms_server_config(void) {
  chttp_server_config config = {0};
  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = WEB_FORMS_CONNECTIONS;
  config.network.backend = web_forms_backend();
  config.network.connection_capacity = WEB_FORMS_CONNECTIONS;
  config.network.command_capacity = WEB_FORMS_COMMANDS;
  config.network.request_capacity = WEB_FORMS_COMMANDS;
  config.network.completion_batch_capacity = WEB_FORMS_CONNECTIONS;
  config.network.event_capacity = WEB_FORMS_COMMANDS;
  config.network.max_send_bytes = WEB_FORMS_SEND_BYTES;
  config.network.command_buffer_bytes = WEB_FORMS_COMMAND_BUFFER_BYTES;
  config.network.receive_buffer_bytes = WEB_FORMS_HEADER_BYTES;
  config.network.connect_timeout_ms = WEB_FORMS_TIMEOUT_MS;
  config.network.read_timeout_ms = WEB_FORMS_TIMEOUT_MS;
  config.network.write_timeout_ms = WEB_FORMS_TIMEOUT_MS;
  config.route_capacity = 8u;
  config.middleware_capacity = 2u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = WEB_FORMS_PATH_BYTES;
  config.max_target_bytes = WEB_FORMS_PATH_BYTES;
  config.max_header_count = WEB_FORMS_HEADERS;
  config.max_header_bytes = WEB_FORMS_HEADER_BYTES;
  config.max_request_body_bytes = WEB_FORMS_BODY_BYTES;
  config.max_response_header_count = WEB_FORMS_HEADERS;
  config.max_response_header_bytes = WEB_FORMS_HEADER_BYTES;
  config.max_response_body_bytes = WEB_FORMS_BODY_BYTES;
  config.max_buffered_response_body_bytes = WEB_FORMS_BODY_BYTES;
  config.buffer_capacity_bytes = WEB_FORMS_BUFFER_CAPACITY_BYTES;
  config.session_capacity = WEB_FORMS_CONNECTIONS;
  config.session_entry_capacity = 6u;
  config.max_session_key_bytes = 64u;
  config.max_session_value_bytes = 1024u;
  config.session_idle_timeout_ms = 60000u;
  config.session_cookie_name = "chttp_web_forms";
  config.poll_slice_ms = 1u;
  return config;
}

static chttp_web_flash_config web_forms_flash_config(void) {
  chttp_web_flash_config config =
      (chttp_web_flash_config)CHTTP_WEB_FLASH_CONFIG_INIT;
  config.max_messages = 2u;
  return config;
}

static int web_forms_copy_view(
    char *output,
    size_t output_capacity,
    chttp_web_string_view value) {
  if (output == NULL || output_capacity == 0u ||
      value.size >= output_capacity ||
      (value.size != 0u && value.data == NULL))
    return SALTS_EMSGSIZE;
  if (value.size != 0u)
    memcpy(output, value.data, value.size);
  output[value.size] = '\0';
  return SALTS_OK;
}

static int web_forms_content_type_form(
    const chttp_server_request_view *request) {
  static const char expected[] = "application/x-www-form-urlencoded";
  const char *content_type =
      chttp_server_request_header(request, "Content-Type");
  const size_t expected_size = sizeof(expected) - 1u;
  if (content_type == NULL ||
      strncmp(content_type, expected, expected_size) != 0)
    return 0;
  return content_type[expected_size] == '\0' ||
         content_type[expected_size] == ';';
}

static chttp_web_status web_forms_parse_form(
    const chttp_server_request_view *request,
    chttp_web_form_pair pairs[WEB_FORMS_FORM_PAIRS],
    char bytes[WEB_FORMS_FORM_BYTES],
    chttp_web_form *form,
    chttp_web_error *error) {
  chttp_web_form_parse_options options =
      (chttp_web_form_parse_options)CHTTP_WEB_FORM_PARSE_OPTIONS_INIT;
  options.max_input_bytes = WEB_FORMS_FORM_BYTES;
  options.max_pairs = WEB_FORMS_FORM_PAIRS;
  options.max_decoded_bytes = WEB_FORMS_FORM_BYTES;
  options.pair_storage = pairs;
  options.pair_capacity = WEB_FORMS_FORM_PAIRS;
  options.byte_storage = bytes;
  options.byte_capacity = WEB_FORMS_FORM_BYTES;
  return chttp_web_form_parse(
      request->body, request->body_size, &options, form, error);
}

static int web_forms_consume_flash(
    chttp_session *session,
    char output[WEB_FORMS_FLASH_BYTES]) {
  chttp_web_flash_message messages[2];
  char bytes[WEB_FORMS_FLASH_BYTES];
  chttp_web_flash_buffer buffer =
      (chttp_web_flash_buffer)CHTTP_WEB_FLASH_BUFFER_INIT;
  chttp_web_flash_config config = web_forms_flash_config();
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  size_t count = 0u;
  chttp_web_status status;

  output[0] = '\0';
  buffer.message_storage = messages;
  buffer.message_capacity = 2u;
  buffer.byte_storage = bytes;
  buffer.byte_capacity = sizeof(bytes);
  status = chttp_web_flash_consume(
      session, &config, &buffer, &count, &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;
  if (count == 0u) return SALTS_OK;
  if (messages[0].text.size >= WEB_FORMS_FLASH_BYTES)
    return SALTS_ENOBUFS;
  memcpy(output, messages[0].text.data, messages[0].text.size);
  output[messages[0].text.size] = '\0';
  return SALTS_OK;
}

static int web_forms_render(
    web_forms_app *app,
    const chttp_server_request_view *request,
    chttp_server_response *response,
    const chttp_web_validation *validation,
    const char *name,
    const char *note,
    const char *flash,
    const char *template_name,
    unsigned int status_code) {
  chttp_web_validation empty_validation = CHTTP_WEB_VALIDATION_INIT;
  web_forms_model model = {0};
  chttp_web_request_context_options options =
      (chttp_web_request_context_options)
          CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  const char *csrf = NULL;
  chttp_web_status status;

  if (app == NULL || request == NULL || response == NULL ||
      template_name == NULL)
    return SALTS_EINVAL;

  status = chttp_web_csrf_ensure(request->session, &csrf, &error);
  if (status != CHTTP_WEB_OK || csrf == NULL)
    return SALTS_EIO;

  if (validation == NULL) {
    status = chttp_web_validation_init(
        &empty_validation, NULL, 0u, NULL, 0u, &error);
    if (status != CHTTP_WEB_OK) return SALTS_EIO;
    validation = &empty_validation;
  }

  status = chttp_web_request_context_init(
      &model.request, request, &options, &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  model.validation = *validation;
  model.name = vstr_from_cstr(name != NULL ? name : "");
  model.note = vstr_from_cstr(note != NULL ? note : "");
  model.flash = vstr_from_cstr(flash != NULL ? flash : "");
  model.upload_label = vstr_from_cstr(app->upload_label);
  model.upload_filename = vstr_from_cstr(app->upload_filename);

  status = chttp_web_render_response(
      &app->renderer,
      response,
      template_name,
      &app->model_desc,
      &model,
      status_code,
      NULL,
      &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(
        stderr,
        "forms render failed: status=%d native=%d message=%s\n",
        (int)status, error.native_status, error.message);
    return SALTS_EIO;
  }
  return SALTS_OK;
}

static int web_forms_index(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_forms_app *app = (web_forms_app *)user;
  char flash[WEB_FORMS_FLASH_BYTES];
  if (app == NULL || request == NULL || request->session == NULL)
    return SALTS_EPROTO;
  if (web_forms_consume_flash(request->session, flash) != SALTS_OK)
    return SALTS_EIO;
  return web_forms_render(
      app, request, response, NULL,
      app->profile_name, app->profile_note, flash,
      "page.html", 200u);
}

static int web_forms_profile_post(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_forms_app *app = (web_forms_app *)user;
  chttp_web_form_pair pairs[WEB_FORMS_FORM_PAIRS];
  char bytes[WEB_FORMS_FORM_BYTES];
  chttp_web_form form = {0};
  const chttp_web_form_pair *name_pair;
  const chttp_web_form_pair *note_pair;
  chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
  chttp_web_validation_error validation_errors[WEB_FORMS_VALIDATION_ERRORS];
  char validation_bytes[WEB_FORMS_VALIDATION_BYTES];
  char name[WEB_FORMS_PROFILE_NAME_BYTES] = {0};
  char note[WEB_FORMS_PROFILE_NOTE_BYTES] = {0};
  chttp_web_flash_config flash_config = web_forms_flash_config();
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;

  if (app == NULL || request == NULL)
    return SALTS_EINVAL;
  if (!web_forms_content_type_form(request))
    return chttp_server_reply(response, 415u, NULL, NULL, 0u);

  status = web_forms_parse_form(
      request, pairs, bytes, &form, &error);
  if (status != CHTTP_WEB_OK)
    return chttp_server_reply(response, 400u, NULL, NULL, 0u);

  status = chttp_web_csrf_validate(request, &form, &error);
  if (status == CHTTP_WEB_CSRF)
    return chttp_server_reply(
        response, 403u, "text/plain; charset=utf-8",
        "csrf rejected", sizeof("csrf rejected") - 1u);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  status = chttp_web_validation_init(
      &validation,
      validation_errors,
      WEB_FORMS_VALIDATION_ERRORS,
      validation_bytes,
      sizeof(validation_bytes),
      &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  name_pair = chttp_web_form_get(&form, "name", 0u);
  note_pair = chttp_web_form_get(&form, "note", 0u);

  if (name_pair != NULL &&
      web_forms_copy_view(
          name, sizeof(name), name_pair->value) != SALTS_OK)
    status = chttp_web_validation_add_field(
        &validation, "name", "name is too long", &error);
  if (note_pair != NULL &&
      web_forms_copy_view(
          note, sizeof(note), note_pair->value) != SALTS_OK)
    status = chttp_web_validation_add_field(
        &validation, "note", "note is too long", &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  if (name_pair == NULL || name_pair->value.size < 3u) {
    status = chttp_web_validation_add_field(
        &validation, "name",
        "name must contain at least 3 characters", &error);
    if (status != CHTTP_WEB_OK) return SALTS_EIO;
  }

  if (!validation.valid) {
    if (chttp_web_request_is_htmx(request)) {
      status = chttp_web_hx_retarget(
          response, "#profile", &error);
      if (status != CHTTP_WEB_OK) return SALTS_EIO;
      return web_forms_render(
          app, request, response, &validation,
          name, note, "", "profile.html", 422u);
    }
    return web_forms_render(
        app, request, response, &validation,
        name, note, "", "page.html", 422u);
  }

  memcpy(app->profile_name, name, strlen(name) + 1u);
  memcpy(app->profile_note, note, strlen(note) + 1u);

  if (chttp_web_request_is_htmx(request)) {
    status = chttp_web_hx_trigger(
        response, "profile:saved", &error);
    if (status == CHTTP_WEB_OK)
      status = chttp_web_hx_retarget(
          response, "#profile", &error);
    if (status != CHTTP_WEB_OK) return SALTS_EIO;
    return web_forms_render(
        app, request, response, NULL,
        app->profile_name, app->profile_note,
        "profile saved", "profile.html", 200u);
  }

  status = chttp_web_flash_push(
      request->session, &flash_config,
      "success", "profile saved", &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;
  status = chttp_web_redirect(
      response, 303u, "/forms", &error);
  return status == CHTTP_WEB_OK ? SALTS_OK : SALTS_EIO;
}

static chttp_web_multipart_limits web_forms_upload_limits(void) {
  chttp_web_multipart_limits limits =
      (chttp_web_multipart_limits)CHTTP_WEB_MULTIPART_LIMITS_INIT;
  limits.max_parts = 8u;
  limits.max_header_count = 8u;
  limits.max_header_bytes = 1024u;
  limits.max_name_bytes = 64u;
  limits.max_filename_bytes = WEB_FORMS_UPLOAD_FILENAME_BYTES - 1u;
  limits.max_content_type_bytes = 128u;
  limits.max_field_bytes = WEB_FORMS_UPLOAD_LABEL_BYTES - 1u;
  limits.max_total_bytes = WEB_FORMS_BODY_BYTES;
  return limits;
}

static int web_forms_upload_begin(
    void *user,
    const chttp_server_request_view *request) {
  web_forms_upload_slot *slot = (web_forms_upload_slot *)user;
  (void)request;
  if (slot == NULL || slot->app == NULL)
    return SALTS_EINVAL;
  slot->current_part = WEB_FORMS_UPLOAD_PART_NONE;
  slot->label[0] = '\0';
  slot->label_size = 0u;
  slot->filename[0] = '\0';
  slot->filename_size = 0u;
  slot->file_size = 0u;
  return SALTS_OK;
}

static int web_forms_upload_part_begin(
    void *user,
    const chttp_web_multipart_part *part) {
  web_forms_upload_slot *slot = (web_forms_upload_slot *)user;
  static const char label_name[] = "label";
  static const char file_name[] = "file";
  if (slot == NULL || part == NULL || part->name.data == NULL)
    return SALTS_EINVAL;

  slot->current_part = WEB_FORMS_UPLOAD_PART_NONE;
  if (part->name.size == sizeof(label_name) - 1u &&
      memcmp(part->name.data, label_name, sizeof(label_name) - 1u) == 0 &&
      !part->has_filename) {
    slot->current_part = WEB_FORMS_UPLOAD_PART_LABEL;
    return SALTS_OK;
  }

  if (part->name.size == sizeof(file_name) - 1u &&
      memcmp(part->name.data, file_name, sizeof(file_name) - 1u) == 0 &&
      part->has_filename) {
    if (part->filename.size >= sizeof(slot->filename))
      return SALTS_EMSGSIZE;
    if (part->filename.size != 0u)
      memcpy(slot->filename, part->filename.data, part->filename.size);
    slot->filename[part->filename.size] = '\0';
    slot->filename_size = part->filename.size;
    slot->current_part = WEB_FORMS_UPLOAD_PART_FILE;
    return SALTS_OK;
  }

  return SALTS_EPROTO;
}

static int web_forms_upload_part_data(
    void *user,
    const void *data,
    size_t size) {
  web_forms_upload_slot *slot = (web_forms_upload_slot *)user;
  if (slot == NULL || (size != 0u && data == NULL))
    return SALTS_EINVAL;

  if (slot->current_part == WEB_FORMS_UPLOAD_PART_LABEL) {
    if (size > sizeof(slot->label) - 1u - slot->label_size)
      return SALTS_ENOBUFS;
    if (size != 0u)
      memcpy(slot->label + slot->label_size, data, size);
    slot->label_size += size;
    slot->label[slot->label_size] = '\0';
    return SALTS_OK;
  }

  if (slot->current_part == WEB_FORMS_UPLOAD_PART_FILE) {
    if (size > sizeof(slot->file) - slot->file_size)
      return SALTS_ENOBUFS;
    if (size != 0u)
      memcpy(slot->file + slot->file_size, data, size);
    slot->file_size += size;
    return SALTS_OK;
  }

  return SALTS_EPROTO;
}

static int web_forms_upload_part_end(void *user) {
  web_forms_upload_slot *slot = (web_forms_upload_slot *)user;
  if (slot == NULL ||
      slot->current_part == WEB_FORMS_UPLOAD_PART_NONE)
    return SALTS_EPROTO;
  slot->current_part = WEB_FORMS_UPLOAD_PART_NONE;
  return SALTS_OK;
}

static int web_forms_upload_commit(void *user) {
  web_forms_upload_slot *slot = (web_forms_upload_slot *)user;
  web_forms_app *app;
  if (slot == NULL || slot->app == NULL)
    return SALTS_EINVAL;
  app = slot->app;
  memcpy(app->upload_label, slot->label, slot->label_size + 1u);
  memcpy(
      app->upload_filename,
      slot->filename,
      slot->filename_size + 1u);
  if (slot->file_size != 0u)
    memcpy(app->upload_file, slot->file, slot->file_size);
  app->upload_file_size = slot->file_size;
  return SALTS_OK;
}

static void web_forms_upload_abort(
    void *user,
    chttp_web_status status,
    int native_status) {
  web_forms_upload_slot *slot = (web_forms_upload_slot *)user;
  (void)status;
  (void)native_status;
  if (slot != NULL)
    slot->current_part = WEB_FORMS_UPLOAD_PART_NONE;
}

static chttp_web_upload_callbacks web_forms_upload_callbacks(void) {
  return (chttp_web_upload_callbacks){
      .size = sizeof(chttp_web_upload_callbacks),
      .begin = web_forms_upload_begin,
      .part_begin = web_forms_upload_part_begin,
      .part_data = web_forms_upload_part_data,
      .part_end = web_forms_upload_part_end,
      .commit = web_forms_upload_commit,
      .abort = web_forms_upload_abort};
}

static web_forms_upload_slot *web_forms_acquire_upload_slot(
    web_forms_app *app) {
  size_t i;
  for (i = 0u; i < WEB_FORMS_UPLOAD_SLOTS; ++i) {
    web_forms_upload_slot *slot = &app->upload_slots[i];
    if (slot->in_use) continue;
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->app = app;
    return slot;
  }
  return NULL;
}

static void web_forms_release_upload_slot(
    web_forms_upload_slot *slot) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  if (slot == NULL) return;
  if (slot->upload.size != 0u &&
      chttp_web_upload_reset(&slot->upload, &error) != CHTTP_WEB_OK) {
    (void)chttp_web_upload_abort(
        &slot->upload, CHTTP_WEB_UPLOAD, SALTS_ECANCELED, &error);
    (void)chttp_web_upload_reset(&slot->upload, &error);
  }
  slot->in_use = false;
}

static int web_forms_upload_open(
    void *user,
    const chttp_server_request_view *request,
    chttp_body_sink *out_sink) {
  web_forms_app *app = (web_forms_app *)user;
  web_forms_upload_slot *slot;
  chttp_web_multipart_limits limits = web_forms_upload_limits();
  chttp_web_upload_callbacks callbacks = web_forms_upload_callbacks();
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;

  if (app == NULL || request == NULL || out_sink == NULL)
    return SALTS_EINVAL;
  slot = web_forms_acquire_upload_slot(app);
  if (slot == NULL) return SALTS_ENOBUFS;

  status = chttp_web_upload_open(
      &slot->upload,
      request,
      &limits,
      &callbacks,
      slot,
      out_sink,
      &error);
  if (status == CHTTP_WEB_OK)
    return SALTS_OK;

  slot->in_use = false;
  return error.native_status != 0
      ? error.native_status
      : SALTS_EPROTO;
}

static void web_forms_upload_close(
    void *user,
    chttp_body_sink *sink,
    int status) {
  chttp_web_upload_request *upload;
  web_forms_upload_slot *slot;
  (void)user;
  if (sink == NULL || sink->user == NULL) return;
  upload = (chttp_web_upload_request *)sink->user;
  slot = (web_forms_upload_slot *)chttp_web_upload_user(upload);
  if (slot == NULL) return;
  chttp_web_upload_close(upload, status);
  if (status != SALTS_OK)
    web_forms_release_upload_slot(slot);
}

static int web_forms_upload_post(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_forms_app *app = (web_forms_app *)user;
  chttp_web_upload_request *upload;
  web_forms_upload_slot *slot;
  chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
  chttp_web_validation_error validation_errors[WEB_FORMS_VALIDATION_ERRORS];
  char validation_bytes[WEB_FORMS_VALIDATION_BYTES];
  chttp_web_flash_config flash_config = web_forms_flash_config();
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  int render_status = SALTS_OK;

  if (app == NULL || request == NULL ||
      request->body_sink_user == NULL)
    return SALTS_EPROTO;

  upload = (chttp_web_upload_request *)request->body_sink_user;
  slot = (web_forms_upload_slot *)chttp_web_upload_user(upload);
  if (slot == NULL || slot->app != app || !slot->in_use)
    return SALTS_EPROTO;

  status = chttp_web_validation_init(
      &validation,
      validation_errors,
      WEB_FORMS_VALIDATION_ERRORS,
      validation_bytes,
      sizeof(validation_bytes),
      &error);
  if (status != CHTTP_WEB_OK) {
    web_forms_release_upload_slot(slot);
    return SALTS_EIO;
  }

  if (slot->label_size == 0u) {
    status = chttp_web_validation_add_field(
        &validation, "label", "upload label is required", &error);
    if (status != CHTTP_WEB_OK) {
      web_forms_release_upload_slot(slot);
      return SALTS_EIO;
    }
  }
  if (slot->filename_size == 0u || slot->file_size == 0u) {
    status = chttp_web_validation_add_field(
        &validation, "file", "a non-empty file is required", &error);
    if (status != CHTTP_WEB_OK) {
      web_forms_release_upload_slot(slot);
      return SALTS_EIO;
    }
  }

  status = chttp_web_upload_finalize(
      upload, request, &validation, &error);
  if (status == CHTTP_WEB_CSRF) {
    (void)chttp_server_reply(
        response, 403u, "text/plain; charset=utf-8",
        "csrf rejected", sizeof("csrf rejected") - 1u);
    web_forms_release_upload_slot(slot);
    return SALTS_OK;
  }

  if (status == CHTTP_WEB_VALIDATION) {
    if (chttp_web_request_is_htmx(request)) {
      if (chttp_web_hx_retarget(
              response, "#upload", &error) != CHTTP_WEB_OK)
        render_status = SALTS_EIO;
      else
        render_status = web_forms_render(
            app, request, response, &validation,
            app->profile_name, app->profile_note, "",
            "upload.html", 422u);
    } else {
      render_status = web_forms_render(
          app, request, response, &validation,
          app->profile_name, app->profile_note, "",
          "page.html", 422u);
    }
    web_forms_release_upload_slot(slot);
    return render_status;
  }

  if (status != CHTTP_WEB_OK) {
    web_forms_release_upload_slot(slot);
    return SALTS_EIO;
  }

  if (chttp_web_request_is_htmx(request)) {
    status = chttp_web_hx_trigger(
        response, "upload:saved", &error);
    if (status == CHTTP_WEB_OK)
      status = chttp_web_hx_retarget(
          response, "#upload", &error);
    if (status == CHTTP_WEB_OK)
      render_status = web_forms_render(
          app, request, response, NULL,
          app->profile_name, app->profile_note,
          "upload saved", "upload.html", 200u);
    else
      render_status = SALTS_EIO;
    web_forms_release_upload_slot(slot);
    return render_status;
  }

  status = chttp_web_flash_push(
      request->session, &flash_config,
      "success", "upload saved", &error);
  if (status == CHTTP_WEB_OK)
    status = chttp_web_redirect(
        response, 303u, "/forms", &error);
  web_forms_release_upload_slot(slot);
  return status == CHTTP_WEB_OK ? SALTS_OK : SALTS_EIO;
}

int main(void) {
  static const char layout[] =
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<title>CHttp::Web Forms</title></head><body>"
      "{% block body %}{% endblock %}</body></html>";
  static const char page[] =
      "{% extends \"layout.html\" %}"
      "{% block body %}<main><h1>Forms and uploads</h1>"
      "<p id=\"flash\">{{ flash }}</p>"
      "{% include \"profile.html\" %}"
      "{% include \"upload.html\" %}"
      "</main>{% endblock %}";
  static const char profile[] =
      "<section id=\"profile\"><h2>Profile</h2>"
      "<ul class=\"errors\">{% for error in validation.errors %}"
      "<li data-field=\"{{ error.field }}\">{{ error.message }}</li>"
      "{% endfor %}</ul>"
      "<p class=\"result\">{{ flash }}</p>"
      "<form method=\"post\" action=\"/profile\" "
      "hx-post=\"/profile\" hx-target=\"#profile\" "
      "hx-swap=\"outerHTML\">"
      "<input type=\"hidden\" name=\"_csrf\" "
      "value=\"{{ request.csrf_token }}\">"
      "<label>Name <input name=\"name\" maxlength=\"63\" "
      "value=\"{{ name }}\"></label>"
      "<label>Note <textarea name=\"note\" maxlength=\"255\">"
      "{{ note }}</textarea></label>"
      "<button type=\"submit\">Save profile</button>"
      "</form></section>";
  static const char upload[] =
      "<section id=\"upload\"><h2>Upload</h2>"
      "<ul class=\"errors\">{% for error in validation.errors %}"
      "<li data-field=\"{{ error.field }}\">{{ error.message }}</li>"
      "{% endfor %}</ul>"
      "<p class=\"result\">{{ flash }}</p>"
      "<p id=\"upload-label\">{{ upload_label }}</p>"
      "<p id=\"upload-filename\">{{ upload_filename }}</p>"
      "<form method=\"post\" action=\"/upload\" "
      "enctype=\"multipart/form-data\" "
      "hx-post=\"/upload\" hx-target=\"#upload\" "
      "hx-swap=\"outerHTML\">"
      "<input type=\"hidden\" name=\"_csrf\" "
      "value=\"{{ request.csrf_token }}\">"
      "<label>Label <input name=\"label\" maxlength=\"95\"></label>"
      "<label>File <input type=\"file\" name=\"file\"></label>"
      "<button type=\"submit\">Upload</button>"
      "</form></section>";
  static const chttp_web_template templates[] = {
      {"layout.html", layout, sizeof(layout) - 1u},
      {"page.html", page, sizeof(page) - 1u},
      {"profile.html", profile, sizeof(profile) - 1u},
      {"upload.html", upload, sizeof(upload) - 1u}};

  web_forms_app app = {0};
  chttp_web_renderer_config renderer_config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_security_policy security =
      chttp_web_security_reference_policy();
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_server server = {0};
  chttp_server_config config = web_forms_server_config();
  chttp_server_route_options upload_route = {
      .method = CHTTP_METHOD_POST,
      .path = "/upload",
      .handler = web_forms_upload_post,
      .user = &app,
      .body_open = web_forms_upload_open,
      .body_close = web_forms_upload_close};
  uint16_t port = 0u;
  int status;

  app.model_desc =
      web_forms_model_desc(app.model_fields, &app.model_shape);

  status = chttp_web_renderer_init(
      &app.renderer, templates, 4u, &renderer_config, &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "forms renderer init failed: %s\n", error.message);
    return EXIT_FAILURE;
  }

  status = chttp_server_init(&server, &config);
  if (status == SALTS_OK)
    status = chttp_web_security_use(&server, &security);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/forms", web_forms_index, &app);
  if (status == SALTS_OK)
    status = chttp_server_post(
        &server, "/profile", web_forms_profile_post, &app);
  if (status == SALTS_OK)
    status = chttp_server_route_with(&server, &upload_route);
  if (status == SALTS_OK)
    status = chttp_server_start(&server);
  if (status == SALTS_OK)
    status = chttp_server_port(&server, &port);

  if (status == SALTS_OK) {
    printf(
        "CHttp::Web forms example: http://127.0.0.1:%u/forms\n",
        (unsigned int)port);
    fflush(stdout);
    (void)getchar();
  }

  if (server.impl) {
    const int stopped =
        chttp_server_stop(&server, WEB_FORMS_TIMEOUT_MS);
    if (status == SALTS_OK) status = stopped;
    if (stopped == SALTS_OK) {
      const int destroyed = chttp_server_destroy(&server);
      if (status == SALTS_OK) status = destroyed;
    }
  }
  chttp_web_renderer_destroy(&app.renderer);

  if (status != SALTS_OK)
    fprintf(stderr, "CHttp::Web forms example failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
