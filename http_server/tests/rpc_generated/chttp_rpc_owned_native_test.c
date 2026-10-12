/* #243: genuine owned native field teardown through Server-scoped DataBind.
 * No test-only RPC runtime: HTTP Server owns the bounded call slot and
 * DataBind restores the entire request after partial ingress or completion. */
#include <http_client/http.h>
#include <http_server/rpc_service.h>
#include <cmeta_cmeta_fixed_width.h>
#include "chttp_rpc_service.rpc.h"
#include "tinytest.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { OWNED_RPC_TIMEOUT_MS = 5000, OWNED_RPC_MAX_BYTES = 256 };
typedef struct OwnedText {
  unsigned char *data;
  size_t size;
} OwnedText;
typedef struct OwnedRequest {
  uint32_t left;
  uint32_t right;
  OwnedText note;
  OwnedText tag;
} OwnedRequest;
typedef struct OwnedResponse {
  uint32_t sum;
} OwnedResponse;

static atomic_int owned_allocations;
static atomic_int owned_releases;
static atomic_int owned_invocations;

static bool owned_is_zero(const void *object) {
  const OwnedText *v = (const OwnedText *)object;
  return v != NULL && v->data == NULL && v->size == 0u;
}
static cmeta_status owned_assign(void *object, const unsigned char *data,
                                 size_t size, size_t max_bytes) {
  OwnedText *v = (OwnedText *)object;
  unsigned char *fresh;
  if (v == NULL || (size != 0u && data == NULL)) return CMETA_INVALID_ARGUMENT;
  if (size > max_bytes || size > OWNED_RPC_MAX_BYTES)
    return CMETA_CAPACITY_EXCEEDED;
  fresh = (unsigned char *)malloc(size + 1u);
  if (fresh == NULL) return CMETA_OUT_OF_MEMORY;
  if (size != 0u) memcpy(fresh, data, size);
  fresh[size] = 0u;
  if (v->data != NULL) {
    free(v->data);
    atomic_fetch_add_explicit(&owned_releases, 1, memory_order_acq_rel);
  }
  v->data = fresh;
  v->size = size;
  atomic_fetch_add_explicit(&owned_allocations, 1, memory_order_acq_rel);
  return CMETA_OK;
}
static void owned_restore_zero(void *object) {
  OwnedText *v = (OwnedText *)object;
  if (v == NULL) return;
  if (v->data != NULL) {
    free(v->data);
    atomic_fetch_add_explicit(&owned_releases, 1, memory_order_acq_rel);
  }
  v->data = NULL;
  v->size = 0u;
}
static cmeta_status owned_read(const void *object,
                               const unsigned char **out_data,
                               size_t *out_size) {
  const OwnedText *v = (const OwnedText *)object;
  if (v == NULL || out_data == NULL || out_size == NULL)
    return CMETA_INVALID_ARGUMENT;
  *out_data = v->data;
  *out_size = v->size;
  return CMETA_OK;
}
static cmeta_status owned_init_zero(void *object) {
  OwnedText *v = (OwnedText *)object;
  if (v == NULL) return CMETA_INVALID_ARGUMENT;
  v->data = NULL;
  v->size = 0u;
  return CMETA_OK;
}
static void owned_move(void *destination, void *source) {
  OwnedText *to = (OwnedText *)destination;
  OwnedText *from = (OwnedText *)source;
  if (to == NULL || from == NULL || to == from) return;
  *to = *from;
  from->data = NULL;
  from->size = 0u;
}

static const cmeta_type_identity OWNED_TEXT_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.rpc.OwnedText");
static const cmeta_type_identity OWNED_REQ_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.rpc.OwnedRequest");
static const cmeta_type_identity OWNED_RES_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.rpc.OwnedResponse");
static const cmeta_type_desc OWNED_TEXT_TYPE = {
    "OwnedText", sizeof(OwnedText), _Alignof(OwnedText),
    CMETA_T_OBJECT, NULL, NULL, &OWNED_TEXT_ID};
static const cmeta_data_buffer_shape OWNED_TEXT_SHAPE = {
    CMETA_DATA_BUFFER_OWNED};
static const cmeta_data_buffer_ops OWNED_TEXT_OPS = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &OWNED_TEXT_TYPE, CMETA_DATA_BUFFER_OWNED, owned_is_zero, owned_assign,
    owned_restore_zero, owned_read, owned_init_zero, owned_move};
static const cmeta_data_desc OWNED_TEXT_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.rpc.OwnedText.data",
    .display_name = "OwnedText", .kind = CMETA_DATA_STRING,
    .storage_type = &OWNED_TEXT_TYPE, .shape = &OWNED_TEXT_SHAPE,
    .buffer_ops = &OWNED_TEXT_OPS};

static const cmeta_type_desc OWNED_REQ_TYPE = {
    "OwnedRequest", sizeof(OwnedRequest), _Alignof(OwnedRequest),
    CMETA_T_OBJECT, NULL, NULL, &OWNED_REQ_ID};
static const cmeta_type_desc OWNED_RES_TYPE = {
    "OwnedResponse", sizeof(OwnedResponse), _Alignof(OwnedResponse),
    CMETA_T_OBJECT, NULL, NULL, &OWNED_RES_ID};
static const cmeta_type_desc OWNED_REQ_PTR = {
    "const OwnedRequest *", sizeof(OwnedRequest *), _Alignof(OwnedRequest *),
    CMETA_T_POINTER, &OWNED_REQ_TYPE, NULL, NULL};
static const cmeta_type_desc OWNED_RES_PTR = {
    "OwnedResponse *", sizeof(OwnedResponse *), _Alignof(OwnedResponse *),
    CMETA_T_POINTER, &OWNED_RES_TYPE, NULL, NULL};
static const cmeta_field_desc OWNED_REQ_FIELDS[] = {
    {"left", "uint32_t", offsetof(OwnedRequest, left), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"right", "uint32_t", offsetof(OwnedRequest, right), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"note", "OwnedText", offsetof(OwnedRequest, note), sizeof(OwnedText),
     _Alignof(OwnedText), &OWNED_TEXT_TYPE, NULL},
    {"tag", "OwnedText", offsetof(OwnedRequest, tag), sizeof(OwnedText),
     _Alignof(OwnedText), &OWNED_TEXT_TYPE, NULL}};
static const cmeta_struct_desc OWNED_REQ_LAYOUT = {
    "OwnedRequest", sizeof(OwnedRequest), _Alignof(OwnedRequest),
    OWNED_REQ_FIELDS, 4u};
static const cmeta_data_field_desc OWNED_REQ_DATA_FIELDS[] = {
    {"test.chttp.rpc.OwnedRequest.left", "left", offsetof(OwnedRequest, left),
     &cmeta_data_uint32},
    {"test.chttp.rpc.OwnedRequest.right", "right", offsetof(OwnedRequest, right),
     &cmeta_data_uint32},
    {"test.chttp.rpc.OwnedRequest.note", "note", offsetof(OwnedRequest, note),
     &OWNED_TEXT_DATA},
    {"test.chttp.rpc.OwnedRequest.tag", "tag", offsetof(OwnedRequest, tag),
     &OWNED_TEXT_DATA}};
static const cmeta_data_struct_shape OWNED_REQ_SHAPE = {
    &OWNED_REQ_LAYOUT, OWNED_REQ_DATA_FIELDS, 4u};
static const cmeta_data_desc OWNED_REQ_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.rpc.OwnedRequest.data",
    .display_name = "OwnedRequest", .kind = CMETA_DATA_STRUCT,
    .storage_type = &OWNED_REQ_TYPE, .shape = &OWNED_REQ_SHAPE};
static const cmeta_field_desc OWNED_RES_FIELDS[] = {
    {"sum", "uint32_t", offsetof(OwnedResponse, sum), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL}};
static const cmeta_struct_desc OWNED_RES_LAYOUT = {
    "OwnedResponse", sizeof(OwnedResponse), _Alignof(OwnedResponse),
    OWNED_RES_FIELDS, 1u};
static const cmeta_data_field_desc OWNED_RES_DATA_FIELDS[] = {
    {"test.chttp.rpc.OwnedResponse.sum", "sum", offsetof(OwnedResponse, sum),
     &cmeta_data_uint32}};
static const cmeta_data_struct_shape OWNED_RES_SHAPE = {
    &OWNED_RES_LAYOUT, OWNED_RES_DATA_FIELDS, 1u};
static const cmeta_data_desc OWNED_RES_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.rpc.OwnedResponse.data",
    .display_name = "OwnedResponse", .kind = CMETA_DATA_STRUCT,
    .storage_type = &OWNED_RES_TYPE, .shape = &OWNED_RES_SHAPE};
static const DataBindNativeTypeBinding OWNED_REQ_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "OwnedRequest", .data = &OWNED_REQ_DATA};
static const DataBindNativeTypeBinding OWNED_RES_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "OwnedResponse", .data = &OWNED_RES_DATA};

FunctionDeclAsAbiResult(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE,
    chhttp_rpc_owned_note,
    (const OwnedRequest *, request, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &OWNED_REQ_PTR, CMETA_ABI_OBJECT_POINTER),
    (OwnedResponse *, response, CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &OWNED_RES_PTR, CMETA_ABI_OBJECT_POINTER));

int chhttp_rpc_owned_note(
    const OwnedRequest *request, OwnedResponse *response) {
  if (request == NULL || response == NULL) return -1;
  if (request->note.data == NULL || request->tag.data == NULL ||
      request->note.size + request->tag.size > OWNED_RPC_MAX_BYTES)
    return -1;
  atomic_fetch_add_explicit(&owned_invocations, 1, memory_order_acq_rel);
  if (request->left == 99u) return 7;
  response->sum = request->left + request->right +
                  (uint32_t)request->note.size +
                  (uint32_t)request->tag.size;
  return 0;
}
static bool DATA_BIND_NATIVE_CALL owned_note_invoke(
    void *ctx, void *return_storage, void *const *params, size_t count) {
  int result;
  (void)ctx;
  if (return_storage == NULL || params == NULL || count != 2u ||
      params[0] == NULL || params[1] == NULL) return false;
  result = chhttp_rpc_owned_note(
      (const OwnedRequest *)params[0], (OwnedResponse *)params[1]);
  *(int *)return_storage = result;
  return true;
}
static native_io_backend_kind owned_rpc_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}
static cnet_client_config owned_rpc_network(void) {
  const cnet_client_config config = {
      .backend = owned_rpc_backend(), .connection_capacity = 4u,
      .command_capacity = 32u, .request_capacity = 8u,
      .completion_batch_capacity = 8u, .event_capacity = 32u,
      .max_send_bytes = 65536u, .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = OWNED_RPC_TIMEOUT_MS,
      .read_timeout_ms = OWNED_RPC_TIMEOUT_MS,
      .write_timeout_ms = OWNED_RPC_TIMEOUT_MS};
  return config;
}
static chttp_server_config owned_rpc_server_config(void) {
  chttp_server_config cfg = {0};
  cfg.host = "127.0.0.1";
  cfg.network = owned_rpc_network();
  cfg.backlog = 8u;
  cfg.route_capacity = 2u;
  cfg.middleware_capacity = 2u;
  cfg.max_route_middleware_count = 2u;
  cfg.max_route_param_count = 2u;
  cfg.max_route_param_bytes = 128u;
  cfg.max_target_bytes = 256u;
  cfg.max_header_count = 16u;
  cfg.max_header_bytes = 4096u;
  cfg.max_request_body_bytes = 8192u;
  cfg.max_response_header_count = 16u;
  cfg.max_response_header_bytes = 4096u;
  cfg.max_buffered_response_body_bytes = 8192u;
  cfg.max_response_body_bytes = 8192u;
  cfg.poll_slice_ms = 2u;
  return cfg;
}
static chttp_client_config owned_rpc_client_config(void) {
  chttp_client_config cfg = {0};
  cfg.network = owned_rpc_network();
  cfg.request_capacity = 4u;
  cfg.max_start_line_bytes = 256u;
  cfg.max_header_count = 16u;
  cfg.max_header_bytes = 4096u;
  cfg.max_request_body_bytes = 8192u;
  cfg.max_response_body_bytes = 8192u;
  cfg.max_informational_responses = 2u;
  return cfg;
}
static int owned_rpc_post(chttp_client *client, const char *uri,
                          const char *payload, chttp_response *out) {
  static const chttp_header headers[] = {
      {"Content-Type", "application/json"},
      {"Accept", "application/json"}};
  const chttp_options options = {
      .connection_uri = uri, .authority = "localhost", .target = "/rpc",
      .headers = headers, .header_count = 2u, .body = payload,
      .body_size = strlen(payload), .timeout_ms = OWNED_RPC_TIMEOUT_MS};
  chttp_error err = {0};
  return chttp_post(client, &options, out, &err);
}
static int owned_rpc_response_contains(
    const chttp_response *response, const char *text) {
  const unsigned char *data = (const unsigned char *)response->body;
  const size_t size = strlen(text);
  size_t i;
  if (data == NULL || size > response->body_size) return 0;
  for (i = 0u; i <= response->body_size - size; ++i)
    if (memcmp(data + i, text, size) == 0) return 1;
  return 0;
}

spec("CHttp generated RPC canonical owned ingress") {
  it("restores heap owners once on partial failure and successful Server drain") {
    static const char schema[] =
        "message OwnedRequest { uint32 left; uint32 right; string note; string tag; }"
        "message OwnedResponse { uint32 sum; }"
        "service Calc { Note: OwnedRequest -> OwnedResponse; }";
    static const char invalid_fourth[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"calc.note\","
        "\"params\":[3,4,\"owned\",777],\"id\":701}";
    static const char duplicate_last[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"calc.note\","
        "\"params\":{\"left\":3,\"right\":4,\"note\":\"owned\","
        "\"tag\":\"a\",\"tag\":\"b\"},\"id\":702}";
    static const char valid[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"calc.note\","
        "\"params\":[3,4,\"hello\",\"!\"],\"id\":703}";
    static const char native_error[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"calc.note\","
        "\"params\":[99,4,\"boom\",\"!\"],\"id\":704}";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding binding =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chhttp_rpc_owned_note),
            &OWNED_REQ_NATIVE, &OWNED_RES_NATIVE);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    const DataBindRpcProjectionConfig *projection;
    DataBindRpcMethodPlan *plan = NULL;
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = owned_rpc_server_config();
    chttp_client_config client_config = owned_rpc_client_config();
    chttp_server_rpc_config rpc_config = CHTTP_SERVER_RPC_CONFIG_INIT;
    chttp_server_rpc_mount_options mount =
        CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;
    chttp_response response = {0};
    uint16_t port = 0u;
    char uri[64];

    atomic_store_explicit(&owned_allocations, 0, memory_order_release);
    atomic_store_explicit(&owned_releases, 0, memory_order_release);
    atomic_store_explicit(&owned_invocations, 0, memory_order_release);
    execution.function = FunctionMeta(chhttp_rpc_owned_note);
    execution.abi = FunctionAbi(chhttp_rpc_owned_note);
    execution.invoke = owned_note_invoke;
    projection = data_bind_rpc_projection_artifact_find(
        &databind_chttp_rpc_service_rpc_projection, "Calc", "Note");
    check_not_null(projection);
    check_equal(data_bind_create_from_text(
        schema, sizeof(schema) - 1u, &contract, &bind_error), DATA_BIND_OK);
    check_equal(data_bind_rpc_method_plan_compile_service(
        contract, "Calc", "Note", projection, &binding, &plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(plan);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    rpc_config.method_capacity = 1u;
    rpc_config.max_inflight_calls = 1u;
    rpc_config.native_max_owned_bytes = OWNED_RPC_MAX_BYTES;
    rpc_config.native_max_items = 64u;
    check_equal(chttp_server_rpc_configure(&server, &rpc_config), SALTS_OK);
    mount.target = "/rpc";
    mount.method_plan = plan;
    mount.native_binding = &binding;
    mount.execution = &execution;
    check_equal(chttp_server_rpc_mount(&server, &mount), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                           (unsigned int)port), 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(owned_rpc_post(
        &client, uri, invalid_fourth, &response), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_true(owned_rpc_response_contains(&response, "\"code\":-32602"));
    chttp_response_destroy(&response);

    check_equal(owned_rpc_post(
        &client, uri, duplicate_last, &response), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_true(owned_rpc_response_contains(&response, "\"code\":-32602"));
    chttp_response_destroy(&response);

    check_equal(owned_rpc_post(&client, uri, valid, &response), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_true(owned_rpc_response_contains(&response, "\"result\":13"));
    chttp_response_destroy(&response);

    check_equal(owned_rpc_post(
        &client, uri, native_error, &response), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_true(owned_rpc_response_contains(&response, "\"code\":-32000"));
    chttp_response_destroy(&response);

    /* Wire completion doesn't join Server worker callbacks. Consume all
     * ownership counters only after the canonical Owner terminal barrier. */
    check_equal(chttp_client_destroy(
        &client, OWNED_RPC_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(
        &server, OWNED_RPC_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load_explicit(
        &owned_invocations, memory_order_acquire), 2);
    check_equal(atomic_load_explicit(
        &owned_allocations, memory_order_acquire), 6);
    check_equal(atomic_load_explicit(
        &owned_releases, memory_order_acquire), 6);
    data_bind_rpc_method_plan_free(plan);
    data_bind_free(contract);
  }
}
