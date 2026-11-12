#include "application_contract.h"
#include <stdio.h>
#include <stdlib.h>

static void bean_destroy(void *context, void *object) {
  (void)context; free(object);
}
static const cmeta_object_lifecycle lifecycle = {
    sizeof(cmeta_object_lifecycle), NULL, NULL, NULL, bean_destroy};

static cmeta_status own(void *object, const cmeta_data_desc *data, cmeta_object_ref *out) {
  cmeta_status status = cmeta_object_borrow(out, object, data, NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(object); }
  return status;
}
static cmeta_status answer_create(void *context, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *deps, size_t count,
    cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count != 0u) return CMETA_INVALID_ARGUMENT;
  AnswerService *service = calloc(1u, sizeof(*service));
  if (service == NULL) return CMETA_OUT_OF_MEMORY;
  service->answer = 42;
  return own(service, cmeta_reflected_data(AnswerService), out);
}
static cmeta_status policy_create(void *context, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *deps, size_t count,
    cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count != 0u) return CMETA_INVALID_ARGUMENT;
  DispatchPolicy *policy = calloc(1u, sizeof(*policy));
  if (policy == NULL) return CMETA_OUT_OF_MEMORY;
  policy->rejected_status = 403;
  return own(policy, cmeta_reflected_data(DispatchPolicy), out);
}

static cmeta_status greeting_create(void *context, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *deps, size_t count,
    cmeta_object_ref *out) {
  (void)context; (void)data; (void)config;
  const salts_configurator_dependency *dependency = NULL;
  if (salts_configurator_dependency_find(deps, count, ExampleAnswer_interface(), &dependency)
      != SALTS_CONFIGURATOR_OK) return CMETA_CALLBACK_ERROR;
  GreetingController *controller = calloc(1u, sizeof(*controller));
  if (controller == NULL) return CMETA_OUT_OF_MEMORY;
  const cmeta_status status = ExampleAnswer_borrow_from_object(dependency->provider_instance,
      dependency->provider_interfaces, &controller->answer);
  if (status != CMETA_OK) { free(controller); return status; }
  controller->initialized = 1;
  return own(controller, cmeta_reflected_data(GreetingController), out);
}

CHTTP_ACTION(greeting);
int greeting(void *self, const chttp_exchange *exchange) {
  GreetingController *controller = self;
  char text[64];
  const int size = snprintf(text, sizeof(text), "{\"answer\":%d}", ExampleAnswer_value(&controller->answer));
  if (size < 0 || (size_t)size >= sizeof(text)) return SALTS_ERANGE;
  return chttp_server_reply(exchange->response, 200u, "application/json", text, (size_t)size);
}

static int run(chttp_application *app, void *context) {
  uint16_t port = 0u;
  (void)context;
  int status = chttp_application_port(app, &port);
  if (status != SALTS_OK) return status;
  printf("http://127.0.0.1:%u/answer (press Enter to stop)\n", (unsigned int)port);
  fflush(stdout);
  (void)getchar();
  return ferror(stdin) ? SALTS_EIO : SALTS_OK;
}

int main(void) {
  const salts_configurator_provider_binding providers[] = {
      {sizeof(salts_configurator_provider_binding), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
       cmeta_component_meta(AnswerComponent), NULL, &answer_interfaces, answer_create, NULL, NULL},
      {sizeof(salts_configurator_provider_binding), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
       cmeta_component_meta(GreetingComponent), NULL, NULL, greeting_create, NULL, NULL},
      {sizeof(salts_configurator_provider_binding), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
       cmeta_component_meta(PolicyComponent), NULL, NULL, policy_create, NULL, NULL}};
  const salts_configurator_deployment components[] = {
      {&providers[0], NULL, NULL}, {&providers[1], NULL, NULL}, {&providers[2], NULL, NULL}};
  const chttp_action actions[] = {CHTTP_GET("/answer", greeting), CHTTP_GET("/reject", greeting)};
  const chttp_controller controller = {.component_id = "GreetingComponent",
      .receiver = cmeta_reflected_data(GreetingController), .actions = actions, .action_count = 2u};
  const chttp_interceptor interceptor = {"PolicyComponent", cmeta_reflected_data(DispatchPolicy),
      before_dispatch, NULL, NULL};
  chttp_application_config config = chttp_application_config_default();
  config.components = components; config.component_count = 3u; config.dependency_capacity = 1u;
  config.controllers = &controller; config.controller_count = 1u;
  config.interceptors = &interceptor; config.interceptor_count = 1u;
  chttp_application app = {0};
  chttp_application_outcome outcome = {0};
  const int status = chttp_application_with(&app, &config, run, NULL, &outcome);
  if (status != SALTS_OK)
    fprintf(stderr, "Application failed: operation=%d close=%d\n", outcome.operation_status, outcome.close_status);
  return status == SALTS_OK ? 0 : 1;
}
