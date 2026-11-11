#include <chttp_app/application.hpp>
#include <cmeta/data_reflect.h>
#include <cstdio>

struct GreetingController { int answer = 42; };
cmeta_reflect_data(GreetingController, "example.cpp.GreetingController", cmeta_field(int, answer));
cmeta_component_empty(GreetingComponent);

CHTTP_ACTION(greeting);
int greeting(void *self, const chttp_exchange *exchange) {
  const auto& controller = *static_cast<GreetingController*>(self);
  char text[64];
  const int size = std::snprintf(text, sizeof(text), "{\"answer\":%d}", controller.answer);
  if (size < 0 || static_cast<size_t>(size) >= sizeof(text)) return SALTS_ERANGE;
  return chttp_server_reply(exchange->response, 200u, "application/json", text, static_cast<size_t>(size));
}

int main() {
  auto factory = [](const cmeta_data_desc*, const void*,
      const salts_configurator_dependency*, size_t) { return std::make_unique<GreetingController>(); };
  chttp::Bean<GreetingController, decltype(factory)> bean(cmeta_component_meta(GreetingComponent),
      cmeta_reflected_data(GreetingController), factory);
  const auto component = bean.deployment();
  const chttp_action actions[] = {CHTTP_CPP_GET("/answer", greeting)};
  const chttp_controller controller = {"GreetingComponent", cmeta_reflected_data(GreetingController),
      actions, 1u, nullptr, 0u, nullptr, 0u, nullptr, 0u};
  auto config = chttp_application_config_default();
  config.components = &component; config.component_count = 1u;
  config.controllers = &controller; config.controller_count = 1u;
  chttp::Application application; // Declared last: Beans/providers outlive HTTP drain.
  int status = application.initialize(config);
  if (status == SALTS_OK) status = application.start();
  uint16_t port = 0u;
  if (status == SALTS_OK) status = application.port(port);
  if (status == SALTS_OK) {
    std::printf("http://127.0.0.1:%u/answer (press Enter to stop)\n", static_cast<unsigned int>(port));
    (void)std::getchar();
  }
  const int closed = application.close(); // Checked error boundary; destructor is the RAII safety net.
  if (status != SALTS_OK || closed != SALTS_OK) {
    std::fprintf(stderr, "Application failed: operation=%d close=%d\n", status, closed);
    return 1;
  }
  return 0;
}
