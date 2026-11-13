#include <chttp_app/application.hpp>
#include "chttp_application_registration.h"
#include <chttp_app/invocation.hpp>
#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include <tinytest.hpp>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <threads/threads.h>

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
int cpp_throw(void*, const chttp_invocation*) {
  struct Scope { ~Scope() noexcept { ++unwound; } } scope;
  throw std::runtime_error("action failure");
}
static_assert(!std::is_copy_constructible_v<chttp::Application>);
static_assert(!std::is_move_constructible_v<chttp::Application>);
struct InvocationCppContext {
  chttp_application *app = nullptr;
  chttp_protocol protocol = CHTTP_HTTP_1_1;
  const void *control = nullptr;
  bool fail = false, worker = false, reflected = false, legacy_rejected = false, header_copied = false;
  bool admission_fail = false, admission_worker = false, admission_values = false;
  int admission_unwound = 0;
  const char *asset_root = nullptr;
  bool asset_fail = false, asset_worker = false;
  int asset_unwound = 0;
  bool source = false, source_fail = false, source_worker = false;
  int source_unwound = 0, source_closed = 0;
  bool upload = false, upload_worker = false;
  int upload_failure = 0, upload_open_unwound = 0, upload_write_unwound = 0, upload_closed = 0, upload_cleaned = 0;
  bool websocket_event_fail = false;
  int websocket_unwound = 0, websocket_closed = 0;
  unsigned int code = 0;
};
struct InvocationCppController {
  InvocationCppContext *call_context;
  int value = 42;
  explicit InvocationCppController(InvocationCppContext *value_context) : call_context(value_context) {}
  ~InvocationCppController() noexcept { ++destroyed; }
};
cmeta_reflect_data(InvocationCppController, "test.InvocationCppController", cmeta_field(int, value));
CHTTP_ACTION(cpp_invocation);
CHTTP_ADMISSION_ACTION(cpp_admission);
CHTTP_ASSET_POLICY_ACTION(cpp_asset_policy);
CHTTP_UPLOAD_OPEN_ACTION(cpp_upload_open);
CHTTP_ACTION(cpp_ws_open);
CHTTP_WEBSOCKET_EVENT_ACTION(cpp_ws_event);
int cpp_ws_open(void *self, const chttp_invocation *invocation) {
  auto& context = *static_cast<InvocationCppController*>(self)->call_context;
  struct Scope { ~Scope() noexcept { ++unwound; } } scope;
  context.worker = chttp_application_is_worker(context.app) && sync_thread_current_token() != context.control;
  if (context.fail) throw std::runtime_error("WS opening failure");
  return chttp_invocation_websocket_accept(invocation);
}
int cpp_ws_event(void *self, const chttp_application_websocket_event *scope) {
  auto& context = *static_cast<InvocationCppController*>(self)->call_context;
  context.worker = context.worker && chttp_application_is_worker(context.app) && !scope->request->session;
  if (scope->event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) { ++context.websocket_closed; return SALTS_OK; }
  struct Scope { int& count; ~Scope() noexcept { ++count; } } unwind{context.websocket_unwound};
  if (context.websocket_event_fail) throw std::runtime_error("WS event failure");
  return chttp_server_websocket_send_text(scope->peer, scope->event->data, scope->event->size);
}
static chttp_worker_config cpp_worker_limits(const InvocationCppContext& context) {
  return {1u, 2u, context.upload ? 256u * 1024u : 8192u, context.asset_root ? 512u : 128u,
      context.asset_root ? 4u : 1u, context.asset_root ? 256u : 32u};
}
static int cpp_ws_perform(chttp_application *app, InvocationCppContext& context) {
  context.app = app;
  int status = SALTS_OK;
  chttp_application_websocket_binding binding{};
  binding.controller.component_id = "CppComponent"; binding.controller.receiver = cmeta_reflected_data(InvocationCppController);
  binding.controller.method = CHTTP_METHOD_GET; binding.controller.path = "/cpp";
  binding.controller.abi = FunctionAbi(cpp_ws_open); binding.controller.invoke = chttp::detail::invocation_action<cpp_ws_open>;
  binding.event_abi = FunctionAbi(cpp_ws_event); binding.event = chttp::detail::websocket_event<cpp_ws_event>;
  binding.event_capacity = 2u; binding.max_frame_bytes = binding.max_message_bytes = 4096u; binding.max_buffered_input_bytes = 8192u;
  status = chttp_application_worker_websocket(app, &binding);
  if (status == SALTS_OK) status = chttp_application_start(app);
  uint16_t port = 0u; if (status == SALTS_OK) status = chttp_application_port(app, &port);
  if (status != SALTS_OK) return status;
  chttp_websocket_client_config config{}; config.size = sizeof(config); config.network = chttp_application_config_default().http.network;
  config.max_frame_bytes = config.max_message_bytes = 4096u; config.max_buffered_input_bytes = 8192u;
  config.max_handshake_header_bytes = 4096u; config.event_capacity = 4u;
  chttp_websocket_client client{}; status = chttp_websocket_client_init(&client, &config);
  if (status != SALTS_OK) return status;
  char uri[96]; std::snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/cpp", unsigned(port));
  chttp_websocket_connect_options options{}; options.size = sizeof(options); options.uri = uri;
  options.timeout_ms = 5000u; options.protocol = context.protocol;
  status = chttp_websocket_client_connect(&client, &options, &context.code);
  if (status == SALTS_OK) status = chttp_websocket_client_send_text(&client, "42", 2u, 5000u);
  chttp_websocket_event event{};
  if (status == SALTS_OK) status = chttp_websocket_client_receive(&client, 5000u, &event);
  if (status == SALTS_OK && !context.websocket_event_fail) {
    if (event.kind != CHTTP_WEBSOCKET_EVENT_MESSAGE || event.size != 2u || std::memcmp(event.data, "42", 2u) != 0)
      status = SALTS_EPROTO;
    else status = chttp_websocket_client_close(&client, 1000u, nullptr, 0u, 5000u);
  }
  const int closed = chttp_websocket_client_destroy(&client, 5000u);
  return status == SALTS_OK ? closed : status;
}
static int cpp_upload_write(void *user, const void *data, size_t size) {
  auto& context = *static_cast<InvocationCppContext*>(user);
  struct Scope { int& count; ~Scope() noexcept { ++count; } } scope{context.upload_write_unwound};
  context.upload_worker = context.upload_worker && chttp_application_is_worker(context.app);
  if (context.upload_failure == 2) throw std::runtime_error("upload write failure");
  return size == 2u && std::memcmp(data, "42", 2u) == 0 ? SALTS_OK : SALTS_EPROTO;
}
int cpp_upload_open(void *self, const chttp_server_request_view *request, chttp_body_sink *sink) {
  auto& context = *static_cast<InvocationCppController*>(self)->call_context;
  struct Scope { int& count; ~Scope() noexcept { ++count; } } scope{context.upload_open_unwound};
  context.upload_worker = chttp_application_is_worker(context.app) && sync_thread_current_token() != context.control;
  *sink = {chttp::detail::sink_write<cpp_upload_write>, &context};
  if (context.upload_failure == 1) throw std::runtime_error("upload open failure");
  return request && !request->session && !request->body && !request->body_size ? SALTS_OK : SALTS_EPROTO;
}
static void cpp_upload_close(void *self, chttp_body_sink*, int) noexcept {
  auto& context = *static_cast<InvocationCppController*>(self)->call_context;
  context.upload_worker = context.upload_worker && chttp_application_is_worker(context.app); ++context.upload_closed;
}
static void cpp_upload_cleanup(void *self, chttp_body_sink*, const chttp_application_work_outcome*) noexcept {
  auto& context = *static_cast<InvocationCppController*>(self)->call_context;
  context.upload_worker = context.upload_worker && chttp_application_is_worker(context.app); ++context.upload_cleaned;
}
static int cpp_source_read(void *user, void *buffer, size_t capacity, size_t *out) {
  auto& context = *static_cast<InvocationCppContext*>(user);
  struct Scope { int& count; ~Scope() noexcept { ++count; } } scope{context.source_unwound};
  context.source_worker = chttp_application_is_worker(context.app) && sync_thread_current_token() != context.control;
  if (capacity < 2u) return SALTS_EMSGSIZE;
  *out = context.source_unwound ? 0u : 2u;
  if (*out) std::memcpy(buffer, "42", 2u);
  if (context.source_fail) throw std::runtime_error("source failure");
  return SALTS_OK;
}
static void cpp_source_close(void *user, int) noexcept { ++static_cast<InvocationCppContext*>(user)->source_closed; }
int cpp_asset_policy(void *self, const char *path, chttp_application_asset_policy_result *result) {
  auto& context = *static_cast<InvocationCppController*>(self)->call_context;
  struct Scope { int& count; ~Scope() noexcept { ++count; } } scope{context.asset_unwound};
  context.asset_worker = chttp_application_is_worker(context.app) && sync_thread_current_token() != context.control;
  *result = {true, "\"cpp-v1\""};
  if (context.asset_fail) throw std::runtime_error("asset policy failure");
  return path && std::strcmp(path, "answer.txt") == 0 ? SALTS_OK : SALTS_EPROTO;
}
int cpp_admission(void *self, const chttp_server_request_view *request, chttp_server_admission_result *result) {
  auto& context = *static_cast<InvocationCppController*>(self)->call_context;
  struct Scope { int& count; ~Scope() noexcept { ++count; } } scope{context.admission_unwound};
  context.admission_worker = chttp_application_is_worker(context.app) && sync_thread_current_token() != context.control;
  context.admission_values = request && !request->body && !request->body_size && !request->session &&
      request->path && std::strcmp(request->path, "/cpp") == 0;
  if (context.admission_fail) throw std::runtime_error("Admission failure");
  *result = {}; return context.admission_values ? SALTS_OK : SALTS_EPROTO;
}
int cpp_invocation(void *self, const chttp_invocation *invocation) {
  auto& controller = *static_cast<InvocationCppController*>(self);
  auto& context = *controller.call_context;
  struct Scope { ~Scope() noexcept { ++unwound; } } scope;
  context.worker = chttp_application_is_worker(context.app) && sync_thread_current_token() != context.control;
  context.reflected = cmeta_function_desc_equal(chttp_invocation_operation(invocation), FunctionAbi(cpp_invocation)->function);
  char field[] = "cpp";
  const int header_status = chttp_invocation_set_header(invocation, "X-Cpp", field);
  std::memset(field, '!', sizeof(field) - 1u);
  if (header_status != SALTS_OK) return header_status;
  if (chttp_invocation_response_file(invocation, 204u, nullptr, "unused") != SALTS_EINVAL) return SALTS_EPROTO;
  if (chttp_invocation_serve_file(invocation, nullptr) != SALTS_EINVAL) return SALTS_EPROTO;
  if (context.fail) throw std::runtime_error("Invocation failure");
  if (!chttp_invocation_request(invocation) || chttp_invocation_request(invocation)->session) return SALTS_EPROTO;
  if (context.upload && (!chttp_invocation_request(invocation)->body_streamed ||
      chttp_invocation_request(invocation)->body_sink_user != &context)) return SALTS_EPROTO;
  if (context.source) {
    const chttp_body_source source{chttp::detail::source_read<cpp_source_read>, &context, 0u, 0};
    return chttp_invocation_response_source(invocation, 200u, "text/plain", &source, cpp_source_close, 16u);
  }
  return chttp_invocation_respond(invocation, 200u, "text/plain", "42", 2u);
}
static_assert(std::is_convertible_v<decltype(&cpp_invocation), chttp_invocation_action_fn>);
static int cpp_wrong_native(void *, const chttp_server_request_view *) { return SALTS_OK; }
static_assert(!std::is_convertible_v<decltype(&cpp_wrong_native), chttp_invocation_action_fn>);
static int cpp_invocation_body(chttp_application *app, void *user) {
  auto& context = *static_cast<InvocationCppContext*>(user); context.app = app;
  int status = SALTS_OK;
  const chttp_application_admission_binding admission{"CppComponent", cmeta_reflected_data(InvocationCppController),
      FunctionAbi(cpp_admission), chttp::detail::admission_action<cpp_admission>};
  if (!context.asset_root) {
    status = chttp_application_worker_admission(app, &admission);
    if (status != SALTS_OK) return status;
  } else {
    chttp_web_asset_mount mount = CHTTP_WEB_ASSET_MOUNT_INIT;
    mount.url_prefix = "/static"; mount.filesystem_root = context.asset_root; mount.max_relative_path_bytes = 128u;
    const chttp_application_asset_binding assets{&mount, "CppComponent", cmeta_reflected_data(InvocationCppController),
        FunctionAbi(cpp_asset_policy), chttp::detail::asset_policy_action<cpp_asset_policy>, nullptr, nullptr, 0u};
    status = chttp_application_worker_assets(app, &assets);
    if (status != SALTS_OK) return status;
  }
  chttp_application_worker_binding binding{};
  binding.component_id = "CppComponent"; binding.receiver = cmeta_reflected_data(InvocationCppController);
  binding.method = context.upload ? CHTTP_METHOD_POST : CHTTP_METHOD_GET; binding.path = "/cpp"; binding.abi = FunctionAbi(cpp_admission);
  binding.invoke = chttp::detail::invocation_action<cpp_invocation>;
  context.legacy_rejected = chttp_application_worker_mount(app, &binding) == SALTS_EINVAL;
  if (!context.legacy_rejected) return SALTS_EPROTO;
  binding.abi = FunctionAbi(cpp_invocation);
  if (context.upload) {
    const chttp_application_upload_binding upload{binding, FunctionAbi(cpp_upload_open),
        chttp::detail::upload_open<cpp_upload_open>, cpp_upload_close, cpp_upload_cleanup};
    status = chttp_application_worker_upload(app, &upload);
  } else status = chttp_application_worker_mount(app, &binding);
  if (status != SALTS_OK) return status;
  status = chttp_application_start(app); if (status != SALTS_OK) return status;
  uint16_t port = 0; status = chttp_application_port(app, &port); if (status != SALTS_OK) return status;
  chttp_client client{}; chttp_response response{}; chttp_error error{};
  const chttp_client_config config{chttp_application_config_default().http.network, 1u, 256u, 16u, 4096u, 8192u, 8192u, 4u};
  status = chttp_client_init(&client, &config); if (status != SALTS_OK) return status;
  char uri[64]; std::snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", static_cast<unsigned>(port));
  chttp_options options{}; options.connection_uri = uri; options.authority = "cpp.test";
  options.target = context.asset_root ? "/static/answer.txt" : "/cpp";
  options.protocol = context.protocol; options.timeout_ms = 5000u;
  if (context.upload) { options.body = "42"; options.body_size = 2u; }
  status = context.upload ? chttp_post(&client, &options, &response, &error) : chttp_get(&client, &options, &response, &error);
  context.code = response.status_code;
  const char *field = chttp_response_header(&response, context.asset_root ? "ETag" : "X-Cpp");
  context.header_copied = context.fail || context.admission_fail || context.asset_fail ? field == nullptr :
      field && std::strcmp(field, context.asset_root ? "\"cpp-v1\"" : "cpp") == 0;
  chttp_response_destroy(&response); const int closed = chttp_client_destroy(&client, 5000u);
  return status == SALTS_OK ? closed : status;
}
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
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) for (int failure = 0; failure < 3; ++failure) {
    it("contains WS opening/event exceptions and drains worker RAII before Bean destruction (protocol=%d failure=%d)", protocol, failure) {
      InvocationCppContext context; context.fail = failure == 1; context.websocket_event_fail = failure == 2;
      context.protocol = static_cast<chttp_protocol>(protocol); context.control = sync_thread_current_token();
      auto factory = [&context](const cmeta_data_desc*, const void*, const salts_configurator_dependency*, size_t) {
        return std::make_unique<InvocationCppController>(&context);
      };
      chttp::Bean<InvocationCppController, decltype(factory)> bean(cmeta_component_meta(CppComponent),
          cmeta_reflected_data(InvocationCppController), factory);
      const auto deployment = bean.deployment();
      chttp_controller controller{}; controller.component_id = "CppComponent"; controller.receiver = cmeta_reflected_data(InvocationCppController);
      auto config = chttp_application_config_default(); config.workers = cpp_worker_limits(context); config.components = &deployment; config.component_count = 1u;
      config.controllers = &controller; config.controller_count = 1u;
      config.workers = {1u, 2u, 131072u, 128u, 1u, 32u};
      chttp_application app{}; check_equal(chttp_application_init(&app, &config), SALTS_OK);
      const int called = cpp_ws_perform(&app, context);
      check_equal(chttp_application_close(&app, 5000u), SALTS_OK);
      if (failure == 1) { check_not_equal(called, SALTS_OK); check_equal(context.code, 500u); }
      else if (failure == 2) check_not_equal(called, SALTS_ETIMEDOUT);
      else check_equal(called, SALTS_OK);
      check_true(context.worker); check_equal(unwound, 1); check_equal(destroyed, 1);
      check_equal(context.websocket_unwound, failure == 1 ? 0 : 1); check_equal(context.websocket_closed, failure == 1 ? 0 : 1);
    }
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) for (int failure = 0; failure < 4; ++failure) {
    it("unwinds upload open/body/action on a worker before resource cleanup (protocol=%d failure=%d)", protocol, failure) {
      InvocationCppContext context; context.upload = true; context.upload_failure = failure; context.fail = failure == 3;
      context.protocol = static_cast<chttp_protocol>(protocol); context.control = sync_thread_current_token();
      auto factory = [&context](const cmeta_data_desc*, const void*, const salts_configurator_dependency*, size_t) {
        return std::make_unique<InvocationCppController>(&context);
      };
      chttp::Bean<InvocationCppController, decltype(factory)> bean(cmeta_component_meta(CppComponent),
          cmeta_reflected_data(InvocationCppController), factory);
      const auto deployment = bean.deployment();
      chttp_controller controller{}; controller.component_id = "CppComponent"; controller.receiver = cmeta_reflected_data(InvocationCppController);
      auto config = chttp_application_config_default(); config.workers = cpp_worker_limits(context); config.components = &deployment; config.component_count = 1u;
      config.controllers = &controller; config.controller_count = 1u;
      chttp_application app{}; check_equal(chttp_application_init(&app, &config), SALTS_OK);
      const int called = cpp_invocation_body(&app, &context);
      check_equal(chttp_application_close(&app, 0u), SALTS_OK);
      if (failure == 1 || failure == 2) {
        if (called == SALTS_OK) check_equal(context.code, 500u);
        else check_not_equal(called, SALTS_ETIMEDOUT);
      } else { check_equal(called, SALTS_OK); check_equal(context.code, failure ? 500u : 200u); }
      check_true(context.upload_worker); check_equal(context.upload_open_unwound, 1);
      check_equal(context.upload_write_unwound, failure == 1 ? 0 : 1);
      check_equal(context.upload_closed, failure == 1 ? 0 : 1); check_equal(context.upload_cleaned, failure == 1 ? 0 : 1);
      check_equal(unwound, failure == 1 || failure == 2 ? 0 : 1); check_equal(destroyed, 1);
    }
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) for (int failure = 0; failure < 2; ++failure) {
    it("unwinds a C++ source on a worker before terminal cleanup (protocol=%d failure=%d)", protocol, failure) {
      InvocationCppContext context; context.source = true; context.source_fail = failure != 0;
      context.protocol = static_cast<chttp_protocol>(protocol); context.control = sync_thread_current_token();
      auto factory = [&context](const cmeta_data_desc*, const void*, const salts_configurator_dependency*, size_t) {
        return std::make_unique<InvocationCppController>(&context);
      };
      chttp::Bean<InvocationCppController, decltype(factory)> bean(cmeta_component_meta(CppComponent),
          cmeta_reflected_data(InvocationCppController), factory);
      const auto deployment = bean.deployment();
      chttp_controller controller{}; controller.component_id = "CppComponent"; controller.receiver = cmeta_reflected_data(InvocationCppController);
      auto config = chttp_application_config_default(); config.workers = cpp_worker_limits(context); config.components = &deployment; config.component_count = 1u;
      config.controllers = &controller; config.controller_count = 1u;
      chttp_application app{};
      check_equal(chttp_application_init(&app, &config), SALTS_OK);
      const int called = cpp_invocation_body(&app, &context);
      const int closed = chttp_application_close(&app, 0u);
      check_equal(closed, SALTS_OK);
      if (failure) { check_not_equal(called, SALTS_OK); check_not_equal(called, SALTS_ETIMEDOUT); }
      else check_equal(called, SALTS_OK);
      check_true(context.source_worker); check_equal(context.source_closed, 1); check_equal(context.source_unwound, failure ? 1 : 2);
      check_equal(unwound, 1); check_equal(destroyed, 1);
    }
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) for (int failure = 0; failure < 2; ++failure) {
    it("admits C++ asset policy and contains exceptions before file selection (protocol=%d failure=%d)", protocol, failure) {
      char *root = tt_make_temp_dir("chttp-cpp-worker-assets-"); check_not_null(root);
      char path[1024]; std::snprintf(path, sizeof(path), "%s/answer.txt", root);
      check_equal(tt_write_file(path, "42", 2u), 0);
      InvocationCppContext context; context.asset_root = root; context.asset_fail = failure != 0;
      context.protocol = static_cast<chttp_protocol>(protocol); context.control = sync_thread_current_token();
      auto factory = [&context](const cmeta_data_desc*, const void*, const salts_configurator_dependency*, size_t) {
        return std::make_unique<InvocationCppController>(&context);
      };
      chttp::Bean<InvocationCppController, decltype(factory)> bean(cmeta_component_meta(CppComponent),
          cmeta_reflected_data(InvocationCppController), factory);
      const auto deployment = bean.deployment();
      chttp_controller controller{}; controller.component_id = "CppComponent"; controller.receiver = cmeta_reflected_data(InvocationCppController);
      auto config = chttp_application_config_default(); config.workers = cpp_worker_limits(context); config.components = &deployment; config.component_count = 1u;
      config.controllers = &controller; config.controller_count = 1u;
      int operation = SALTS_EIO, closed = SALTS_EIO;
      {
        chttp_application app{};
        struct Close {
          chttp_application *owner; int& status;
          ~Close() noexcept { status = chttp_application_close(owner, 0u); if (owner->impl) std::terminate(); }
        } close{&app, closed};
        operation = chttp_application_init(&app, &config);
        if (operation == SALTS_OK) operation = cpp_invocation_body(&app, &context);
      }
      check_equal(tt_remove_tree(root), 0); std::free(root);
      check_equal(operation, SALTS_OK); check_equal(closed, SALTS_OK); check_equal(context.code, failure ? 500u : 200u);
      check_true(context.asset_worker); check_equal(context.asset_unwound, 1); check_equal(destroyed, 1);
      check_true(context.header_copied); check_false(context.worker); check_equal(unwound, 0);
    }
  }
  for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) for (int failure = 0; failure <= 3; ++failure) {
    it("admits C++17 header/action callbacks and contains exceptions (protocol=%d failure=%d)", protocol, failure) {
      InvocationCppContext context; context.protocol = static_cast<chttp_protocol>(protocol);
      context.fail = (failure & 1) != 0; context.admission_fail = (failure & 2) != 0; context.control = sync_thread_current_token();
      auto factory = [&context](const cmeta_data_desc*, const void*, const salts_configurator_dependency*, size_t) {
        return std::make_unique<InvocationCppController>(&context);
      };
      chttp::Bean<InvocationCppController, decltype(factory)> bean(cmeta_component_meta(CppComponent),
          cmeta_reflected_data(InvocationCppController), factory);
      const auto deployment = bean.deployment();
      chttp_controller controller{}; controller.component_id = "CppComponent"; controller.receiver = cmeta_reflected_data(InvocationCppController);
      auto config = chttp_application_config_default(); config.workers = cpp_worker_limits(context); config.components = &deployment; config.component_count = 1;
      config.controllers = &controller; config.controller_count = 1;
      int operation = SALTS_EIO, closed = SALTS_EIO;
      {
        /* The public with-helper starts before its body. This private fixture
         * mounts before start and still closes the C owner on C++ scope exit. */
        chttp_application app{};
        struct Close {
          chttp_application *owner; int& status;
          ~Close() noexcept {
            status = chttp_application_close(owner, 0u);
            if (owner->impl) std::terminate();
          }
        } close{&app, closed};
        operation = chttp_application_init(&app, &config);
        if (operation == SALTS_OK) operation = cpp_invocation_body(&app, &context);
      }
      check_equal(operation, SALTS_OK); check_equal(closed, SALTS_OK);
      check_equal(context.worker, !context.admission_fail); check_equal(context.reflected, !context.admission_fail); check_true(context.legacy_rejected);
      check_true(context.admission_worker); check_true(context.admission_values); check_equal(context.admission_unwound, 1);
      check_true(context.header_copied);
      check_equal(context.code, failure ? 500u : 200u); check_equal(unwound, context.admission_fail ? 0 : 1); check_equal(destroyed, 1);
    }
  }
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
