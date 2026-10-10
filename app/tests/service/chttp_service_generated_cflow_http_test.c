#include <chttp_app/service.h>

#include <http_client/http.h>
#include "chttp_service_cflow.http.h"
#include "chttp_service_cflow.service_native.h"
#include "chttp_service_cflow_native.h"
#include "chttp_service_cflow_unsupported.service_native.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/thread.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CHTTP_SERVICE_GENERATED_CFLOW_TIMEOUT_MS = 5000 };

static native_io_backend_kind generated_cflow_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config generated_cflow_network(size_t connections) {
  const cnet_client_config config = {
      .backend = generated_cflow_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 4096u,
      .receive_buffer_bytes = 256u,
      .connect_timeout_ms = CHTTP_SERVICE_GENERATED_CFLOW_TIMEOUT_MS,
      .read_timeout_ms = CHTTP_SERVICE_GENERATED_CFLOW_TIMEOUT_MS,
      .write_timeout_ms = CHTTP_SERVICE_GENERATED_CFLOW_TIMEOUT_MS};
  return config;
}

static chttp_server_config generated_cflow_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = generated_cflow_network(4u),
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

static chttp_client_config generated_cflow_client_config(void) {
  const chttp_client_config config = {
      .network = generated_cflow_network(2u),
      .request_capacity = 1u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_body_bytes = 512u,
      .max_informational_responses = 2u};
  return config;
}

static int generated_cflow_call(
    chttp_client *client, const char *uri, const char *target,
    chttp_response *response) {
  const chttp_options options = {
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = target,
      .timeout_ms = CHTTP_SERVICE_GENERATED_CFLOW_TIMEOUT_MS};
  chttp_error error = {0};
  return chttp_get(client, &options, response, &error);
}

static void run_generated_mode(
    const DataBindHttpMethodPlan *method_plan,
    const DataBindServiceNativeBinding *native,
    const DataBindNativeExecution *execution,
    const cflow_function_typed_adapter_projection *projection,
    chttp_service_execution_mode mode) {
  cflow_executor executor = {0};
  chttp_service service = {0};
  chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
  chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
  chttp_server server = {0};
  chttp_server_config server_config = generated_cflow_server_config();
  chttp_client client = {0};
  chttp_client_config client_config = generated_cflow_client_config();
  chttp_response response = {0};
  uint16_t port = 0u;
  char uri[64];

  service_config.method_capacity = 1u;
  service_config.max_binding_value_bytes = 64u;
  service_config.max_response_body_bytes = 2u;
  service_config.max_call_frame_bytes = 512u;
  service_config.native_workspace_bytes = 4096u;
  service_config.native_max_depth = 16u;
  service_config.native_max_items = 64u;
  service_config.native_max_owned_bytes = 1024u;

  check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 2u));
  check_equal(chttp_service_init(&service, &service_config), SALTS_OK);
  check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

  mount.method_plan = method_plan;
  mount.native_binding = native;
  mount.execution_mode = mode;
  mount.executor = &executor;
  if (mode == CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT) {
    mount.execution = execution;
  } else {
    mount.execution = NULL;
    mount.cflow_projection = projection;
  }
  check_equal(chttp_service_mount_http(&service, &server, &mount), SALTS_OK);

  check_equal(chttp_server_start(&server), SALTS_OK);
  check_equal(chttp_server_port(&server, &port), SALTS_OK);
  check_greater(
      snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
               (unsigned int)port),
      0);
  check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

  check_equal(
      generated_cflow_call(
          &client, uri, "/generated-cflow/3?right=4", &response),
      SALTS_OK);
  check_equal(response.status_code, 201u);
  check_equal(response.body_size, (size_t)1u);
  check_equal(response.body, "7", 1u);
  chttp_response_destroy(&response);
  check_true(cflow_executor_wait_idle(&executor));

  response = (chttp_response){0};
  check_equal(
      generated_cflow_call(
          &client, uri, "/generated-cflow/0?right=4", &response),
      SALTS_OK);
  check_equal(response.status_code, 422u);
  check_equal(response.body, "Validation Error", 16u);
  chttp_response_destroy(&response);

  response = (chttp_response){0};
  check_equal(
      generated_cflow_call(
          &client, uri, "/generated-cflow/3", &response),
      SALTS_OK);
  check_equal(response.status_code, 400u);
  check_equal(response.body, "Binding Error", 13u);
  chttp_response_destroy(&response);

  check_equal(
      chttp_client_destroy(
          &client, CHTTP_SERVICE_GENERATED_CFLOW_TIMEOUT_MS),
      SALTS_OK);
  check_equal(
      chttp_server_stop(
          &server, CHTTP_SERVICE_GENERATED_CFLOW_TIMEOUT_MS),
      SALTS_OK);
  check_true(cflow_executor_wait_idle(&executor));
  check_equal(chttp_server_destroy(&server), SALTS_OK);
  check_equal(chttp_service_destroy(&service), SALTS_OK);
  cflow_executor_destroy(&executor);
}

spec("CHttp::App generated CFlow projection") {
  it("runs the same generated MethodPlan through direct and CFlow providers") {
    DataBind *contract = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindNativeTypeBinding request_native =
        DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindNativeTypeBinding response_native =
        DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
    const DataBindNativeExecution *execution = NULL;
    cflow_function_typed_adapter_projection cflow_projection = {0};
    const DataBindHttpProjectionConfig *http_projection = NULL;
    DataBindHttpMethodPlan *method_plan = NULL;

    check_equal(CFlowGen_codec_create(&contract, &error), DATA_BIND_OK);
    check_not_null(contract);

    check_equal(
        databind_8_CFlowGen_4_Calc_3_Add__databind_native_binding(
            &request_native, &response_native, &native, &error),
        DATA_BIND_OK);

    execution =
        databind_8_CFlowGen_4_Calc_3_Add__databind_execution();
    check_not_null(execution);
    check_true(data_bind_native_execution_valid(execution));

    check_equal(
        databind_8_CFlowGen_4_Calc_3_Add__databind_cflow_projection(
            &cflow_projection),
        CFLOW_FUNCTION_PROJECTION_OK);
    check_true(
        cflow_function_typed_adapter_projection_valid(&cflow_projection));

    http_projection = data_bind_http_projection_artifact_find(
        &databind_chttp_service_cflow_http_projection, "Calc", "Add");
    check_not_null(http_projection);

    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", http_projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);

    run_generated_mode(
        method_plan, &native, execution, NULL,
        CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT);
    run_generated_mode(
        method_plan, &native, NULL, &cflow_projection,
        CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW);

    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }

  it("rejects generated optional nullable and typed-error CFlow shapes") {
    cflow_function_typed_adapter_projection projection = {0};

    check_equal(
        databind_7_FlowBad_4_Calc_8_Optional__databind_cflow_projection(
            &projection),
        CFLOW_FUNCTION_PROJECTION_UNSUPPORTED_SHAPE);
    check_false(cflow_function_typed_adapter_projection_valid(&projection));

    projection = (cflow_function_typed_adapter_projection){0};
    check_equal(
        databind_7_FlowBad_4_Calc_8_Nullable__databind_cflow_projection(
            &projection),
        CFLOW_FUNCTION_PROJECTION_UNSUPPORTED_SHAPE);
    check_false(cflow_function_typed_adapter_projection_valid(&projection));

    projection = (cflow_function_typed_adapter_projection){0};
    check_equal(
        databind_7_FlowBad_4_Calc_7_Failing__databind_cflow_projection(
            &projection),
        CFLOW_FUNCTION_PROJECTION_UNSUPPORTED_SHAPE);
    check_false(cflow_function_typed_adapter_projection_valid(&projection));
  }
}
