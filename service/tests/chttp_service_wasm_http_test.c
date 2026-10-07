#include <chttp_service/service.h>

#include <http_client/http.h>
#include "chttp_service_wasm.http.h"
#include "chttp_service_wasm.wasm.h"
#include "chttp_service_wasm_native.h"
#include "chttp_service_wasm_path.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/thread.h>

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { CHTTP_SERVICE_WASM_TEST_TIMEOUT_MS = 5000 };

static native_io_backend_kind chttp_service_wasm_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config chttp_service_wasm_network(size_t connections) {
  const cnet_client_config config = {
      .backend = chttp_service_wasm_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 4096u,
      .receive_buffer_bytes = 256u,
      .connect_timeout_ms = CHTTP_SERVICE_WASM_TEST_TIMEOUT_MS,
      .read_timeout_ms = CHTTP_SERVICE_WASM_TEST_TIMEOUT_MS,
      .write_timeout_ms = CHTTP_SERVICE_WASM_TEST_TIMEOUT_MS};
  return config;
}

static chttp_server_config chttp_service_wasm_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = chttp_service_wasm_network(4u),
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

static chttp_client_config chttp_service_wasm_client_config(void) {
  const chttp_client_config config = {
      .network = chttp_service_wasm_network(2u),
      .request_capacity = 1u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_body_bytes = 512u,
      .max_informational_responses = 2u};
  return config;
}

static int chttp_service_wasm_call(
    chttp_client *client, const char *uri, const char *target,
    chttp_response *response) {
  const chttp_options options = {
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = target,
      .timeout_ms = CHTTP_SERVICE_WASM_TEST_TIMEOUT_MS};
  chttp_error error = {0};
  return chttp_get(client, &options, response, &error);
}

static int chttp_service_wasm_read_file(
    const char *path, uint8_t **out, size_t *out_size) {
  FILE *file;
  long length;
  uint8_t *bytes;

  if (path == NULL || out == NULL || out_size == NULL) return 0;
  *out = NULL;
  *out_size = 0u;
  file = fopen(path, "rb");
  if (file == NULL) return 0;
  if (fseek(file, 0, SEEK_END) != 0 ||
      (length = ftell(file)) <= 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  bytes = (uint8_t *)malloc((size_t)length);
  if (bytes == NULL) {
    fclose(file);
    return 0;
  }
  if (fread(bytes, 1u, (size_t)length, file) != (size_t)length ||
      fclose(file) != 0) {
    free(bytes);
    return 0;
  }
  *out = bytes;
  *out_size = (size_t)length;
  return 1;
}

typedef struct chttp_service_wasm_executor_gate {
  _Atomic int started;
  _Atomic int release;
} chttp_service_wasm_executor_gate;

static void chttp_service_wasm_executor_gate_run(void *user) {
  chttp_service_wasm_executor_gate *gate =
      (chttp_service_wasm_executor_gate *)user;
  if (gate == NULL) return;
  atomic_store_explicit(&gate->started, 1, memory_order_release);
  while (atomic_load_explicit(&gate->release, memory_order_acquire) == 0)
    cmeta_thread_yield();
}

static void chttp_service_wasm_executor_noop(void *user) {
  (void)user;
}

spec("CHttp::Service generated WASM execution") {
  it("runs the generated capability through existing DEFERRED_DIRECT") {
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindNativeTypeBinding request_native =
        DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindNativeTypeBinding response_native =
        DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    const DataBindHttpProjectionConfig *projection = NULL;
    DataBindHttpMethodPlan *method_plan = NULL;
    uint8_t *component_bytes = NULL;
    size_t component_size = 0u;
    databind_chttp_service_wasm_wasm_host wasm_host = {0};
    cflow_executor executor = {0};
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_service_http_mount invalid = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_wasm_server_config();
    chttp_client client = {0};
    chttp_client_config client_config =
        chttp_service_wasm_client_config();
    chttp_response response = {0};
    chttp_service_wasm_executor_gate gate;
    cflow_executor_task gate_task;
    cflow_executor_task queued_task;
    uint64_t deadline;
    uint16_t port = 0u;
    char uri[64];

    check_equal(
        CHttpWasm_codec_create(&contract, &bind_error),
        DATA_BIND_OK);
    check_not_null(contract);

    check_equal(
        databind_9_CHttpWasm_4_Calc_3_Add__databind_native_binding(
            &request_native, &response_native, &native, &bind_error),
        DATA_BIND_OK);

    projection = data_bind_http_projection_artifact_find(
        &databind_chttp_service_wasm_http_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(projection->route, "/wasm/{left}");

    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);

    check_true(chttp_service_wasm_read_file(
        GENERATED_CHTTP_SERVICE_WASM_PATH,
        &component_bytes, &component_size));
    check_true(
        databind_chttp_service_wasm_wasm_host_init(
            &wasm_host, component_bytes, component_size));
    check_true(
        databind_9_CHttpWasm_4_Calc_3_Add__databind_wasm_execution(
            &wasm_host, &execution));
    check_true(data_bind_native_execution_valid(&execution));

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 2u;
    service_config.max_call_frame_bytes = 512u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;

    check_true(cflow_executor_worker_init_with_capacity(
        &executor, 1u, 1u));
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

    invalid.method_plan = method_plan;
    invalid.native_binding = &native;
    invalid.execution = &execution;
    invalid.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
    check_equal(
        chttp_service_mount_http(&service, &server, &invalid),
        SALTS_EINVAL);

    mount.method_plan = method_plan;
    mount.native_binding = &native;
    mount.execution = &execution;
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
    mount.executor = &executor;
    check_equal(
        chttp_service_mount_http(&service, &server, &mount),
        SALTS_OK);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(
        chttp_service_wasm_call(
            &client, uri, "/wasm/3?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body_size, (size_t)2u);
    check_equal(response.body, "11", 2u);
    chttp_response_destroy(&response);
    check_true(cflow_executor_wait_idle(&executor));

    response = (chttp_response){0};
    check_equal(
        chttp_service_wasm_call(
            &client, uri, "/wasm/0?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 422u);
    check_equal(response.body, "Validation Error", 16u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_wasm_call(
            &client, uri, "/wasm/3?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 400u);
    check_equal(response.body, "Binding Error", 13u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_wasm_call(
            &client, uri, "/wasm/99?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 500u);
    check_equal(response.body, "Application Error", 17u);
    chttp_response_destroy(&response);
    check_true(cflow_executor_wait_idle(&executor));

    atomic_init(&gate.started, 0);
    atomic_init(&gate.release, 0);
    gate_task = (cflow_executor_task){
        .run = chttp_service_wasm_executor_gate_run,
        .user = &gate};
    queued_task = (cflow_executor_task){
        .run = chttp_service_wasm_executor_noop,
        .user = NULL};
    check_equal(
        cflow_executor_try_post_task(&executor, &gate_task),
        CFLOW_ADMISSION_ACCEPTED);
    deadline = cmeta_monotonic_ms() + CHTTP_SERVICE_WASM_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(&gate.started, memory_order_acquire) == 0 &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    check_equal(
        atomic_load_explicit(&gate.started, memory_order_acquire), 1);
    check_equal(
        cflow_executor_try_post_task(&executor, &queued_task),
        CFLOW_ADMISSION_ACCEPTED);

    response = (chttp_response){0};
    check_equal(
        chttp_service_wasm_call(
            &client, uri, "/wasm/3?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 503u);
    check_equal(response.body, "Service Unavailable", 19u);
    chttp_response_destroy(&response);

    atomic_store_explicit(&gate.release, 1, memory_order_release);
    check_true(cflow_executor_wait_idle(&executor));

    check_equal(
        chttp_client_destroy(&client, CHTTP_SERVICE_WASM_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_WASM_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_true(cflow_executor_wait_idle(&executor));
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);

    cflow_executor_destroy(&executor);
    databind_chttp_service_wasm_wasm_host_destroy(&wasm_host);
    free(component_bytes);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }
}
