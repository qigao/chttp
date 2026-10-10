#include <chttp_app/app.h>
#include <chttp_rpc_service/service.h>
#include "chttp_document.service_native.h"
#include "chttp_document_native.h"
#include "tinytest.h"
#include <fmt.h>
#include <salts/clock.h>
#include <string.h>

enum { DOCUMENT_TIMEOUT_MS = 5000 };

static const char DOCUMENT_ECHO[] =
    "{\"sum\":18446744073709551615,\"ready\":1,\"text\":\"hello<&\"}";

static cnet_client_config document_network(size_t connections) {
  const cnet_client_config config = {
#if defined(_WIN32)
      .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
      .backend = NATIVE_IO_BACKEND_EPOLL,
#else
      .backend = NATIVE_IO_BACKEND_KQUEUE,
#endif
      .connection_capacity = connections, .command_capacity = 32u,
      .request_capacity = 16u, .completion_batch_capacity = 8u,
      .event_capacity = 32u, .max_send_bytes = 65536u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = DOCUMENT_TIMEOUT_MS,
      .read_timeout_ms = DOCUMENT_TIMEOUT_MS,
      .write_timeout_ms = DOCUMENT_TIMEOUT_MS};
  return config;
}

static crpc_server_config document_server_config(void) {
  const crpc_server_config config = {
      .http = {.host = "127.0.0.1", .backlog = 8u,
          .network = document_network(8u), .route_capacity = 8u,
          .middleware_capacity = 2u, .max_route_middleware_count = 2u,
          .max_route_param_count = 2u, .max_route_param_bytes = 128u,
          .max_target_bytes = 256u, .max_header_count = 16u,
          .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
          .max_response_header_count = 16u, .max_response_header_bytes = 4096u,
          .max_response_body_bytes = 8192u,
          .max_buffered_response_body_bytes = 8192u, .poll_slice_ms = 2u},
      .method_capacity = 4u, .max_method_bytes = 64u,
      .max_json_depth = 8u, .max_batch_items = 4u};
  return config;
}

static void document_contains(const chttp_response *response, const char *text) {
  check_true(vstr_contains(vstr_from_buf((const char *)response->body, response->body_size),
      vstr_from_cstr(text)));
}

typedef struct document_body_fixture {
  DataBind *contract;
  DataBindNativeTypeBinding input, output, state_input, state_output;
  DataBindServiceNativeBinding native, state_native;
  const DataBindMessagePlan *message, *state_message, *wrong_message;
  DataBindHttpMethodPlan *json, *xml, *state, *mixed;
  DataBindHttpMethodPlan *negotiated, *negotiated_xml;
  DataBindFormatPlan *json_format, *xml_format, *wrong_format;
  chttp_service service;
  crpc_server server;
  chttp_client client;
  chttp_async_client async_client;
  cflow_executor executor;
  tstr uri;
} document_body_fixture;

static document_body_fixture db;

typedef struct document_accept_probe { int called, valid, xml; } document_accept_probe;
static document_accept_probe accept_probes[2];

static void document_accept_complete(void *user, chttp_request request,
    const chttp_response_view *response, const chttp_error *error) {
  (void)request;
  document_accept_probe *probe = user;
  ++probe->called;
  probe->valid = (error == NULL || error->status == SALTS_OK) && response != NULL &&
      response->status_code == 200u &&
      vstr_eq(vstr_from_cstr(chttp_response_view_header(response, "Content-Type")),
          vstr_from_cstr(probe->xml ? "application/xml" : "application/json")) &&
      vstr_eq(vstr_from_cstr(chttp_response_view_header(response, "Vary")),
          vstr_from_cstr("Origin, Accept")) &&
      vstr_contains(vstr_from_buf(response->body, response->body_size),
          vstr_from_cstr(probe->xml ? "<text>hello&lt;&amp;</text>" : DOCUMENT_ECHO));
}

static int document_vary_check(void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user;
  (void)request;
  int status = chttp_server_response_append_vary(response, "Origin");
  if (status != SALTS_OK) return status;
  const char *invalid[] = {NULL, "", "Origin,", "Origin, *", "Accept\r\nInjected: true", "@bad"};
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    if (chttp_server_response_append_vary(response, invalid[i]) != SALTS_EINVAL)
      return SALTS_EINVAL;
  status = chttp_server_response_append_vary(response, "Accept, X*Example");
  if (status != SALTS_OK) return status;
  return chttp_server_reply(response, 200u, "text/plain", "checked", 7u);
}

static int document_vary_before(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  (void)user;
  const char *vary = chttp_server_request_header(request, "X-Test-Vary");
  int status = chttp_server_response_set_header(response, "Vary", vary != NULL ? vary : "Origin");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

static void document_accept_post(const char *target, const char *accept,
    const char *second_accept, const char *vary, unsigned status, int xml) {
  chttp_header headers[4] = {{"Content-Type", "application/json"}};
  size_t count = 1u;
  if (accept != NULL) headers[count++] = (chttp_header){"Accept", accept};
  if (second_accept != NULL) headers[count++] = (chttp_header){"accept", second_accept};
  if (vary != NULL) headers[count++] = (chttp_header){"X-Test-Vary", vary};
  chttp_options options = {.connection_uri = db.uri, .authority = "127.0.0.1",
      .target = target, .headers = headers, .header_count = count,
      .body = DOCUMENT_ECHO, .body_size = sizeof(DOCUMENT_ECHO) - 1u,
      .timeout_ms = DOCUMENT_TIMEOUT_MS};
  chttp_response response = {0};
  chttp_error error = {0};
  check_equal(chttp_post(&db.client, &options, &response, &error), SALTS_OK);
  check_equal(response.status_code, status);
  const int negotiated = strcmp(target, "/echo") != 0;
  check_equal(chttp_response_header(&response, "Vary"),
      !negotiated ? "Origin" : vary != NULL ? "*" : "Origin, Accept");
  check_equal(chttp_response_header(&response, "Content-Type"),
      status != 200u ? "text/plain" : xml ? "application/xml" : "application/json");
  if (status == 200u) document_contains(&response,
      xml ? "<text>hello&lt;&amp;</text>" : DOCUMENT_ECHO);
  chttp_response_destroy(&response);
}

static void document_body_post(const char *target, const char *type,
    const char *body, unsigned status, const char *expected) {
  const chttp_header header = {"Content-Type", type};
  chttp_options options = {.connection_uri = db.uri, .authority = "127.0.0.1",
      .target = target, .headers = type != NULL ? &header : NULL,
      .header_count = type != NULL ? 1u : 0u,
      .body = body, .body_size = strlen(body), .timeout_ms = DOCUMENT_TIMEOUT_MS};
  chttp_response response = {0};
  chttp_error error = {0};
  check_equal(chttp_post(&db.client, &options, &response, &error), SALTS_OK);
  check_equal(response.status_code, status);
  if (status != 200u)
    check_equal(chttp_response_header(&response, "Content-Type"), "text/plain");
  if (expected != NULL) document_contains(&response, expected);
  chttp_response_destroy(&response);
}

spec("HTTP document request bodies") {
  before_each() {
    db = (document_body_fixture){0};
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    db.input = (DataBindNativeTypeBinding)DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    db.output = (DataBindNativeTypeBinding)DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    db.state_input = (DataBindNativeTypeBinding)DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    db.state_output = (DataBindNativeTypeBinding)DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    db.native = (DataBindServiceNativeBinding)DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
    db.state_native = (DataBindServiceNativeBinding)DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
    check_equal(data_bind_create(DOCUMENT_SCHEMA_PATH, &db.contract, &error), DATA_BIND_OK);
    check_equal(databind_8_Document_3_Api_4_Echo__databind_native_binding(
        &db.input, &db.output, &db.native, &error), DATA_BIND_OK);
    check_equal(databind_8_Document_3_Api_9_EchoState__databind_native_binding(
        &db.state_input, &db.state_output, &db.state_native, &error), DATA_BIND_OK);
    check_equal(data_bind_message_plan_acquire_generated(db.contract, Reply_native_artifact(),
        &db.message, &error), DATA_BIND_OK);
    check_equal(data_bind_message_plan_acquire_generated(db.contract, StateReply_native_artifact(),
        &db.state_message, &error), DATA_BIND_OK);
    check_equal(data_bind_message_plan_acquire_generated(db.contract, Request_native_artifact(),
        &db.wrong_message, &error), DATA_BIND_OK);
    DataBindHttpProjectionConfig projection = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
    projection.method = "POST";
    projection.route = "/echo";
    check_equal(data_bind_http_method_plan_compile_service(db.contract, "Api", "Echo",
        &projection, &db.native, &db.json, &diagnostic), DATA_BIND_OK);
    projection.route = "/echo-xml";
    projection.ingress_format = DATA_BIND_FORMAT_XML;
    projection.egress_format = DATA_BIND_FORMAT_XML;
    check_equal(data_bind_http_method_plan_compile_service(db.contract, "Api", "Echo",
        &projection, &db.native, &db.xml, &diagnostic), DATA_BIND_OK);
    projection.route = "/negotiate-xml";
    projection.ingress_format = DATA_BIND_FORMAT_JSON;
    check_equal(data_bind_http_method_plan_compile_service(db.contract, "Api", "Echo",
        &projection, &db.native, &db.negotiated_xml, &diagnostic), DATA_BIND_OK);
    projection.route = "/negotiate";
    projection.egress_format = DATA_BIND_FORMAT_JSON;
    check_equal(data_bind_http_method_plan_compile_service(db.contract, "Api", "Echo",
        &projection, &db.native, &db.negotiated, &diagnostic), DATA_BIND_OK);
    check_equal(data_bind_format_plan_compile(db.contract, "Reply", DATA_BIND_FORMAT_JSON,
        &db.json_format, &error), DATA_BIND_OK);
    check_equal(data_bind_format_plan_compile(db.contract, "Reply", DATA_BIND_FORMAT_XML,
        &db.xml_format, &error), DATA_BIND_OK);
    check_equal(data_bind_format_plan_compile(db.contract, "Request", DATA_BIND_FORMAT_XML,
        &db.wrong_format, &error), DATA_BIND_OK);
    projection.route = "/echo-state";
    projection.ingress_format = DATA_BIND_FORMAT_JSON;
    projection.egress_format = DATA_BIND_FORMAT_JSON;
    check_equal(data_bind_http_method_plan_compile_service(db.contract, "Api", "EchoState",
        &projection, &db.state_native, &db.state, &diagnostic), DATA_BIND_OK);
    const DataBindHttpFieldProjection query = {sizeof(DataBindHttpFieldProjection),
        DATA_BIND_BINDING_INGRESS, "ready", DATA_BIND_HTTP_QUERY, "ready", SIZE_MAX};
    projection.route = "/mixed";
    projection.fields = &query;
    projection.field_count = 1u;
    check_equal(data_bind_http_method_plan_compile_service(db.contract, "Api", "Echo",
        &projection, &db.native, &db.mixed, &diagnostic), DATA_BIND_OK);
    chttp_service_config config = CHTTP_SERVICE_CONFIG_INIT;
    config.method_capacity = 5u;
    config.native_max_owned_bytes = 512u;
    config.native_max_depth = 16u;
    config.native_max_items = 128u;
    config.max_response_body_bytes = 2048u;
    check_equal(chttp_service_init(&db.service, &config), SALTS_OK);
    crpc_server_config server_config = document_server_config();
    server_config.http.max_request_body_bytes = 1024u;
    server_config.http.enable_http2 = 1;
    server_config.http.h2_stream_capacity = 4u;
    server_config.http.h2_input_buffer_bytes = 65536u;
    server_config.http.h2_output_buffer_bytes = 65536u;
    server_config.http.h2_hpack_dynamic_table_bytes = 4096u;
    server_config.http.h2_max_settings_count = 16u;
    check_equal(crpc_server_init(&db.server, &server_config), SALTS_OK);
    check_true(cflow_executor_worker_init_with_capacity(&db.executor, 1u, 2u));
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    mount.native_binding = &db.native;
    mount.execution = databind_8_Document_3_Api_4_Echo__databind_execution();
    const chttp_server_middleware middleware = {document_vary_before, NULL};
    mount.middleware = &middleware;
    mount.middleware_count = 1u;
    mount.method_plan = db.json;
    check_equal(chttp_service_mount_http_document_body(&db.service, crpc_server_http(&db.server),
        &mount, NULL, NULL, NULL), SALTS_EINVAL);
    check_equal(chttp_service_mount_http_document_body(&db.service, crpc_server_http(&db.server),
        &mount, db.wrong_message, NULL, NULL), SALTS_EINVAL);
    check_equal(chttp_service_mount_http_document(&db.service, crpc_server_http(&db.server),
        &mount, NULL, NULL), SALTS_ENOTSUP);
    mount.method_plan = db.mixed;
    check_equal(chttp_service_mount_http_document_body(&db.service, crpc_server_http(&db.server),
        &mount, db.message, NULL, NULL), SALTS_ENOTSUP);
    mount.method_plan = db.json;
    check_equal(chttp_service_mount_http_document_body(&db.service, crpc_server_http(&db.server),
        &mount, db.message, NULL, NULL), SALTS_OK);
    mount.method_plan = db.negotiated;
    check_equal(chttp_service_mount_http_negotiated_document(&db.service, crpc_server_http(&db.server),
        &mount, db.message, NULL, NULL, NULL), SALTS_EINVAL);
    check_equal(chttp_service_mount_http_negotiated_document(&db.service, crpc_server_http(&db.server),
        &mount, db.message, db.json_format, NULL, NULL), SALTS_EINVAL);
    check_equal(chttp_service_mount_http_negotiated_document(&db.service, crpc_server_http(&db.server),
        &mount, db.message, db.wrong_format, NULL, NULL), SALTS_EINVAL);
    check_equal(chttp_service_mount_http_negotiated_document(&db.service, crpc_server_http(&db.server),
        &mount, db.message, db.xml_format, NULL, NULL), SALTS_OK);
    mount.method_plan = db.xml;
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
    mount.executor = &db.executor;
    check_equal(chttp_service_mount_http_document_body(&db.service, crpc_server_http(&db.server),
        &mount, db.message, NULL, NULL), SALTS_OK);
    mount.method_plan = db.negotiated_xml;
    check_equal(chttp_service_mount_http_negotiated_document(&db.service, crpc_server_http(&db.server),
        &mount, db.message, db.json_format, NULL, NULL), SALTS_OK);
    mount.method_plan = db.state;
    mount.native_binding = &db.state_native;
    mount.execution = databind_8_Document_3_Api_9_EchoState__databind_execution();
    check_equal(chttp_service_mount_http_document_body(&db.service, crpc_server_http(&db.server),
        &mount, db.state_message, NULL, NULL), SALTS_OK);
    check_equal(chttp_server_get(crpc_server_http(&db.server), "/vary", document_vary_check, NULL), SALTS_OK);
    check_equal(crpc_server_start(&db.server), SALTS_OK);
    uint16_t port = 0u;
    check_equal(crpc_server_port(&db.server, &port), SALTS_OK);
    db.uri = tstr_format("tcp://127.0.0.1:{}", port);
    check_not_null(db.uri);
    chttp_client_config client_config = {
        .network = document_network(4u), .request_capacity = 4u,
        .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
        .max_response_body_bytes = 8192u, .max_informational_responses = 2u};
    check_equal(chttp_client_init(&db.client, &client_config), SALTS_OK);
  }

  after_each() {
    if (db.async_client.impl != NULL) {
      check_equal(chttp_async_client_stop(&db.async_client, DOCUMENT_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_async_client_destroy(&db.async_client), SALTS_OK);
    }
    if (db.client.impl != NULL) check_equal(chttp_client_destroy(&db.client, DOCUMENT_TIMEOUT_MS), SALTS_OK);
    if (db.server.impl != NULL) {
      check_equal(crpc_server_stop(&db.server, DOCUMENT_TIMEOUT_MS), SALTS_OK);
      check_equal(crpc_server_destroy(&db.server), SALTS_OK);
    }
    cflow_executor_destroy(&db.executor);
    if (db.service.impl != NULL) check_equal(chttp_service_destroy(&db.service), SALTS_OK);
    data_bind_format_plan_free(db.wrong_format);
    data_bind_format_plan_free(db.xml_format);
    data_bind_format_plan_free(db.json_format);
    data_bind_http_method_plan_free(db.negotiated_xml);
    data_bind_http_method_plan_free(db.negotiated);
    data_bind_http_method_plan_free(db.mixed);
    data_bind_http_method_plan_free(db.state);
    data_bind_http_method_plan_free(db.xml);
    data_bind_http_method_plan_free(db.json);
    data_bind_free(db.contract);
    tstr_free(db.uri);
  }

  it("decodes JSON and deferred flat XML with canonical names and owned text") {
    document_body_post("/echo", "application/json", DOCUMENT_ECHO, 200u, DOCUMENT_ECHO);
    document_body_post("/echo", "Application/JSON; charset = \"UTF-8\"",
        "{\"legacySum\":7,\"text\":\"a\\u0000b\",\"ready\":1}",
        200u, "\"sum\":7");
    document_body_post("/echo-xml", "application/xml; charset=utf-8",
        "<Reply><legacySum>18446744073709551615</legacySum><ready>1</ready>"
        "<text>hello&lt;&amp;</text></Reply>", 200u,
        "<sum>18446744073709551615</sum><ready>1</ready><text>hello&lt;&amp;</text>");
    document_body_post("/echo-xml", "text/xml", "<Reply><sum>7</sum><ready>1</ready>"
        "<text>second</text></Reply>", 200u, "<text>second</text>");
  }

  it("negotiates response weights and specificity without changing fixed routes") {
    const struct { const char *accept; unsigned status; int xml; } cases[] = {
        {NULL, 200u, 0}, {"*/*", 200u, 0}, {"application/*", 200u, 0},
        {"application/xml", 200u, 1}, {"APPLICATION/XML;Q=1.000", 200u, 1},
        {"application/json;q=0.2, application/xml;q=0.8", 200u, 1},
        {"application/xml;q=0.2, application/json;q=0.8", 200u, 0},
        {"application/xml,application/json", 200u, 0},
        {"application/json;q=0, */*;q=1", 200u, 1},
        {"*/*;q=0, application/json;q=0.001", 200u, 0},
        {"application/*;q=0, */*;q=1", 406u, 0},
        {"application/json;q=0,application/json;q=1,application/xml", 200u, 1},
        {"text/html, application/xml;q=0.9, */*;q=0.8", 200u, 1},
        {"application/json;profile=\"v1,v2\",application/xml", 200u, 1},
        {"application/json;profile=\"escaped\\\"quote\",application/xml", 200u, 1},
        {"application/json;charset=utf-8", 406u, 0},
        {", , application/xml,,", 200u, 1}, {"text/xml", 406u, 0},
        {"", 406u, 0}, {"*/*;q=0", 406u, 0}, {"image/png", 406u, 0}};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i)
      document_accept_post("/negotiate", cases[i].accept, NULL, NULL, cases[i].status, cases[i].xml);
    document_accept_post("/echo", "application/xml", NULL, NULL, 200u, 0);
    document_accept_post("/echo", "invalid", NULL, NULL, 200u, 0);
  }

  it("keeps negotiated format and Vary on deferred responses and repeated headers") {
    document_accept_post("/negotiate-xml", NULL, NULL, NULL, 200u, 1);
    document_accept_post("/negotiate-xml", "application/json", NULL, NULL, 200u, 0);
    document_accept_post("/negotiate-xml", "application/xml", NULL, NULL, 200u, 1);
    document_accept_post("/negotiate-xml", "application/json;q=0.2", "application/xml;q=0.8", NULL, 200u, 1);
    document_accept_post("/negotiate-xml", "*/*", "application/xml;q=0", NULL, 200u, 0);
    document_accept_post("/negotiate", "application/xml", NULL, "*", 200u, 1);
    document_accept_post("/negotiate-xml", "image/png", NULL, NULL, 406u, 0);
  }

  it("rejects malformed Accept without poisoning the next request") {
    const char *bad[] = {"application", "*/json", "application/", "application/json q=1",
        "application/json;q=2", "application/json;q=-1", "application/json;q=.5",
        "application/json;q=0.1234", "application/json;q=1.001", "application/json;q=1e0",
        "application/json;q=\"0.5\"", "application/json;q=1;q=0",
        "application/json;profile=\"unterminated", "application/json;profile", "application/json;q="};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i)
      document_accept_post("/negotiate", bad[i], NULL, NULL, 400u, 0);
    document_accept_post("/negotiate-xml", "application/json", "application/xml;q=2", NULL, 400u, 0);
    document_accept_post("/negotiate", "application/xml", NULL, NULL, 200u, 1);
  }

  it("keeps simultaneous deferred H1 and H2 response selections independent") {
    chttp_client_config config = {
        .network = document_network(4u), .request_capacity = 4u,
        .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
        .max_response_body_bytes = 8192u, .max_informational_responses = 2u};
    check_equal(chttp_async_client_init(&db.async_client, &config), SALTS_OK);
    for (int protocol = CHTTP_HTTP_1_1; protocol <= CHTTP_HTTP_2; ++protocol) {
      accept_probes[0] = (document_accept_probe){.xml = 0};
      accept_probes[1] = (document_accept_probe){.xml = 1};
      for (size_t i = 0u; i < 2u; ++i) {
        const chttp_header headers[] = {{"Content-Type", "application/json"},
            {"Accept", i == 0u ? "application/json" : "application/xml"}};
        chttp_request_options options = {.connection_uri = db.uri, .authority = "127.0.0.1",
            .target = "/negotiate-xml", .method = CHTTP_METHOD_POST,
            .headers = headers, .header_count = 2u,
            .body = DOCUMENT_ECHO, .body_size = sizeof(DOCUMENT_ECHO) - 1u,
            .on_complete = document_accept_complete, .user = &accept_probes[i],
            .protocol = (chttp_protocol)protocol};
        chttp_request request = {0};
        check_equal(chttp_async_client_submit(&db.async_client, &options, &request), SALTS_OK);
      }
      const uint64_t deadline = cmeta_monotonic_ms() + DOCUMENT_TIMEOUT_MS;
      while ((!accept_probes[0].called || !accept_probes[1].called) && cmeta_monotonic_ms() < deadline) {
        size_t completed = 0u;
        check_equal(chttp_async_client_poll(&db.async_client, 5u, &completed), SALTS_OK);
      }
      for (size_t i = 0u; i < 2u; ++i) {
        check_equal(accept_probes[i].called, 1);
        check_true(accept_probes[i].valid);
      }
    }
  }

  it("validates public Vary appends before modifying existing header values") {
    chttp_options options = {.connection_uri = db.uri, .authority = "127.0.0.1",
        .target = "/vary", .timeout_ms = DOCUMENT_TIMEOUT_MS};
    chttp_response response = {0};
    chttp_error error = {0};
    check_equal(chttp_get(&db.client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(chttp_response_header(&response, "Vary"), "Origin, Accept, X*Example");
    chttp_response_destroy(&response);
  }

  it("preserves absent null and value states across deferred execution") {
    document_body_post("/echo-state", "application/json",
        "{\"count\":null,\"text\":\"null\"}", 200u, "{\"count\":null,\"text\":\"null\"}");
    document_body_post("/echo-state", "application/json",
        "{\"note\":0,\"count\":0,\"text\":\"value\"}", 200u,
        "{\"note\":0,\"count\":0,\"text\":\"value\"}");
    document_body_post("/echo-state", "application/json",
        "{\"text\":\"allocated before failure\",\"count\":false}", 400u, "Binding Error");
    document_body_post("/echo-state", "application/json",
        "{\"count\":null,\"text\":\"recovered\"}", 200u, "\"text\":\"recovered\"");
  }

  it("rejects incorrect media types and encodings before business dispatch") {
    const char *types[] = {"text/plain", "application/xml", "application/json; charset=utf-16",
        "application/json; other=1", "application/json; charset=utf-8; charset=utf-8"};
    for (size_t i = 0u; i < sizeof(types) / sizeof(types[0]); ++i)
      document_body_post("/echo", types[i], DOCUMENT_ECHO, 415u, "Unsupported Media Type");
    document_body_post("/echo", NULL, DOCUMENT_ECHO, 415u, "Unsupported Media Type");
    document_body_post("/echo-xml", "application/json", DOCUMENT_ECHO, 415u, "Unsupported Media Type");
    document_body_post("/echo", "application/json", DOCUMENT_ECHO, 200u, DOCUMENT_ECHO);
  }

  it("rejects malformed missing duplicate unknown and invalid fields with recovery") {
    const char *bad_json[] = {"{", "[]", "null", "{}",
        "{\"sum\":1,\"ready\":1,\"text\":\"owned\",\"legacySum\":2}",
        "{\"sum\":1,\"ready\":1,\"text\":\"owned\",\"unknown\":1}",
        "{\"sum\":1,\"text\":\"owned\",\"ready\":null}",
        "{\"sum\":1,\"text\":\"owned\",\"ready\":\"bad\"}"};
    for (size_t i = 0u; i < sizeof(bad_json) / sizeof(bad_json[0]); ++i)
      document_body_post("/echo", "application/json", bad_json[i], 400u, "Binding Error");
    document_body_post("/echo", "application/json",
        "{\"sum\":1,\"text\":\"owned\",\"ready\":0}", 422u, "Validation Error");
    document_body_post("/echo-xml", "application/xml", "<Reply>", 400u, "Binding Error");
    document_body_post("/echo-xml", "application/xml",
        "<Reply><sum>1</sum><text>owned</text><ready>no</ready></Reply>", 400u, "Binding Error");
    document_body_post("/echo-xml", "application/xml",
        "<Reply><sum>1</sum><text>owned</text><ready>0</ready></Reply>", 422u, "Validation Error");
    document_body_post("/echo", "application/json", DOCUMENT_ECHO, 200u, DOCUMENT_ECHO);
  }

  it("enforces native ownership and wire body budgets then accepts the next request") {
    tstr body = tstr_from_v(vstr_from_cstr("{\"sum\":1,\"ready\":1,\"text\":\""));
    check_not_null(body);
    for (size_t i = 0u; i < 70u; ++i) {
      tstr next = tstr_cat_v(body, vstr_from_cstr("abcdefghij"));
      check_not_null(next);
      body = next;
    }
    body = tstr_cat_v(body, vstr_from_cstr("\"}"));
    check_not_null(body);
    document_body_post("/echo", "application/json", body, 413u, "Binding Limit Error");
    for (size_t i = 0u; i < 40u; ++i) {
      tstr next = tstr_cat_v(body, vstr_from_cstr("          "));
      check_not_null(next);
      body = next;
    }
    document_body_post("/echo", "application/json", body, 413u, NULL);
    tstr_free(body);
    document_body_post("/echo", "application/json", DOCUMENT_ECHO, 200u, DOCUMENT_ECHO);
  }
}

static void document_get(chttp_client *client, const char *uri,
    const char *target, unsigned status, const char *type, int xml) {
  chttp_options options = {.connection_uri = uri, .authority = "127.0.0.1",
      .target = target, .timeout_ms = DOCUMENT_TIMEOUT_MS};
  chttp_response response = {0};
  chttp_error error = {0};
  check_equal(chttp_get(client, &options, &response, &error), SALTS_OK);
  check_equal(response.status_code, status);
  check_equal(chttp_response_header(&response, "Content-Type"), type);
  if (status == 200u) {
    if (xml) {
      document_contains(&response, "<Reply>");
      document_contains(&response, "<sum>18446744073709551615</sum>");
      document_contains(&response, "<ready>1</ready>");
      document_contains(&response, "&lt;tag&gt;&amp;");
      document_contains(&response, "</Reply>");
    } else {
      const char *expected = "{\"sum\":18446744073709551615,\"ready\":1,\"text\":\"<tag>&\\\"hello\\\"\"}";
      check_equal(response.body_size, strlen(expected));
      check_equal(response.body, expected, strlen(expected));
    }
  } else {
    /* A failed transaction must not publish a prefix of a JSON/XML document. */
    check_equal(response.body_size, strlen("Internal Server Error"));
    check_equal(response.body, "Internal Server Error", response.body_size);
  }
  chttp_response_destroy(&response);
}

static void document_rpc(chttp_client *client, const char *uri,
    const char *body, const char *expected) {
  const chttp_header content_type = {"Content-Type", "application/json"};
  chttp_options options = {.connection_uri = uri, .authority = "127.0.0.1",
      .target = "/rpc", .headers = &content_type, .header_count = 1u,
      .body = body, .body_size = strlen(body), .timeout_ms = DOCUMENT_TIMEOUT_MS};
  chttp_response response = {0};
  chttp_error error = {0};
  check_equal(chttp_post(client, &options, &response, &error), SALTS_OK);
  if (expected == NULL) {
    check_equal(response.status_code, 204u);
    check_equal(response.body_size, (size_t)0u);
  } else {
    check_equal(response.status_code, 200u);
    check_equal(chttp_response_header(&response, "Content-Type"), "application/json");
    document_contains(&response, "\"jsonrpc\":\"2.0\"");
    document_contains(&response, expected);
  }
  chttp_response_destroy(&response);
}

spec("HTTP and RPC document responses") {
  it("uses generated native values and canonical names with bounded transactional output") {
    DataBind *contract = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindNativeTypeBinding request = DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindNativeTypeBinding response = DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindServiceNativeBinding native = DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
    DataBindHttpMethodPlan *json_plan = NULL, *xml_plan = NULL;
    DataBindHttpMethodPlan *renamed_plan = NULL;
    DataBindRpcMethodPlan *rpc_plan = NULL, *rpc_xml_plan = NULL;
    chttp_service service = {0};
    chttp_rpc_service rpc_service = {0};
    chttp_service_config config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_rpc_service_config rpc_config = CHTTP_RPC_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_rpc_service_mount_options rpc_mount = CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT;
    crpc_server server = {0};
    crpc_server_config server_config = document_server_config();
    chttp_client client = {0};
    chttp_client_config client_config = {
        .network = document_network(4u), .request_capacity = 4u,
        .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
        .max_response_body_bytes = 8192u, .max_informational_responses = 2u};
    cflow_executor executor = {0};
    uint16_t port = 0u;

    check_equal(data_bind_create(DOCUMENT_SCHEMA_PATH, &contract, &error), DATA_BIND_OK);
    check_equal(databind_8_Document_3_Api_3_Get__databind_native_binding(
        &request, &response, &native, &error), DATA_BIND_OK);
    const DataBindNativeExecution *execution =
        databind_8_Document_3_Api_3_Get__databind_execution();
    const DataBindHttpFieldProjection query = {sizeof(DataBindHttpFieldProjection),
        DATA_BIND_BINDING_INGRESS, "large", DATA_BIND_HTTP_QUERY, "large", SIZE_MAX};
    DataBindHttpProjectionConfig http_projection = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
    http_projection.method = "GET";
    http_projection.fields = &query;
    http_projection.field_count = 1u;
    http_projection.route = "/json";
    DataBindStatus compile_status = data_bind_http_method_plan_compile_service(contract, "Api", "Get",
        &http_projection, &native, &json_plan, &diagnostic);
    check_equal(diagnostic.message, "");
    check_equal(compile_status, DATA_BIND_OK);
    const DataBindHttpFieldProjection renamed_fields[] = {query,
        {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_EGRESS,
            "total", DATA_BIND_HTTP_RESPONSE_BODY, "transportRename", SIZE_MAX}};
    DataBindHttpProjectionConfig renamed_projection = http_projection;
    renamed_projection.fields = renamed_fields;
    renamed_projection.field_count = 2u;
    check_equal(data_bind_http_method_plan_compile_service(contract, "Api", "Get",
        &renamed_projection, &native, &renamed_plan, &diagnostic), DATA_BIND_OK);
    http_projection.route = "/xml";
    http_projection.egress_format = DATA_BIND_FORMAT_XML;
    check_equal(data_bind_http_method_plan_compile_service(contract, "Api", "Get",
        &http_projection, &native, &xml_plan, &diagnostic), DATA_BIND_OK);
    const DataBindRpcFieldProjection param = {sizeof(DataBindRpcFieldProjection),
        DATA_BIND_BINDING_INGRESS, "large", "large", 0u};
    DataBindRpcProjectionConfig rpc_projection = DATA_BIND_RPC_PROJECTION_CONFIG_INIT;
    rpc_projection.wire_method = "document.get";
    rpc_projection.fields = &param;
    rpc_projection.field_count = 1u;
    check_equal(data_bind_rpc_method_plan_compile_service(contract, "Api", "Get",
        &rpc_projection, &native, &rpc_plan, &diagnostic), DATA_BIND_OK);
    rpc_projection.egress_format = DATA_BIND_FORMAT_XML;
    check_equal(data_bind_rpc_method_plan_compile_service(contract, "Api", "Get",
        &rpc_projection, &native, &rpc_xml_plan, &diagnostic), DATA_BIND_OK);
    config.method_capacity = 2u;
    config.max_response_body_bytes = 256u;
    rpc_config.method_capacity = 1u;
    rpc_config.max_output_value_bytes = 512u;
    check_equal(chttp_service_init(&service, &config), SALTS_OK);
    check_equal(chttp_rpc_service_init(&rpc_service, &rpc_config), SALTS_OK);
    check_equal(crpc_server_init(&server, &server_config), SALTS_OK);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 2u));
    mount.native_binding = &native;
    mount.execution = execution;
    mount.method_plan = renamed_plan;
    check_equal(chttp_service_mount_http_document(&service, crpc_server_http(&server),
        &mount, NULL, NULL), SALTS_ENOTSUP);
    mount.method_plan = json_plan;
    /* Existing mount remains scalar-only and cannot silently change wire shape. */
    check_equal(chttp_service_mount_http(&service, crpc_server_http(&server), &mount), SALTS_ENOTSUP);
    check_equal(chttp_service_mount_http_document(&service, crpc_server_http(&server),
        &mount, NULL, NULL), SALTS_OK);
    mount.method_plan = xml_plan;
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
    mount.executor = &executor;
    check_equal(chttp_service_mount_http_document(&service, crpc_server_http(&server),
        &mount, NULL, NULL), SALTS_OK);
    rpc_mount.target = "/rpc";
    rpc_mount.native_binding = &native;
    rpc_mount.execution = execution;
    rpc_mount.method_plan = rpc_xml_plan;
    check_equal(chttp_rpc_service_mount_document(&rpc_service, &server, &rpc_mount), SALTS_ENOTSUP);
    rpc_mount.method_plan = rpc_plan;
    check_equal(chttp_rpc_service_mount(&rpc_service, &server, &rpc_mount), SALTS_ENOTSUP);
    check_equal(chttp_rpc_service_mount_document(&rpc_service, &server, &rpc_mount), SALTS_OK);
    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    tstr uri = tstr_format("tcp://127.0.0.1:{}", port);
    check_not_null(uri);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    document_get(&client, uri, "/json?large=0", 200u, "application/json", 0);
    document_get(&client, uri, "/xml?large=0", 200u, "application/xml", 1);
    document_get(&client, uri, "/json?large=1", 500u, "text/plain", 0);
    document_get(&client, uri, "/xml?large=1", 500u, "text/plain", 1);
    document_get(&client, uri, "/json?large=0", 200u, "application/json", 0);
    document_get(&client, uri, "/xml?large=0", 200u, "application/xml", 1);
    const char *rpc_request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"document.get\",\"params\":{\"large\":0}}";
    const char *rpc_result = "\"result\":{\"sum\":18446744073709551615,\"ready\":1,\"text\":\"<tag>&\\\"hello\\\"\"}";
    document_rpc(&client, uri, rpc_request, rpc_result);
    document_rpc(&client, uri,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"document.get\",\"params\":[1]}",
        "\"code\":-32603");
    document_rpc(&client, uri,
        "{\"jsonrpc\":\"2.0\",\"method\":\"document.get\",\"params\":[0]}", NULL);
    document_rpc(&client, uri,
        "[{\"jsonrpc\":\"2.0\",\"method\":\"document.get\",\"params\":[0]},"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"document.get\",\"params\":[0]}]",
        rpc_result);
    document_rpc(&client, uri, rpc_request, rpc_result);

    check_equal(chttp_client_destroy(&client, DOCUMENT_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_stop(&server, DOCUMENT_TIMEOUT_MS), SALTS_OK);
    check_true(cflow_executor_wait_idle(&executor));
    check_equal(crpc_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);
    check_equal(chttp_rpc_service_destroy(&rpc_service), SALTS_OK);
    cflow_executor_destroy(&executor);
    tstr_free(uri);
    data_bind_rpc_method_plan_free(rpc_xml_plan);
    data_bind_rpc_method_plan_free(rpc_plan);
    data_bind_http_method_plan_free(xml_plan);
    data_bind_http_method_plan_free(renamed_plan);
    data_bind_http_method_plan_free(json_plan);
    data_bind_free(contract);
  }
}

/* Scripted peers exercise malformed result documents through the real HTTP /
 * CRPC response parser. They never replace the production decoder. */
typedef struct document_peer {
  const char *path;
  const char *body;
} document_peer;

static const document_peer DOCUMENT_PEERS[] = {
    {"/alias", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"text\":\"a\\u0000b\",\"legacySum\":18446744073709551615,\"ready\":1}}"},
    {"/missing", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"text\":\"owned\",\"sum\":1}}"},
    {"/type", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"text\":\"owned\",\"sum\":1,\"ready\":\"bad\"}}"},
    {"/duplicate", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"text\":\"owned\",\"sum\":1,\"legacySum\":2,\"ready\":1}}"},
    {"/constraint", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"text\":\"owned\",\"sum\":1,\"ready\":0}}"},
    {"/unknown", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"text\":\"owned\",\"sum\":1,\"ready\":1,\"other\":2}}"},
    {"/null", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":null}"},
    {"/scalar", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":7}"},
    {"/state-failure", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"text\":\"owned\",\"note\":9,\"count\":\"bad\"}}"},
    {"/remote-error", "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32001,\"message\":\"unavailable\"}}"}};

static int document_peer_reply(void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  const document_peer *peer = user;
  (void)request;
  return chttp_server_reply(response, 200u, "application/json", peer->body, strlen(peer->body));
}

typedef struct document_client_fixture {
  DataBind *contract;
  DataBindNativeTypeBinding request_binding, response_binding;
  DataBindServiceNativeBinding native;
  DataBindRpcMethodPlan *plan, *xml_plan;
  const DataBindMessagePlan *message, *wrong_message;
  DataBindNativeTypeBinding state_binding;
  DataBindServiceNativeBinding state_native;
  DataBindRpcMethodPlan *state_plan;
  const DataBindMessagePlan *state_message;
  StateReply_t state_response;
  chttp_rpc_service service;
  crpc_server server;
  crpc_client client;
  tstr uri;
  unsigned char workspace[16384];
  DataBindNativeOptions options;
  Request_t request;
  Reply_t response;
  chttp_rpc_service_client_call_options call;
  chttp_rpc_service_client_outcome outcome;
  crpc_error error;
} document_client_fixture;

static document_client_fixture dc;

typedef struct document_nested_fixture {
  DataBind *contract;
  DataBindNativeTypeBinding input, output, rpc_input;
  DataBindServiceNativeBinding native, rpc_native;
  DataBindNativeTypeBinding binary_input, binary_output;
  DataBindServiceNativeBinding binary_native;
  const DataBindMessagePlan *message;
  DataBindHttpMethodPlan *json, *xml, *binary;
  DataBindRpcMethodPlan *rpc;
  DataBindFormatPlan *xml_format;
  chttp_service service;
  chttp_rpc_service rpc_service;
  crpc_server server;
  crpc_client client;
  chttp_client http;
  cflow_executor executor;
  tstr uri;
  Nested_t response;
  unsigned char workspace[16384];
} document_nested_fixture;

static document_nested_fixture dn;

static const char NESTED_JSON[] =
    "{\"detail\":{\"sum\":18446744073709551615,\"ready\":1,\"text\":\"a<&\"},"
    "\"row\":[{\"sum\":2,\"ready\":1,\"text\":\"b\"},"
    "{\"sum\":3,\"ready\":1,\"text\":\"c\"}],\"tag\":[4,5]}";
static const char NESTED_XML[] =
    "<Nested><detail><sum>18446744073709551615</sum><ready>1</ready><text>a&lt;&amp;</text></detail>"
    "<row><sum>2</sum><ready>1</ready><text>b</text></row>"
    "<row><sum>3</sum><ready>1</ready><text>c</text></row><tag>4</tag><tag>5</tag></Nested>";

static void document_nested_post(const char *path, const char *type, const char *accept,
    const char *body, unsigned status, const char *expected) {
  const chttp_header headers[] = {{"Content-Type", type}, {"Accept", accept}};
  chttp_options options = {.connection_uri = dn.uri, .authority = "127.0.0.1",
      .target = path, .headers = headers, .header_count = 2u,
      .body = body, .body_size = strlen(body), .timeout_ms = DOCUMENT_TIMEOUT_MS};
  chttp_response response = {0};
  chttp_error error = {0};
  check_equal(chttp_post(&dn.http, &options, &response, &error), SALTS_OK);
  check_equal(response.status_code, status);
  if (expected != NULL)
    check_true(vstr_eq(vstr_from_buf(response.body, response.body_size), vstr_from_cstr(expected)));
  chttp_response_destroy(&response);
}

spec("Nested HTTP and RPC documents") {
  before_each() {
    dn = (document_nested_fixture){0};
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    check_equal(data_bind_create(DOCUMENT_SCHEMA_PATH, &dn.contract, &error), DATA_BIND_OK);
    check_equal(databind_8_Document_3_Api_10_EchoNested__databind_native_binding(
        &dn.input, &dn.output, &dn.native, &error), DATA_BIND_OK);
    check_equal(databind_8_Document_3_Api_9_GetNested__databind_native_binding(
        &dn.rpc_input, &dn.output, &dn.rpc_native, &error), DATA_BIND_OK);
    check_equal(data_bind_message_plan_acquire_generated(dn.contract, Nested_native_artifact(),
        &dn.message, &error), DATA_BIND_OK);
    DataBindHttpProjectionConfig projection = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
    projection.method = "POST";
    projection.route = "/nested";
    check_equal(data_bind_http_method_plan_compile_service(dn.contract, "Api", "EchoNested",
        &projection, &dn.native, &dn.json, &diagnostic), DATA_BIND_OK);
    projection.route = "/nested-xml";
    projection.ingress_format = DATA_BIND_FORMAT_XML;
    projection.egress_format = DATA_BIND_FORMAT_XML;
    check_equal(data_bind_http_method_plan_compile_service(dn.contract, "Api", "EchoNested",
        &projection, &dn.native, &dn.xml, &diagnostic), DATA_BIND_OK);
    check_equal(data_bind_format_plan_compile(dn.contract, "Nested", DATA_BIND_FORMAT_XML,
        &dn.xml_format, &error), DATA_BIND_OK);
    DataBindRpcProjectionConfig rpc_projection = DATA_BIND_RPC_PROJECTION_CONFIG_INIT;
    rpc_projection.wire_method = "document.nested";
    check_equal(data_bind_rpc_method_plan_compile_service(dn.contract, "Api", "GetNested",
        &rpc_projection, &dn.rpc_native, &dn.rpc, &diagnostic), DATA_BIND_OK);
    chttp_service_config config = CHTTP_SERVICE_CONFIG_INIT;
    config.method_capacity = 3u;
    config.native_max_depth = 6u;
    check_equal(chttp_service_init(&dn.service, &config), SALTS_OK);
    chttp_rpc_service_config rpc_config = CHTTP_RPC_SERVICE_CONFIG_INIT;
    rpc_config.method_capacity = 1u;
    check_equal(chttp_rpc_service_init(&dn.rpc_service, &rpc_config), SALTS_OK);
    crpc_server_config server_config = document_server_config();
    check_equal(crpc_server_init(&dn.server, &server_config), SALTS_OK);
    check_true(cflow_executor_worker_init_with_capacity(&dn.executor, 1u, 2u));
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    mount.native_binding = &dn.native;
    mount.execution = databind_8_Document_3_Api_10_EchoNested__databind_execution();
    mount.method_plan = dn.json;
    check_equal(chttp_service_mount_http_negotiated_document(&dn.service,
        crpc_server_http(&dn.server), &mount, dn.message, dn.xml_format, NULL, NULL), SALTS_OK);
    mount.method_plan = dn.xml;
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
    mount.executor = &dn.executor;
    check_equal(chttp_service_mount_http_document_body(&dn.service, crpc_server_http(&dn.server),
        &mount, dn.message, NULL, NULL), SALTS_OK);
    chttp_rpc_service_mount_options rpc_mount = CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT;
    rpc_mount.target = "/rpc";
    rpc_mount.method_plan = dn.rpc;
    rpc_mount.native_binding = &dn.rpc_native;
    rpc_mount.execution = databind_8_Document_3_Api_9_GetNested__databind_execution();
    check_equal(chttp_rpc_service_mount_document(&dn.rpc_service, &dn.server, &rpc_mount), SALTS_OK);
    check_equal(crpc_server_start(&dn.server), SALTS_OK);
    uint16_t port = 0u;
    check_equal(crpc_server_port(&dn.server, &port), SALTS_OK);
    dn.uri = tstr_format("tcp://127.0.0.1:{}", port);
    check_not_null(dn.uri);
    crpc_client_config client_config = {
        .http = {.network = document_network(4u), .request_capacity = 4u,
            .max_start_line_bytes = 256u, .max_header_count = 16u,
            .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
            .max_response_body_bytes = 8192u, .max_informational_responses = 2u},
        .request_capacity = 2u, .max_method_bytes = 64u, .max_json_depth = 8u};
    check_equal(crpc_client_init(&dn.client, &client_config), SALTS_OK);
    check_equal(chttp_client_init(&dn.http, &client_config.http), SALTS_OK);
  }

  after_each() {
    if (dn.http.impl != NULL) check_equal(chttp_client_destroy(&dn.http, DOCUMENT_TIMEOUT_MS), SALTS_OK);
    if (dn.client.impl != NULL) check_equal(crpc_client_destroy(&dn.client, DOCUMENT_TIMEOUT_MS), SALTS_OK);
    if (dn.server.impl != NULL) {
      check_equal(crpc_server_stop(&dn.server, DOCUMENT_TIMEOUT_MS), SALTS_OK);
      check_equal(crpc_server_destroy(&dn.server), SALTS_OK);
    }
    cflow_executor_destroy(&dn.executor);
    if (dn.service.impl != NULL) check_equal(chttp_service_destroy(&dn.service), SALTS_OK);
    if (dn.rpc_service.impl != NULL) check_equal(chttp_rpc_service_destroy(&dn.rpc_service), SALTS_OK);
    Nested_clear(&dn.response);
    data_bind_format_plan_free(dn.xml_format);
    data_bind_http_method_plan_free(dn.xml);
    data_bind_http_method_plan_free(dn.json);
    data_bind_http_method_plan_free(dn.binary);
    data_bind_rpc_method_plan_free(dn.rpc);
    data_bind_free(dn.contract);
    tstr_free(dn.uri);
  }

  it("canonicalizes names recursively and negotiates nested JSON or XML") {
    const char *aliases =
        "{\"oldDetail\":{\"legacySum\":18446744073709551615,\"ready\":1,\"text\":\"a<&\"},"
        "\"oldRow\":[{\"legacySum\":2,\"ready\":1,\"text\":\"b\"},"
        "{\"sum\":3,\"ready\":1,\"text\":\"c\"}],\"oldTag\":[4,5]}";
    document_nested_post("/nested", "application/json", "application/json", aliases, 200u, NESTED_JSON);
    document_nested_post("/nested", "application/json", "application/xml", aliases, 200u, NESTED_XML);
  }

  it("groups XML aliases as repeated records and scalars before deferred dispatch") {
    const char *aliases =
        "<Nested><oldDetail><legacySum>18446744073709551615</legacySum>"
        "<ready>1</ready><text>a&lt;&amp;</text></oldDetail>"
        "<oldRow><legacySum>2</legacySum><ready>1</ready><text>b</text></oldRow><oldTag>4</oldTag>"
        "<row><sum>3</sum><ready>1</ready><text>c</text></row><tag>5</tag></Nested>";
    document_nested_post("/nested-xml", "application/xml", "application/xml", aliases, 200u, NESTED_XML);
    document_nested_post("/nested-xml", "application/xml", "application/xml", NESTED_XML, 200u, NESTED_XML);
    const char *empty = "<Nested><detail><sum>1</sum><ready>1</ready><text>x</text></detail></Nested>";
    document_nested_post("/nested-xml", "application/xml", "application/xml", empty, 200u, empty);
  }

  it("rejects malformed nested input and recovers on the next request") {
    document_nested_post("/nested", "application/json", "application/json",
        "{\"detail\":{\"sum\":1,\"legacySum\":2,\"ready\":1,\"text\":\"owned\"},\"row\":[],\"tag\":[]}",
        400u, NULL);
    document_nested_post("/nested-xml", "application/xml", "application/xml",
        "<Nested><detail><sum>1</sum><legacySum>2</legacySum><ready>1</ready><text>x</text></detail></Nested>",
        400u, NULL);
    document_nested_post("/nested-xml", "application/xml", "application/xml",
        "<Nested><detail><sum>bad</sum><ready>1</ready><text>x</text></detail></Nested>", 400u, NULL);
    document_nested_post("/nested", "application/json", "application/json",
        "{\"detail\":{\"sum\":1,\"ready\":1,\"text\":\"owned\",\"unknown\":1},\"row\":[],\"tag\":[]}", 400u, NULL);
    document_nested_post("/nested", "application/json", "application/json",
        "{\"detail\":{\"sum\":[[[[[[[1]]]]]]]},\"row\":[],\"tag\":[]}", 400u, NULL);
    document_nested_post("/nested", "application/json", "application/json", NESTED_JSON, 200u, NESTED_JSON);
  }

  it("rejects native binary leaves below XML records before mounting") {
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    check_equal(databind_8_Document_3_Api_9_GetBinary__databind_native_binding(
        &dn.binary_input, &dn.binary_output, &dn.binary_native, &error), DATA_BIND_OK);
    DataBindHttpProjectionConfig projection = DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
    projection.method = "GET";
    projection.route = "/binary";
    projection.egress_format = DATA_BIND_FORMAT_XML;
    const DataBindHttpFieldProjection field = {sizeof(DataBindHttpFieldProjection),
        DATA_BIND_BINDING_INGRESS, "large", DATA_BIND_HTTP_QUERY, "large", SIZE_MAX};
    projection.fields = &field;
    projection.field_count = 1u;
    DataBindStatus status = data_bind_http_method_plan_compile_service(dn.contract, "Api", "GetBinary",
        &projection, &dn.binary_native, &dn.binary, &diagnostic);
    if (status == DATA_BIND_OK) {
      chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
      mount.method_plan = dn.binary;
      mount.native_binding = &dn.binary_native;
      mount.execution = databind_8_Document_3_Api_9_GetBinary__databind_execution();
      check_equal(chttp_service_mount_http_document(&dn.service, crpc_server_http(&dn.server),
          &mount, NULL, NULL), SALTS_ENOTSUP);
    } else {
      check_equal(status, DATA_BIND_ERR_SCHEMA);
    }
  }

  it("round-trips nested RPC results into owned native output") {
    DataBindNativeOptions options = DATA_BIND_NATIVE_OPTIONS_INIT;
    options.workspace = dn.workspace;
    options.workspace_bytes = sizeof(dn.workspace);
    options.max_depth = 16u;
    options.max_items = 128u;
    options.max_owned_bytes = 4096u;
    Request_t request = {0};
    chttp_rpc_service_client_call_options call = CHTTP_RPC_SERVICE_CLIENT_CALL_OPTIONS_INIT;
    call.connection_uri = dn.uri;
    call.authority = "127.0.0.1";
    call.target = "/rpc";
    call.request_id = 7u;
    call.deadline_ms = DOCUMENT_TIMEOUT_MS;
    call.method_plan = dn.rpc;
    call.native_binding = &dn.rpc_native;
    call.native_options = &options;
    call.request = &request;
    call.request_bytes = sizeof(request);
    call.response = &dn.response;
    call.response_bytes = sizeof(dn.response);
    chttp_rpc_service_client_outcome outcome = CHTTP_RPC_SERVICE_CLIENT_OUTCOME_INIT;
    crpc_error error = {0};
    check_equal(chttp_rpc_service_client_call_document(&dn.client, &call, dn.message, &outcome, &error), SALTS_OK);
    check_equal(outcome.kind, CHTTP_RPC_SERVICE_CLIENT_SUCCESS);
    check_equal(dn.response.child.total, UINT64_MAX);
    check_equal(dn.response.child.text, "<tag>&\"hello\"");
    check_equal(crpc_client_destroy(&dn.client, DOCUMENT_TIMEOUT_MS), SALTS_OK);
    check_equal(dn.response.child.text, "<tag>&\"hello\"");
  }
}

static int document_typed_call(void) {
  return chttp_rpc_service_client_call_document(
      &dc.client, &dc.call, dc.message, &dc.outcome, &dc.error);
}

static void document_client_clear(void) {
  DataBindNativeDiagnostic diagnostic = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  check_equal(data_bind_native_clear(&dc.options, dc.response_binding.data,
      &dc.response, sizeof(dc.response), &diagnostic), DATA_BIND_OK);
}

spec("RPC document typed client") {
  before_each() {
    dc = (document_client_fixture){0};
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    check_equal(data_bind_create(DOCUMENT_SCHEMA_PATH, &dc.contract, &error), DATA_BIND_OK);
    check_equal(databind_8_Document_3_Api_3_Get__databind_native_binding(
        &dc.request_binding, &dc.response_binding, &dc.native, &error), DATA_BIND_OK);
    check_equal(data_bind_message_plan_acquire_generated(dc.contract, Reply_native_artifact(),
        &dc.message, &error), DATA_BIND_OK);
    check_equal(data_bind_message_plan_acquire_generated(dc.contract, Request_native_artifact(),
        &dc.wrong_message, &error), DATA_BIND_OK);
    DataBindRpcProjectionConfig projection = DATA_BIND_RPC_PROJECTION_CONFIG_INIT;
    projection.wire_method = "document.get";
    check_equal(data_bind_rpc_method_plan_compile_service(dc.contract, "Api", "Get",
        &projection, &dc.native, &dc.plan, &diagnostic), DATA_BIND_OK);
    projection.egress_format = DATA_BIND_FORMAT_XML;
    check_equal(data_bind_rpc_method_plan_compile_service(dc.contract, "Api", "Get",
        &projection, &dc.native, &dc.xml_plan, &diagnostic), DATA_BIND_OK);
    check_equal(databind_8_Document_3_Api_5_State__databind_native_binding(
        &dc.request_binding, &dc.state_binding, &dc.state_native, &error), DATA_BIND_OK);
    check_equal(data_bind_message_plan_acquire_generated(dc.contract, StateReply_native_artifact(),
        &dc.state_message, &error), DATA_BIND_OK);
    projection.egress_format = DATA_BIND_FORMAT_JSON;
    projection.wire_method = "document.state";
    check_equal(data_bind_rpc_method_plan_compile_service(dc.contract, "Api", "State",
        &projection, &dc.state_native, &dc.state_plan, &diagnostic), DATA_BIND_OK);
    chttp_rpc_service_config service_config = CHTTP_RPC_SERVICE_CONFIG_INIT;
    service_config.method_capacity = 2u;
    check_equal(chttp_rpc_service_init(&dc.service, &service_config), SALTS_OK);
    crpc_server_config server_config = document_server_config();
    server_config.http.route_capacity = 16u;
    check_equal(crpc_server_init(&dc.server, &server_config), SALTS_OK);
    chttp_rpc_service_mount_options mount = CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT;
    mount.target = "/rpc";
    mount.method_plan = dc.plan;
    mount.native_binding = &dc.native;
    mount.execution = databind_8_Document_3_Api_3_Get__databind_execution();
    check_equal(chttp_rpc_service_mount_document(&dc.service, &dc.server, &mount), SALTS_OK);
    mount.method_plan = dc.state_plan;
    mount.native_binding = &dc.state_native;
    mount.execution = databind_8_Document_3_Api_5_State__databind_execution();
    check_equal(chttp_rpc_service_mount_document(&dc.service, &dc.server, &mount), SALTS_OK);
    for (size_t i = 0u; i < sizeof(DOCUMENT_PEERS) / sizeof(DOCUMENT_PEERS[0]); ++i)
      check_equal(chttp_server_route(crpc_server_http(&dc.server), CHTTP_METHOD_POST,
          DOCUMENT_PEERS[i].path, document_peer_reply, (void *)&DOCUMENT_PEERS[i]), SALTS_OK);
    check_equal(crpc_server_start(&dc.server), SALTS_OK);
    uint16_t port = 0u;
    check_equal(crpc_server_port(&dc.server, &port), SALTS_OK);
    dc.uri = tstr_format("tcp://127.0.0.1:{}", port);
    check_not_null(dc.uri);
    crpc_client_config client_config = {
        .http = {.network = document_network(4u), .request_capacity = 4u,
            .max_start_line_bytes = 256u, .max_header_count = 16u,
            .max_header_bytes = 4096u, .max_request_body_bytes = 8192u,
            .max_response_body_bytes = 8192u, .max_informational_responses = 2u},
        .request_capacity = 2u, .max_method_bytes = 64u, .max_json_depth = 8u};
    check_equal(crpc_client_init(&dc.client, &client_config), SALTS_OK);
    dc.options = (DataBindNativeOptions)DATA_BIND_NATIVE_OPTIONS_INIT;
    dc.options.workspace = dc.workspace;
    dc.options.workspace_bytes = sizeof(dc.workspace);
    dc.options.max_depth = 16u;
    dc.options.max_items = 128u;
    dc.options.max_owned_bytes = 1024u;
    dc.call = (chttp_rpc_service_client_call_options)CHTTP_RPC_SERVICE_CLIENT_CALL_OPTIONS_INIT;
    dc.call.connection_uri = dc.uri;
    dc.call.authority = "127.0.0.1";
    dc.call.target = "/rpc";
    dc.call.request_id = 7u;
    dc.call.deadline_ms = DOCUMENT_TIMEOUT_MS;
    dc.call.method_plan = dc.plan;
    dc.call.native_binding = &dc.native;
    dc.call.native_options = &dc.options;
    dc.call.request = &dc.request;
    dc.call.request_bytes = sizeof(dc.request);
    dc.call.response = &dc.response;
    dc.call.response_bytes = sizeof(dc.response);
  }

  after_each() {
    if (dc.client.impl != NULL)
      check_equal(crpc_client_destroy(&dc.client, DOCUMENT_TIMEOUT_MS), SALTS_OK);
    if (dc.server.impl != NULL) {
      check_equal(crpc_server_stop(&dc.server, DOCUMENT_TIMEOUT_MS), SALTS_OK);
      check_equal(crpc_server_destroy(&dc.server), SALTS_OK);
    }
    if (dc.service.impl != NULL) check_equal(chttp_rpc_service_destroy(&dc.service), SALTS_OK);
    if (dc.options.workspace != NULL) {
      dc.options.workspace_bytes = sizeof(dc.workspace);
      dc.options.max_items = 128u;
      dc.options.max_owned_bytes = 1024u;
      document_client_clear();
    }
    tstr_free(dc.uri);
    StateReply_clear(&dc.state_response);
    data_bind_rpc_method_plan_free(dc.state_plan);
    data_bind_rpc_method_plan_free(dc.xml_plan);
    data_bind_rpc_method_plan_free(dc.plan);
    data_bind_free(dc.contract);
  }

  it("round-trips generated records, canonical aliases and owned strings") {
    check_equal(document_typed_call(), SALTS_OK);
    check_equal(dc.outcome.kind, CHTTP_RPC_SERVICE_CLIENT_SUCCESS);
    check_equal(dc.response.total, UINT64_MAX);
    check_equal(dc.response.ready, (uint32_t)1u);
    check_equal(dc.response.text, "<tag>&\"hello\"");
    document_client_clear();
    dc.call.target = "/alias";
    check_equal(document_typed_call(), SALTS_OK);
    check_equal(dc.response.total, UINT64_MAX);
    check_equal(tstr_len(dc.response.text), (size_t)3u);
    check_equal(dc.response.text, "a\0b", 3u);
    document_client_clear();
    dc.call.target = "/rpc";
    check_equal(document_typed_call(), SALTS_OK);
    check_equal(dc.response.text, "<tag>&\"hello\"");
  }

  it("rejects mismatched plans before touching caller storage or sending") {
    dc.response.total = 123u;
    dc.call.connection_uri = "invalid://must-not-send";
    check_equal(chttp_rpc_service_client_call_document(&dc.client, &dc.call,
        dc.wrong_message, &dc.outcome, &dc.error), SALTS_EINVAL);
    check_equal(dc.response.total, UINT64_C(123));
    check_equal(dc.outcome.kind, CHTTP_RPC_SERVICE_CLIENT_NONE);
    check_equal(chttp_rpc_service_client_call_document(&dc.client, &dc.call,
        NULL, &dc.outcome, &dc.error), SALTS_EINVAL);
    check_equal(dc.response.total, UINT64_C(123));
    dc.call.method_plan = dc.xml_plan;
    check_equal(document_typed_call(), SALTS_ENOTSUP);
    check_equal(dc.response.total, UINT64_C(123));
    dc.call.method_plan = dc.plan;
    dc.call.response_bytes = sizeof(dc.response) - 1u;
    check_equal(document_typed_call(), SALTS_EINVAL);
    check_equal(dc.response.total, UINT64_C(123));
  }

  it("preserves ABSENT NULL and VALUE and rolls back generated state overlays") {
    dc.call.method_plan = dc.state_plan;
    dc.call.native_binding = &dc.state_native;
    dc.call.response = &dc.state_response;
    dc.call.response_bytes = sizeof(dc.state_response);
    check_equal(chttp_rpc_service_client_call_document(&dc.client, &dc.call,
        dc.state_message, &dc.outcome, &dc.error), SALTS_OK);
    check_equal(dc.state_response._presence[0], (uint8_t)0u);
    check_equal(dc.state_response._nulls[0], (uint8_t)(1u << StateReply_NULLABLE_count));
    check_equal(dc.state_response.text, "state");
    StateReply_clear(&dc.state_response);
    dc.request.large = 1u;
    check_equal(chttp_rpc_service_client_call_document(&dc.client, &dc.call,
        dc.state_message, &dc.outcome, &dc.error), SALTS_OK);
    check_equal(dc.state_response._presence[0], (uint8_t)(1u << StateReply_OPTIONAL_note));
    check_equal(dc.state_response.note, (uint32_t)9u);
    check_equal(dc.state_response._nulls[0], (uint8_t)0u);
    check_equal(dc.state_response.count, (uint32_t)0u);
    StateReply_clear(&dc.state_response);
    dc.call.target = "/state-failure";
    check_not_equal(chttp_rpc_service_client_call_document(&dc.client, &dc.call,
        dc.state_message, &dc.outcome, &dc.error), SALTS_OK);
    check_equal(dc.state_response._presence[0], (uint8_t)0u);
    check_equal(dc.state_response._nulls[0], (uint8_t)0u);
    check_null(dc.state_response.text);
    check_equal(dc.outcome.kind, CHTTP_RPC_SERVICE_CLIENT_NONE);
  }

  it("rolls back partial owned results on malformed fields and validation failure") {
    const char *paths[] = {"/missing", "/type", "/duplicate", "/constraint", "/unknown", "/null", "/scalar"};
    for (size_t i = 0u; i < sizeof(paths) / sizeof(paths[0]); ++i) {
      dc.call.target = paths[i];
      check_not_equal(document_typed_call(), SALTS_OK);
      check_equal(dc.outcome.kind, CHTTP_RPC_SERVICE_CLIENT_NONE);
      check_equal(dc.error.stage, "rpc-service-result");
      check_equal(dc.response.total, UINT64_C(0));
      check_equal(dc.response.ready, (uint32_t)0u);
      check_null(dc.response.text);
      dc.call.target = "/rpc";
      check_equal(document_typed_call(), SALTS_OK);
      document_client_clear();
    }
  }

  it("enforces decode bounds and preserves remote errors as protocol outcomes") {
    dc.options.max_owned_bytes = 2u;
    check_equal(document_typed_call(), SALTS_EMSGSIZE);
    check_equal(dc.error.stage, "rpc-service-result");
    check_null(dc.response.text);
    dc.options.max_owned_bytes = 1024u;
    dc.options.max_items = 3u;
    check_equal(document_typed_call(), SALTS_EMSGSIZE);
    check_null(dc.response.text);
    dc.options.max_items = 128u;
    dc.call.target = "/remote-error";
    check_equal(document_typed_call(), SALTS_OK);
    check_equal(dc.outcome.kind, CHTTP_RPC_SERVICE_CLIENT_REMOTE_ERROR);
    check_equal(dc.outcome.remote_code, INT64_C(-32001));
    check_null(dc.response.text);
    dc.call.target = "/rpc";
    check_equal(document_typed_call(), SALTS_OK);
  }
}
