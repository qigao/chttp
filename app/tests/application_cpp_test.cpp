#include <chttp_app/application.hpp>
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include <tinytest.hpp>
#include <cstdio>
#include <stdexcept>

static int destroyed;
static int unwound;
struct CppController {
  int value = 42;
  ~CppController() noexcept { ++destroyed; }
};
cmeta_reflect_data(CppController, "test.CppController", cmeta_field(int, value));
cmeta_component_empty(CppComponent);
CHTTP_ACTION(cpp_throw);
int cpp_throw(void*, const chttp_exchange*) {
  struct Scope { ~Scope() noexcept { ++unwound; } } scope;
  throw std::runtime_error("action failure");
}
static_assert(!std::is_copy_constructible_v<chttp::Application>);
static_assert(!std::is_move_constructible_v<chttp::Application>);

spec("Application C++ RAII") {
  before_each() { destroyed = 0; unwound = 0; }
  it("contains action exceptions and destroys an ACE-owned native object on scope exit") {
    auto factory = [](const cmeta_data_desc*, const void*,
        const salts_configurator_dependency*, size_t) { return std::make_unique<CppController>(); };
    chttp::Bean<CppController, decltype(factory)> bean(cmeta_component_meta(CppComponent),
        cmeta_reflected_data(CppController), factory);
    const auto deployment = bean.deployment();
    const chttp_action actions[] = {CHTTP_CPP_GET("/throw", cpp_throw)};
    chttp_controller controller{};
    controller.component_id = "CppComponent";
    controller.receiver = cmeta_reflected_data(CppController);
    controller.actions = actions; controller.action_count = 1;
    auto config = chttp_application_config_default();
    config.components = &deployment; config.component_count = 1;
    config.controllers = &controller; config.controller_count = 1;
    int initialized, started, called = SALTS_EIO, client_closed = SALTS_EIO;
    unsigned int code = 0;
    {
      chttp::Application app;
      initialized = app.initialize(config);
      started = initialized == SALTS_OK ? app.start() : initialized;
      uint16_t port = 0;
      if (started == SALTS_OK) started = app.port(port);
      chttp_client client{};
      chttp_client_config cc{};
      cc.network = config.http.network;
      cc.request_capacity = 1; cc.max_start_line_bytes = 256; cc.max_header_count = 16;
      cc.max_header_bytes = 4096; cc.max_request_body_bytes = 8192;
      cc.max_response_body_bytes = 8192; cc.max_informational_responses = 4;
      called = chttp_client_init(&client, &cc);
      if (called == SALTS_OK && started == SALTS_OK) {
        char uri[64]; std::snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", unsigned(port));
        chttp_options options{};
        options.connection_uri = uri; options.authority = "test";
        options.target = "/throw"; options.timeout_ms = 5000;
        chttp_response response{}; chttp_error error{};
        called = chttp_get(&client, &options, &response, &error);
        code = response.status_code;
        chttp_response_destroy(&response);
      }
      client_closed = chttp_client_destroy(&client, 5000);
    }
    check_equal(initialized, SALTS_OK); check_equal(started, SALTS_OK);
    check_equal(called, SALTS_OK); check_equal(client_closed, SALTS_OK);
    check_equal(code, 500u); check_equal(unwound, 1); check_equal(destroyed, 1);
  }
  it("contains factory exceptions and rolls back an initialized Bean before returning") {
    int count = 0;
    auto factory = [&count](const cmeta_data_desc*, const void*,
        const salts_configurator_dependency*, size_t) -> std::unique_ptr<CppController> {
      auto temporary = std::make_unique<CppController>();
      ++count;
      throw std::bad_alloc();
    };
    chttp::Bean<CppController, decltype(factory)> bean(cmeta_component_meta(CppComponent),
        cmeta_reflected_data(CppController), factory);
    const auto deployment = bean.deployment();
    chttp_controller controller{};
    controller.component_id = "CppComponent"; controller.receiver = cmeta_reflected_data(CppController);
    auto config = chttp_application_config_default();
    config.components = &deployment; config.component_count = 1;
    config.controllers = &controller; config.controller_count = 1;
    chttp::Application app;
    check_equal(app.initialize(config), SALTS_EINVAL);
    check_false(app.valid()); check_equal(count, 1); check_equal(destroyed, 1);
  }
}
