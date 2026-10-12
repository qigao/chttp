#include <http_client/rpc_service.h>
#include <http_server/rpc_service.h>

#include <http_client/rpc.h>
#include <cmeta_cmeta_fixed_width.h>
#include "chttp_rpc_service.rpc.h"
#include "tinytest.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <string.h>

enum { CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS = 5000 };

/* Test-only transport bounds: production exposes only CHttp Server/Client
 * configuration and no independently owned CRPC Server/Client. */
typedef struct chhttp_rpc_fixture_server_config {
  chttp_server_config http;
  size_t method_capacity;
  size_t max_method_bytes;
  size_t max_json_depth;
  size_t max_batch_items;
} chhttp_rpc_fixture_server_config;

typedef struct chhttp_rpc_fixture_client_config {
  chttp_client_config http;
  size_t request_capacity;
  size_t max_method_bytes;
  size_t max_json_depth;
} chhttp_rpc_fixture_client_config;



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

FunctionDeclAsAbiResult(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    CMETA_RESULT_VALUE, chttp_rpc_service_test_add,
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

typedef struct chttp_rpc_service_test_race_probe {
  atomic_int enabled;
  atomic_int entered;
  atomic_int release;
} chttp_rpc_service_test_race_probe;

static bool DATA_BIND_NATIVE_CALL chttp_rpc_service_test_concurrent_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  chttp_rpc_service_test_race_probe *probe =
      (chttp_rpc_service_test_race_probe *)context;
  if (probe != NULL &&
      atomic_load_explicit(&probe->enabled, memory_order_acquire)) {
    const uint64_t deadline = cmeta_monotonic_ms() + 3000u;
    atomic_fetch_add_explicit(&probe->entered, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(&probe->release, memory_order_acquire)) {
      if (cmeta_monotonic_ms() >= deadline) return false;
      cmeta_thread_yield();
    }
  }
  return chttp_rpc_service_test_invoke(
      NULL, return_storage, params, param_count);
}

typedef struct chttp_rpc_service_test_call_thread {
  chhttp_rpc_fixture_client_config config;
  char uri[64];
  const DataBindRpcMethodPlan *plan;
  const DataBindServiceNativeBinding *binding;
  AddRequest request;
  AddResponse response;
  uint64_t id;
  int status;
  atomic_int completed;
  int64_t remote_code;
  chttp_client_rpc_outcome_kind outcome_kind;
} chttp_rpc_service_test_call_thread;

static void chttp_rpc_service_test_call_worker(void *user) {
  chttp_rpc_service_test_call_thread *args =
      (chttp_rpc_service_test_call_thread *)user;
  chttp_client client = {0};
  chttp_client_rpc_call_options options =
      CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
  chttp_client_rpc_outcome outcome =
      CHTTP_CLIENT_RPC_OUTCOME_INIT;
  DataBindNativeOptions native = DATA_BIND_NATIVE_OPTIONS_INIT;
  crpc_error error = {0};
  unsigned char workspace[4096];
  int status;
  if (args == NULL) return;
  status = chttp_client_init(&client, &args->config.http);
  if (status == SALTS_OK) {
    native.workspace = workspace;
    native.workspace_bytes = sizeof(workspace);
    native.max_depth = 16u;
    native.max_items = 64u;
    native.max_owned_bytes = 1024u;
    options.connection_uri = args->uri;
    options.authority = "localhost";
    options.target = "/rpc";
    options.request_id = args->id;
    options.deadline_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS;
    options.method_plan = args->plan;
    options.native_binding = args->binding;
    options.native_options = &native;
    options.request = &args->request;
    options.request_bytes = sizeof(args->request);
    options.response = &args->response;
    options.response_bytes = sizeof(args->response);
    status = chttp_client_rpc_call(
        &client, &options, &outcome, &error);
    args->outcome_kind = outcome.kind;
    args->remote_code = outcome.remote_code;
    atomic_store_explicit(&args->completed, 1, memory_order_release);
    {
      const int teardown = chttp_client_destroy(
          &client, CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS);
      if (status == SALTS_OK && teardown != SALTS_OK) status = teardown;
    }
  }
  args->status = status;
  atomic_store_explicit(&args->completed, 1, memory_order_release);
}

FunctionDeclAsAbiResult(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    CMETA_RESULT_VALUE, chttp_rpc_service_test_other,
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

static chhttp_rpc_fixture_server_config chttp_rpc_service_test_server_config(void) {
  chhttp_rpc_fixture_server_config config = {0};
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

static chhttp_rpc_fixture_client_config chttp_rpc_service_test_client_config(void) {
  chhttp_rpc_fixture_client_config config = {0};
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

static int chttp_rpc_service_test_health(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user;
  (void)request;
  return chttp_server_reply(response, 200u, "text/plain", "healthy", 7u);
}

typedef struct chttp_rpc_service_test_async_typed_probe {
  int terminal_calls;
  int status;
  chttp_client_rpc_outcome_kind kind;
  int64_t remote_code;
} chttp_rpc_service_test_async_typed_probe;

static void chttp_rpc_service_test_async_typed_complete(
    void *user, chttp_request request,
    const chttp_client_rpc_outcome *outcome, const crpc_error *error) {
  chttp_rpc_service_test_async_typed_probe *probe =
      (chttp_rpc_service_test_async_typed_probe *)user;
  (void)request;
  if (probe == NULL) return;
  ++probe->terminal_calls;
  probe->status = error == NULL ? SALTS_OK : error->status;
  probe->kind = outcome != NULL ? outcome->kind : CHTTP_CLIENT_RPC_NONE;
  probe->remote_code = outcome != NULL ? outcome->remote_code : 0;
}


spec("CHttp Server/Client generated RPC MethodPlan") {
  it("mounts immutable generated plans and qualifies client/Owner lifetimes") {
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
    DataBindRpcMethodPlan *unknown_result_plan = NULL;
    DataBindRpcMethodPlan *unborrowed_request_plan = NULL;
    cmeta_function_desc unknown_result_function = {0};
    cmeta_function_abi_desc unknown_result_abi = {0};
    DataBindServiceNativeBinding unknown_result_native = {0};
    DataBindNativeExecution unknown_result_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    cmeta_param_desc unborrowed_request_params[2];
    cmeta_function_desc unborrowed_request_function = {0};
    cmeta_function_abi_desc unborrowed_request_abi = {0};
    DataBindServiceNativeBinding unborrowed_request_native = {0};
    DataBindNativeExecution unborrowed_request_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindNativeExecution other_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindNativeExecution invalid_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    chhttp_rpc_fixture_server_config server_config =
        chttp_rpc_service_test_server_config();
    chhttp_rpc_fixture_client_config client_config =
        chttp_rpc_service_test_client_config();
    crpc_error error = {0};
    unsigned char client_workspace[4096];
    DataBindNativeOptions client_native =
        (DataBindNativeOptions)DATA_BIND_NATIVE_OPTIONS_INIT;
    AddRequest typed_request = {3u, 4u, 2u, 1u};
    AddResponse typed_response = {0};
    execution.function = FunctionMeta(chttp_rpc_service_test_add);
    execution.abi = FunctionAbi(chttp_rpc_service_test_add);
    execution.invoke = chttp_rpc_service_test_invoke;
    other_execution.function = FunctionMeta(chttp_rpc_service_test_other);
    other_execution.abi = FunctionAbi(chttp_rpc_service_test_other);
    other_execution.invoke = chttp_rpc_service_test_other_invoke;
    invalid_execution = execution;
    invalid_execution.invoke = NULL;

    unknown_result_function = *FunctionMeta(chttp_rpc_service_test_add);
    unknown_result_function.result_flags = CMETA_RESULT_UNKNOWN;
    unknown_result_abi = *FunctionAbi(chttp_rpc_service_test_add);
    unknown_result_abi.function = &unknown_result_function;
    unknown_result_native = native;
    unknown_result_native.function = &unknown_result_function;
    unknown_result_execution = execution;
    unknown_result_execution.function = &unknown_result_function;
    unknown_result_execution.abi = &unknown_result_abi;

    unborrowed_request_params[0] =
        FunctionMeta(chttp_rpc_service_test_add)->params[0];
    unborrowed_request_params[1] =
        FunctionMeta(chttp_rpc_service_test_add)->params[1];
    unborrowed_request_params[0].flags = CMETA_PARAM_IN;
    unborrowed_request_function = *FunctionMeta(chttp_rpc_service_test_add);
    unborrowed_request_function.params = unborrowed_request_params;
    unborrowed_request_abi = *FunctionAbi(chttp_rpc_service_test_add);
    unborrowed_request_abi.function = &unborrowed_request_function;
    unborrowed_request_native = native;
    unborrowed_request_native.function = &unborrowed_request_function;
    unborrowed_request_execution = execution;
    unborrowed_request_execution.function = &unborrowed_request_function;
    unborrowed_request_execution.abi = &unborrowed_request_abi;

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

    check_equal(
        data_bind_rpc_method_plan_compile_service(
            contract, "Calc", "Add", projection, &unknown_result_native,
            &unknown_result_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(unknown_result_plan);
    check_equal(
        data_bind_rpc_method_plan_compile_service(
            contract, "Calc", "Add", projection, &unborrowed_request_native,
            &unborrowed_request_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(unborrowed_request_plan);

    client_native.workspace = client_workspace;
    client_native.workspace_bytes = sizeof(client_workspace);
    client_native.max_depth = 16u;
    client_native.max_items = 64u;
    client_native.max_owned_bytes = 1024u;

    /* #241: same producer MethodPlan on an already initialized HTTP Server.
     * No RpcService, CRPC Server, or additional HTTP listener owner. */
    {
      chttp_server host = {0};
      chttp_server_config http_config = server_config.http;
      chttp_server_rpc_config rpc_config = CHTTP_SERVER_RPC_CONFIG_INIT;
      chttp_server_rpc_mount_options generated =
          CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;
      chttp_server_execution_options topology =
          CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
      DataBindNativeExecution concurrent_execution = execution;
      chttp_rpc_service_test_race_probe race = {0};
      char attached_uri[64];
      uint16_t attached_port = 0u;

      generated.target = "/rpc";
      generated.method_plan = method_plan;
      generated.native_binding = &native;
      generated.execution = &execution;
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_EINVAL);
      check_equal(chttp_server_init(&host, &http_config), SALTS_OK);
      check_equal(chttp_server_route(&host, CHTTP_METHOD_GET, "/health",
                                    chttp_rpc_service_test_health, NULL), SALTS_OK);
      topology.owner_count = 3u;
      check_equal(chttp_server_set_execution_options(&host, &topology), SALTS_OK);
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_EINVAL);
      rpc_config.method_capacity = 2u;
      rpc_config.max_inflight_calls = 2u;
      rpc_config.native_max_items = 64u;
      rpc_config.native_max_owned_bytes = 1024u;
      concurrent_execution.context = &race;
      concurrent_execution.invoke = chttp_rpc_service_test_concurrent_invoke;
      generated.execution = &concurrent_execution;
      check_equal(chttp_server_rpc_configure(&host, &rpc_config), SALTS_OK);
      generated.execution = &other_execution;
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_EINVAL);
      generated.execution = &invalid_execution;
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_EINVAL);
      generated.method_plan = unknown_result_plan;
      generated.native_binding = &unknown_result_native;
      generated.execution = &unknown_result_execution;
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_ENOTSUP);
      generated.method_plan = unborrowed_request_plan;
      generated.native_binding = &unborrowed_request_native;
      generated.execution = &unborrowed_request_execution;
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_ENOTSUP);
      generated.method_plan = method_plan;
      generated.native_binding = &native;
      generated.execution = &concurrent_execution;
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_OK);
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_EALREADY);
      check_equal(chttp_server_start(&host), SALTS_OK);
      check_equal(chttp_server_rpc_mount(&host, &generated), SALTS_EBUSY);
      check_equal(chttp_server_port(&host, &attached_port), SALTS_OK);
      check_greater(
          snprintf(attached_uri, sizeof(attached_uri), "tcp://127.0.0.1:%u",
                   (unsigned int)attached_port), 0);
      /* #242: one initialized CHttp Client handles generated RPC, ordinary
       * HTTP GET and another RPC, all through its same connection-pool owner. */
      {
        chttp_client http_client = {0};
        chttp_client_rpc_call_options direct =
            CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
        chttp_client_rpc_outcome direct_outcome =
            CHTTP_CLIENT_RPC_OUTCOME_INIT;
        chttp_response http_response = {0};
        chttp_error http_error = {0};
        chttp_options health = {0};
        check_equal(chttp_client_init(&http_client, &client_config.http),
                    SALTS_OK);
        direct.connection_uri = attached_uri;
        direct.authority = "localhost";
        direct.target = "/rpc";
        direct.request_id = UINT64_C(301);
        direct.deadline_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS;
        direct.method_plan = method_plan;
        direct.native_binding = &native;
        direct.native_options = &client_native;
        direct.request = &typed_request;
        direct.request_bytes = sizeof(typed_request);
        direct.response = &typed_response;
        direct.response_bytes = sizeof(typed_response);
        typed_request = (AddRequest){.left = 3u, .right = 4u,
                                     .scale = 2u, .presence = 1u};
        typed_response = (AddResponse){0};
        check_equal(chttp_client_rpc_call(
            &http_client, &direct, &direct_outcome, &error), SALTS_OK);
        check_equal(direct_outcome.kind, CHTTP_CLIENT_RPC_SUCCESS);
        check_equal(typed_response.sum, (uint32_t)11u);
        health = (chttp_options){.connection_uri = attached_uri,
                                 .authority = "localhost",
                                 .target = "/health",
                                 .timeout_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS};
        check_equal(chttp_get(&http_client, &health,
                              &http_response, &http_error), SALTS_OK);
        check_equal(http_response.status_code, 200u);
        check_equal(http_response.body, "healthy", 7u);
        chttp_response_destroy(&http_response);
        typed_request = (AddRequest){.left = 99u, .right = 1u,
                                     .scale = 1u, .presence = 1u};
        typed_response = (AddResponse){0};
        direct.request_id = UINT64_C(302);
        direct_outcome = (chttp_client_rpc_outcome)
            CHTTP_CLIENT_RPC_OUTCOME_INIT;
        check_equal(chttp_client_rpc_call(
            &http_client, &direct, &direct_outcome, &error), SALTS_OK);
        check_equal(direct_outcome.kind, CHTTP_CLIENT_RPC_REMOTE_ERROR);
        check_equal(direct_outcome.remote_code, INT64_C(-32000));
        check_equal(chttp_client_destroy(
            &http_client, CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS), SALTS_OK);
      }

      /* Generated async native result/remote error are projected from
       * the same HTTP Owner terminal callback, with no RpcService client. */
      {
        chttp_async_client async_client = {0};
        chttp_client_rpc_call_options async_options =
            CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
        chttp_rpc_service_test_async_typed_probe completed = {0};
        chttp_request handle = {0};
        uint64_t deadline;
        int poll_status = SALTS_OK;
        check_equal(chttp_async_client_init(
            &async_client, &client_config.http), SALTS_OK);
        async_options.connection_uri = attached_uri;
        async_options.authority = "localhost";
        async_options.target = "/rpc";
        async_options.request_id = UINT64_C(401);
        async_options.deadline_ms = CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS;
        async_options.method_plan = method_plan;
        async_options.native_binding = &native;
        async_options.native_options = &client_native;
        async_options.request = &typed_request;
        async_options.request_bytes = sizeof(typed_request);
        async_options.response = &typed_response;
        async_options.response_bytes = sizeof(typed_response);
        typed_request = (AddRequest){.left = 3u, .right = 4u,
                                     .scale = 2u, .presence = 1u};
        typed_response = (AddResponse){0};
        check_equal(chttp_async_client_rpc_call(
            &async_client, &async_options,
            chttp_rpc_service_test_async_typed_complete,
            &completed, &handle), SALTS_OK);
        check(handle.slot != 0u);
        /* Immediate duplicate RPC admission must not change any new caller-
         * supplied native output, nor produce a callback or request handle. */
        {
          AddResponse rejected_storage = {.sum = UINT32_C(0xdecafbad)};
          chttp_client_rpc_call_options rejected_options = async_options;
          chttp_request rejected_handle = {0};
          rejected_options.response = &rejected_storage;
          check_equal(chttp_async_client_rpc_call(
              &async_client, &rejected_options,
              chttp_rpc_service_test_async_typed_complete,
              &completed, &rejected_handle), SALTS_EALREADY);
          check_equal(rejected_handle.slot, 0u);
          check_equal(rejected_storage.sum, UINT32_C(0xdecafbad));
          check_equal(completed.terminal_calls, 0);
        }
        deadline = cmeta_monotonic_ms() + CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS;
        while (completed.terminal_calls == 0 &&
               cmeta_monotonic_ms() < deadline) {
          size_t events = 0u;
          poll_status = chttp_async_client_poll(&async_client, 5u, &events);
          if (poll_status != SALTS_OK) break;
        }
        check_equal(poll_status, SALTS_OK);
        check_equal(completed.terminal_calls, 1);
        check_equal(completed.status, SALTS_OK);
        check_equal(completed.kind, CHTTP_CLIENT_RPC_SUCCESS);
        check_equal(typed_response.sum, (uint32_t)11u);
        typed_request = (AddRequest){.left = 99u, .right = 1u,
                                     .scale = 1u, .presence = 1u};
        typed_response = (AddResponse){0};
        completed = (chttp_rpc_service_test_async_typed_probe){0};
        async_options.request_id = UINT64_C(402);
        check_equal(chttp_async_client_rpc_call(
            &async_client, &async_options,
            chttp_rpc_service_test_async_typed_complete,
            &completed, &handle), SALTS_OK);
        deadline = cmeta_monotonic_ms() + CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS;
        while (completed.terminal_calls == 0 &&
               cmeta_monotonic_ms() < deadline) {
          size_t events = 0u;
          poll_status = chttp_async_client_poll(&async_client, 5u, &events);
          if (poll_status != SALTS_OK) break;
        }
        check_equal(poll_status, SALTS_OK);
        check_equal(completed.terminal_calls, 1);
        check_equal(completed.status, SALTS_OK);
        check_equal(completed.kind, CHTTP_CLIENT_RPC_REMOTE_ERROR);
        check_equal(completed.remote_code, INT64_C(-32000));
        check_equal(chttp_async_client_stop(
            &async_client, CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS), SALTS_OK);
        check_equal(chttp_async_client_destroy(&async_client), SALTS_OK);
      }

      /* Force overlapping same-method execution on two real HTTP Owners.
       * Immutable method descriptors must not share native call frames. */
      {
        cmeta_thread_t t1 = {0};
        cmeta_thread_t t2 = {0};
        cmeta_thread_t t3 = {0};
        chttp_rpc_service_test_call_thread a = {0};
        chttp_rpc_service_test_call_thread b = {0};
        chttp_rpc_service_test_call_thread c = {0};
        uint64_t deadline;
        int ready;
        int rejected_before_release;
        a.config = client_config;
        b.config = client_config;
        a.plan = method_plan;
        b.plan = method_plan;
        a.binding = &native;
        b.binding = &native;
        c.config = client_config;
        c.plan = method_plan;
        c.binding = &native;
        a.id = UINT64_C(211);
        b.id = UINT64_C(212);
        c.id = UINT64_C(213);
        a.request = (AddRequest){.left = 3u, .right = 4u,
                                 .scale = 2u, .presence = 1u};
        b.request = (AddRequest){.left = 2u, .right = 5u,
                                 .scale = 3u, .presence = 1u};
        c.request = (AddRequest){.left = 5u, .right = 2u,
                                 .scale = 1u, .presence = 1u};
        check_greater(
            snprintf(a.uri, sizeof(a.uri), "%s", attached_uri), 0);
        check_greater(
            snprintf(b.uri, sizeof(b.uri), "%s", attached_uri), 0);
        check_greater(
            snprintf(c.uri, sizeof(c.uri), "%s", attached_uri), 0);
        atomic_init(&a.completed, 0);
        atomic_init(&b.completed, 0);
        atomic_init(&c.completed, 0);
        atomic_store_explicit(&race.entered, 0, memory_order_release);
        atomic_store_explicit(&race.release, 0, memory_order_release);
        atomic_store_explicit(&race.enabled, 1, memory_order_release);
        check_equal(cmeta_thread_create(
            &t1, chttp_rpc_service_test_call_worker, &a), SALTS_OK);
        check_equal(cmeta_thread_create(
            &t2, chttp_rpc_service_test_call_worker, &b), SALTS_OK);
        deadline = cmeta_monotonic_ms() + 3000u;
        while (atomic_load_explicit(&race.entered,
                                    memory_order_acquire) < 2 &&
               cmeta_monotonic_ms() < deadline)
          cmeta_thread_yield();
        ready = atomic_load_explicit(&race.entered,
                                     memory_order_acquire);
        /* With two slots already leased, a third actual Owner must return
         * bounded FULL without executing the business adapter. */
        check_equal(cmeta_thread_create(
            &t3, chttp_rpc_service_test_call_worker, &c), SALTS_OK);
        deadline = cmeta_monotonic_ms() + 2500u;
        while (!atomic_load_explicit(&c.completed, memory_order_acquire) &&
               cmeta_monotonic_ms() < deadline)
          cmeta_thread_yield();
        rejected_before_release =
            atomic_load_explicit(&c.completed, memory_order_acquire);
        atomic_store_explicit(&race.release, 1, memory_order_release);
        check_equal(cmeta_thread_join(&t1), SALTS_OK);
        check_equal(cmeta_thread_join(&t2), SALTS_OK);
        check_equal(cmeta_thread_join(&t3), SALTS_OK);
        cmeta_thread_destroy(&t1);
        cmeta_thread_destroy(&t2);
        cmeta_thread_destroy(&t3);
        check_equal(ready, 2);
        check_equal(rejected_before_release, 1);
        check_equal(c.status, SALTS_OK);
        check_equal(c.outcome_kind, CHTTP_CLIENT_RPC_REMOTE_ERROR);
        check_equal(c.remote_code, INT64_C(-32603));
        check_equal(atomic_load_explicit(&race.entered,
                                        memory_order_acquire), 2);
        check_equal(a.status, SALTS_OK);
        check_equal(b.status, SALTS_OK);
        check_equal(a.outcome_kind, CHTTP_CLIENT_RPC_SUCCESS);
        check_equal(b.outcome_kind, CHTTP_CLIENT_RPC_SUCCESS);
        check_equal(a.response.sum, (uint32_t)11u);
        check_equal(b.response.sum, (uint32_t)17u);
      }

      /* #243 stop-timeout gate: old generated descriptor graph and leased
       * native workspace cannot be released while an Owner callback blocks.
       * The successful second stop is the sole lifetime release boundary. */
      {
        cmeta_thread_t blocked = {0};
        chttp_rpc_service_test_call_thread pending = {0};
        uint64_t deadline;
        int admitted;
        pending.config = client_config;
        pending.plan = method_plan;
        pending.binding = &native;
        pending.id = UINT64_C(314);
        pending.request = (AddRequest){.left = 3u, .right = 4u,
                                       .scale = 1u, .presence = 1u};
        check_greater(
            snprintf(pending.uri, sizeof(pending.uri), "%s", attached_uri), 0);
        atomic_init(&pending.completed, 0);
        atomic_store_explicit(&race.entered, 0, memory_order_release);
        atomic_store_explicit(&race.release, 0, memory_order_release);
        atomic_store_explicit(&race.enabled, 1, memory_order_release);
        check_equal(cmeta_thread_create(
            &blocked, chttp_rpc_service_test_call_worker, &pending), SALTS_OK);
        deadline = cmeta_monotonic_ms() + 2500u;
        while (atomic_load_explicit(&race.entered, memory_order_acquire) == 0 &&
               cmeta_monotonic_ms() < deadline)
          cmeta_thread_yield();
        admitted = atomic_load_explicit(&race.entered, memory_order_acquire);
        if (admitted != 0) {
          check_equal(chttp_server_stop(&host, 10u), SALTS_ETIMEDOUT);
          check_equal(chttp_server_destroy(&host), SALTS_EBUSY);
        }
        atomic_store_explicit(&race.release, 1, memory_order_release);
        check_equal(cmeta_thread_join(&blocked), SALTS_OK);
        cmeta_thread_destroy(&blocked);
        check_equal(admitted, 1);
        check_equal(atomic_load_explicit(&pending.completed,
                                        memory_order_acquire), 1);
      }
      check_equal(chttp_server_stop(&host,
                                    CHTTP_RPC_SERVICE_TEST_TIMEOUT_MS),
                  SALTS_OK);
      check_equal(chttp_server_destroy(&host), SALTS_OK);
    }

    data_bind_rpc_method_plan_free(unborrowed_request_plan);
    data_bind_rpc_method_plan_free(unknown_result_plan);
    data_bind_rpc_method_plan_free(method_plan);
    data_bind_free(contract);
  }
}
