#include <chttp_rpc_service/service.h>

#include <http_client/rpc.h>
#include <salts_cmeta_fixed_width.h>
#include "chttp_rpc_service.rpc.h"
#include "tinytest.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS = 5000 };

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
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.rpc.AddRequest");
static const cmeta_type_identity ADD_RESPONSE_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.rpc.AddResponse");

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
    {"test.chttp.rpc.AddRequest.left", "left", offsetof(AddRequest, left),
     &salts_uint32_cmeta_data},
    {"test.chttp.rpc.AddRequest.right", "right", offsetof(AddRequest, right),
     &salts_uint32_cmeta_data},
    {"test.chttp.rpc.AddRequest.scale", "scale", offsetof(AddRequest, scale),
     &salts_uint32_cmeta_data}};
static const cmeta_data_struct_shape ADD_REQUEST_SHAPE = {
    &ADD_REQUEST_LAYOUT, ADD_REQUEST_FIELDS, 3u};
static const cmeta_data_desc ADD_REQUEST_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.rpc.AddRequest.data",
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
    {"test.chttp.rpc.AddResponse.sum", "sum", offsetof(AddResponse, sum),
     &salts_uint32_cmeta_data}};
static const cmeta_data_struct_shape ADD_RESPONSE_SHAPE = {
    &ADD_RESPONSE_LAYOUT, ADD_RESPONSE_FIELDS, 1u};
static const cmeta_data_desc ADD_RESPONSE_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.rpc.AddResponse.data",
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
    value, void, &cmeta_type_void, chttp_rpc_service_test_add,
    (const AddRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &ADD_REQUEST_PTR_TYPE),
    (AddResponse *, response, CMETA_PARAM_OUT, &ADD_RESPONSE_PTR_TYPE));

void chttp_rpc_service_test_add(
    const AddRequest *request, AddResponse *response) {
  if (request == NULL || response == NULL) return;
  response->sum = request->left + request->right * request->scale;
}

static DataBindStatus chttp_rpc_service_test_execute(
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
      FunctionMeta(chttp_rpc_service_test_add))
    return DATA_BIND_ERR_SCHEMA;

  frame.request = &request;
  frame.request_bytes = sizeof(request);
  frame.params = params;
  frame.param_bytes = param_bytes;
  frame.param_count = 2u;

  status = data_bind_binding_plan_bind_inputs(
      plan, provider, native_options, &frame, diagnostic);
  if (status != DATA_BIND_OK) return status;

  chttp_rpc_service_test_add(&request, &response);
  return data_bind_binding_plan_write_outcome(
      plan, provider, &frame, 0, outcome, diagnostic);
}

static native_io_backend_kind chttp_rpc_service_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config chttp_rpc_service_test_network(
    size_t connection_capacity) {
  const cnet_client_config config = {
      .backend = chttp_rpc_service_test_backend(),
      .connection_capacity = connection_capacity,
      .command_capacity = 32u,
      .request_capacity = 16u,
      .completion_batch_capacity = 8u,
      .event_capacity = 32u,
      .max_send_bytes = 64u * 1024u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS,
      .read_timeout_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS,
      .write_timeout_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS};
  return config;
}

static crpc_server_config chttp_rpc_service_test_server_config(void) {
  crpc_server_config config = {0};
  config.http.host = "127.0.0.1";
  config.http.port = 0u;
  config.http.backlog = 8u;
  config.http.network = chttp_rpc_service_test_network(8u);
  config.http.route_capacity = 4u;
  config.http.middleware_capacity = 2u;
  config.http.max_route_middleware_count = 2u;
  config.http.max_route_param_count = 2u;
  config.http.max_route_param_bytes = 128u;
  config.http.max_target_bytes = 256u;
  config.http.max_header_count = 16u;
  config.http.max_header_bytes = 4096u;
  config.http.max_request_body_bytes = 8192u;
  config.http.max_response_header_count = 16u;
  config.http.max_response_header_bytes = 4096u;
  config.http.max_response_body_bytes = 8192u;
  config.http.max_buffered_response_body_bytes = 8192u;
  config.http.poll_slice_ms = 2u;
  config.method_capacity = 4u;
  config.max_method_bytes = 64u;
  config.max_json_depth = 8u;
  config.max_batch_items = 4u;
  return config;
}

static crpc_client_config chttp_rpc_service_test_client_config(void) {
  crpc_client_config config = {0};
  config.http.network = chttp_rpc_service_test_network(4u);
  config.http.request_capacity = 4u;
  config.http.max_start_line_bytes = 256u;
  config.http.max_header_count = 16u;
  config.http.max_header_bytes = 4096u;
  config.http.max_request_body_bytes = 8192u;
  config.http.max_response_body_bytes = 8192u;
  config.http.max_informational_responses = 2u;
  config.request_capacity = 2u;
  config.max_method_bytes = 64u;
  config.max_json_depth = 8u;
  return config;
}

typedef struct chttp_rpc_service_params {
  uint64_t values[3];
  size_t count;
  int invalid_first;
} chttp_rpc_service_params;

static cserde_status chttp_rpc_service_write(
    cserde_writer *writer, cserde_token token) {
  return cserde_writer_write(writer, &token);
}

static cserde_status chttp_rpc_service_encode_params(
    void *user, cserde_writer *writer) {
  chttp_rpc_service_params *params =
      (chttp_rpc_service_params *)user;
  size_t i;
  cserde_status status;

  if (params == NULL) return CSERDE_INVALID_ARGUMENT;
  status = chttp_rpc_service_write(
      writer, (cserde_token){.kind = CSERDE_ARRAY_BEGIN});
  for (i = 0u; status == CSERDE_OK && i < params->count; ++i) {
    if (i == 0u && params->invalid_first) {
      static const unsigned char bad[] = "bad";
      status = chttp_rpc_service_write(
          writer,
          (cserde_token){
              .kind = CSERDE_STRING,
              .value.slice = {bad, sizeof(bad) - 1u, CSERDE_VIEW_STABLE}});
    } else {
      status = chttp_rpc_service_write(
          writer,
          (cserde_token){
              .kind = CSERDE_UINT,
              .value.uint = params->values[i]});
    }
  }
  if (status == CSERDE_OK)
    status = chttp_rpc_service_write(
        writer, (cserde_token){.kind = CSERDE_ARRAY_END});
  return status;
}

spec("CHttp::RpcService generated RPC MethodPlan") {
  it("mounts generated wire mapping and preserves DataBind defaults") {
    static const char schema[] =
        "message AddRequest {"
        " @Min(1) uint32 left;"
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
            FunctionMeta(chttp_rpc_service_test_add),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    const DataBindRpcProjectionConfig *projection;
    DataBindRpcMethodPlan *method_plan = NULL;
    chttp_rpc_service service = {0};
    chttp_rpc_service_config service_config =
        CHTTP_RPC_SERVICE_CONFIG_INIT;
    chttp_rpc_service_mount mount = CHTTP_RPC_SERVICE_MOUNT_INIT;
    crpc_server server = {0};
    crpc_server_config server_config =
        chttp_rpc_service_test_server_config();
    crpc_client client = {0};
    crpc_client_config client_config =
        chttp_rpc_service_test_client_config();
    crpc_method method = {.name = "calc.add"};
    crpc_options options = {0};
    crpc_response response = {0};
    crpc_error error = {0};
    chttp_rpc_service_params params = {{3u, 4u, 2u}, 3u, 0};
    cserde_token token = {0};
    char uri[64];
    uint16_t port = 0u;

    projection = data_bind_rpc_projection_artifact_find(
        &databind_chttp_rpc_service_rpc_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(projection->wire_method, "calc.add");

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_equal(
        data_bind_rpc_method_plan_compile_service(
            contract, "Calc", "Add", projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);
    check_equal(
        data_bind_rpc_method_plan_wire_method(method_plan), "calc.add");

    service_config.method_capacity = 1u;
    service_config.max_output_value_bytes = 64u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;
    check_equal(
        chttp_rpc_service_init(&service, &service_config), SALTS_OK);

    check_equal(crpc_server_init(&server, &server_config), SALTS_OK);
    mount.target = "/rpc";
    mount.method_plan = method_plan;
    mount.invoke = chttp_rpc_service_test_execute;
    check_equal(
        chttp_rpc_service_mount(&service, &server, &mount), SALTS_OK);

    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(crpc_client_init(&client, &client_config), SALTS_OK);

    options = (crpc_options){
        .connection_uri = uri,
        .authority = "localhost",
        .target = "/rpc",
        .method = method,
        .request_id = UINT64_C(1),
        .deadline_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS,
        .encode_params = chttp_rpc_service_encode_params,
        .params_user = &params};
    check_equal(
        crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.kind, CRPC_RESPONSE_RESULT);
    check_equal(cserde_reader_next(response.value.result, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_UINT);
    check_equal(token.value.uint, UINT64_C(11));
    crpc_response_destroy(&response);

    params.count = 2u;
    params.invalid_first = 0;
    options.request_id = UINT64_C(2);
    response = (crpc_response){0};
    check_equal(
        crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.kind, CRPC_RESPONSE_RESULT);
    check_equal(cserde_reader_next(response.value.result, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_UINT);
    check_equal(token.value.uint, UINT64_C(7));
    crpc_response_destroy(&response);

    params.count = 2u;
    params.invalid_first = 0;
    params.values[0] = 0u;
    options.request_id = UINT64_C(3);
    response = (crpc_response){0};
    check_equal(
        crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.kind, CRPC_RESPONSE_REMOTE_ERROR);
    check_equal(response.value.remote_error.code, (int64_t)-32602);
    crpc_response_destroy(&response);

    params.values[0] = 3u;
    params.invalid_first = 1;
    options.request_id = UINT64_C(4);
    response = (crpc_response){0};
    check_equal(
        crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.kind, CRPC_RESPONSE_REMOTE_ERROR);
    check_equal(response.value.remote_error.code, (int64_t)-32602);
    crpc_response_destroy(&response);

    check_equal(
        crpc_client_destroy(&client, CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        crpc_server_stop(&server, CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(crpc_server_destroy(&server), SALTS_OK);
    check_equal(chttp_rpc_service_destroy(&service), SALTS_OK);
    data_bind_rpc_method_plan_free(method_plan);
    data_bind_free(contract);
  }
}
