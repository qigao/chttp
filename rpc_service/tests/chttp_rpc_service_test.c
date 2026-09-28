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
     _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"right", "uint32_t", offsetof(AddRequest, right), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"scale", "uint32_t", offsetof(AddRequest, scale), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL}};
static const cmeta_struct_desc ADD_REQUEST_LAYOUT = {
    "AddRequest", sizeof(AddRequest), _Alignof(AddRequest),
    ADD_REQUEST_LAYOUT_FIELDS, 3u};
static const cmeta_data_field_desc ADD_REQUEST_FIELDS[] = {
    {"test.chttp.rpc.AddRequest.left", "left", offsetof(AddRequest, left),
     &cmeta_data_uint32},
    {"test.chttp.rpc.AddRequest.right", "right", offsetof(AddRequest, right),
     &cmeta_data_uint32},
    {"test.chttp.rpc.AddRequest.scale", "scale", offsetof(AddRequest, scale),
     &cmeta_data_uint32}};
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
     _Alignof(uint32_t), &cmeta_type_uint32, NULL}};
static const cmeta_struct_desc ADD_RESPONSE_LAYOUT = {
    "AddResponse", sizeof(AddResponse), _Alignof(AddResponse),
    ADD_RESPONSE_LAYOUT_FIELDS, 1u};
static const cmeta_data_field_desc ADD_RESPONSE_FIELDS[] = {
    {"test.chttp.rpc.AddResponse.sum", "sum", offsetof(AddResponse, sum),
     &cmeta_data_uint32}};
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
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "AddRequest",
    .data = &ADD_REQUEST_DATA,
    .presence = ADD_REQUEST_PRESENCE,
    .presence_count = 1u};

static const DataBindNativeTypeBinding ADD_RESPONSE_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "AddResponse",
    .data = &ADD_RESPONSE_DATA};

FunctionDeclAsAbi(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    chttp_rpc_service_test_add,
    (const AddRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &ADD_REQUEST_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (AddResponse *, response,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &ADD_RESPONSE_PTR_TYPE, CMETA_ABI_OBJECT_POINTER));

int chttp_rpc_service_test_add(
    const AddRequest *request, AddResponse *response) {
  if (request == NULL || response == NULL) return -1;
  if (request->left == 99u) return 7;
  response->sum = request->left + request->right * request->scale;
  return 0;
}

static bool DATA_BIND_NATIVE_CALL chttp_rpc_service_test_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int result;
  (void)context;
  if (return_storage == NULL || params == NULL || param_count != 2u ||
      params[0] == NULL || params[1] == NULL)
    return false;
  result = chttp_rpc_service_test_add(
      (const AddRequest *)params[0], (AddResponse *)params[1]);
  *(int *)return_storage = result;
  return true;
}

FunctionDeclAsAbi(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    chttp_rpc_service_test_other,
    (const AddRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &ADD_REQUEST_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (AddResponse *, response,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &ADD_RESPONSE_PTR_TYPE, CMETA_ABI_OBJECT_POINTER));

int chttp_rpc_service_test_other(
    const AddRequest *request, AddResponse *response) {
  return chttp_rpc_service_test_add(request, response);
}

static bool DATA_BIND_NATIVE_CALL chttp_rpc_service_test_other_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int result;
  (void)context;
  if (return_storage == NULL || params == NULL || param_count != 2u ||
      params[0] == NULL || params[1] == NULL)
    return false;
  result = chttp_rpc_service_test_other(
      (const AddRequest *)params[0], (AddResponse *)params[1]);
  *(int *)return_storage = result;
  return true;
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
    chttp_rpc_service_mount_options mount = CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT;
    chttp_rpc_service_mount_options mismatch = CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT;
    chttp_rpc_service_mount_options invalid = CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT;
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindNativeExecution other_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindNativeExecution invalid_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
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
    unsigned char client_workspace[4096];
    DataBindNativeOptions client_native =
        (DataBindNativeOptions)DATA_BIND_NATIVE_OPTIONS_INIT;
    chttp_rpc_service_client_call_options typed_call =
        CHTTP_RPC_SERVICE_CLIENT_CALL_OPTIONS_INIT;
    chttp_rpc_service_client_outcome typed_outcome =
        CHTTP_RPC_SERVICE_CLIENT_OUTCOME_INIT;
    AddRequest typed_request = {3u, 4u, 2u, 1u};
    AddResponse typed_response = {0};
    char uri[64];
    uint16_t port = 0u;

    execution.function = FunctionMeta(chttp_rpc_service_test_add);
    execution.abi = FunctionAbi(chttp_rpc_service_test_add);
    execution.invoke = chttp_rpc_service_test_invoke;
    other_execution.function = FunctionMeta(chttp_rpc_service_test_other);
    other_execution.abi = FunctionAbi(chttp_rpc_service_test_other);
    other_execution.invoke = chttp_rpc_service_test_other_invoke;
    invalid_execution = execution;
    invalid_execution.invoke = NULL;

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
    service_config.max_call_frame_bytes = 512u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;
    check_equal(
        chttp_rpc_service_init(&service, &service_config), SALTS_OK);

    check_equal(crpc_server_init(&server, &server_config), SALTS_OK);

    mismatch.target = "/rpc";
    mismatch.method_plan = method_plan;
    mismatch.native_binding = &native;
    mismatch.execution = &other_execution;
    check_equal(
        chttp_rpc_service_mount(&service, &server, &mismatch), SALTS_EINVAL);

    invalid.target = "/rpc";
    invalid.method_plan = method_plan;
    invalid.native_binding = &native;
    invalid.execution = &invalid_execution;
    check_equal(
        chttp_rpc_service_mount(&service, &server, &invalid), SALTS_EINVAL);

    mount.target = "/rpc";
    mount.method_plan = method_plan;
    mount.native_binding = &native;
    mount.execution = &execution;
    check_equal(
        chttp_rpc_service_mount(&service, &server, &mount), SALTS_OK);

    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(crpc_client_init(&client, &client_config), SALTS_OK);

    client_native.workspace = client_workspace;
    client_native.workspace_bytes = sizeof(client_workspace);
    client_native.max_depth = 16u;
    client_native.max_items = 64u;
    client_native.max_owned_bytes = 1024u;

    typed_call.connection_uri = uri;
    typed_call.authority = "localhost";
    typed_call.target = "/rpc";
    typed_call.request_id = UINT64_C(100);
    typed_call.deadline_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS;
    typed_call.method_plan = method_plan;
    typed_call.native_binding = &native;
    typed_call.native_options = &client_native;
    typed_call.request = &typed_request;
    typed_call.request_bytes = sizeof(typed_request);
    typed_call.response = &typed_response;
    typed_call.response_bytes = sizeof(typed_response);

    check_equal(
        chttp_rpc_service_client_call(
            &client, &typed_call, &typed_outcome, &error),
        SALTS_OK);
    check_equal(
        typed_outcome.kind, CHTTP_RPC_SERVICE_CLIENT_SUCCESS);
    check_equal(typed_response.sum, (uint32_t)11u);

    typed_request =
        (AddRequest){.left = 3u, .right = 4u, .scale = 0u, .presence = 0u};
    typed_response = (AddResponse){0};
    typed_outcome =
        (chttp_rpc_service_client_outcome)
            CHTTP_RPC_SERVICE_CLIENT_OUTCOME_INIT;
    typed_call.request_id = UINT64_C(101);
    check_equal(
        chttp_rpc_service_client_call(
            &client, &typed_call, &typed_outcome, &error),
        SALTS_OK);
    check_equal(
        typed_outcome.kind, CHTTP_RPC_SERVICE_CLIENT_SUCCESS);
    check_equal(typed_response.sum, (uint32_t)7u);

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

    params.count = 2u;
    params.invalid_first = 0;
    params.values[0] = 99u;
    params.values[1] = 1u;
    options.request_id = UINT64_C(5);
    response = (crpc_response){0};
    check_equal(
        crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.kind, CRPC_RESPONSE_REMOTE_ERROR);
    check_equal(response.value.remote_error.code, (int64_t)-32000);
    check_not_null(response.value.remote_error.data);
    check_equal(
        cserde_reader_next(response.value.remote_error.data, &token), CSERDE_OK);
    check(token.kind == CSERDE_SINT || token.kind == CSERDE_UINT);
    if (token.kind == CSERDE_SINT)
      check_equal(token.value.sint, INT64_C(7));
    else
      check_equal(token.value.uint, UINT64_C(7));
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
