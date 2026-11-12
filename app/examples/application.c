#include <chttp_app/app.h>
#include <cmeta/data_reflect.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct GreetingController { int answer; } GreetingController;
cmeta_reflect_data(GreetingController, "example.GreetingController",
    cmeta_field(int, answer));
cmeta_component_empty(GreetingComponent);

static void greeting_destroy(void *context, void *object) {
  (void)context; free(object);
}
static const cmeta_object_lifecycle greeting_lifecycle = {
    sizeof(cmeta_object_lifecycle), NULL, NULL, NULL, greeting_destroy};

static cmeta_status greeting_create(void *context, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *deps, size_t count,
    cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)deps;
  if (count != 0u) return CMETA_INVALID_ARGUMENT;
  GreetingController *controller = calloc(1u, sizeof(*controller));
  if (controller == NULL) return CMETA_OUT_OF_MEMORY;
  controller->answer = 42;
  cmeta_status status = cmeta_object_borrow(out, controller,
      cmeta_reflected_data(GreetingController), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &greeting_lifecycle);
  if (status != CMETA_OK) { (void)cmeta_object_release(out); free(controller); }
  return status;
}

CHTTP_ACTION(greeting);
int greeting(void *self, const chttp_exchange *exchange) {
  const GreetingController *controller = self;
  char text[64];
  const int size = snprintf(text, sizeof(text), "{\"answer\":%d}", controller->answer);
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
  const salts_configurator_provider_binding provider = {
      sizeof(provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
      cmeta_component_meta(GreetingComponent), NULL, NULL, greeting_create, NULL, NULL};
  const salts_configurator_deployment component = {&provider, NULL, NULL};
  const chttp_action actions[] = {CHTTP_GET("/answer", greeting)};
  const chttp_controller controller = {.component_id = "GreetingComponent",
      .receiver = cmeta_reflected_data(GreetingController), .actions = actions, .action_count = 1u};
  chttp_application_config config = chttp_application_config_default();
  config.components = &component; config.component_count = 1u;
  config.controllers = &controller; config.controller_count = 1u;
  chttp_application app = {0};
  chttp_application_outcome outcome = {0};
  const int status = chttp_application_with(&app, &config, run, NULL, &outcome);
  if (status != SALTS_OK)
    fprintf(stderr, "Application failed: operation=%d close=%d\n", outcome.operation_status, outcome.close_status);
  return status == SALTS_OK ? 0 : 1;
}
