#include "chttp_injected_example_application.h"
#include "chttp_injected_example.service_native.h"
#include <http_client/http.h>
#include <fmt.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static chttp_application apps[2];
static chttp_client client;
static chttp_application_options options;
static chttp_application_component_diagnostic diagnostic;
static FactorSettings settings[2];
static salts_component_provider_binding providers[2];
static salts_component_deployment deployments[2];
static int creates, destroys, fail_create, fail_projection;

cmeta_component_configured(AlternateFactor, cmeta_reflected_data(FactorSettings),
    cmeta_provides(FactorSource) cmeta_provides(OffsetSource));
cmeta_component_configured(CyclicFactor, cmeta_reflected_data(FactorSettings),
    cmeta_provides(FactorSource) cmeta_provides(OffsetSource) cmeta_requires(FactorSource));

static void tracked_destroy(void *context, void *object) {
  (void)context;
  ++destroys;
  free(object);
}
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, tracked_destroy};
static cmeta_status SALTS_COMPONENT_CALL tracked_create(void *context, const cmeta_data_desc *data,
    const void *config, const salts_component_dependency *dependencies, size_t count, cmeta_object_ref *out) {
  (void)context; (void)dependencies;
  if (count != 0u || !cmeta_data_desc_equal(data, cmeta_reflected_data(FactorSettings)) || config == NULL)
    return CMETA_INVALID_ARGUMENT;
  FactorSettings *value = malloc(sizeof(*value));
  if (value == NULL) return CMETA_OUT_OF_MEMORY;
  *value = *(const FactorSettings *)config;
  cmeta_status status = cmeta_object_borrow(out, value, data, NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { cmeta_object_release(out); free(value); return status; }
  ++creates;
  return fail_create ? CMETA_CALLBACK_ERROR : CMETA_OK;
}
static cmeta_status tracked_project(void *context, const cmeta_object_ref *object,
    const cmeta_interface_desc *expected, cmeta_interface_projection *out) {
  if (fail_projection) return CMETA_TYPE_MISMATCH;
  return factor_source_provider()->interfaces->project(context, object, expected, out);
}
static const cmeta_object_interface_provider interfaces = {sizeof(interfaces), NULL, tracked_project};

static void post(size_t app_index, const char *target, const char *expected) {
  tstr uri = tstr_format("tcp://127.0.0.1:{}", chttp_application_port(&apps[app_index]));
  check_not_null(uri);
  const chttp_header header = {"Content-Type", "application/json"};
  const char body[] = "{\"value\":6}";
  chttp_options request = {.connection_uri = uri, .authority = "127.0.0.1",
      .target = target, .headers = &header, .header_count = 1u,
      .body = body, .body_size = sizeof(body) - 1u, .timeout_ms = 5000u};
  chttp_response response = {0};
  chttp_error error = {0};
  int status = chttp_post(&client, &request, &response, &error);
  tstr_free(uri);
  check_equal(status, SALTS_OK);
  check_equal(response.status_code, 200u);
  check_equal(chttp_response_header(&response, "Content-Type"), "application/json");
  check_true(vstr_contains(vstr_from_buf((const char *)response.body, response.body_size), vstr_from_cstr(expected)));
  if (strcmp(target, "/scale") == 0)
    check_equal(chttp_response_header(&response, "Cache-Control"), "no-store");
  chttp_response_destroy(&response);
}

spec("Application automatic dependency injection") {
  before_each() {
    creates = destroys = fail_create = fail_projection = 0;
    options = chttp_application_options_default();
    options.component_diagnostic = &diagnostic;
    settings[0].factor = 7u; settings[1].factor = 11u;
    for (size_t i = 0u; i < 2u; ++i) {
      providers[i] = *factor_source_provider();
      providers[i].create = tracked_create;
      providers[i].interfaces = &interfaces;
      deployments[i] = (salts_component_deployment){&providers[i], cmeta_reflected_data(FactorSettings), &settings[i]};
    }
    providers[1].component = cmeta_component_meta(AlternateFactor);
    options.providers = deployments; options.provider_count = 1u;
    chttp_client_config config = {.network = options.server.network,
        .request_capacity = 4u, .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 4096u, .max_request_body_bytes = 4096u,
        .max_response_body_bytes = 4096u, .max_informational_responses = 2u};
    check_equal(chttp_client_init(&client, &config), SALTS_OK);
  }
  after_each() {
    check_equal(chttp_client_destroy(&client, 5000u), SALTS_OK);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(chttp_application_close(&apps[i], 5000u), SALTS_OK);
      check_null(apps[i].impl);
    }
    check_equal(destroys, creates);
  }
  it("injects one provider into all methods and isolates simultaneous applications") {
    check_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_OK);
    check_equal(creates, 1);
    check_equal(diagnostic.status, SALTS_COMPONENT_OK);
    check_equal(chttp_application_start(&apps[0]), SALTS_OK);
    options.providers = &deployments[1];
    check_equal(chttp_injected_example_application_init(&apps[1], &options), SALTS_OK);
    check_equal(chttp_application_start(&apps[1]), SALTS_OK);
    check_equal(creates, 2);
    /* Providers copy configuration: mutations after init cannot affect instances. */
    settings[0].factor = 99u; settings[1].factor = 99u;
    post(0u, "/scale", "\"value\":42");
    post(0u, "/next", "\"value\":13");
    post(1u, "/scale", "\"value\":66");
    check_equal(chttp_application_close(&apps[0], 5000u), SALTS_OK);
    check_equal(destroys, 1);
    post(1u, "/next", "\"value\":17");
  }
  it("reports a missing provider before listening") {
    options.providers = NULL; options.provider_count = 0u;
    check_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_ENOENT);
    check_null(apps[0].impl);
    check_equal(diagnostic.status, SALTS_COMPONENT_MISSING_PROVIDER);
    check_equal(diagnostic.failure.phase, SALTS_COMPONENT_PHASE_RESOLVE);
    check_equal(creates, 0);
  }
  it("rejects ambiguity and accepts an explicit provider selection") {
    options.provider_count = 2u;
    check_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_EINVAL);
    check_null(apps[0].impl);
    check_equal(diagnostic.status, SALTS_COMPONENT_AMBIGUOUS_PROVIDER);
    check_equal(creates, 0);
    const salts_component_selection selections[] = {
        {databind_8_Injected_4_Calc_dependencies_component()->component->stable_id,
         FactorSource_interface(), cmeta_component_meta(AlternateFactor)->stable_id},
        {databind_8_Injected_4_Calc_dependencies_component()->component->stable_id,
         OffsetSource_interface(), cmeta_component_meta(AlternateFactor)->stable_id}};
    options.selections = selections; options.selection_count = 2u;
    check_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_OK);
    check_equal(chttp_application_start(&apps[0]), SALTS_OK);
    post(0u, "/scale", "\"value\":66");
  }
  it("rejects dependency cycles without invoking factories") {
    providers[0].component = cmeta_component_meta(CyclicFactor);
    check_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_EINVAL);
    check_equal(diagnostic.status, SALTS_COMPONENT_DEPENDENCY_CYCLE);
    check_equal(creates, 0);
  }
  it("releases an owned output returned by a failed factory exactly once") {
    fail_create = 1;
    check_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_EINVAL);
    check_null(apps[0].impl);
    check_equal(diagnostic.status, SALTS_COMPONENT_CREATE_FAILED);
    check_equal(diagnostic.failure.provider_status, CMETA_CALLBACK_ERROR);
    check_equal(creates, 1); check_equal(destroys, 1);
  }
  it("unwinds providers when their Interface projection is rejected") {
    fail_projection = 1;
    check_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_EINVAL);
    check_null(apps[0].impl);
    check_not_equal(diagnostic.status, SALTS_COMPONENT_OK);
    check_equal(diagnostic.failure.provider_status, CMETA_TYPE_MISMATCH);
    check_equal(creates, 1); check_equal(destroys, 1);
  }
  it("unwinds an activated graph when a later route cannot be mounted") {
    options.server.route_capacity = 1u;
    check_not_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_OK);
    check_null(apps[0].impl);
    check_equal(creates, 1); check_equal(destroys, 1);
  }
  it("rejects simultaneous manual and automatic graph ownership") {
    salts_component_context manual = SALTS_COMPONENT_CONTEXT_INIT;
    options.components = &manual;
    check_equal(chttp_injected_example_application_init(&apps[0], &options), SALTS_EINVAL);
    check_equal(creates, 0);
    check_equal(manual.state, SALTS_COMPONENT_CONTEXT_ZERO);
  }
  it("rejects an absent or wrongly typed receiver before creating an execution") {
    DataBindNativeExecution execution = DATA_BIND_NATIVE_EXECUTION_INIT;
    check_equal(databind_8_Injected_4_Calc_5_Scale__databind_bind_execution(NULL, &execution), CMETA_TYPE_MISMATCH);
    check_null(execution.context);
    cmeta_object_ref wrong = CMETA_OBJECT_REF_INIT;
    check_equal(cmeta_object_borrow(&wrong, &settings[0], cmeta_reflected_data(FactorSettings), NULL), CMETA_OK);
    check_not_equal(databind_8_Injected_4_Calc_5_Scale__databind_bind_execution(&wrong, &execution), CMETA_OK);
    check_null(execution.context);
    cmeta_object_release(&wrong);
  }
}
