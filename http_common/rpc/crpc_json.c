#include "crpc_internal.h"

#include <fmt.h>
#include <json_cserde_reader.h>
#include <json_parser.h>
#include <vstr.h>

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct crpc_buffer {
  unsigned char *data;
  size_t size;
  size_t capacity;
  size_t limit;
} crpc_buffer;

typedef enum crpc_json_frame_kind { CRPC_JSON_ARRAY = 1, CRPC_JSON_MAP } crpc_json_frame_kind;

typedef struct crpc_json_frame {
  crpc_json_frame_kind kind;
  size_t count;
  bool expecting_key;
} crpc_json_frame;

typedef struct crpc_json_writer_context {
  cserde_writer writer;
  crpc_buffer *buffer;
  crpc_json_frame *frames;
  size_t depth;
  size_t max_depth;
  bool root_written;
  bool root_structured;
  bool require_structured_root;
} crpc_json_writer_context;

static cserde_status crpc_buffer_reserve(crpc_buffer *buffer, size_t extra) {
  size_t needed;
  size_t maximum_capacity;
  size_t capacity;
  unsigned char *next;

  if (buffer == NULL || buffer->size > buffer->limit || extra > buffer->limit - buffer->size)
    return CSERDE_LIMIT_EXCEEDED;
  if (buffer->limit == SIZE_MAX) return CSERDE_LIMIT_EXCEEDED;
  needed = buffer->size + extra + 1u;
  if (needed <= buffer->capacity) return CSERDE_OK;
  maximum_capacity = buffer->limit + 1u;
  capacity = buffer->capacity == 0u ? 256u : buffer->capacity;
  if (capacity > maximum_capacity) capacity = maximum_capacity;
  while (capacity < needed) {
    if (capacity > maximum_capacity / 2u) {
      capacity = maximum_capacity;
      break;
    }
    capacity *= 2u;
  }
  if (capacity < needed) return CSERDE_LIMIT_EXCEEDED;
  next = (unsigned char *)realloc(buffer->data, capacity);
  if (next == NULL) return CSERDE_SINK_ERROR;
  buffer->data = next;
  buffer->capacity = capacity;
  return CSERDE_OK;
}

static cserde_status crpc_buffer_append(crpc_buffer *buffer, const void *data, size_t size) {
  cserde_status status;
  if (size != 0u && data == NULL) return CSERDE_SINK_ERROR;
  status = crpc_buffer_reserve(buffer, size);
  if (status != CSERDE_OK) return status;
  if (size != 0u) memcpy(buffer->data + buffer->size, data, size);
  buffer->size += size;
  buffer->data[buffer->size] = '\0';
  return CSERDE_OK;
}

static cserde_status crpc_buffer_byte(crpc_buffer *buffer, unsigned char byte) {
  return crpc_buffer_append(buffer, &byte, 1u);
}

static cserde_status crpc_json_string_contents(crpc_buffer *buffer, const unsigned char *data,
                                               size_t size) {
  static const char hex[] = "0123456789abcdef";
  size_t index;
  cserde_status status;
  vstr view;

  if (size != 0u && data == NULL) return CSERDE_SINK_ERROR;
  view = vstr_from_buf((const char *)data, size);
  if (!vstr_utf8_valid(view)) return CSERDE_UNSUPPORTED;
  for (index = 0u; index < size; ++index) {
    const unsigned char byte = data[index];
    const char *escape = NULL;
    size_t escape_size = 0u;
    char unicode_escape[6];
    switch (byte) {
    case '"':
      escape = "\\\"";
      escape_size = 2u;
      break;
    case '\\':
      escape = "\\\\";
      escape_size = 2u;
      break;
    case '\b':
      escape = "\\b";
      escape_size = 2u;
      break;
    case '\f':
      escape = "\\f";
      escape_size = 2u;
      break;
    case '\n':
      escape = "\\n";
      escape_size = 2u;
      break;
    case '\r':
      escape = "\\r";
      escape_size = 2u;
      break;
    case '\t':
      escape = "\\t";
      escape_size = 2u;
      break;
    default:
      break;
    }
    if (escape != NULL) {
      status = crpc_buffer_append(buffer, escape, escape_size);
    } else if (byte < 0x20u) {
      unicode_escape[0] = '\\';
      unicode_escape[1] = 'u';
      unicode_escape[2] = '0';
      unicode_escape[3] = '0';
      unicode_escape[4] = hex[byte >> 4u];
      unicode_escape[5] = hex[byte & 0x0fu];
      status = crpc_buffer_append(buffer, unicode_escape, sizeof(unicode_escape));
    } else {
      status = crpc_buffer_byte(buffer, byte);
    }
    if (status != CSERDE_OK) return status;
  }
  return CSERDE_OK;
}

static cserde_status crpc_json_string(crpc_buffer *buffer, const unsigned char *data, size_t size) {
  cserde_status status = crpc_buffer_byte(buffer, (unsigned char)'"');
  if (status == CSERDE_OK) status = crpc_json_string_contents(buffer, data, size);
  if (status == CSERDE_OK) status = crpc_buffer_byte(buffer, (unsigned char)'"');
  return status;
}

static cserde_status crpc_json_prepare_value(crpc_json_writer_context *context) {
  crpc_json_frame *frame;
  cserde_status status;
  if (context->depth == 0u) {
    if (context->root_written) return CSERDE_INVALID_TOKEN;
    context->root_written = true;
    return CSERDE_OK;
  }
  frame = &context->frames[context->depth - 1u];
  if (frame->kind == CRPC_JSON_ARRAY) {
    if (frame->count != 0u) {
      status = crpc_buffer_byte(context->buffer, (unsigned char)',');
      if (status != CSERDE_OK) return status;
    }
    ++frame->count;
    return CSERDE_OK;
  }
  if (frame->expecting_key) return CSERDE_INVALID_TOKEN;
  frame->expecting_key = true;
  ++frame->count;
  return CSERDE_OK;
}

static cserde_status crpc_json_write_number(crpc_buffer *buffer, const cserde_token *token) {
  char text[96];
  int length;
  if (token->kind == CSERDE_SINT)
    length = snprintf(text, sizeof(text), "%" PRId64, token->value.sint);
  else if (token->kind == CSERDE_UINT)
    length = snprintf(text, sizeof(text), "%" PRIu64, token->value.uint);
  else {
    if (!isfinite(token->value.floating)) return CSERDE_UNSUPPORTED;
    length = fmt(text, sizeof(text), "{}", token->value.floating);
  }
  if (length <= 0 || (size_t)length >= sizeof(text)) return CSERDE_SINK_ERROR;
  return crpc_buffer_append(buffer, text, (size_t)length);
}

static cserde_status crpc_json_writer_write(void *opaque, const cserde_token *token) {
  crpc_json_writer_context *context = (crpc_json_writer_context *)opaque;
  crpc_json_frame *frame;
  cserde_status status;
  const char *literal;
  size_t literal_size;

  if (context == NULL || token == NULL) return CSERDE_SINK_ERROR;
  if (context->depth != 0u) {
    frame = &context->frames[context->depth - 1u];
    if (frame->kind == CRPC_JSON_MAP && frame->expecting_key && token->kind != CSERDE_MAP_END) {
      if (token->kind != CSERDE_STRING) return CSERDE_INVALID_TOKEN;
      if (frame->count != 0u) {
        status = crpc_buffer_byte(context->buffer, (unsigned char)',');
        if (status != CSERDE_OK) return status;
      }
      status = crpc_json_string(context->buffer, token->value.slice.data, token->value.slice.size);
      if (status != CSERDE_OK) return status;
      status = crpc_buffer_byte(context->buffer, (unsigned char)':');
      if (status != CSERDE_OK) return status;
      frame->expecting_key = false;
      return CSERDE_OK;
    }
  }

  if (token->kind == CSERDE_ARRAY_END || token->kind == CSERDE_MAP_END) {
    const crpc_json_frame_kind expected =
        token->kind == CSERDE_ARRAY_END ? CRPC_JSON_ARRAY : CRPC_JSON_MAP;
    if (context->depth == 0u) return CSERDE_INVALID_TOKEN;
    frame = &context->frames[context->depth - 1u];
    if (frame->kind != expected || (frame->kind == CRPC_JSON_MAP && !frame->expecting_key))
      return CSERDE_INVALID_TOKEN;
    status = crpc_buffer_byte(
        context->buffer, token->kind == CSERDE_ARRAY_END ? (unsigned char)']' : (unsigned char)'}');
    if (status != CSERDE_OK) return status;
    --context->depth;
    return CSERDE_OK;
  }

  status = crpc_json_prepare_value(context);
  if (status != CSERDE_OK) return status;
  switch (token->kind) {
  case CSERDE_NULL:
    literal = "null";
    literal_size = 4u;
    break;
  case CSERDE_BOOL:
    literal = token->value.boolean ? "true" : "false";
    literal_size = token->value.boolean ? 4u : 5u;
    break;
  case CSERDE_SINT:
  case CSERDE_UINT:
  case CSERDE_FLOAT:
    return crpc_json_write_number(context->buffer, token);
  case CSERDE_STRING:
    return crpc_json_string(context->buffer, token->value.slice.data, token->value.slice.size);
  case CSERDE_BYTES:
    return CSERDE_UNSUPPORTED;
  case CSERDE_ARRAY_BEGIN:
  case CSERDE_MAP_BEGIN:
    if (context->depth >= context->max_depth) return CSERDE_LIMIT_EXCEEDED;
    status =
        crpc_buffer_byte(context->buffer, token->kind == CSERDE_ARRAY_BEGIN ? (unsigned char)'['
                                                                            : (unsigned char)'{');
    if (status != CSERDE_OK) return status;
    frame = &context->frames[context->depth++];
    *frame = (crpc_json_frame){.kind = token->kind == CSERDE_ARRAY_BEGIN ? CRPC_JSON_ARRAY
                                                                         : CRPC_JSON_MAP,
                               .expecting_key = token->kind == CSERDE_MAP_BEGIN};
    if (context->depth == 1u) context->root_structured = true;
    return CSERDE_OK;
  case CSERDE_ARRAY_END:
  case CSERDE_MAP_END:
  default:
    return CSERDE_INVALID_TOKEN;
  }
  return crpc_buffer_append(context->buffer, literal, literal_size);
}

static cserde_status crpc_json_writer_finish(void *opaque) {
  crpc_json_writer_context *context = (crpc_json_writer_context *)opaque;
  if (context == NULL) return CSERDE_SINK_ERROR;
  if (!context->root_written || (context->require_structured_root && !context->root_structured) ||
      context->depth != 0u)
    return CSERDE_SINK_ERROR;
  return CSERDE_OK;
}

static const cserde_writer_ops crpc_json_writer_ops = {
    sizeof(cserde_writer_ops), CSERDE_WRITER_OPS_ABI_VERSION, crpc_json_writer_write,
    crpc_json_writer_finish};

static int crpc_cserde_status(cserde_status status) {
  switch (status) {
  case CSERDE_OK:
    return SALTS_OK;
  case CSERDE_LIMIT_EXCEEDED:
    return SALTS_EMSGSIZE;
  case CSERDE_UNSUPPORTED:
    return SALTS_ENOTSUP;
  case CSERDE_VALUE_OUT_OF_RANGE:
    return SALTS_ERANGE;
  case CSERDE_SOURCE_ERROR:
  case CSERDE_SINK_ERROR:
    return SALTS_EIO;
  case CSERDE_DONE:
  case CSERDE_INVALID_ARGUMENT:
  case CSERDE_INVALID_STATE:
  case CSERDE_INVALID_TOKEN:
  case CSERDE_UNEXPECTED_END:
  case CSERDE_CALLBACK_ERROR:
  default:
    return SALTS_EINVAL;
  }
}

static int crpc_bounded_length(const char *text, size_t limit, size_t *out_size) {
  size_t index;
  if (text == NULL || out_size == NULL) return SALTS_EINVAL;
  for (index = 0u; index <= limit; ++index) {
    if (text[index] == '\0') {
      *out_size = index;
      return SALTS_OK;
    }
  }
  return SALTS_EMSGSIZE;
}

static bool crpc_method_reserved(const char *service, size_t service_size, const char *name,
                                 size_t name_size) {
  char prefix[4];
  size_t index;
  for (index = 0u; index < sizeof(prefix); ++index) {
    if (service != NULL) {
      if (index < service_size) prefix[index] = service[index];
      else if (index == service_size) prefix[index] = '.';
      else if (index - service_size - 1u < name_size)
        prefix[index] = name[index - service_size - 1u];
      else return false;
    } else {
      if (index >= name_size) return false;
      prefix[index] = name[index];
    }
  }
  return memcmp(prefix, "rpc.", sizeof(prefix)) == 0;
}

static int crpc_method_validate(const crpc_method *method, size_t max_method_bytes,
                                size_t *out_service_size, size_t *out_name_size) {
  size_t service_size = 0u;
  size_t name_size = 0u;
  size_t combined_size;
  int status;
  if (method == NULL || method->name == NULL || max_method_bytes == 0u) return SALTS_EINVAL;
  status = crpc_bounded_length(method->name, max_method_bytes, &name_size);
  if (status != SALTS_OK) return status;
  if (name_size == 0u || !vstr_utf8_valid(vstr_from_buf(method->name, name_size)))
    return SALTS_EINVAL;
  combined_size = name_size;
  if (method->service != NULL) {
    status = crpc_bounded_length(method->service, max_method_bytes, &service_size);
    if (status != SALTS_OK) return status;
    if (service_size == 0u || !vstr_utf8_valid(vstr_from_buf(method->service, service_size)))
      return SALTS_EINVAL;
    if (service_size >= max_method_bytes || name_size > max_method_bytes - service_size - 1u)
      return SALTS_EMSGSIZE;
    combined_size = service_size + 1u + name_size;
  }
  if (combined_size > max_method_bytes) return SALTS_EMSGSIZE;
  if (crpc_method_reserved(method->service, service_size, method->name, name_size))
    return SALTS_EPERM;
  *out_service_size = service_size;
  *out_name_size = name_size;
  return SALTS_OK;
}

int crpc_method_format(const crpc_method *method, size_t max_method_bytes, char *out,
                       size_t out_capacity) {
  size_t service_size = 0u;
  size_t name_size = 0u;
  size_t size;
  int status;
  if (out == NULL || out_capacity == 0u) return SALTS_EINVAL;
  out[0] = '\0';
  status = crpc_method_validate(method, max_method_bytes, &service_size, &name_size);
  if (status != SALTS_OK) return status;
  size = service_size == 0u ? name_size : service_size + 1u + name_size;
  if (size >= out_capacity) return SALTS_EMSGSIZE;
  if (service_size != 0u) {
    memcpy(out, method->service, service_size);
    out[service_size] = '.';
    memcpy(out + service_size + 1u, method->name, name_size);
  } else {
    memcpy(out, method->name, name_size);
  }
  out[size] = '\0';
  return SALTS_OK;
}

static cserde_status crpc_json_encode_value(crpc_buffer *buffer, crpc_encode_value_fn encode,
                                            void *user, size_t max_depth,
                                            bool require_structured_root) {
  crpc_json_writer_context context = {
      .buffer = buffer,
      .max_depth = max_depth,
      .require_structured_root = require_structured_root,
  };
  cserde_token null_token = {.kind = CSERDE_NULL};
  cserde_status status;

  if (max_depth != 0u) {
    if (max_depth > SIZE_MAX / sizeof(*context.frames)) return CSERDE_LIMIT_EXCEEDED;
    context.frames = (crpc_json_frame *)calloc(max_depth, sizeof(*context.frames));
    if (context.frames == NULL) return CSERDE_SINK_ERROR;
  }
  status = cserde_writer_init(&context.writer, &crpc_json_writer_ops, &context);
  if (status == CSERDE_OK)
    status = encode != NULL ? encode(user, &context.writer)
                            : cserde_writer_write(&context.writer, &null_token);
  if (status == CSERDE_OK) status = cserde_writer_finish(&context.writer);
  free(context.frames);
  return status;
}

static cserde_status crpc_json_append_uint64(crpc_buffer *buffer, uint64_t value) {
  char text[3u * sizeof(value) + 1u];
  const int size = snprintf(text, sizeof(text), "%" PRIu64, value);
  if (size <= 0 || (size_t)size >= sizeof(text)) return CSERDE_SINK_ERROR;
  return crpc_buffer_append(buffer, text, (size_t)size);
}

static cserde_status crpc_json_append_int64(crpc_buffer *buffer, int64_t value) {
  char text[3u * sizeof(value) + 2u];
  const int size = snprintf(text, sizeof(text), "%" PRId64, value);
  if (size <= 0 || (size_t)size >= sizeof(text)) return CSERDE_SINK_ERROR;
  return crpc_buffer_append(buffer, text, (size_t)size);
}

int crpc_json_encode_request(const crpc_method *method, uint64_t request_id,
                             crpc_encode_params_fn encode_params, void *params_user,
                             size_t max_method_bytes, size_t max_json_depth, size_t max_body_bytes,
                             crpc_encoded_request *out) {
  static const char prefix[] = "{\"jsonrpc\":\"2.0\",\"method\":\"";
  static const char params_prefix[] = ",\"params\":";
  static const char id_prefix[] = ",\"id\":";
  crpc_buffer buffer = {.limit = max_body_bytes};
  crpc_json_writer_context writer_context = {0};
  size_t service_size = 0u;
  size_t name_size = 0u;
  cserde_status serde_status;
  char id_text[3u * sizeof(uint64_t) + 1u];
  int id_size;
  int status;

  if (out == NULL) return SALTS_EINVAL;
  *out = (crpc_encoded_request){0};
  if (max_json_depth < 2u || max_body_bytes == 0u || max_body_bytes == SIZE_MAX)
    return SALTS_EINVAL;
  status = crpc_method_validate(method, max_method_bytes, &service_size, &name_size);
  if (status != SALTS_OK) return status;
  serde_status = crpc_buffer_append(&buffer, prefix, sizeof(prefix) - 1u);
  if (serde_status == CSERDE_OK && method->service != NULL)
    serde_status =
        crpc_json_string_contents(&buffer, (const unsigned char *)method->service, service_size);
  if (serde_status == CSERDE_OK && method->service != NULL)
    serde_status = crpc_buffer_byte(&buffer, (unsigned char)'.');
  if (serde_status == CSERDE_OK)
    serde_status =
        crpc_json_string_contents(&buffer, (const unsigned char *)method->name, name_size);
  if (serde_status == CSERDE_OK) serde_status = crpc_buffer_byte(&buffer, (unsigned char)'"');
  if (serde_status != CSERDE_OK) {
    free(buffer.data);
    return crpc_cserde_status(serde_status);
  }

  if (encode_params != NULL) {
    serde_status = crpc_buffer_append(&buffer, params_prefix, sizeof(params_prefix) - 1u);
    if (serde_status != CSERDE_OK) {
      free(buffer.data);
      return crpc_cserde_status(serde_status);
    }
    if (max_json_depth - 1u > SIZE_MAX / sizeof(*writer_context.frames)) {
      free(buffer.data);
      return SALTS_EMSGSIZE;
    }
    writer_context.frames =
        (crpc_json_frame *)calloc(max_json_depth - 1u, sizeof(*writer_context.frames));
    if (writer_context.frames == NULL) {
      free(buffer.data);
      return SALTS_ENOMEM;
    }
    writer_context.buffer = &buffer;
    writer_context.max_depth = max_json_depth - 1u;
    writer_context.require_structured_root = true;
    serde_status =
        cserde_writer_init(&writer_context.writer, &crpc_json_writer_ops, &writer_context);
    if (serde_status == CSERDE_OK)
      serde_status = encode_params(params_user, &writer_context.writer);
    if (serde_status == CSERDE_OK &&
        (!writer_context.root_written || !writer_context.root_structured ||
         writer_context.depth != 0u))
      serde_status = CSERDE_INVALID_TOKEN;
    if (serde_status == CSERDE_OK) serde_status = cserde_writer_finish(&writer_context.writer);
    free(writer_context.frames);
    if (serde_status != CSERDE_OK) {
      free(buffer.data);
      return crpc_cserde_status(serde_status);
    }
  }

  serde_status = crpc_buffer_append(&buffer, id_prefix, sizeof(id_prefix) - 1u);
  id_size = snprintf(id_text, sizeof(id_text), "%" PRIu64, request_id);
  if (serde_status == CSERDE_OK && (id_size <= 0 || (size_t)id_size >= sizeof(id_text)))
    serde_status = CSERDE_SINK_ERROR;
  if (serde_status == CSERDE_OK)
    serde_status = crpc_buffer_append(&buffer, id_text, (size_t)id_size);
  if (serde_status == CSERDE_OK) serde_status = crpc_buffer_byte(&buffer, (unsigned char)'}');
  if (serde_status != CSERDE_OK) {
    free(buffer.data);
    return crpc_cserde_status(serde_status);
  }
  out->data = buffer.data;
  out->size = buffer.size;
  return SALTS_OK;
}

int crpc_json_encode_result(uint64_t request_id, crpc_encode_value_fn encode, void *user,
                            size_t max_json_depth, size_t max_body_bytes,
                            crpc_encoded_request *out) {
  static const char prefix[] = "{\"jsonrpc\":\"2.0\",\"result\":";
  static const char id_prefix[] = ",\"id\":";
  crpc_buffer buffer = {.limit = max_body_bytes};
  cserde_status serde_status;

  if (out == NULL) return SALTS_EINVAL;
  *out = (crpc_encoded_request){0};
  if (max_json_depth < 2u || max_body_bytes == 0u || max_body_bytes == SIZE_MAX)
    return SALTS_EINVAL;
  serde_status = crpc_buffer_append(&buffer, prefix, sizeof(prefix) - 1u);
  if (serde_status == CSERDE_OK)
    serde_status = crpc_json_encode_value(&buffer, encode, user, max_json_depth - 1u, false);
  if (serde_status == CSERDE_OK)
    serde_status = crpc_buffer_append(&buffer, id_prefix, sizeof(id_prefix) - 1u);
  if (serde_status == CSERDE_OK) serde_status = crpc_json_append_uint64(&buffer, request_id);
  if (serde_status == CSERDE_OK) serde_status = crpc_buffer_byte(&buffer, (unsigned char)'}');
  if (serde_status != CSERDE_OK) {
    free(buffer.data);
    return crpc_cserde_status(serde_status);
  }
  out->data = buffer.data;
  out->size = buffer.size;
  return SALTS_OK;
}

int crpc_json_encode_error(bool null_id, uint64_t request_id, int64_t code, const char *message,
                           crpc_encode_value_fn encode_data, void *data_user, size_t max_json_depth,
                           size_t max_body_bytes, crpc_encoded_request *out) {
  static const char prefix[] = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":";
  static const char message_prefix[] = ",\"message\":";
  static const char data_prefix[] = ",\"data\":";
  static const char id_prefix[] = "},\"id\":";
  crpc_buffer buffer = {.limit = max_body_bytes};
  size_t message_size = 0u;
  cserde_status serde_status;
  int status;

  if (out == NULL) return SALTS_EINVAL;
  *out = (crpc_encoded_request){0};
  if (message == NULL || max_json_depth < 2u || max_body_bytes == 0u || max_body_bytes == SIZE_MAX)
    return SALTS_EINVAL;
  status = crpc_bounded_length(message, max_body_bytes, &message_size);
  if (status != SALTS_OK) return status;

  serde_status = crpc_buffer_append(&buffer, prefix, sizeof(prefix) - 1u);
  if (serde_status == CSERDE_OK) serde_status = crpc_json_append_int64(&buffer, code);
  if (serde_status == CSERDE_OK)
    serde_status = crpc_buffer_append(&buffer, message_prefix, sizeof(message_prefix) - 1u);
  if (serde_status == CSERDE_OK)
    serde_status = crpc_json_string(&buffer, (const unsigned char *)message, message_size);
  if (serde_status == CSERDE_OK && encode_data != NULL)
    serde_status = crpc_buffer_append(&buffer, data_prefix, sizeof(data_prefix) - 1u);
  if (serde_status == CSERDE_OK && encode_data != NULL)
    serde_status =
        crpc_json_encode_value(&buffer, encode_data, data_user, max_json_depth - 2u, false);
  if (serde_status == CSERDE_OK)
    serde_status = crpc_buffer_append(&buffer, id_prefix, sizeof(id_prefix) - 1u);
  if (serde_status == CSERDE_OK)
    serde_status = null_id ? crpc_buffer_append(&buffer, "null", 4u)
                           : crpc_json_append_uint64(&buffer, request_id);
  if (serde_status == CSERDE_OK) serde_status = crpc_buffer_byte(&buffer, (unsigned char)'}');
  if (serde_status != CSERDE_OK) {
    free(buffer.data);
    return crpc_cserde_status(serde_status);
  }
  out->data = buffer.data;
  out->size = buffer.size;
  return SALTS_OK;
}

int crpc_json_encode_batch(const crpc_encoded_request *items, size_t item_count,
                           size_t max_body_bytes, crpc_encoded_request *out) {
  crpc_buffer buffer = {.limit = max_body_bytes};
  cserde_status serde_status;
  size_t index;

  if (out == NULL) return SALTS_EINVAL;
  *out = (crpc_encoded_request){0};
  if (items == NULL || item_count == 0u || max_body_bytes == 0u || max_body_bytes == SIZE_MAX)
    return SALTS_EINVAL;
  serde_status = crpc_buffer_byte(&buffer, (unsigned char)'[');
  for (index = 0u; serde_status == CSERDE_OK && index < item_count; ++index) {
    if (items[index].data == NULL || items[index].size == 0u) {
      serde_status = CSERDE_INVALID_TOKEN;
      break;
    }
    if (index != 0u) serde_status = crpc_buffer_byte(&buffer, (unsigned char)',');
    if (serde_status == CSERDE_OK)
      serde_status = crpc_buffer_append(&buffer, items[index].data, items[index].size);
  }
  if (serde_status == CSERDE_OK) serde_status = crpc_buffer_byte(&buffer, (unsigned char)']');
  if (serde_status != CSERDE_OK) {
    free(buffer.data);
    return crpc_cserde_status(serde_status);
  }
  out->data = buffer.data;
  out->size = buffer.size;
  return SALTS_OK;
}

void crpc_encoded_request_destroy(crpc_encoded_request *request) {
  if (request == NULL) return;
  free(request->data);
  *request = (crpc_encoded_request){0};
}

bool crpc_json_depth_valid(const unsigned char *data, size_t size, size_t max_depth) {
  size_t depth = 0u;
  size_t index;
  bool in_string = false;
  bool escaped = false;
  if (data == NULL || size == 0u || max_depth == 0u) return false;
  for (index = 0u; index < size; ++index) {
    const unsigned char byte = data[index];
    if (in_string) {
      if (escaped) escaped = false;
      else if (byte == (unsigned char)'\\') escaped = true;
      else if (byte == (unsigned char)'"') in_string = false;
      continue;
    }
    if (byte == (unsigned char)'"') {
      in_string = true;
    } else if (byte == (unsigned char)'{' || byte == (unsigned char)'[') {
      if (depth == max_depth) return false;
      ++depth;
    } else if (byte == (unsigned char)'}' || byte == (unsigned char)']') {
      if (depth == 0u) return false;
      --depth;
    }
  }
  return depth == 0u;
}

json_value_t *crpc_json_unique_member(const json_value_t *object, const char *name,
                                      size_t *out_count) {
  const size_t name_size = strlen(name);
  const size_t count = json_object_size(object);
  json_value_t *found = NULL;
  size_t matches = 0u;
  size_t index;
  for (index = 0u; index < count; ++index) {
    const char *key = json_object_key(object, index);
    const size_t key_size = json_object_key_len(object, index);
    if (key != NULL && key_size == name_size && memcmp(key, name, name_size) == 0) {
      found = json_object_value(object, index);
      ++matches;
    }
  }
  if (out_count != NULL) *out_count = matches;
  return matches == 1u ? found : NULL;
}

static bool crpc_decimal_integer(const char *text, size_t size, bool *negative,
                                 uint64_t *magnitude) {
  size_t index = 0u;
  size_t fraction_digits = 0u;
  size_t digit_count = 0u;
  size_t leading_zeros = 0u;
  size_t trailing_zeros = 0u;
  size_t significant_digits;
  size_t remove_digits = 0u;
  size_t append_zeros = 0u;
  int exponent_sign = 1;
  size_t exponent = 0u;
  uint64_t value = 0u;
  bool seen_nonzero = false;
  bool exponent_overflow = false;

  if (text == NULL || size == 0u || negative == NULL || magnitude == NULL) return false;
  *negative = text[index] == '-';
  if (*negative && ++index == size) return false;
  while (index < size && text[index] >= '0' && text[index] <= '9') {
    const bool zero = text[index] == '0';
    ++digit_count;
    if (!seen_nonzero && zero) ++leading_zeros;
    else seen_nonzero = true;
    if (zero && trailing_zeros == SIZE_MAX) return false;
    trailing_zeros = zero ? trailing_zeros + 1u : 0u;
    ++index;
  }
  if (digit_count == 0u) return false;
  if (index < size && text[index] == '.') {
    const size_t fraction_start = ++index;
    while (index < size && text[index] >= '0' && text[index] <= '9') {
      const bool zero = text[index] == '0';
      ++digit_count;
      if (!seen_nonzero && zero) ++leading_zeros;
      else seen_nonzero = true;
      if (zero && trailing_zeros == SIZE_MAX) return false;
      trailing_zeros = zero ? trailing_zeros + 1u : 0u;
      ++index;
    }
    fraction_digits = index - fraction_start;
    if (fraction_digits == 0u) return false;
  }
  if (index < size && (text[index] == 'e' || text[index] == 'E')) {
    ++index;
    if (index < size && (text[index] == '+' || text[index] == '-')) {
      if (text[index] == '-') exponent_sign = -1;
      ++index;
    }
    if (index == size || text[index] < '0' || text[index] > '9') return false;
    while (index < size && text[index] >= '0' && text[index] <= '9') {
      const size_t digit = (size_t)(text[index] - '0');
      if (!exponent_overflow) {
        if (exponent > (SIZE_MAX - digit) / 10u) exponent_overflow = true;
        else exponent = exponent * 10u + digit;
      }
      ++index;
    }
  }
  if (index != size) return false;
  if (!seen_nonzero) {
    *magnitude = 0u;
    return true;
  }
  if (exponent_overflow) return false;
  significant_digits = digit_count - leading_zeros;
  if (exponent_sign < 0) {
    if (exponent > SIZE_MAX - fraction_digits) return false;
    remove_digits = fraction_digits + exponent;
  } else if (exponent < fraction_digits) {
    remove_digits = fraction_digits - exponent;
  } else {
    append_zeros = exponent - fraction_digits;
  }
  if (remove_digits > trailing_zeros || remove_digits > significant_digits) return false;
  significant_digits -= remove_digits;
  if (append_zeros > 20u || significant_digits > 20u - append_zeros) return false;

  index = *negative ? 1u : 0u;
  {
    size_t skipped = 0u;
    size_t consumed = 0u;
    const size_t wanted = digit_count - remove_digits;
    for (; index < size && consumed < wanted; ++index) {
      const unsigned char byte = (unsigned char)text[index];
      uint64_t digit;
      if (byte == '.' || byte == 'e' || byte == 'E') {
        if (byte == 'e' || byte == 'E') break;
        continue;
      }
      if (byte == '+' || byte == '-') continue;
      if (skipped < leading_zeros) {
        ++skipped;
        ++consumed;
        continue;
      }
      digit = (uint64_t)(byte - (unsigned char)'0');
      if (value > (UINT64_MAX - digit) / UINT64_C(10)) return false;
      value = value * UINT64_C(10) + digit;
      ++consumed;
    }
  }
  while (append_zeros-- > 0u) {
    if (value > UINT64_MAX / UINT64_C(10)) return false;
    value *= UINT64_C(10);
  }
  *magnitude = value;
  return true;
}

bool crpc_json_uint64(const json_value_t *value, uint64_t *out) {
  const char *text;
  size_t size = 0u;
  bool negative;
  uint64_t magnitude;
  if (value == NULL || json_type(value) != JSON_NUMBER || out == NULL) return false;
  text = json_number_text(value, &size);
  if (!crpc_decimal_integer(text, size, &negative, &magnitude) || negative) return false;
  *out = magnitude;
  return true;
}

static bool crpc_json_int64(const json_value_t *value, int64_t *out) {
  const char *text;
  size_t size = 0u;
  bool negative;
  uint64_t magnitude;
  const uint64_t negative_limit = (uint64_t)INT64_MAX + UINT64_C(1);
  if (value == NULL || json_type(value) != JSON_NUMBER || out == NULL) return false;
  text = json_number_text(value, &size);
  if (!crpc_decimal_integer(text, size, &negative, &magnitude)) return false;
  if ((!negative && magnitude > (uint64_t)INT64_MAX) || (negative && magnitude > negative_limit))
    return false;
  *out = negative ? (magnitude == negative_limit ? INT64_MIN : -(int64_t)magnitude)
                  : (int64_t)magnitude;
  return true;
}

int crpc_json_decode_response(const void *data, size_t size, uint64_t expected_id,
                              unsigned int http_status, size_t max_json_depth,
                              const cmeta_callable *callable, crpc_decoded_response *out,
                              const char **out_stage) {
  json_value_t *root;
  json_value_t *version;
  json_value_t *id;
  json_value_t *result;
  json_value_t *error;
  size_t version_count = 0u;
  size_t id_count = 0u;
  size_t result_count = 0u;
  size_t error_count = 0u;
  uint64_t response_id;
  cserde_reader *reader = NULL;
  int status = SALTS_EPROTO;

  if (out == NULL || out_stage == NULL) return SALTS_EINVAL;
  *out = (crpc_decoded_response){0};
  *out_stage = "rpc-envelope";
  if (http_status < 200u || http_status >= 300u) {
    *out_stage = "http-status";
    return SALTS_EPROTO;
  }
  if (data == NULL || size == 0u) {
    *out_stage = "json-parse";
    return SALTS_EPROTO;
  }
  if (!crpc_json_depth_valid((const unsigned char *)data, size, max_json_depth)) {
    *out_stage = "json-depth";
    return SALTS_EMSGSIZE;
  }
  root = json_parse((const char *)data, size);
  if (root == NULL) {
    *out_stage = "json-parse";
    return SALTS_EPROTO;
  }
  if (json_type(root) != JSON_OBJECT) goto fail;
  version = crpc_json_unique_member(root, "jsonrpc", &version_count);
  id = crpc_json_unique_member(root, "id", &id_count);
  result = crpc_json_unique_member(root, "result", &result_count);
  error = crpc_json_unique_member(root, "error", &error_count);
  if (version_count != 1u || id_count != 1u || result_count > 1u || error_count > 1u ||
      (result_count == 1u) == (error_count == 1u) || version == NULL || id == NULL ||
      json_type(version) != JSON_STRING || json_string_len(version) != 3u ||
      memcmp(json_string(version), "2.0", 3u) != 0 || !crpc_json_uint64(id, &response_id) ||
      response_id != expected_id)
    goto fail;

  if (callable != NULL) {
    out->callable = *callable;
    out->has_callable = true;
  }
  out->response = (crpc_response_view){.request_id = response_id,
                                       .http_status = http_status,
                                       .callable = out->has_callable ? &out->callable : NULL};
  if (result_count == 1u) {
    reader = json_cserde_reader_create(result, max_json_depth);
    if (reader == NULL) {
      status = SALTS_ENOMEM;
      *out_stage = "result-reader";
      goto fail;
    }
    out->response.kind = CRPC_RESPONSE_RESULT;
    out->response.value.result = reader;
  } else {
    json_value_t *code;
    json_value_t *message;
    json_value_t *error_data;
    size_t code_count = 0u;
    size_t message_count = 0u;
    size_t data_count = 0u;
    int64_t error_code;
    if (error == NULL || json_type(error) != JSON_OBJECT) goto fail;
    code = crpc_json_unique_member(error, "code", &code_count);
    message = crpc_json_unique_member(error, "message", &message_count);
    error_data = crpc_json_unique_member(error, "data", &data_count);
    if (code_count != 1u || message_count != 1u || data_count > 1u ||
        !crpc_json_int64(code, &error_code) || message == NULL || json_type(message) != JSON_STRING)
      goto fail;
    if (data_count == 1u) {
      reader = json_cserde_reader_create(error_data, max_json_depth);
      if (reader == NULL) {
        status = SALTS_ENOMEM;
        *out_stage = "error-data-reader";
        goto fail;
      }
    }
    out->response.kind = CRPC_RESPONSE_REMOTE_ERROR;
    out->response.value.remote_error =
        (crpc_remote_error){.code = error_code,
                            .message = {(const unsigned char *)json_string(message),
                                        json_string_len(message), CSERDE_VIEW_STABLE},
                            .data = reader};
  }
  out->json_root = root;
  out->reader = reader;
  return SALTS_OK;

fail:
  json_cserde_reader_destroy(reader);
  json_free(root);
  return status;
}

void crpc_decoded_response_destroy(crpc_decoded_response *response) {
  if (response == NULL) return;
  json_cserde_reader_destroy(response->reader);
  json_free((json_value_t *)response->json_root);
  *response = (crpc_decoded_response){0};
}
