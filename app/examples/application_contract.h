#ifndef CHTTP_EXAMPLE_APPLICATION_CONTRACT_H
#define CHTTP_EXAMPLE_APPLICATION_CONTRACT_H
#include <chttp_app/app.h>
#include <cmeta/data_reflect.h>
#include <string.h>

#define EXAMPLE_ANSWER_METHODS(X,I) X(I,R0,int,value,_)
CMETA_INTERFACE(ExampleAnswer, EXAMPLE_ANSWER_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(ExampleAnswer);

typedef struct AnswerService { int answer; } AnswerService;
typedef struct GreetingController { int initialized; ExampleAnswer answer; } GreetingController;
typedef struct DispatchPolicy { int rejected_status; } DispatchPolicy;
cmeta_reflect_data(AnswerService, "example.AnswerService", cmeta_field(int, answer));
cmeta_reflect_data(GreetingController, "example.GreetingController", cmeta_field(int, initialized));
cmeta_reflect_data(DispatchPolicy, "example.DispatchPolicy", cmeta_field(int, rejected_status));
cmeta_component(AnswerComponent, cmeta_provides(ExampleAnswer));
cmeta_component(GreetingComponent, cmeta_requires(ExampleAnswer));
cmeta_component_empty(PolicyComponent);

static int answer_value(void *self) { return ((const AnswerService *)self)->answer; }
CMETA_IMPLEMENTS(ExampleAnswer, answer_impl, 0u, .value = answer_value);
static cmeta_status answer_project(void *context, const cmeta_object_ref *object,
    const cmeta_interface_desc *expected, cmeta_interface_projection *out) {
  (void)context;
  if (!cmeta_interface_desc_equal(expected, ExampleAnswer_interface())) return CMETA_TRAIT_MISSING;
  const cmeta_interface_projection projection = {
      sizeof(projection), ExampleAnswer_interface(), object->object, &answer_impl_vtable};
  *out = projection;
  return CMETA_OK;
}
static const cmeta_object_interface_provider answer_interfaces = {
    sizeof(answer_interfaces), NULL, answer_project};

/* Explicit demo rejection, not an authentication mechanism. The header makes
 * chain entry observable on successful, rejected and unmatched requests. */
static cmeta_status before_dispatch(void *self, const chttp_invocation *exchange, bool *proceed) {
  const DispatchPolicy *policy = (const DispatchPolicy *)self;
  if (chttp_invocation_set_header(exchange, "X-Example-Interceptor", "ACE") != SALTS_OK)
    return CMETA_CALLBACK_ERROR;
  if (strcmp(chttp_invocation_request(exchange)->path, "/reject") == 0) {
    if (chttp_invocation_reject(exchange, proceed, (unsigned int)policy->rejected_status,
        "text/plain", "Rejected", 8u) != SALTS_OK) return CMETA_CALLBACK_ERROR;
    *proceed = false;
  }
  return CMETA_OK;
}
#endif
