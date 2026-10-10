#include "chttp_server_runtime.h"

#include <string.h>

/* Synchronous request interceptors borrow this stack-scoped continuation.
 * No global cursor is shared between connections or Owner threads. */
typedef struct chttp_server_next_impl {
  chttp_server_chain *chain;
  size_t index;
  bool called;
} chttp_server_next_impl;

int chttp_server_use(chttp_server *server, chttp_server_middleware_fn middleware, void *user) {
  chttp_server_impl *impl;
  if (server == NULL || server->impl == NULL || middleware == NULL) return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called) return SALTS_EBUSY;
  if (impl->middleware_count >= impl->config.middleware_capacity) return SALTS_ENOBUFS;
  impl->middleware[impl->middleware_count++] = (chttp_server_middleware){middleware, user};
  return SALTS_OK;
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

int chttp_server_dispatch_request(chttp_server_request_state *state,
                                  const chttp_server_request_view *request) {
  chttp_server_impl *server;
  chttp_server_request_view routed_request;
  chttp_server_route_record *route;
  chttp_server_chain chain;
  chttp_server_impl *previous_callback_server;
  int status;

  if (state == NULL || state->server == NULL || request == NULL) return SALTS_EINVAL;
  if (!state->admission_complete || state->admission_rejected) return SALTS_EPERM;
  if (chttp_server_deadline_expired(state)) return SALTS_ETIMEDOUT;
  chttp_server_deadline_start(state, state->server->deadlines.handler_ms);

  server = state->server;
  route = state->admitted_route;
  routed_request = *request;
  routed_request.params = state->params;
  routed_request.param_count = state->param_count;
  routed_request.session = server->config.session_capacity == 0u ? NULL : &state->session;
  routed_request.jwt_claims = state->jwt_owner != NULL ? &state->jwt_claims : NULL;
  routed_request.body_sink_user =
      state->body_was_streamed ? state->body_sink_user : NULL;
  chttp_server_response_builder_reset(&state->response_builder);
  chttp_session_request_begin(state, &routed_request);
  chain = (chttp_server_chain){.server = server,
                               .request_state = state,
                               .request = &routed_request,
                               .response = &state->response,
                               .route = route,
                               .fallback_status = state->admitted_fallback_status,
                               .allowed_methods = state->admitted_allowed_methods};
  previous_callback_server = chttp_active_callback_server;
  chttp_active_callback_server = server;
  status = chttp_server_chain_run(&chain);
  chttp_active_callback_server = previous_callback_server;
  if (state->response_builder.deferred) {
    /* A worker may already be reading the sealed headers. Keep both builders
     * intact and let H1 close / H2 RST retire the request on dispatch failure;
     * the deferred token still governs the in-flight completion's lifetime. */
    if (status != SALTS_OK) chttp_server_stats_handler_error(server);
    return status;
  }
  if (chttp_server_deadline_expired(state)) {
    state->deadline_ms = 0;
    chttp_session_request_abort(state);
    chttp_server_response_builder_close_source(&state->response_builder, SALTS_ETIMEDOUT);
    chttp_server_response_builder_reset(&state->response_builder);
    return SALTS_ETIMEDOUT;
  }
  state->deadline_ms = 0;
  if (status == SALTS_OK && !state->response_builder.replied)
    status = chttp_server_reply(&state->response, 204u, NULL, NULL, 0u);
  if (status == SALTS_OK) status = chttp_session_request_finish(state);
  if (status != SALTS_OK) {
    chttp_session_request_abort(state);
    chttp_server_stats_handler_error(server);
    chttp_server_response_builder_reset(&state->response_builder);
    status = chttp_server_reply(&state->response, 500u, "text/plain", "Internal Server Error", 21u);
  }
  return status;
}
