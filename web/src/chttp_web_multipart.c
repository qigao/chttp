#include <chttp_web/web.h>

#include <salts/error_codes.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
  CHTTP_WEB_MP_INITIAL = 1,
  CHTTP_WEB_MP_HEADERS = 2,
  CHTTP_WEB_MP_BODY = 3,
  CHTTP_WEB_MP_SUFFIX = 4,
  CHTTP_WEB_MP_DONE = 5,
  CHTTP_WEB_MP_FAILED = 6
};

static chttp_web_status chttp_web_multipart_error(
    chttp_web_error *error,
    chttp_web_status status,
    int native_status,
    size_t offset,
    const char *message) {
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

static chttp_web_status chttp_web_multipart_fail(
    chttp_web_multipart_parser *parser,
    chttp_web_error *error,
    chttp_web_status status,
    int native_status,
    size_t offset,
    const char *message) {
  if (parser != NULL) {
    parser->failed = true;
    parser->state = CHTTP_WEB_MP_FAILED;
  }
  return chttp_web_multipart_error(
      error, status, native_status, offset, message);
}

static unsigned char chttp_web_multipart_ascii_lower(unsigned char value) {
  if (value >= 'A' && value <= 'Z')
    return (unsigned char)(value + ('a' - 'A'));
  return value;
}

static int chttp_web_multipart_ieq(
    const char *data, size_t size, const char *literal) {
  size_t i;
  const size_t literal_size = literal != NULL ? strlen(literal) : 0u;
  if (data == NULL || literal == NULL || size != literal_size) return 0;
  for (i = 0u; i < size; ++i)
    if (chttp_web_multipart_ascii_lower((unsigned char)data[i]) !=
        chttp_web_multipart_ascii_lower((unsigned char)literal[i]))
      return 0;
  return 1;
}

static int chttp_web_multipart_ows(unsigned char value) {
  return value == ' ' || value == '\t';
}

static int chttp_web_multipart_token_char(unsigned char value) {
  if ((value >= '0' && value <= '9') ||
      (value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z'))
    return 1;
  switch (value) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return 1;
  default:
    return 0;
  }
}

static int chttp_web_multipart_boundary_char(unsigned char value) {
  if ((value >= '0' && value <= '9') ||
      (value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z'))
    return 1;
  switch (value) {
  case '\'':
  case '(':
  case ')':
  case '+':
  case '_':
  case ',':
  case '-':
  case '.':
  case '/':
  case ':':
  case '=':
  case '?':
  case ' ':
    return 1;
  default:
    return 0;
  }
}

static void chttp_web_multipart_skip_ows(
    const char *data, size_t size, size_t *position) {
  while (*position < size &&
         chttp_web_multipart_ows((unsigned char)data[*position]))
    ++(*position);
}

static int chttp_web_multipart_read_parameter_value(
    const char *data,
    size_t size,
    size_t *position,
    char *output,
    size_t output_capacity,
    size_t *out_size) {
  size_t used = 0u;
  int quoted = 0;

  chttp_web_multipart_skip_ows(data, size, position);
  if (*position >= size) return 0;

  if (data[*position] == '"') {
    quoted = 1;
    ++(*position);
    while (*position < size) {
      unsigned char value = (unsigned char)data[*position];
      if (value == '"') {
        ++(*position);
        break;
      }
      if (value == '\\') {
        ++(*position);
        if (*position >= size) return 0;
        value = (unsigned char)data[*position];
      }
      if (value == 0u || value == '\r' || value == '\n' ||
          value == 0x7fu || value < 0x20u)
        return 0;
      if (output != NULL) {
        if (used >= output_capacity) return -1;
        output[used] = (char)value;
      }
      ++used;
      ++(*position);
    }
    if (*position == 0u || data[*position - 1u] != '"') return 0;
  } else {
    while (*position < size &&
           data[*position] != ';' &&
           !chttp_web_multipart_ows((unsigned char)data[*position])) {
      const unsigned char value = (unsigned char)data[*position];
      if (value == 0u || value == '\r' || value == '\n' ||
          value == 0x7fu || value < 0x20u)
        return 0;
      if (output != NULL) {
        if (used >= output_capacity) return -1;
        output[used] = (char)value;
      }
      ++used;
      ++(*position);
    }
  }

  chttp_web_multipart_skip_ows(data, size, position);
  if (*position < size && data[*position] != ';') return 0;
  if (!quoted && used == 0u) return 0;
  if (output != NULL) output[used] = '\0';
  if (out_size != NULL) *out_size = used;
  return 1;
}

static int chttp_web_multipart_parse_outer_boundary(
    const char *content_type,
    char boundary[CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX + 1u],
    size_t *out_boundary_size) {
  size_t size;
  size_t position = 0u;
  size_t media_begin;
  size_t media_end;
  int found = 0;

  if (content_type == NULL || out_boundary_size == NULL) return 0;
  size = strlen(content_type);
  chttp_web_multipart_skip_ows(content_type, size, &position);
  media_begin = position;
  while (position < size && content_type[position] != ';') ++position;
  media_end = position;
  while (media_end > media_begin &&
         chttp_web_multipart_ows((unsigned char)content_type[media_end - 1u]))
    --media_end;
  if (!chttp_web_multipart_ieq(
          content_type + media_begin, media_end - media_begin,
          "multipart/form-data"))
    return 0;

  while (position < size) {
    size_t name_begin;
    size_t name_end;
    size_t value_size = 0u;
    int result;

    if (content_type[position] != ';') return 0;
    ++position;
    chttp_web_multipart_skip_ows(content_type, size, &position);
    name_begin = position;
    while (position < size &&
           chttp_web_multipart_token_char(
               (unsigned char)content_type[position]))
      ++position;
    name_end = position;
    if (name_end == name_begin) return 0;
    chttp_web_multipart_skip_ows(content_type, size, &position);
    if (position >= size || content_type[position] != '=') return 0;
    ++position;

    if (chttp_web_multipart_ieq(
            content_type + name_begin, name_end - name_begin, "boundary")) {
      size_t i;
      if (found) return 0;
      result = chttp_web_multipart_read_parameter_value(
          content_type, size, &position,
          boundary, CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX,
          &value_size);
      if (result != 1 ||
          value_size == 0u ||
          value_size > CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX ||
          boundary[value_size - 1u] == ' ')
        return 0;
      for (i = 0u; i < value_size; ++i)
        if (!chttp_web_multipart_boundary_char(
                (unsigned char)boundary[i]))
          return 0;
      boundary[value_size] = '\0';
      *out_boundary_size = value_size;
      found = 1;
    } else {
      result = chttp_web_multipart_read_parameter_value(
          content_type, size, &position, NULL, 0u, NULL);
      if (result != 1) return 0;
    }
  }
  return found;
}

static int chttp_web_multipart_limits_valid(
    const chttp_web_multipart_limits *limits) {
  return limits != NULL &&
         limits->size >= sizeof(*limits) &&
         limits->max_parts != 0u &&
         limits->max_header_count != 0u &&
         limits->max_header_count <=
             CHTTP_WEB_MULTIPART_HEADER_COUNT_HARD_MAX &&
         limits->max_header_bytes >= 4u &&
         limits->max_header_bytes <=
             CHTTP_WEB_MULTIPART_HEADER_BYTES_HARD_MAX &&
         limits->max_name_bytes != 0u &&
         limits->max_name_bytes <= CHTTP_WEB_MULTIPART_NAME_HARD_MAX &&
         limits->max_filename_bytes <=
             CHTTP_WEB_MULTIPART_FILENAME_HARD_MAX &&
         limits->max_content_type_bytes <=
             CHTTP_WEB_MULTIPART_CONTENT_TYPE_HARD_MAX &&
         limits->max_total_bytes != 0u &&
         limits->max_field_bytes <= limits->max_total_bytes;
}

static int chttp_web_multipart_callbacks_valid(
    const chttp_web_multipart_callbacks *callbacks) {
  return callbacks != NULL &&
         callbacks->size >= sizeof(*callbacks) &&
         callbacks->part_begin != NULL &&
         callbacks->part_data != NULL &&
         callbacks->part_end != NULL;
}

static int chttp_web_multipart_parser_valid(
    const chttp_web_multipart_parser *parser) {
  return parser != NULL &&
         parser->size >= sizeof(*parser) &&
         parser->boundary_size != 0u &&
         parser->boundary_size <= CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX &&
         chttp_web_multipart_limits_valid(&parser->limits) &&
         chttp_web_multipart_callbacks_valid(&parser->callbacks);
}

static void chttp_web_multipart_clear_part(
    chttp_web_multipart_parser *parser) {
  parser->header_size = 0u;
  parser->part_name[0] = '\0';
  parser->part_name_size = 0u;
  parser->part_filename[0] = '\0';
  parser->part_filename_size = 0u;
  parser->part_content_type[0] = '\0';
  parser->part_content_type_size = 0u;
  parser->pending_size = 0u;
  parser->field_bytes = 0u;
  parser->suffix_state = 0u;
  parser->current_file = false;
  parser->has_filename = false;
  parser->has_content_type = false;
}

static void chttp_web_multipart_start_body(
    chttp_web_multipart_parser *parser) {
  parser->state = CHTTP_WEB_MP_INITIAL;
  parser->initial_index = 0u;
  parser->total_bytes = 0u;
  parser->part_count = 0u;
  parser->active = true;
  parser->failed = false;
  chttp_web_multipart_clear_part(parser);
}

chttp_web_status chttp_web_multipart_init(
    chttp_web_multipart_parser *parser,
    const char *content_type,
    const chttp_web_multipart_limits *limits,
    const chttp_web_multipart_callbacks *callbacks,
    void *user,
    chttp_web_error *error) {
  char boundary[CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX + 1u];
  size_t boundary_size = 0u;

  if (parser == NULL ||
      !chttp_web_multipart_limits_valid(limits) ||
      !chttp_web_multipart_callbacks_valid(callbacks) ||
      !chttp_web_multipart_parse_outer_boundary(
          content_type, boundary, &boundary_size))
    return chttp_web_multipart_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "multipart parser configuration or Content-Type is invalid");

  *parser = (chttp_web_multipart_parser)CHTTP_WEB_MULTIPART_PARSER_INIT;
  parser->limits = *limits;
  parser->callbacks = *callbacks;
  parser->user = user;
  memcpy(parser->boundary, boundary, boundary_size + 1u);
  parser->boundary_size = boundary_size;
  chttp_web_multipart_start_body(parser);
  return chttp_web_multipart_error(
      error, CHTTP_WEB_OK, 0, 0u, NULL);
}

static char chttp_web_multipart_initial_char(
    const chttp_web_multipart_parser *parser, size_t index) {
  if (index < 2u) return '-';
  if (index < 2u + parser->boundary_size)
    return parser->boundary[index - 2u];
  return index == 2u + parser->boundary_size ? '\r' : '\n';
}

static char chttp_web_multipart_marker_char(
    const chttp_web_multipart_parser *parser, size_t index) {
  if (index == 0u) return '\r';
  if (index == 1u) return '\n';
  if (index == 2u || index == 3u) return '-';
  return parser->boundary[index - 4u];
}

static chttp_web_status chttp_web_multipart_emit(
    chttp_web_multipart_parser *parser,
    const void *data,
    size_t size,
    chttp_web_error *error) {
  int native_status;
  if (size == 0u) return CHTTP_WEB_OK;
  if (!parser->current_file &&
      size > parser->limits.max_field_bytes - parser->field_bytes)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_CAPACITY, 0, parser->total_bytes,
        "multipart field bytes exceed configured limit");

  native_status = parser->callbacks.part_data(parser->user, data, size);
  if (native_status != SALTS_OK)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_MULTIPART, native_status,
        parser->total_bytes,
        "multipart part-data callback failed");
  if (!parser->current_file) parser->field_bytes += size;
  return CHTTP_WEB_OK;
}

static int chttp_web_multipart_copy_parameter(
    const char *data,
    size_t size,
    size_t *position,
    char *output,
    size_t limit,
    size_t *out_size) {
  const int result = chttp_web_multipart_read_parameter_value(
      data, size, position, output, limit, out_size);
  return result == 1;
}

static int chttp_web_multipart_parse_disposition(
    chttp_web_multipart_parser *parser,
    const char *value,
    size_t value_size) {
  size_t position = 0u;
  size_t type_begin;
  size_t type_end;
  int have_name = 0;
  int have_filename = 0;

  chttp_web_multipart_skip_ows(value, value_size, &position);
  type_begin = position;
  while (position < value_size && value[position] != ';') ++position;
  type_end = position;
  while (type_end > type_begin &&
         chttp_web_multipart_ows((unsigned char)value[type_end - 1u]))
    --type_end;
  if (!chttp_web_multipart_ieq(
          value + type_begin, type_end - type_begin, "form-data"))
    return 0;

  while (position < value_size) {
    size_t name_begin;
    size_t name_end;

    if (value[position] != ';') return 0;
    ++position;
    chttp_web_multipart_skip_ows(value, value_size, &position);
    name_begin = position;
    while (position < value_size &&
           chttp_web_multipart_token_char((unsigned char)value[position]))
      ++position;
    name_end = position;
    if (name_end == name_begin) return 0;
    chttp_web_multipart_skip_ows(value, value_size, &position);
    if (position >= value_size || value[position] != '=') return 0;
    ++position;

    if (chttp_web_multipart_ieq(
            value + name_begin, name_end - name_begin, "name")) {
      if (have_name ||
          !chttp_web_multipart_copy_parameter(
              value, value_size, &position,
              parser->part_name, parser->limits.max_name_bytes,
              &parser->part_name_size) ||
          parser->part_name_size == 0u)
        return 0;
      have_name = 1;
    } else if (chttp_web_multipart_ieq(
                   value + name_begin, name_end - name_begin, "filename")) {
      if (have_filename ||
          !chttp_web_multipart_copy_parameter(
              value, value_size, &position,
              parser->part_filename, parser->limits.max_filename_bytes,
              &parser->part_filename_size))
        return 0;
      parser->has_filename = true;
      have_filename = 1;
    } else {
      if (chttp_web_multipart_read_parameter_value(
              value, value_size, &position, NULL, 0u, NULL) != 1)
        return 0;
    }
  }

  return have_name;
}

static int chttp_web_multipart_header_name_valid(
    const char *data, size_t size) {
  size_t i;
  if (size == 0u) return 0;
  for (i = 0u; i < size; ++i)
    if (!chttp_web_multipart_token_char((unsigned char)data[i])) return 0;
  return 1;
}

static int chttp_web_multipart_header_value_valid(
    const char *data, size_t size) {
  size_t i;
  for (i = 0u; i < size; ++i) {
    const unsigned char value = (unsigned char)data[i];
    if (value == 0u || value == '\r' || value == '\n' ||
        value == 0x7fu || (value < 0x20u && value != '\t'))
      return 0;
  }
  return 1;
}

static chttp_web_status chttp_web_multipart_begin_part(
    chttp_web_multipart_parser *parser,
    chttp_web_error *error) {
  size_t position = 0u;
  size_t header_count = 0u;
  int have_disposition = 0;
  int have_content_type = 0;
  chttp_web_multipart_part part =
      (chttp_web_multipart_part)CHTTP_WEB_MULTIPART_PART_INIT;
  int native_status;

  parser->part_name[0] = '\0';
  parser->part_name_size = 0u;
  parser->part_filename[0] = '\0';
  parser->part_filename_size = 0u;
  parser->part_content_type[0] = '\0';
  parser->part_content_type_size = 0u;
  parser->has_filename = false;
  parser->has_content_type = false;

  while (position + 1u < parser->header_size) {
    size_t line_end = position;
    size_t colon;
    size_t value_begin;
    size_t value_end;

    while (line_end + 1u < parser->header_size &&
           !(parser->header_bytes[line_end] == '\r' &&
             parser->header_bytes[line_end + 1u] == '\n'))
      ++line_end;

    if (line_end + 1u >= parser->header_size)
      return chttp_web_multipart_fail(
          parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
          "multipart header block is truncated");
    if (line_end == position) {
      if (line_end + 2u != parser->header_size)
        return chttp_web_multipart_fail(
            parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
            "multipart header terminator is malformed");
      break;
    }

    if (header_count >= parser->limits.max_header_count)
      return chttp_web_multipart_fail(
          parser, error, CHTTP_WEB_CAPACITY, 0, parser->total_bytes,
          "multipart header count exceeds configured limit");
    ++header_count;

    colon = position;
    while (colon < line_end && parser->header_bytes[colon] != ':') ++colon;
    if (colon == position || colon == line_end ||
        !chttp_web_multipart_header_name_valid(
            parser->header_bytes + position, colon - position))
      return chttp_web_multipart_fail(
          parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
          "multipart header name is invalid");

    value_begin = colon + 1u;
    while (value_begin < line_end &&
           chttp_web_multipart_ows(
               (unsigned char)parser->header_bytes[value_begin]))
      ++value_begin;
    value_end = line_end;
    while (value_end > value_begin &&
           chttp_web_multipart_ows(
               (unsigned char)parser->header_bytes[value_end - 1u]))
      --value_end;
    if (!chttp_web_multipart_header_value_valid(
            parser->header_bytes + value_begin, value_end - value_begin))
      return chttp_web_multipart_fail(
          parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
          "multipart header value is invalid");

    if (chttp_web_multipart_ieq(
            parser->header_bytes + position, colon - position,
            "Content-Disposition")) {
      if (have_disposition ||
          !chttp_web_multipart_parse_disposition(
              parser,
              parser->header_bytes + value_begin,
              value_end - value_begin))
        return chttp_web_multipart_fail(
            parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
            "multipart Content-Disposition is invalid");
      have_disposition = 1;
    } else if (chttp_web_multipart_ieq(
                   parser->header_bytes + position, colon - position,
                   "Content-Type")) {
      const size_t content_type_size = value_end - value_begin;
      if (have_content_type || content_type_size == 0u)
        return chttp_web_multipart_fail(
            parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
            "multipart Content-Type is invalid");
      if (content_type_size > parser->limits.max_content_type_bytes)
        return chttp_web_multipart_fail(
            parser, error, CHTTP_WEB_CAPACITY, 0, parser->total_bytes,
            "multipart Content-Type exceeds configured limit");
      memcpy(
          parser->part_content_type,
          parser->header_bytes + value_begin,
          content_type_size);
      parser->part_content_type[content_type_size] = '\0';
      parser->part_content_type_size = content_type_size;
      parser->has_content_type = true;
      have_content_type = 1;
    }

    position = line_end + 2u;
  }

  if (!have_disposition)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
        "multipart part is missing Content-Disposition");
  if (parser->part_count >= parser->limits.max_parts)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_CAPACITY, 0, parser->total_bytes,
        "multipart part count exceeds configured limit");

  part.name = (chttp_web_string_view){
      parser->part_name, parser->part_name_size};
  part.has_filename = parser->has_filename;
  part.filename = parser->has_filename
      ? (chttp_web_string_view){
            parser->part_filename, parser->part_filename_size}
      : (chttp_web_string_view){NULL, 0u};
  part.has_content_type = parser->has_content_type;
  part.content_type = parser->has_content_type
      ? (chttp_web_string_view){
            parser->part_content_type, parser->part_content_type_size}
      : (chttp_web_string_view){NULL, 0u};

  native_status = parser->callbacks.part_begin(parser->user, &part);
  if (native_status != SALTS_OK)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_MULTIPART, native_status,
        parser->total_bytes,
        "multipart part-begin callback failed");

  ++parser->part_count;
  parser->current_file = parser->has_filename;
  parser->field_bytes = 0u;
  parser->pending_size = 0u;
  parser->suffix_state = 0u;
  parser->state = CHTTP_WEB_MP_BODY;
  return CHTTP_WEB_OK;
}

static chttp_web_status chttp_web_multipart_end_part(
    chttp_web_multipart_parser *parser,
    chttp_web_error *error,
    int final) {
  const int native_status = parser->callbacks.part_end(parser->user);
  if (native_status != SALTS_OK)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_MULTIPART, native_status,
        parser->total_bytes,
        "multipart part-end callback failed");

  if (final) {
    parser->state = CHTTP_WEB_MP_DONE;
  } else {
    chttp_web_multipart_clear_part(parser);
    parser->state = CHTTP_WEB_MP_HEADERS;
  }
  return CHTTP_WEB_OK;
}

static chttp_web_status chttp_web_multipart_feed_header_byte(
    chttp_web_multipart_parser *parser,
    unsigned char value,
    chttp_web_error *error) {
  if (value == 0u)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
        "multipart header contains NUL");
  if (parser->header_size >= parser->limits.max_header_bytes)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_CAPACITY, 0, parser->total_bytes,
        "multipart header bytes exceed configured limit");
  if (parser->header_size != 0u &&
      parser->header_bytes[parser->header_size - 1u] == '\r' &&
      value != '\n')
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
        "multipart header uses invalid CR framing");
  if (value == '\n' &&
      (parser->header_size == 0u ||
       parser->header_bytes[parser->header_size - 1u] != '\r'))
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
        "multipart header uses bare LF");

  parser->header_bytes[parser->header_size++] = (char)value;
  if (parser->header_size >= 4u &&
      memcmp(
          parser->header_bytes + parser->header_size - 4u,
          "\r\n\r\n", 4u) == 0)
    return chttp_web_multipart_begin_part(parser, error);
  return CHTTP_WEB_OK;
}

static chttp_web_status chttp_web_multipart_feed_suffix_byte(
    chttp_web_multipart_parser *parser,
    unsigned char value,
    chttp_web_error *error) {
  switch (parser->suffix_state) {
  case 0u:
    if (value == '\r') {
      parser->suffix_state = 1u;
      return CHTTP_WEB_OK;
    }
    if (value == '-') {
      parser->suffix_state = 2u;
      return CHTTP_WEB_OK;
    }
    break;
  case 1u:
    if (value == '\n') {
      parser->suffix_state = 0u;
      return chttp_web_multipart_end_part(parser, error, 0);
    }
    break;
  case 2u:
    if (value == '-') {
      parser->suffix_state = 3u;
      return CHTTP_WEB_OK;
    }
    break;
  case 3u:
    if (value == '\r') {
      parser->suffix_state = 4u;
      return CHTTP_WEB_OK;
    }
    break;
  case 4u:
    if (value == '\n') {
      parser->suffix_state = 0u;
      return chttp_web_multipart_end_part(parser, error, 1);
    }
    break;
  default:
    break;
  }

  return chttp_web_multipart_fail(
      parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
      "multipart boundary suffix is malformed");
}

chttp_web_status chttp_web_multipart_feed(
    chttp_web_multipart_parser *parser,
    const void *data,
    size_t data_size,
    chttp_web_error *error) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t position = 0u;

  if (!chttp_web_multipart_parser_valid(parser) ||
      !parser->active || parser->failed ||
      (data_size != 0u && data == NULL))
    return chttp_web_multipart_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "multipart feed arguments or parser state are invalid");
  if (data_size > parser->limits.max_total_bytes - parser->total_bytes)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_CAPACITY, 0, parser->total_bytes,
        "multipart body exceeds configured total-byte limit");

  while (position < data_size) {
    chttp_web_status status = CHTTP_WEB_OK;

    if (parser->state == CHTTP_WEB_MP_DONE)
      return chttp_web_multipart_fail(
          parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
          "multipart epilogue bytes are not accepted");

    if (parser->state == CHTTP_WEB_MP_INITIAL) {
      const size_t initial_size = parser->boundary_size + 4u;
      const unsigned char value = bytes[position];
      if ((char)value != chttp_web_multipart_initial_char(
                             parser, parser->initial_index))
        return chttp_web_multipart_fail(
            parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
            "multipart opening delimiter is malformed");
      ++parser->initial_index;
      ++position;
      ++parser->total_bytes;
      if (parser->initial_index == initial_size) {
        parser->initial_index = 0u;
        parser->state = CHTTP_WEB_MP_HEADERS;
      }
      continue;
    }

    if (parser->state == CHTTP_WEB_MP_HEADERS) {
      const unsigned char value = bytes[position++];
      status = chttp_web_multipart_feed_header_byte(
          parser, value, error);
      ++parser->total_bytes;
      if (status != CHTTP_WEB_OK) return status;
      continue;
    }

    if (parser->state == CHTTP_WEB_MP_SUFFIX) {
      const unsigned char value = bytes[position++];
      status = chttp_web_multipart_feed_suffix_byte(
          parser, value, error);
      ++parser->total_bytes;
      if (status != CHTTP_WEB_OK) return status;
      continue;
    }

    if (parser->state != CHTTP_WEB_MP_BODY)
      return chttp_web_multipart_fail(
          parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
          "multipart parser entered an invalid state");

    if (parser->pending_size == 0u) {
      const size_t start = position;
      while (position < data_size && bytes[position] != '\r') ++position;
      if (position > start) {
        status = chttp_web_multipart_emit(
            parser, bytes + start, position - start, error);
        if (status != CHTTP_WEB_OK) return status;
        parser->total_bytes += position - start;
      }
      if (position == data_size) continue;
      parser->pending[0] = '\r';
      parser->pending_size = 1u;
      ++position;
      ++parser->total_bytes;
      continue;
    }

    {
      const unsigned char value = bytes[position];
      const char expected = chttp_web_multipart_marker_char(
          parser, parser->pending_size);
      if ((char)value == expected) {
        parser->pending[parser->pending_size++] = (char)value;
        ++position;
        ++parser->total_bytes;
        if (parser->pending_size == parser->boundary_size + 4u) {
          parser->pending_size = 0u;
          parser->suffix_state = 0u;
          parser->state = CHTTP_WEB_MP_SUFFIX;
        }
      } else {
        status = chttp_web_multipart_emit(
            parser, parser->pending, parser->pending_size, error);
        if (status != CHTTP_WEB_OK) return status;
        parser->pending_size = 0u;
      }
    }
  }

  return chttp_web_multipart_error(
      error, CHTTP_WEB_OK, 0, parser->total_bytes, NULL);
}

chttp_web_status chttp_web_multipart_finish(
    chttp_web_multipart_parser *parser,
    chttp_web_error *error) {
  if (!chttp_web_multipart_parser_valid(parser) ||
      !parser->active || parser->failed)
    return chttp_web_multipart_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "multipart finish parser state is invalid");
  if (parser->state != CHTTP_WEB_MP_DONE)
    return chttp_web_multipart_fail(
        parser, error, CHTTP_WEB_MULTIPART, 0, parser->total_bytes,
        "multipart body is truncated before the closing delimiter");
  parser->active = false;
  return chttp_web_multipart_error(
      error, CHTTP_WEB_OK, 0, parser->total_bytes, NULL);
}

chttp_web_status chttp_web_multipart_reset(
    chttp_web_multipart_parser *parser,
    chttp_web_error *error) {
  if (!chttp_web_multipart_parser_valid(parser))
    return chttp_web_multipart_error(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "multipart reset parser state is invalid");
  chttp_web_multipart_start_body(parser);
  return chttp_web_multipart_error(
      error, CHTTP_WEB_OK, 0, 0u, NULL);
}
