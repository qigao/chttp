#include <chttp_service/service.h>

#include <http_client/http.h>
#include <salts_cmeta_fixed_width.h>
#include "chttp_service.http.h"
#include "tinytest.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CHTTP_SERVICE_TEST_TIMEOUT_MS = 5000 };

typedef struct AddRequest {
  uint32_t left;
  uint32_t right;
  uint32_t scale;
  uint8_t presence;
} AddRequest;

typedef struct AddResponse {
  uint32_t sum;
} AddResponse;

static const cmeta_type_identity ADD_REQUEST_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.AddRequest");
static const cmeta_type_identity ADD_RESPONSE_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.AddResponse");

static const cmeta_type_desc ADD_REQUEST_TYPE = {
    "AddRequest", sizeof(AddRequest), _Alignof(AddRequest),
    CMETA_T_OBJECT, NULL, NULL, &ADD_REQUEST_ID};
static const cmeta_type_desc ADD_RESPONSE_TYPE = {
    "AddResponse", sizeof(AddResponse), _Alignof(AddResponse),
    CMETA_T_OBJECT, NULL, NULL, &ADD_RESPONSE_ID};

static const cmeta_type_desc ADD_REQUEST_PTR_TYPE = {
    "const AddRequest *", sizeof(AddRequest *), _Alignof(AddRequest *),
    CMETA_T_POINTER, &ADD_REQUEST_TYPE, NULL, NULL};
static const cmeta_type_desc ADD_RESPONSE_PTR_TYPE = {
    "AddResponse *", sizeof(AddResponse *), _Alignof(AddResponse *),
    CMETA_T_POINTER, &ADD_RESPONSE_TYPE, NULL, NULL};

static const cmeta_field_desc ADD_REQUEST_LAYOUT_FIELDS[] = {
    {"left", "uint32_t", offsetof(AddRequest, left), sizeof(uint32_t),
     _Alignof(uint32_t), &salts_uint32_cmeta_type, NULL},
    {"right", "uint32_t", offsetof(AddRequest, right), sizeof(uint32_t),
     _Alignof(uint32_t), &salts_uint32_cmeta_type, NULL},
    {"scale", "uint32_t", offsetof(AddRequest, scale), sizeof(uint32_t),
     _Alignof(uint32_t), &salts_uint32_cmeta_type, NULL}};
static const cmeta_struct_desc ADD_REQUEST_LAYOUT = {
    "AddRequest", sizeof(AddRequest), _Alignof(AddRequest),
    ADD_REQUEST_LAYOUT_FIELDS, 3u};
static const cmeta_data_field_desc ADD_REQUEST_FIELDS[] = {
    {"test.chttp.AddRequest.left", "left", offsetof(AddRequest, left),
     &salts_uint32_cmeta_data},
    {"test.chttp.AddRequest.right", "right", offsetof(AddRequest, right),
     &salts_uint32_cmeta_data},
    {"test.chttp.AddRequest.scale", "scale", offsetof(AddRequest, scale),
     &salts_uint32_cmeta_data}};
static const cmeta_data_struct_shape ADD_REQUEST_SHAPE = {
    &ADD_REQUEST_LAYOUT, ADD_REQUEST_FIELDS, 3u};
static const cmeta_data_desc ADD_REQUEST_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.AddRequest.data",
    .display_name = "AddRequest",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &ADD_REQUEST_TYPE,
    .shape = &ADD_REQUEST_SHAPE};

static const cmeta_field_desc ADD_RESPONSE_LAYOUT_FIELDS[] = {
    {"sum", "uint32_t", offsetof(AddResponse, sum), sizeof(uint32_t),
     _Alignof(uint32_t), &salts_uint32_cmeta_type, NULL}};
static const cmeta_struct_desc ADD_RESPONSE_LAYOUT = {
    "AddResponse", sizeof(AddResponse), _Alignof(AddResponse),
    ADD_RESPONSE_LAYOUT_FIELDS, 1u};
static const cmeta_data_field_desc ADD_RESPONSE_FIELDS[] = {
    {"test.chttp.AddResponse.sum", "sum", offsetof(AddResponse, sum),
     &salts_uint32_cmeta_data}};
static const cmeta_data_struct_shape ADD_RESPONSE_SHAPE = {
    &ADD_RESPONSE_LAYOUT, ADD_RESPONSE_FIELDS, 1u};
static const cmeta_data_desc ADD_RESPONSE_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.AddResponse.data",
    .display_name = "AddResponse",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &ADD_RESPONSE_TYPE,
    .shape = &ADD_RESPONSE_SHAPE};

static const DataBindNativeStateBinding ADD_REQUEST_PRESENCE[] = {
    {sizeof(DataBindNativeStateBinding), "scale",
     offsetof(AddRequest, presence), 0u}};

static const DataBindNativeTypeBinding ADD_REQUEST_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_BINDING_PLAN_ABI_VERSION,
    .idl_type_name = "AddRequest",
    .data = &ADD_REQUEST_DATA,
    .presence = ADD_REQUEST_PRESENCE,
    .presence_count = 1u};

static const DataBindNativeTypeBinding ADD_RESPONSE_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_BINDING_PLAN_ABI_VERSION,
    .idl_type_name = "AddResponse",
    .data = &ADD_RESPONSE_DATA};

FunctionDeclAs(
    value, void, &cmeta_type_void, chttp_service_test_add,
    (const AddRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &ADD_REQUEST_PTR_TYPE),
    (AddResponse *, response, CMETA_PARAM_OUT, &ADD_RESPONSE_PTR_TYPE));

void chttp_service_test_add(const AddRequest *request, AddResponse *response) {
  if (request == NULL || response == NULL) return;
  response->sum = request->left + request->right * request->scale;
}

static DataBindStatus chttp_service_test_execute(
    void *user,
    const DataBindBindingPlan *plan,
    const DataBindBindingProvider *provider,
    const DataBindNativeOptions *native_options,
    DataBindBindingOutcome *outcome,
    DataBindBindingPlanDiagnostic *diagnostic) {
  AddRequest request = {0};
  AddResponse response = {0};
  void *params[2] = {NULL, &response};
  const size_t param_bytes[2] = {0u, sizeof(response)};
  DataBindBindingCallFrame frame =
      (DataBindBindingCallFrame)DATA_BIND_BINDING_CALL_FRAME_INIT;
  DataBindStatus status;

  (void)user;
  if (plan == NULL || provider == NULL || native_options == NULL ||
      outcome == NULL || diagnostic == NULL)
    return DATA_BIND_ERR_INVALID_ARG;
  if (data_bind_binding_plan_function(plan) !=
      FunctionMeta(chttp_service_test_add))
    return DATA_BIND_ERR_SCHEMA;

  frame.request = &request;
  frame.request_bytes = sizeof(request);
  frame.params = params;
  frame.param_bytes = param_bytes;
  frame.param_count = 2u;

  status = data_bind_binding_plan_bind_inputs(
      plan, provider, native_options, &frame, diagnostic);
  if (status != DATA_BIND_OK) return status;

  chttp_service_test_add(&request, &response);

  return data_bind_binding_plan_write_outcome(
      plan, provider, &frame, 0, outcome, diagnostic);
}

static native_io_backend_kind chttp_service_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config chttp_service_test_network(size_t connections) {
  const cnet_client_config config = {
      .backend = chttp_service_test_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 4096u,
      .receive_buffer_bytes = 256u,
      .connect_timeout_ms = CHTTP_SERVICE_TEST_TIMEOUT_MS,
      .read_timeout_ms = CHTTP_SERVICE_TEST_TIMEOUT_MS,
      .write_timeout_ms = CHTTP_SERVICE_TEST_TIMEOUT_MS};
  return config;
}

static chttp_server_config chttp_service_test_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = chttp_service_test_network(4u),
      .route_capacity = 4u,
      .middleware_capacity = 2u,
      .max_route_middleware_count = 2u,
      .max_route_param_count = 4u,
      .max_route_param_bytes = 128u,
      .max_target_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 1024u,
      .max_response_body_bytes = 512u,
      .poll_slice_ms = 2u};
  return config;
}

static chttp_client_config chttp_service_test_client_config(void) {
  const chttp_client_config config = {
      .network = chttp_service_test_network(2u),
      .request_capacity = 1u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_body_bytes = 512u,
      .max_informational_responses = 2u};
  return config;
}

static int chttp_service_test_call(
    chttp_client *client, const char *uri, const char *target,
    chttp_response *response) {
  const chttp_options options = {
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = target,
      .timeout_ms = CHTTP_SERVICE_TEST_TIMEOUT_MS};
  chttp_error error = {0};
  return chttp_get(client, &options, response, &error);
}

spec("CHttp::Service generated HTTP MethodPlan") {
  it("mounts generated transport projection without IDL HTTP annotations") {
    static const char schema[] =
        "message AddRequest {"
        " uint32 left;"
        " uint32 right;"
        " optional uint32 scale default 1;"
        "}"
        "message AddResponse { uint32 sum; }"
        "service Calc { Add: AddRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chttp_service_test_add),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    const DataBindHttpProjectionConfig *projection;
    DataBindHttpMethodPlan *method_plan = NULL;
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_test_server_config();
    chttp_client client = {0};
    chttp_client_config client_config =
        chttp_service_test_client_config();
    chttp_response response = {0};
    uint16_t port = 0u;
    char uri[64];

    projection = data_bind_http_projection_artifact_find(
        &databind_chttp_service_http_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(projection->method, "GET");
    check_equal(projection->route, "/add/{left}");
    check_equal(projection->success_status, 201);

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);
    check_equal(data_bind_http_method_plan_route(method_plan), "/add/{left}");

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 64u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    mount.method_plan = method_plan;
    mount.invoke = chttp_service_test_execute;
    check_equal(
        chttp_service_mount_http(&service, &server, &mount), SALTS_OK);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(port != 0u);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body_size, (size_t)2u);
    check_equal(response.body, "11", 2u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body_size, (size_t)1u);
    check_equal(response.body, "7", 1u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/not-a-number?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 400u);
    check_equal(response.body, "Bad Request", 11u);
    chttp_response_destroy(&response);

    check_equal(
        chttp_client_destroy(&client, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }
}
