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
struct LargerController { int value; char extra[64]; };
cmeta_reflect_data(LargerController, "test.LargerController", cmeta_field(int, value));
struct PlainController { int value; char extra[12]; };
cmeta_reflect_data(PlainController, "test.PlainController", cmeta_field(int, value));
struct alignas(16) AlignedController {
  int value = 42;
  char extra[12]{};
  ~AlignedController() noexcept { ++destroyed; }
};
cmeta_reflect_data(AlignedController, "test.AlignedController", cmeta_field(int, value));
static_assert(sizeof(PlainController) == sizeof(AlignedController));
static_assert(alignof(PlainController) != alignof(AlignedController));

template<class T> static void rejects_storage(const cmeta_data_desc *data) {
  int calls = 0;
  auto factory = [&calls](const cmeta_data_desc*, const void*,
      const salts_configurator_dependency*, size_t) { ++calls; return std::make_unique<T>(); };
  chttp::Bean<T, decltype(factory)> bean(cmeta_component_meta(CppComponent), data, factory);
  const auto deployment = bean.deployment();
  chttp_controller controller{};
  controller.component_id = "CppComponent"; controller.receiver = data;
  auto config = chttp_application_config_default();
  config.components = &deployment; config.component_count = 1;
  config.controllers = &controller; config.controller_count = 1;
  int initialized;
  bool retained;
  {
    chttp::Application app;
    initialized = app.initialize(config); retained = app.valid();
  }
  check_equal(initialized, SALTS_EINVAL); check_false(retained);
  check_equal(calls, 0); check_equal(destroyed, 0);
}
CHTTP_ACTION(cpp_throw);
int cpp_throw(void*, const chttp_exchange*) {
  struct Scope { ~Scope() noexcept { ++unwound; } } scope;
  throw std::runtime_error("action failure");
}
static_assert(!std::is_copy_constructible_v<chttp::Application>);
static_assert(!std::is_move_constructible_v<chttp::Application>);
struct CustomDelete { void operator()(CppController*) const noexcept; };
struct DerivedController : CppController {};
using ExactFactory = std::unique_ptr<CppController>(*)(const cmeta_data_desc*, const void*,
    const salts_configurator_dependency*, size_t);
using CustomFactory = std::unique_ptr<CppController, CustomDelete>(*)(const cmeta_data_desc*, const void*,
    const salts_configurator_dependency*, size_t);
using DerivedFactory = std::unique_ptr<DerivedController>(*)(const cmeta_data_desc*, const void*,
    const salts_configurator_dependency*, size_t);
static_assert(std::is_constructible_v<chttp::Bean<CppController, ExactFactory>,
    const cmeta_component_desc*, const cmeta_data_desc*, ExactFactory&>);
static_assert(!std::is_constructible_v<chttp::Bean<CppController, CustomFactory>,
    const cmeta_component_desc*, const cmeta_data_desc*, CustomFactory&>,
    "Bean must not erase a custom deletion contract");
static_assert(!std::is_constructible_v<chttp::Bean<CppController, DerivedFactory>,
    const cmeta_component_desc*, const cmeta_data_desc*, DerivedFactory&>,
    "Bean must not erase derived-pointer destruction semantics");

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
  it("rejects a larger reflected storage layout before invoking a typed factory") {
    rejects_storage<CppController>(cmeta_reflected_data(LargerController));
  }
  it("rejects a reflected alignment mismatch even when the storage sizes agree") {
    rejects_storage<AlignedController>(cmeta_reflected_data(PlainController));
  }
  it("rejects missing reflection before creating a native object") {
    rejects_storage<CppController>(nullptr);
  }
  it("retains normal RAII ownership for matching explicitly aligned native storage") {
    int calls = 0;
    auto factory = [&calls](const cmeta_data_desc*, const void*,
        const salts_configurator_dependency*, size_t) {
      ++calls; return std::make_unique<AlignedController>();
    };
    chttp::Bean<AlignedController, decltype(factory)> bean(cmeta_component_meta(CppComponent),
        cmeta_reflected_data(AlignedController), factory);
    const auto deployment = bean.deployment();
    chttp_controller controller{};
    controller.component_id = "CppComponent"; controller.receiver = cmeta_reflected_data(AlignedController);
    auto config = chttp_application_config_default();
    config.components = &deployment; config.component_count = 1;
    config.controllers = &controller; config.controller_count = 1;
    int initialized;
    {
      chttp::Application app;
      initialized = app.initialize(config);
    }
    check_equal(initialized, SALTS_OK); check_equal(calls, 1); check_equal(destroyed, 1);
  }
}
