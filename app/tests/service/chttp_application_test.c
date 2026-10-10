#include "chttp_schema_example_application.h"
#include "chttp_policy_fixture_application.h"
#include <http_client/http.h>
#include <cmeta/struct.h>
#include <cmeta/data_reflect.h>
#include <fmt.h>
#include <tinytest.h>
#include <salts/thread.h>
#include <salts/clock.h>
#include <stdatomic.h>
#include <string.h>

static chttp_application app;
static chttp_client client;
static tstr uri;
static chttp_application_options options;
static atomic_int sequence;
static atomic_int blocked, release_request;
static cmeta_thread_t request_thread;
static int block_request;
static int allowed;

typedef struct test_resource { int active; } test_resource;
cmeta_reflect_data(test_resource, "chttp.test.ApplicationResource", cmeta_field(int, active));
cmeta_component_empty(ApplicationResource);
static test_resource resource;
static int creates, destroys, fail_create;
static salts_component_context components;
static salts_component_provider_binding provider;
static salts_component_deployment deployment;
static salts_component_instance instance;
static size_t activation;

static void resource_destroy(void *context, void *object) {
  (void)context;
  ((test_resource *)object)->active = 0;
  ++destroys;
}
static const cmeta_object_lifecycle lifecycle = {
    sizeof(cmeta_object_lifecycle), NULL, NULL, NULL, resource_destroy};
static cmeta_status resource_create(void *context, const cmeta_data_desc *data, const void *config,
    const salts_component_dependency *dependencies, size_t count, cmeta_object_ref *out) {
  (void)context; (void)data; (void)config; (void)dependencies;
  if (count != 0u) return CMETA_INVALID_ARGUMENT;
  cmeta_status status = cmeta_object_borrow(out, &resource, cmeta_reflected_data(test_resource), NULL);
  if (status != CMETA_OK) return status;
  status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) return status;
  resource.active = 1;
  ++creates;
  return fail_create ? CMETA_CALLBACK_ERROR : CMETA_OK;
}
static void prepare_components(void) {
  provider = (salts_component_provider_binding){sizeof(provider), SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION,
      cmeta_component_meta(ApplicationResource), NULL, NULL, resource_create, NULL, NULL};
  deployment = (salts_component_deployment){&provider, NULL, NULL};
  check_equal(salts_component_context_init(&components, &deployment, 1u, NULL, 0u,
      &instance, 1u, NULL, 0u, &activation, 1u), SALTS_COMPONENT_OK);
  options.components = &components;
}

static cmeta_status gate_before(void *context, const chttp_service_call *call, bool *proceed) {
  (void)context; (void)call;
  atomic_store(&sequence, 1);
  if (block_request) {
    atomic_store(&blocked, 1);
    while (!atomic_load(&release_request)) cmeta_sleep_ms(1u);
  }
  *proceed = allowed != 0;
  return CMETA_OK;
}
static cmeta_status observe_before(void *context, const chttp_service_call *call, bool *proceed) {
  (void)context; (void)call;
  *proceed = true;
  atomic_store(&sequence, atomic_load(&sequence) * 10 + 2);
  return CMETA_OK;
}
static void observe_after(void *context, const chttp_service_call *call,
    const chttp_service_dispatch_result *result) {
  (void)context; (void)call; (void)result;
  atomic_store(&sequence, atomic_load(&sequence) * 10 + 3);
}
static void gate_after(void *context, const chttp_service_call *call,
    const chttp_service_dispatch_result *result) {
  (void)context; (void)call; (void)result;
  atomic_store(&sequence, atomic_load(&sequence) * 10 + 4);
}
static const chttp_application_policy policies[] = {
    {"gate", {NULL, gate_before, gate_after, NULL}},
    {"observe", {NULL, observe_before, observe_after, NULL}}};

static void blocked_post(void *context) {
  (void)context;
  /* Client progress belongs to the thread that initializes the client. */
  chttp_client worker_client = {0};
  chttp_client_config config = {.network = options.server.network,
      .request_capacity = 2u, .max_start_line_bytes = 256u, .max_header_count = 16u,
      .max_header_bytes = 4096u, .max_request_body_bytes = 4096u,
      .max_response_body_bytes = 4096u, .max_informational_responses = 2u};
  if (chttp_client_init(&worker_client, &config) != SALTS_OK) return;
  const chttp_header header = {"Content-Type", "application/json"};
  const char body[] = "{\"value\":1}";
  chttp_options request_options = {.connection_uri = uri, .authority = "127.0.0.1",
      .target = "/echo", .headers = &header, .header_count = 1u,
      .body = body, .body_size = sizeof(body) - 1u, .timeout_ms = 5000u};
  chttp_response response = {0};
  chttp_error error = {0};
  /* Stopping the listener may cancel this accepted response; only quiescence
   * and resource retention are asserted by the controlling test thread. */
  (void)chttp_post(&worker_client, &request_options, &response, &error);
  chttp_response_destroy(&response);
  (void)chttp_client_destroy(&worker_client, 5000u);
}

static void listen_and_connect(void) {
  check_equal(chttp_application_start(&app), SALTS_OK);
  check_not_equal(chttp_application_port(&app), 0u);
  uri = tstr_format("tcp://127.0.0.1:{}", chttp_application_port(&app));
  check_not_null(uri);
  chttp_client_config config = {.network = options.server.network,
      .request_capacity = 4u, .max_start_line_bytes = 256u, .max_header_count = 32u,
      .max_header_bytes = 8192u, .max_request_body_bytes = 8192u,
      .max_response_body_bytes = 8192u, .max_informational_responses = 2u};
  check_equal(chttp_client_init(&client, &config), SALTS_OK);
}

static void request(const char *target, const char *body, unsigned expected_status,
    const char *type, const char *expected, int no_store) {
  chttp_header header = {"Content-Type", "application/json"};
  chttp_options request_options = {.connection_uri = uri, .authority = "127.0.0.1",
      .target = target, .headers = body != NULL ? &header : NULL,
      .header_count = body != NULL ? 1u : 0u, .body = body,
      .body_size = body != NULL ? strlen(body) : 0u, .timeout_ms = 5000u};
  chttp_response response = {0};
  chttp_error error = {0};
  int status = body != NULL ? chttp_post(&client, &request_options, &response, &error)
                           : chttp_get(&client, &request_options, &response, &error);
  check_equal(status, SALTS_OK);
  check_equal(response.status_code, expected_status);
  check_equal(chttp_response_header(&response, "Content-Type"), type);
  if (expected != NULL)
    check_true(vstr_contains(vstr_from_buf((const char *)response.body, response.body_size), vstr_from_cstr(expected)));
  if (no_store) check_equal(chttp_response_header(&response, "Cache-Control"), "no-store");
  else check_null(chttp_response_header(&response, "Cache-Control"));
  chttp_response_destroy(&response);
}

spec("Schema-generated application") {
  before_each() {
    options = chttp_application_options_default();
    allowed = 1;
    atomic_store(&sequence, 0);
    atomic_store(&blocked, 0); atomic_store(&release_request, 0);
    block_request = 0;
    creates = destroys = fail_create = 0;
    components = (salts_component_context)SALTS_COMPONENT_CONTEXT_INIT;
    resource.active = 0;
  }
  after_each() {
    atomic_store(&release_request, 1);
    if (request_thread != NULL) check_equal(cmeta_thread_join(&request_thread), SALTS_OK);
    check_equal(chttp_client_destroy(&client, 5000u), SALTS_OK);
    check_equal(chttp_application_close(&app, 5000u), SALTS_OK);
    check_null(app.impl);
    tstr_free(uri); uri = NULL;
  }
  it("serves multiple services, query/path parameters and JSON-to-XML bodies") {
    check_equal(chttp_schema_example_application_init(&app, &options), SALTS_OK);
    check_equal(chttp_application_port(&app), 0u);
    listen_and_connect();
    request("/add?left=20&right=22", NULL, 200u, "application/json", "\"value\":42", 1);
    request("/multiply", "{\"left\":6,\"right\":7}", 200u, "application/xml", "<value>42</value>", 0);
    request("/health/123", NULL, 200u, "application/json", "\"ready\":true", 0);
    request("/multiply", "{bad", 400u, "text/plain", NULL, 0);
  }
  it("resolves named policies once and runs before/after in opposite orders") {
    options.policies = policies; options.policy_count = 2u;
    check_equal(chttp_policy_fixture_application_init(&app, &options), SALTS_OK);
    listen_and_connect();
    request("/echo", "{\"value\":42}", 200u, "application/json", "\"value\":42", 1);
    check_equal(atomic_load(&sequence), 1234);
  }
  it("denies a method through its schema-selected interceptor") {
    allowed = 0;
    options.policies = policies; options.policy_count = 2u;
    check_equal(chttp_policy_fixture_application_init(&app, &options), SALTS_OK);
    listen_and_connect();
    request("/echo", "{\"value\":42}", 403u, "text/plain", NULL, 1);
  }
  it("rolls back unknown policies before listening and can be initialized again") {
    prepare_components();
    check_equal(chttp_policy_fixture_application_init(&app, &options), SALTS_ENOENT);
    check_null(app.impl);
    check_equal(creates, 1); check_equal(destroys, 1);
    check_equal(components.state, SALTS_COMPONENT_CONTEXT_STOPPED);
    check_equal(chttp_application_start(&app), SALTS_EINVAL);
    options.components = NULL;
    options.policies = policies; options.policy_count = 2u;
    check_equal(chttp_policy_fixture_application_init(&app, &options), SALTS_OK);
    listen_and_connect();
    request("/echo", "{\"value\":7}", 200u, "application/json", "\"value\":7", 1);
  }
  it("owns graph activation through server shutdown and stops each resource once") {
    prepare_components();
    check_equal(chttp_schema_example_application_init(&app, &options), SALTS_OK);
    check_equal(components.state, SALTS_COMPONENT_CONTEXT_ACTIVE);
    listen_and_connect();
    request("/health/1", NULL, 200u, "application/json", "\"ready\":true", 0);
    check_equal(chttp_application_close(&app, 5000u), SALTS_OK);
    check_equal(components.state, SALTS_COMPONENT_CONTEXT_STOPPED);
    check_equal(creates, 1); check_equal(destroys, 1);
    check_equal(resource.active, 0);
  }
  it("preserves component failure diagnostics and does not destroy twice") {
    prepare_components(); fail_create = 1;
    check_equal(chttp_schema_example_application_init(&app, &options), SALTS_EINVAL);
    check_null(app.impl);
    check_equal(components.state, SALTS_COMPONENT_CONTEXT_FAILED);
    check_equal(components.failure.phase, SALTS_COMPONENT_PHASE_CREATE);
    check_equal(creates, 1); check_equal(destroys, 1);
  }
  it("rejects an already active graph without taking its ownership") {
    prepare_components();
    check_equal(salts_component_context_resolve(&components), SALTS_COMPONENT_OK);
    check_equal(salts_component_context_start(&components), SALTS_COMPONENT_OK);
    check_equal(chttp_schema_example_application_init(&app, &options), SALTS_EINVAL);
    check_null(app.impl);
    check_equal(components.state, SALTS_COMPONENT_CONTEXT_ACTIVE);
    check_equal(destroys, 0);
    check_equal(salts_component_context_stop(&components), SALTS_COMPONENT_OK);
    check_equal(destroys, 1);
  }
  it("closes a failed listener and keeps an independent application running") {
    chttp_application other = {0};
    check_equal(chttp_schema_example_application_init(&app, &options), SALTS_OK);
    listen_and_connect();
    options.server.port = chttp_application_port(&app);
    check_equal(chttp_schema_example_application_init(&other, &options), SALTS_OK);
    int started = chttp_application_start(&other);
    int closed = chttp_application_close(&other, 5000u);
    check_not_equal(started, SALTS_OK);
    check_true(closed == SALTS_OK || closed == started);
    check_null(other.impl);
    request("/add?left=1&right=2", NULL, 200u, "application/json", "\"value\":3", 1);
  }
  it("retains the whole graph after stop timeout until the in-flight hook finishes") {
    prepare_components();
    options.policies = policies; options.policy_count = 2u;
    block_request = 1;
    check_equal(chttp_policy_fixture_application_init(&app, &options), SALTS_OK);
    listen_and_connect();
    check_equal(cmeta_thread_create(&request_thread, blocked_post, NULL), SALTS_OK);
    uint64_t started = cmeta_monotonic_ms();
    while (!atomic_load(&blocked) && cmeta_monotonic_ms() - started < 5000u) cmeta_sleep_ms(1u);
    check_equal(atomic_load(&blocked), 1);
    check_equal(chttp_application_close(&app, 1u), SALTS_ETIMEDOUT);
    check_not_null(app.impl);
    check_equal(components.state, SALTS_COMPONENT_CONTEXT_ACTIVE);
    check_equal(destroys, 0);
    atomic_store(&release_request, 1);
    check_equal(cmeta_thread_join(&request_thread), SALTS_OK);
    check_equal(chttp_application_close(&app, 5000u), SALTS_OK);
    check_equal(destroys, 1);
  }
  it("rejects duplicate/reserved policies and insufficient route capacity") {
    chttp_application_policy invalid[] = {policies[0], policies[0]};
    options.policies = invalid; options.policy_count = 2u;
    check_equal(chttp_schema_example_application_init(&app, &options), SALTS_EINVAL);
    invalid[1].name = "no_store";
    check_equal(chttp_schema_example_application_init(&app, &options), SALTS_EINVAL);
    options.policies = NULL; options.policy_count = 0u;
    options.server.route_capacity = 1u;
    check_not_equal(chttp_schema_example_application_init(&app, &options), SALTS_OK);
    check_null(app.impl);
  }
}
