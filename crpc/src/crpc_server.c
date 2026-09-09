#include "crpc_internal.h"

#include <json_cserde_reader.h>
#include <vstr.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  CRPC_SERVER_RESPONSE_MAGIC = 0x43525043u,
  CRPC_PARSE_ERROR = -32700,
  CRPC_INVALID_REQUEST = -32600,
  CRPC_METHOD_NOT_FOUND = -32601,
  CRPC_INTERNAL_ERROR = -32603
};

static const char CRPC_SERVER_LARGEST_PROTOCOL_ERROR[] =
    "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32601,\"message\":\"Method not "
    "found\"},\"id\":18446744073709551615}";

typedef struct crpc_server_method_record {
  char *target;
  char *method;
  cmeta_callable callable;
  bool has_callable;
  crpc_server_method_fn handler;
  void *user;
} crpc_server_method_record;

typedef struct crpc_server_impl {
  chttp_server http;
  crpc_server_config config;
  crpc_server_method_record *methods;
  char *target_storage;
  char *method_storage;
  char *method_scratch;
  size_t method_count;
  size_t response_body_limit;
  bool started;
} crpc_server_impl;

typedef struct crpc_server_response_context {
  uint32_t magic;
  crpc_server_impl *server;
  size_t response_body_limit;
  uint64_t request_id;
  bool notification;
  bool completed;
  crpc_encoded_request encoded;
} crpc_server_response_context;

typedef struct crpc_server_dispatch_result {
  bool emit;
  crpc_encoded_request encoded;
} crpc_server_dispatch_result;

static crpc_server_impl *crpc_server_get(crpc_server *server) {
  return server != NULL ? (crpc_server_impl *)server->impl : NULL;
}

static const crpc_server_impl *crpc_server_get_const(const crpc_server *server) {
  return server != NULL ? (const crpc_server_impl *)server->impl : NULL;
}

static bool crpc_server_multiply(size_t left, size_t right, size_t *out) {
  if (out == NULL || (left != 0u && right > SIZE_MAX / left)) return false;
  *out = left * right;
  return true;
}

static bool crpc_server_add(size_t left, size_t right, size_t *out) {
  if (out == NULL || right > SIZE_MAX - left) return false;
  *out = left + right;
  return true;
}

static bool crpc_server_batch_minimum_response_size(size_t item_count, size_t *out) {
  size_t encoded_items_size;
  size_t separators_size;
  size_t batch_size;
  if (item_count == 0u || out == NULL ||
      !crpc_server_multiply(item_count, sizeof(CRPC_SERVER_LARGEST_PROTOCOL_ERROR) - 1u,
                            &encoded_items_size))
    return false;
  separators_size = item_count - 1u;
  if (!crpc_server_add(encoded_items_size, separators_size, &batch_size) ||
      !crpc_server_add(batch_size, 2u, out))
    return false;
  return true;
}

static int crpc_server_bounded_length(const char *text, size_t limit, size_t *out_size) {
  size_t index;
  if (text == NULL || out_size == NULL) return SALTS_EINVAL;
  for (index = 0u; index <= limit; ++index) {
    if (text[index] == '\0') {
      *out_size = index;
      return SALTS_OK;
    }
  }
  return SALTS_ENAMETOOLONG;
}

static int crpc_server_config_validate(const crpc_server_config *config, size_t *out_target_bytes,
                                       size_t *out_method_bytes) {
  size_t minimum_response_size;
  if (config == NULL || config->method_capacity == 0u || config->max_method_bytes == 0u ||
      config->max_method_bytes == SIZE_MAX || config->max_json_depth < 2u ||
      config->max_batch_items == 0u || config->http.max_target_bytes == 0u ||
      config->http.max_target_bytes == SIZE_MAX || config->http.max_request_body_bytes == 0u ||
      config->http.max_response_body_bytes == 0u ||
      config->http.max_buffered_response_body_bytes == 0u || out_target_bytes == NULL ||
      out_method_bytes == NULL)
    return SALTS_EINVAL;
  if (config->method_capacity > SIZE_MAX / sizeof(crpc_server_method_record) ||
      config->max_batch_items > SIZE_MAX / sizeof(crpc_encoded_request) ||
      !crpc_server_batch_minimum_response_size(config->max_batch_items, &minimum_response_size))
    return SALTS_ERANGE;
  if (config->http.max_buffered_response_body_bytes < minimum_response_size) return SALTS_EMSGSIZE;
  if (!crpc_server_multiply(config->method_capacity, config->http.max_target_bytes + 1u,
                            out_target_bytes) ||
      !crpc_server_multiply(config->method_capacity, config->max_method_bytes + 1u,
                            out_method_bytes))
    return SALTS_ERANGE;
  return SALTS_OK;
}

static void crpc_server_impl_free(crpc_server_impl *impl) {
  if (impl == NULL) return;
  free(impl->method_storage);
  free(impl->method_scratch);
  free(impl->target_storage);
  free(impl->methods);
  free(impl);
}

static crpc_server_method_record *crpc_server_find_method(crpc_server_impl *server,
                                                          const char *target,
                                                          const unsigned char *method,
                                                          size_t method_size) {
  size_t index;
  for (index = 0u; index < server->method_count; ++index) {
    crpc_server_method_record *record = &server->methods[index];
    if (strcmp(record->target, target) == 0 && strlen(record->method) == method_size &&
        memcmp(record->method, method, method_size) == 0)
      return record;
  }
  return NULL;
}

static bool crpc_server_target_registered(const crpc_server_impl *server, const char *target) {
  size_t index;
  for (index = 0u; index < server->method_count; ++index)
    if (strcmp(server->methods[index].target, target) == 0) return true;
  return false;
}

static int crpc_server_protocol_error(crpc_server_impl *server, size_t response_body_limit,
                                      bool null_id, uint64_t request_id, int64_t code,
                                      const char *message, crpc_server_dispatch_result *out) {
  int status =
      crpc_json_encode_error(null_id, request_id, code, message, NULL, NULL,
                             server->config.max_json_depth, response_body_limit, &out->encoded);
  if (status == SALTS_OK) out->emit = true;
  return status;
}

static bool crpc_server_version_valid(const json_value_t *version) {
  return version != NULL && json_type(version) == JSON_STRING && json_string_len(version) == 3u &&
         memcmp(json_string(version), "2.0", 3u) == 0;
}

static bool crpc_server_method_name_valid(const json_value_t *method, size_t max_method_bytes) {
  const char *name;
  size_t size;
  if (method == NULL || json_type(method) != JSON_STRING) return false;
  name = json_string(method);
  size = json_string_len(method);
  return name != NULL && size != 0u && size <= max_method_bytes &&
         memchr(name, '\0', size) == NULL && vstr_utf8_valid(vstr_from_buf(name, size)) &&
         (size < 4u || memcmp(name, "rpc.", 4u) != 0);
}

static int crpc_server_dispatch_object(crpc_server_impl *server,
                                       const chttp_server_request_view *http_request,
                                       const json_value_t *object, size_t response_body_limit,
                                       crpc_server_dispatch_result *out) {
  json_value_t *version;
  json_value_t *method;
  json_value_t *params;
  json_value_t *id;
  size_t version_count = 0u;
  size_t method_count = 0u;
  size_t params_count = 0u;
  size_t id_count = 0u;
  uint64_t request_id = 0u;
  bool notification;
  bool notification_shape;
  crpc_server_method_record *record;
  cserde_reader *params_reader = NULL;
  crpc_server_response_context response_context;
  crpc_server_request_view request_view;
  crpc_server_response response;
  int handler_status;
  int status;

  *out = (crpc_server_dispatch_result){0};
  if (object == NULL || json_type(object) != JSON_OBJECT)
    return crpc_server_protocol_error(server, response_body_limit, true, 0u, CRPC_INVALID_REQUEST,
                                      "Invalid Request", out);
  version = crpc_json_unique_member(object, "jsonrpc", &version_count);
  method = crpc_json_unique_member(object, "method", &method_count);
  params = crpc_json_unique_member(object, "params", &params_count);
  id = crpc_json_unique_member(object, "id", &id_count);
  notification_shape = version_count == 1u && method_count == 1u &&
                       crpc_server_version_valid(version) &&
                       crpc_server_method_name_valid(method, server->config.max_method_bytes);
  notification = notification_shape && id_count == 0u;

  if (!notification_shape || params_count > 1u || id_count > 1u ||
      (params_count == 1u && json_type(params) != JSON_ARRAY && json_type(params) != JSON_OBJECT) ||
      (id_count == 1u && !crpc_json_uint64(id, &request_id))) {
    if (notification) return SALTS_OK;
    return crpc_server_protocol_error(server, response_body_limit, true, 0u, CRPC_INVALID_REQUEST,
                                      "Invalid Request", out);
  }

  record =
      crpc_server_find_method(server, http_request->path,
                              (const unsigned char *)json_string(method), json_string_len(method));
  if (record == NULL) {
    if (notification) return SALTS_OK;
    return crpc_server_protocol_error(server, response_body_limit, false, request_id,
                                      CRPC_METHOD_NOT_FOUND, "Method not found", out);
  }
  if (params_count == 1u) {
    params_reader = json_cserde_reader_create(params, server->config.max_json_depth);
    if (params_reader == NULL) {
      if (notification) return SALTS_OK;
      return crpc_server_protocol_error(server, response_body_limit, false, request_id,
                                        CRPC_INTERNAL_ERROR, "Internal error", out);
    }
  }

  response_context = (crpc_server_response_context){.magic = CRPC_SERVER_RESPONSE_MAGIC,
                                                    .server = server,
                                                    .response_body_limit = response_body_limit,
                                                    .request_id = request_id,
                                                    .notification = notification};
  request_view =
      (crpc_server_request_view){.http = http_request,
                                 .target = record->target,
                                 .method = record->method,
                                 .request_id = request_id,
                                 .notification = notification ? 1 : 0,
                                 .params = params_reader,
                                 .callable = record->has_callable ? &record->callable : NULL};
  response = (crpc_server_response){.impl = &response_context};
  handler_status = record->handler(record->user, &request_view, &response);
  response_context.magic = 0u;
  json_cserde_reader_destroy(params_reader);

  if (notification) {
    crpc_encoded_request_destroy(&response_context.encoded);
    return SALTS_OK;
  }
  if (handler_status != SALTS_OK || !response_context.completed) {
    crpc_encoded_request_destroy(&response_context.encoded);
    return crpc_server_protocol_error(server, response_body_limit, false, request_id,
                                      CRPC_INTERNAL_ERROR, "Internal error", out);
  }
  status = response_context.encoded.data != NULL ? SALTS_OK : SALTS_EPROTO;
  if (status == SALTS_OK) {
    out->emit = true;
    out->encoded = response_context.encoded;
  }
  return status;
}

static int crpc_server_batch(crpc_server_impl *server,
                             const chttp_server_request_view *http_request,
                             const json_value_t *array, crpc_server_dispatch_result *out) {
  crpc_encoded_request *responses;
  crpc_server_dispatch_result item_result;
  const size_t item_count = json_array_size(array);
  size_t response_count = 0u;
  size_t aggregate_size = 2u;
  size_t framing_size;
  size_t item_response_limit;
  size_t index;
  int status = SALTS_OK;

  *out = (crpc_server_dispatch_result){0};
  if (item_count == 0u || item_count > server->config.max_batch_items)
    return crpc_server_protocol_error(server, server->response_body_limit, true, 0u,
                                      CRPC_INVALID_REQUEST, "Invalid Request", out);
  if (!crpc_server_add(item_count, 1u, &framing_size) || framing_size > server->response_body_limit)
    return SALTS_ERANGE;
  item_response_limit = (server->response_body_limit - framing_size) / item_count;
  if (item_response_limit < sizeof(CRPC_SERVER_LARGEST_PROTOCOL_ERROR) - 1u) return SALTS_EMSGSIZE;
  responses = (crpc_encoded_request *)calloc(item_count, sizeof(*responses));
  if (responses == NULL) return SALTS_ENOMEM;
  for (index = 0u; index < item_count; ++index) {
    const int item_status = crpc_server_dispatch_object(
        server, http_request, json_array_get(array, index), item_response_limit, &item_result);
    if (item_status != SALTS_OK) {
      if (status == SALTS_OK) status = item_status;
      crpc_encoded_request_destroy(&item_result.encoded);
      continue;
    }
    if (item_result.emit) {
      const size_t separator_size = response_count == 0u ? 0u : 1u;
      if (status != SALTS_OK || aggregate_size > server->response_body_limit ||
          separator_size > server->response_body_limit - aggregate_size ||
          item_result.encoded.size >
              server->response_body_limit - aggregate_size - separator_size) {
        crpc_encoded_request_destroy(&item_result.encoded);
        if (status == SALTS_OK) status = SALTS_EMSGSIZE;
        continue;
      }
      aggregate_size += separator_size + item_result.encoded.size;
      responses[response_count++] = item_result.encoded;
    }
  }
  if (status == SALTS_OK && response_count != 0u) {
    status = crpc_json_encode_batch(responses, response_count, server->response_body_limit,
                                    &out->encoded);
    if (status == SALTS_OK) out->emit = true;
  }
  for (index = 0u; index < response_count; ++index)
    crpc_encoded_request_destroy(&responses[index]);
  free(responses);
  return status;
}

static int crpc_server_route_handler(void *user, const chttp_server_request_view *request,
                                     chttp_server_response *response) {
  crpc_server_impl *server = (crpc_server_impl *)user;
  crpc_server_dispatch_result result = {0};
  json_value_t *root = NULL;
  int status;

  if (server == NULL || request == NULL || response == NULL) return SALTS_EINVAL;
  if (request->body == NULL || request->body_size == 0u) {
    status = crpc_server_protocol_error(server, server->response_body_limit, true, 0u,
                                        CRPC_PARSE_ERROR, "Parse error", &result);
  } else if (!crpc_json_depth_valid((const unsigned char *)request->body, request->body_size,
                                    server->config.max_json_depth)) {
    status = crpc_server_protocol_error(server, server->response_body_limit, true, 0u,
                                        CRPC_PARSE_ERROR, "Parse error", &result);
  } else {
    root = json_parse((const char *)request->body, request->body_size);
    if (root == NULL)
      status = crpc_server_protocol_error(server, server->response_body_limit, true, 0u,
                                          CRPC_PARSE_ERROR, "Parse error", &result);
    else if (json_type(root) == JSON_ARRAY)
      status = crpc_server_batch(server, request, root, &result);
    else
      status =
          crpc_server_dispatch_object(server, request, root, server->response_body_limit, &result);
  }
  if (status == SALTS_OK)
    status = result.emit ? chttp_server_reply(response, 200u, "application/json",
                                              result.encoded.data, result.encoded.size)
                         : chttp_server_reply(response, 204u, NULL, NULL, 0u);
  crpc_encoded_request_destroy(&result.encoded);
  json_free(root);
  return status;
}

int crpc_server_init(crpc_server *server, const crpc_server_config *config) {
  crpc_server_impl *impl;
  size_t target_bytes;
  size_t method_bytes;
  int status;

  if (server == NULL || config == NULL) return SALTS_EINVAL;
  if (server->impl != NULL) return SALTS_EALREADY;
  status = crpc_server_config_validate(config, &target_bytes, &method_bytes);
  if (status != SALTS_OK) return status;
  impl = (crpc_server_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->methods =
      (crpc_server_method_record *)calloc(config->method_capacity, sizeof(*impl->methods));
  impl->target_storage = (char *)calloc(target_bytes, 1u);
  impl->method_storage = (char *)calloc(method_bytes, 1u);
  impl->method_scratch = (char *)calloc(config->max_method_bytes + 1u, 1u);
  if (impl->methods == NULL || impl->target_storage == NULL || impl->method_storage == NULL ||
      impl->method_scratch == NULL) {
    crpc_server_impl_free(impl);
    return SALTS_ENOMEM;
  }
  status = chttp_server_init(&impl->http, &config->http);
  if (status != SALTS_OK) {
    crpc_server_impl_free(impl);
    return status;
  }
  impl->config = *config;
  impl->config.http.tls = NULL;
  impl->response_body_limit = config->http.max_buffered_response_body_bytes;
  server->impl = impl;
  return SALTS_OK;
}

chttp_server *crpc_server_http(crpc_server *server) {
  crpc_server_impl *impl = crpc_server_get(server);
  return impl != NULL ? &impl->http : NULL;
}

int crpc_server_register(crpc_server *server, const char *target, const crpc_method *method,
                         crpc_server_method_fn handler, void *user) {
  crpc_server_impl *impl = crpc_server_get(server);
  crpc_server_method_record *record;
  cmeta_callable callable = {0};
  bool has_callable = false;
  bool target_exists;
  size_t target_size = 0u;
  size_t index;
  int status;

  if (impl == NULL || target == NULL || method == NULL || handler == NULL) return SALTS_EINVAL;
  if (impl->started) return SALTS_EBUSY;
  status = crpc_server_bounded_length(target, impl->config.http.max_target_bytes, &target_size);
  if (status != SALTS_OK || target_size == 0u) return status != SALTS_OK ? status : SALTS_EINVAL;
  if (memchr(target, ':', target_size) != NULL) return SALTS_EINVAL;
  status = crpc_method_format(method, impl->config.max_method_bytes, impl->method_scratch,
                              impl->config.max_method_bytes + 1u);
  if (status != SALTS_OK) return status;
  for (index = 0u; index < impl->method_count; ++index)
    if (strcmp(impl->methods[index].target, target) == 0 &&
        strcmp(impl->methods[index].method, impl->method_scratch) == 0)
      return SALTS_EALREADY;
  if (impl->method_count == impl->config.method_capacity) return SALTS_ENOBUFS;
  record = &impl->methods[impl->method_count];
  record->target =
      impl->target_storage + impl->method_count * (impl->config.http.max_target_bytes + 1u);
  record->method = impl->method_storage + impl->method_count * (impl->config.max_method_bytes + 1u);
  memcpy(record->target, target, target_size + 1u);
  memcpy(record->method, impl->method_scratch, strlen(impl->method_scratch) + 1u);
  status = crpc_bind_callable(method->callable, &callable, &has_callable);
  if (status != SALTS_OK) return status;
  target_exists = crpc_server_target_registered(impl, record->target);
  if (!target_exists) {
    status = chttp_server_post(&impl->http, record->target, crpc_server_route_handler, impl);
    if (status != SALTS_OK) return status;
  }
  record->callable = callable;
  record->has_callable = has_callable;
  record->handler = handler;
  record->user = user;
  ++impl->method_count;
  return SALTS_OK;
}

int crpc_server_response_result(crpc_server_response *response, crpc_encode_value_fn encode,
                                void *user) {
  crpc_server_response_context *context;
  int status;
  if (response == NULL || response->impl == NULL) return SALTS_EINVAL;
  context = (crpc_server_response_context *)response->impl;
  if (context->magic != CRPC_SERVER_RESPONSE_MAGIC || context->server == NULL) return SALTS_EINVAL;
  if (context->completed) return SALTS_EALREADY;
  if (context->notification) {
    context->completed = true;
    return SALTS_OK;
  }
  status = crpc_json_encode_result(context->request_id, encode, user,
                                   context->server->config.max_json_depth,
                                   context->response_body_limit, &context->encoded);
  if (status == SALTS_OK) context->completed = true;
  return status;
}

int crpc_server_response_error(crpc_server_response *response, int64_t code, const char *message,
                               crpc_encode_value_fn encode_data, void *data_user) {
  crpc_server_response_context *context;
  int status;
  if (response == NULL || response->impl == NULL || message == NULL) return SALTS_EINVAL;
  context = (crpc_server_response_context *)response->impl;
  if (context->magic != CRPC_SERVER_RESPONSE_MAGIC || context->server == NULL) return SALTS_EINVAL;
  if (context->completed) return SALTS_EALREADY;
  if (context->notification) {
    context->completed = true;
    return SALTS_OK;
  }
  status = crpc_json_encode_error(false, context->request_id, code, message, encode_data, data_user,
                                  context->server->config.max_json_depth,
                                  context->response_body_limit, &context->encoded);
  if (status == SALTS_OK) context->completed = true;
  return status;
}

int crpc_server_start(crpc_server *server) {
  crpc_server_impl *impl = crpc_server_get(server);
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->started) return SALTS_EALREADY;
  status = chttp_server_start(&impl->http);
  if (status == SALTS_OK) impl->started = true;
  return status;
}

int crpc_server_port(const crpc_server *server, uint16_t *out_port) {
  const crpc_server_impl *impl = crpc_server_get_const(server);
  if (impl == NULL) {
    if (out_port != NULL) *out_port = 0u;
    return SALTS_EINVAL;
  }
  return chttp_server_port(&impl->http, out_port);
}

int crpc_server_stop(crpc_server *server, uint32_t timeout_ms) {
  crpc_server_impl *impl = crpc_server_get(server);
  return impl != NULL ? chttp_server_stop(&impl->http, timeout_ms) : SALTS_EINVAL;
}

int crpc_server_destroy(crpc_server *server) {
  crpc_server_impl *impl;
  int status;
  if (server == NULL) return SALTS_EINVAL;
  impl = crpc_server_get(server);
  if (impl == NULL) return SALTS_OK;
  status = chttp_server_destroy(&impl->http);
  if (status != SALTS_OK) return status;
  crpc_server_impl_free(impl);
  server->impl = NULL;
  return SALTS_OK;
}
