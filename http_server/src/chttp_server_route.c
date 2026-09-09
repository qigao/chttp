#include "chttp_server_runtime.h"

#include <stdio.h>
#include <string.h>

enum { CHTTP_SERVER_DEFAULT_WEBSOCKET_FRAME_BYTES = 64u * 1024u };

static bool chttp_server_method_valid(chttp_method method) {
  return method >= CHTTP_METHOD_GET && method <= CHTTP_METHOD_OPTIONS;
}

static bool chttp_server_param_name_char(unsigned char value) {
  return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
         (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
         (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
         value == (unsigned char)'_';
}

static int chttp_server_route_validate(const chttp_server_impl *server, chttp_method method,
                                       const char *path, size_t *out_param_count) {
  const char *cursor;
  size_t param_count = 0u;
  size_t param_name_bytes = 0u;
  if (!chttp_server_method_valid(method) || path == NULL || path[0] == '\0') return SALTS_EINVAL;
  if (strcmp(path, "*") == 0) {
    if (method != CHTTP_METHOD_OPTIONS) return SALTS_EINVAL;
    *out_param_count = 0u;
    return SALTS_OK;
  }
  if (path[0] != '/' || strchr(path, '?') != NULL || strchr(path, '#') != NULL) return SALTS_EINVAL;
  if (strlen(path) > server->config.max_target_bytes) return SALTS_ENAMETOOLONG;
  cursor = path;
  while (*cursor != '\0') {
    const char *segment;
    const char *end;
    size_t name_size;
    if (*cursor != '/') return SALTS_EINVAL;
    segment = ++cursor;
    end = strchr(segment, '/');
    if (end == NULL) end = segment + strlen(segment);
    if (segment[0] == ':') {
      const char *name = segment + 1;
      const char *scan;
      if (name == end) return SALTS_EINVAL;
      for (scan = name; scan != end; ++scan)
        if (!chttp_server_param_name_char((unsigned char)*scan)) return SALTS_EINVAL;
      name_size = (size_t)(end - name) + 1u;
      if (param_count == server->config.max_route_param_count ||
          param_name_bytes > server->config.max_route_param_bytes ||
          name_size > server->config.max_route_param_bytes - param_name_bytes)
        return SALTS_ENOBUFS;
      ++param_count;
      param_name_bytes += name_size;
    } else if (memchr(segment, ':', (size_t)(end - segment)) != NULL) return SALTS_EINVAL;
    cursor = end;
  }
  *out_param_count = param_count;
  return SALTS_OK;
}

int chttp_server_route_register(chttp_server_impl *server,
                                const chttp_server_route_options *options) {
  chttp_server_route_record *route;
  size_t param_count = 0u;
  size_t index;
  int status;
  if (server == NULL || options == NULL || options->handler == NULL ||
      (options->middleware_count != 0u && options->middleware == NULL) ||
      ((options->body_open == NULL) != (options->body_close == NULL)))
    return SALTS_EINVAL;
  if (server->start_called) return SALTS_EBUSY;
  if (server->route_count >= server->config.route_capacity ||
      options->middleware_count > server->config.max_route_middleware_count)
    return SALTS_ENOBUFS;
  status = chttp_server_route_validate(server, options->method, options->path, &param_count);
  if (status != SALTS_OK) return status;
  for (index = 0u; index < options->middleware_count; ++index)
    if (options->middleware[index].handler == NULL) return SALTS_EINVAL;
  for (index = 0u; index < server->route_count; ++index)
    if (server->routes[index].method == options->method &&
        strcmp(server->routes[index].path, options->path) == 0)
      return SALTS_EALREADY;
  route = &server->routes[server->route_count];
  memcpy(route->path, options->path, strlen(options->path) + 1u);
  if (options->middleware_count != 0u)
    memcpy(route->middleware, options->middleware,
           options->middleware_count * sizeof(*route->middleware));
  route->method = options->method;
  route->middleware_count = options->middleware_count;
  route->param_count = param_count;
  route->handler = options->handler;
  route->user = options->user;
  route->body_open = options->body_open;
  route->body_close = options->body_close;
  route->dynamic = param_count != 0u;
  ++server->route_count;
  return SALTS_OK;
}

int chttp_server_route(chttp_server *server, chttp_method method, const char *path,
                       chttp_server_handler_fn handler, void *user) {
  const chttp_server_route_options options = {
      .method = method, .path = path, .handler = handler, .user = user};
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  return chttp_server_route_register((chttp_server_impl *)server->impl, &options);
}

int chttp_server_route_with(chttp_server *server, const chttp_server_route_options *options) {
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  return chttp_server_route_register((chttp_server_impl *)server->impl, options);
}

int chttp_server_route_with_jwt_bearer(chttp_server *server,
                                       const chttp_server_route_options *options,
                                       chttp_jwt_bearer_validator *validator) {
  chttp_server_impl *impl;
  chttp_server_route_record *route;
  int status;
  if (server == NULL || server->impl == NULL || options == NULL || validator == NULL ||
      validator->impl == NULL)
    return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  status = chttp_server_route_register(impl, options);
  if (status != SALTS_OK) return status;
  route = &impl->routes[impl->route_count - 1u];
  route->jwt_bearer_validator = validator;
  return SALTS_OK;
}

static int chttp_server_websocket_upgrade_required(void *user,
                                                   const chttp_server_request_view *request,
                                                   chttp_server_response *response) {
  int status;
  (void)user;
  (void)request;
  status = chttp_server_response_set_header(response, "Sec-WebSocket-Version", "13");
  if (status != SALTS_OK) return status;
  return chttp_server_reply(response, 426u, "text/plain", "Upgrade Required", 16u);
}

int chttp_server_websocket_route_register(chttp_server_impl *server,
                                          const chttp_server_websocket_options *options) {
  chttp_server_route_options route_options;
  chttp_server_route_record *route;
  size_t max_frame_bytes;
  size_t max_message_bytes;
  size_t max_buffered_input_bytes;
  size_t output_bytes;
  int status;
  if (server == NULL || options == NULL || options->size != sizeof(*options) ||
      options->on_open == NULL || options->on_event == NULL ||
      (options->middleware_count != 0u && options->middleware == NULL))
    return SALTS_EINVAL;
  if (server->config.network.max_send_bytes <= CNET_WEBSOCKET_MAX_HEADER_BYTES)
    return SALTS_EMSGSIZE;
  max_frame_bytes = options->max_frame_bytes;
  if (max_frame_bytes == 0u) {
    max_frame_bytes = server->config.network.max_send_bytes - CNET_WEBSOCKET_MAX_HEADER_BYTES;
    if (max_frame_bytes > CHTTP_SERVER_DEFAULT_WEBSOCKET_FRAME_BYTES)
      max_frame_bytes = CHTTP_SERVER_DEFAULT_WEBSOCKET_FRAME_BYTES;
  }
  max_message_bytes =
      options->max_message_bytes == 0u ? max_frame_bytes : options->max_message_bytes;
  if (max_frame_bytes < CNET_WEBSOCKET_MIN_FRAME_BYTES || max_message_bytes < max_frame_bytes ||
      max_frame_bytes > SIZE_MAX - CNET_WEBSOCKET_MAX_HEADER_BYTES)
    return SALTS_EINVAL;
  output_bytes = max_frame_bytes + CNET_WEBSOCKET_MAX_HEADER_BYTES;
  max_buffered_input_bytes =
      options->max_buffered_input_bytes == 0u ? output_bytes : options->max_buffered_input_bytes;
  if (output_bytes > server->config.network.max_send_bytes ||
      max_buffered_input_bytes < output_bytes)
    return SALTS_EMSGSIZE;
  if (max_buffered_input_bytes > SIZE_MAX - max_message_bytes ||
      max_buffered_input_bytes + max_message_bytes > SIZE_MAX - output_bytes)
    return SALTS_ERANGE;
  route_options = (chttp_server_route_options){.method = CHTTP_METHOD_GET,
                                               .path = options->path,
                                               .middleware = options->middleware,
                                               .middleware_count = options->middleware_count,
                                               .handler = chttp_server_websocket_upgrade_required};
  status = chttp_server_route_register(server, &route_options);
  if (status != SALTS_OK) return status;
  route = &server->routes[server->route_count - 1u];
  route->websocket_open = options->on_open;
  route->websocket_event = options->on_event;
  route->websocket_max_frame_bytes = max_frame_bytes;
  route->websocket_max_message_bytes = max_message_bytes;
  route->websocket_max_buffered_input_bytes = max_buffered_input_bytes;
  route->websocket_user = options->user;
  route->websocket = true;
  return SALTS_OK;
}

int chttp_server_websocket_with(chttp_server *server,
                                const chttp_server_websocket_options *options) {
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  return chttp_server_websocket_route_register((chttp_server_impl *)server->impl, options);
}

int chttp_server_websocket_with_jwt_bearer(
    chttp_server *server, const chttp_server_websocket_options *options,
    chttp_jwt_bearer_validator *validator) {
  chttp_server_impl *impl;
  chttp_server_route_record *route;
  int status;
  if (server == NULL || server->impl == NULL || options == NULL || validator == NULL ||
      validator->impl == NULL)
    return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  status = chttp_server_websocket_route_register(impl, options);
  if (status != SALTS_OK) return status;
  route = &impl->routes[impl->route_count - 1u];
  route->jwt_bearer_validator = validator;
  return SALTS_OK;
}

int chttp_server_websocket(chttp_server *server, const char *path, chttp_websocket_open_fn on_open,
                           chttp_websocket_event_fn on_event, void *user) {
  const chttp_server_websocket_options options = {.size = sizeof(options),
                                                  .path = path,
                                                  .on_open = on_open,
                                                  .on_event = on_event,
                                                  .user = user};
  return chttp_server_websocket_with(server, &options);
}

#define CHTTP_SERVER_ROUTE_METHOD(name, method_value)                                              \
  int chttp_server_##name(chttp_server *server, const char *path, chttp_server_handler_fn handler, \
                          void *user) {                                                            \
    return chttp_server_route(server, method_value, path, handler, user);                          \
  }

CHTTP_SERVER_ROUTE_METHOD(get, CHTTP_METHOD_GET)
CHTTP_SERVER_ROUTE_METHOD(head, CHTTP_METHOD_HEAD)
CHTTP_SERVER_ROUTE_METHOD(post, CHTTP_METHOD_POST)
CHTTP_SERVER_ROUTE_METHOD(put, CHTTP_METHOD_PUT)
CHTTP_SERVER_ROUTE_METHOD(delete, CHTTP_METHOD_DELETE)
CHTTP_SERVER_ROUTE_METHOD(patch, CHTTP_METHOD_PATCH)
CHTTP_SERVER_ROUTE_METHOD(options, CHTTP_METHOD_OPTIONS)

#undef CHTTP_SERVER_ROUTE_METHOD

int chttp_server_use(chttp_server *server, chttp_server_middleware_fn middleware, void *user) {
  chttp_server_impl *impl;
  if (server == NULL || server->impl == NULL || middleware == NULL) return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called) return SALTS_EBUSY;
  if (impl->middleware_count >= impl->config.middleware_capacity) return SALTS_ENOBUFS;
  impl->middleware[impl->middleware_count++] = (chttp_server_middleware){middleware, user};
  return SALTS_OK;
}

int chttp_server_use_jwt_bearer(chttp_server *server,
                                chttp_jwt_bearer_validator *validator) {
  chttp_server_impl *impl;
  if (server == NULL || server->impl == NULL || validator == NULL || validator->impl == NULL)
    return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called) return SALTS_EBUSY;
  if (impl->jwt_bearer_validator != NULL) return SALTS_EALREADY;
  impl->jwt_bearer_validator = validator;
  return SALTS_OK;
}

static int chttp_server_route_param_copy(chttp_server_request_state *state, const char *name,
                                         size_t name_size, const char *value, size_t value_size) {
  char *name_copy;
  char *value_copy;
  size_t needed;
  if (state->param_count >= state->server->config.max_route_param_count ||
      name_size > SIZE_MAX - value_size - 2u)
    return SALTS_ENOBUFS;
  needed = name_size + value_size + 2u;
  if (state->param_storage_used > state->param_storage_capacity ||
      needed > state->param_storage_capacity - state->param_storage_used)
    return SALTS_ENOBUFS;
  name_copy = state->param_storage + state->param_storage_used;
  memcpy(name_copy, name, name_size);
  name_copy[name_size] = '\0';
  value_copy = name_copy + name_size + 1u;
  memcpy(value_copy, value, value_size);
  value_copy[value_size] = '\0';
  state->params[state->param_count++] = (chttp_server_param){name_copy, value_copy};
  state->param_storage_used += needed;
  return SALTS_OK;
}

static int chttp_server_route_match(chttp_server_request_state *state,
                                    const chttp_server_route_record *route, const char *path) {
  const char *pattern = route->path;
  const char *actual = path;
  state->param_count = 0u;
  state->param_storage_used = 0u;
  if (!route->dynamic) return strcmp(pattern, path) == 0 ? SALTS_OK : SALTS_ENOENT;
  while (*pattern != '\0' || *actual != '\0') {
    const char *pattern_segment;
    const char *pattern_end;
    const char *actual_segment;
    const char *actual_end;
    size_t pattern_size;
    size_t actual_size;
    int status;
    if (*pattern != '/' || *actual != '/') return SALTS_ENOENT;
    pattern_segment = ++pattern;
    actual_segment = ++actual;
    pattern_end = strchr(pattern_segment, '/');
    actual_end = strchr(actual_segment, '/');
    if (pattern_end == NULL) pattern_end = pattern_segment + strlen(pattern_segment);
    if (actual_end == NULL) actual_end = actual_segment + strlen(actual_segment);
    pattern_size = (size_t)(pattern_end - pattern_segment);
    actual_size = (size_t)(actual_end - actual_segment);
    if (pattern_size != 0u && pattern_segment[0] == ':') {
      if (actual_size == 0u) return SALTS_ENOENT;
      status = chttp_server_route_param_copy(state, pattern_segment + 1u, pattern_size - 1u,
                                             actual_segment, actual_size);
      if (status != SALTS_OK) return status;
    } else if (pattern_size != actual_size ||
               memcmp(pattern_segment, actual_segment, pattern_size) != 0)
      return SALTS_ENOENT;
    pattern = pattern_end;
    actual = actual_end;
  }
  return SALTS_OK;
}

static bool chttp_server_route_method_matches(chttp_method request_method,
                                              chttp_method route_method, bool fallback) {
  if (!fallback) return request_method == route_method;
  return request_method == CHTTP_METHOD_HEAD && route_method == CHTTP_METHOD_GET;
}

chttp_server_route_record *chttp_server_route_find(chttp_server_request_state *state,
                                                   chttp_method method, const char *path,
                                                   unsigned int *out_allowed_methods,
                                                   int *out_status) {
  chttp_server_impl *server;
  size_t fallback;
  size_t dynamic;
  size_t index;
  if (out_allowed_methods != NULL) *out_allowed_methods = 0u;
  if (out_status != NULL) *out_status = SALTS_OK;
  if (state == NULL || state->server == NULL || path == NULL) {
    if (out_status != NULL) *out_status = SALTS_EINVAL;
    return NULL;
  }
  server = state->server;
  for (fallback = 0u; fallback < 2u; ++fallback) {
    if (fallback != 0u && method != CHTTP_METHOD_HEAD) break;
    for (dynamic = 0u; dynamic < 2u; ++dynamic) {
      for (index = 0u; index < server->route_count; ++index) {
        chttp_server_route_record *route = &server->routes[index];
        int status;
        if ((size_t)route->dynamic != dynamic ||
            !chttp_server_route_method_matches(method, route->method, fallback != 0u))
          continue;
        status = chttp_server_route_match(state, route, path);
        if (status == SALTS_OK) return route;
        if (status != SALTS_ENOENT) {
          if (out_status != NULL) *out_status = status;
          return NULL;
        }
      }
    }
  }
  state->param_count = 0u;
  state->param_storage_used = 0u;
  for (index = 0u; index < server->route_count; ++index) {
    chttp_server_route_record *route = &server->routes[index];
    int status = chttp_server_route_match(state, route, path);
    if (status == SALTS_OK && out_allowed_methods != NULL) {
      *out_allowed_methods |= 1u << (unsigned int)route->method;
      if (route->method == CHTTP_METHOD_GET)
        *out_allowed_methods |= 1u << (unsigned int)CHTTP_METHOD_HEAD;
    } else if (status != SALTS_OK && status != SALTS_ENOENT) {
      if (out_status != NULL) *out_status = status;
      return NULL;
    }
  }
  state->param_count = 0u;
  state->param_storage_used = 0u;
  return NULL;
}

const char *chttp_server_request_param(const chttp_server_request_view *request, const char *name) {
  size_t index;
  if (request == NULL || name == NULL) return NULL;
  for (index = 0u; index < request->param_count; ++index)
    if (strcmp(request->params[index].name, name) == 0) return request->params[index].value;
  return NULL;
}

static int chttp_server_allow_header(chttp_server_response *response, unsigned int methods) {
  static const struct {
    chttp_method method;
    const char *name;
  } names[] = {{CHTTP_METHOD_GET, "GET"},        {CHTTP_METHOD_HEAD, "HEAD"},
               {CHTTP_METHOD_POST, "POST"},      {CHTTP_METHOD_PUT, "PUT"},
               {CHTTP_METHOD_DELETE, "DELETE"},  {CHTTP_METHOD_PATCH, "PATCH"},
               {CHTTP_METHOD_OPTIONS, "OPTIONS"}};
  char value[64];
  size_t used = 0u;
  size_t index;
  for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
    size_t name_size;
    if ((methods & (1u << (unsigned int)names[index].method)) == 0u) continue;
    name_size = strlen(names[index].name);
    if (used != 0u) {
      if (used + 2u >= sizeof(value)) return SALTS_EMSGSIZE;
      value[used++] = ',';
      value[used++] = ' ';
    }
    if (name_size >= sizeof(value) - used) return SALTS_EMSGSIZE;
    memcpy(value + used, names[index].name, name_size);
    used += name_size;
  }
  value[used] = '\0';
  return chttp_server_response_set_header(response, "Allow", value);
}

static int chttp_server_chain_dispatch(chttp_server_chain *chain, size_t index) {
  const size_t global_count = chain->server->middleware_count;
  const size_t route_count = chain->route == NULL ? 0u : chain->route->middleware_count;
  const size_t total = global_count + route_count;
  if (index < total) {
    const chttp_server_middleware *binding = index < global_count
                                                 ? &chain->server->middleware[index]
                                                 : &chain->route->middleware[index - global_count];
    chttp_server_next_impl next_impl = {.chain = chain, .index = index + 1u};
    chttp_server_next next = {&next_impl};
    return binding->handler(binding->user, chain->request, chain->response, &next);
  }
  if (chain->route != NULL) {
    chttp_server_handler_fn terminal =
        chain->terminal != NULL ? chain->terminal : chain->route->handler;
    void *terminal_user = chain->terminal != NULL ? chain->terminal_user : chain->route->user;
    return terminal(terminal_user, chain->request, chain->response);
  }
  if (chain->fallback_status == 405u) {
    const int status = chttp_server_allow_header(chain->response, chain->allowed_methods);
    if (status != SALTS_OK) return status;
  }
  return chttp_server_reply(chain->response, chain->fallback_status, "text/plain",
                            chain->fallback_status == 404u ? "Not Found" : "Method Not Allowed",
                            chain->fallback_status == 404u ? 9u : 18u);
}

int chttp_server_chain_run(chttp_server_chain *chain) {
  if (chain == NULL || chain->server == NULL || chain->request == NULL || chain->response == NULL)
    return SALTS_EINVAL;
  return chttp_server_chain_dispatch(chain, 0u);
}

int chttp_server_next_call(chttp_server_next *next) {
  chttp_server_next_impl *impl;
  if (next == NULL || next->impl == NULL) return SALTS_EINVAL;
  impl = (chttp_server_next_impl *)next->impl;
  if (impl->called) return SALTS_EALREADY;
  impl->called = true;
  return chttp_server_chain_dispatch(impl->chain, impl->index);
}
