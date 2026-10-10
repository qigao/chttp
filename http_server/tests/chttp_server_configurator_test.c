#include "chttp_server_runtime.h"
#include "tinytest.h"

#include <stdlib.h>

static size_t allocation_attempts;
static size_t allocation_failure;
static chttp_server fixture_server;

static void *configurator_malloc(size_t size) {
  if (++allocation_attempts == allocation_failure) return NULL;
  return malloc(size);
}

static void *configurator_calloc(size_t count, size_t size) {
  if (++allocation_attempts == allocation_failure) return NULL;
  return calloc(count, size);
}

/* Exercise the production Configurator's allocation failures without adding
 * hooks to the library. CNet, protocol storage and lifecycle stay real.
 * Rename this TU's entry points so DLL symbol interposition is unnecessary. */
#define malloc configurator_malloc
#define calloc configurator_calloc
#define chttp_server_init configurator_init
#define chttp_server_impl_free configurator_free
#define chttp_server_set_deadlines configurator_set_deadlines
#define chttp_server_set_admission configurator_set_admission
#define chttp_server_set_socket_options configurator_set_socket_options
#define chttp_server_set_execution_options configurator_set_execution_options
#define chttp_server_set_owner_placement configurator_set_owner_placement
#define chttp_server_use_jwt_bearer configurator_use_jwt_bearer
#define chttp_server_set_websocket_transport configurator_set_websocket_transport
#include "../src/chttp_server_configurator.c"
#undef malloc
#undef calloc
#undef chttp_server_init
#undef chttp_server_impl_free
#undef chttp_server_set_deadlines
#undef chttp_server_set_admission
#undef chttp_server_set_socket_options
#undef chttp_server_set_execution_options
#undef chttp_server_set_owner_placement
#undef chttp_server_use_jwt_bearer
#undef chttp_server_set_websocket_transport

static chttp_server_config configurator_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1", .backlog = 4u,
      .network = {.backend =
#if defined(_WIN32)
        NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
        NATIVE_IO_BACKEND_EPOLL,
#else
        NATIVE_IO_BACKEND_KQUEUE,
#endif
        .connection_capacity = 4u, .command_capacity = 8u, .request_capacity = 8u,
        .completion_batch_capacity = 4u, .event_capacity = 8u,
        .max_send_bytes = 4096u, .receive_buffer_bytes = 512u,
        .connect_timeout_ms = 1000u, .read_timeout_ms = 1000u, .write_timeout_ms = 1000u},
      .route_capacity = 2u, .max_target_bytes = 128u, .max_header_count = 8u,
      .max_header_bytes = 512u, .max_request_body_bytes = 128u,
      .max_response_header_count = 8u, .max_response_header_bytes = 512u,
      .max_response_body_bytes = 256u, .poll_slice_ms = 2u,
      .middleware_capacity = 2u, .max_route_middleware_count = 2u};
}

static int configurator_interceptor(void *user, const chttp_server_request_view *request,
                                   chttp_server_response *response, chttp_server_next *next) {
  (void)user;
  (void)request;
  (void)response;
  return chttp_server_next_call(next);
}

spec("CHttp Configurator prepare and commit") {
  before_each() {
    allocation_attempts = allocation_failure = 0u;
    fixture_server = (chttp_server){0};
  }

  after_each() {
    allocation_failure = 0u;
    if (fixture_server.impl != NULL) check_equal(chttp_server_stop(&fixture_server, 2000u), SALTS_OK);
    check_equal(chttp_server_destroy(&fixture_server), SALTS_OK);
  }

  it("validates and copies the pre-start WebSocket transport policy") {
    const chttp_server_config config = configurator_config();
    chttp_server_websocket_transport_options options =
        CHTTP_SERVER_WEBSOCKET_TRANSPORT_OPTIONS_INIT;
    check_equal(configurator_init(&fixture_server, &config), SALTS_OK);
    chttp_server_impl *impl = (chttp_server_impl *)fixture_server.impl;
    check_false(impl->websocket_dedicated_h1);
    options.dedicated_h1 = 1;
    check_equal(chttp_server_set_websocket_transport(&fixture_server, &options), SALTS_OK);
    check_true(impl->websocket_dedicated_h1);
    options.version += 1u;
    check_equal(chttp_server_set_websocket_transport(&fixture_server, &options), SALTS_EINVAL);
    check_true(impl->websocket_dedicated_h1);
    options.version = CHTTP_SERVER_WEBSOCKET_TRANSPORT_OPTIONS_VERSION;
    options.dedicated_h1 = 2;
    check_equal(chttp_server_set_websocket_transport(&fixture_server, &options), SALTS_EINVAL);
    options.dedicated_h1 = 0;
    check_equal(chttp_server_start(&fixture_server), SALTS_OK);
    check_equal(chttp_server_set_websocket_transport(&fixture_server, &options), SALTS_EBUSY);
    check_true(impl->websocket_dedicated_h1);
  }

  it("rolls back every local construction allocation without publishing an owner") {
    const chttp_server_config config = configurator_config();
    check_equal(configurator_init(&fixture_server, &config), SALTS_OK);
    const size_t total = allocation_attempts;
    check(total > 0u);
    check_equal(chttp_server_destroy(&fixture_server), SALTS_OK);
    for (size_t failure = 1u; failure <= total; ++failure) {
      allocation_attempts = 0u;
      allocation_failure = failure;
      check_equal(configurator_init(&fixture_server, &config), SALTS_ENOMEM);
      check(fixture_server.impl == NULL);
    }
    allocation_failure = 0u;
    check_equal(configurator_init(&fixture_server, &config), SALTS_OK);
    check_equal(chttp_server_start(&fixture_server), SALTS_OK);
  }

  it("preserves topology policy and interceptor bindings after each preparation failure") {
    const chttp_server_config config = configurator_config();
    chttp_server_execution_options execution = CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    chttp_server_owner_placement_options placement = CHTTP_SERVER_OWNER_PLACEMENT_OPTIONS_INIT;
    check_equal(configurator_init(&fixture_server, &config), SALTS_OK);
    execution.owner_count = 3u;
    allocation_attempts = 0u;
    check_equal(configurator_set_execution_options(&fixture_server, &execution), SALTS_OK);
    const size_t total = allocation_attempts;
    check(total > 0u);
    execution.owner_count = 1u;
    check_equal(configurator_set_execution_options(&fixture_server, &execution), SALTS_OK);
    placement.kind = CNET_OWNER_PLACE_EXPLICIT;
    check_equal(configurator_set_owner_placement(&fixture_server, &placement), SALTS_OK);
    check_equal(chttp_server_use(&fixture_server, configurator_interceptor, &fixture_server), SALTS_OK);
    chttp_server_impl *impl = fixture_server.impl;
    void *original_handoff = impl->owner.handoff.impl;
    cnet_owner_placement_hint *original_hints = impl->acceptor.placement_hints;
    execution.owner_count = 3u;
    for (size_t failure = 1u; failure <= total; ++failure) {
      allocation_attempts = 0u;
      allocation_failure = failure;
      check_equal(configurator_set_execution_options(&fixture_server, &execution), SALTS_ENOMEM);
      check_equal(impl->owner_count, (size_t)1u);
      check_equal(impl->execution_options.owner_count, (size_t)1u);
      check(impl->additional_owners == NULL);
      check(impl->owner.handoff.impl == original_handoff);
      check(impl->acceptor.placement_hints == original_hints);
      check_equal(impl->owner_placement_options.kind, CNET_OWNER_PLACE_EXPLICIT);
      check_equal(impl->middleware_count, (size_t)1u);
      check(impl->middleware[0].handler == configurator_interceptor);
      check(impl->middleware[0].user == &fixture_server);
      for (size_t index = 0u; index < config.network.connection_capacity; ++index)
        check(impl->connections[index].owner == &impl->owner);
    }
    allocation_failure = 0u;
    check_equal(configurator_set_execution_options(&fixture_server, &execution), SALTS_OK);
    check_equal(impl->owner_count, (size_t)3u);
    check_equal(chttp_server_start(&fixture_server), SALTS_OK);
  }
}
