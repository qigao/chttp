#include <chttp_app/application.hpp>
#include "application_contract.h"
#include <cstdio>
#include <stdexcept>

CHTTP_ACTION(greeting);
int greeting(void *self, const chttp_exchange *exchange) {
  auto& controller = *static_cast<GreetingController*>(self);
  char text[64];
  const int size = std::snprintf(text, sizeof(text), "{\"answer\":%d}", ExampleAnswer_value(&controller.answer));
  if (size < 0 || static_cast<size_t>(size) >= sizeof(text)) return SALTS_ERANGE;
  return chttp_server_reply(exchange->response, 200u, "application/json", text, static_cast<size_t>(size));
}

int main() {
  auto answer_factory = [](const cmeta_data_desc*, const void*,
      const salts_configurator_dependency*, size_t) {
    auto service = std::make_unique<AnswerService>(); service->answer = 42; return service;
  };
  auto controller_factory = [](const cmeta_data_desc*, const void*,
      const salts_configurator_dependency *deps, size_t count) {
    const salts_configurator_dependency *dependency = nullptr;
    if (salts_configurator_dependency_find(deps, count, ExampleAnswer_interface(), &dependency)
        != SALTS_CONFIGURATOR_OK) throw std::runtime_error("Answer dependency was not admitted");
    auto controller = std::make_unique<GreetingController>();
    if (ExampleAnswer_borrow_from_object(dependency->provider_instance,
        dependency->provider_interfaces, &controller->answer) != CMETA_OK)
      throw std::runtime_error("Answer dependency projection failed");
    controller->initialized = 1; return controller;
  };
  auto policy_factory = [](const cmeta_data_desc*, const void*,
      const salts_configurator_dependency*, size_t) {
    auto policy = std::make_unique<DispatchPolicy>(); policy->rejected_status = 403; return policy;
  };
  chttp::Bean<AnswerService, decltype(answer_factory)> answer(cmeta_component_meta(AnswerComponent),
      cmeta_reflected_data(AnswerService), answer_factory, &answer_interfaces);
  chttp::Bean<GreetingController, decltype(controller_factory)> controller_bean(cmeta_component_meta(GreetingComponent),
      cmeta_reflected_data(GreetingController), controller_factory);
  chttp::Bean<DispatchPolicy, decltype(policy_factory)> policy(cmeta_component_meta(PolicyComponent),
      cmeta_reflected_data(DispatchPolicy), policy_factory);
  const salts_configurator_deployment components[] = {answer.deployment(), controller_bean.deployment(), policy.deployment()};
  const chttp_action actions[] = {CHTTP_CPP_GET("/answer", greeting), CHTTP_CPP_GET("/reject", greeting)};
  const chttp_controller controller = {"GreetingComponent", cmeta_reflected_data(GreetingController),
      actions, 2u, nullptr, 0u, nullptr, 0u, nullptr, 0u};
  const chttp_interceptor interceptor = {"PolicyComponent", cmeta_reflected_data(DispatchPolicy),
      before_dispatch, nullptr, nullptr};
  auto config = chttp_application_config_default();
  config.components = components; config.component_count = 3u; config.dependency_capacity = 1u;
  config.controllers = &controller; config.controller_count = 1u;
  config.interceptors = &interceptor; config.interceptor_count = 1u;
  chttp::Application application; // Declared last: Beans/providers outlive HTTP drain.
  int status = application.initialize(config);
  if (status == SALTS_OK) status = application.start();
  uint16_t port = 0u;
  if (status == SALTS_OK) status = application.port(port);
  if (status == SALTS_OK) {
    std::printf("http://127.0.0.1:%u/answer (press Enter to stop)\n", static_cast<unsigned int>(port));
    std::fflush(stdout);
    (void)std::getchar();
    if (std::ferror(stdin)) status = SALTS_EIO;
  }
  const int closed = application.close(); // Checked error boundary; destructor is the RAII safety net.
  if (status != SALTS_OK || closed != SALTS_OK) {
    std::fprintf(stderr, "Application failed: operation=%d close=%d\n", status, closed);
    return 1;
  }
  return 0;
}
