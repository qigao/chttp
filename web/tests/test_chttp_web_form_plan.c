#include <chttp_web/web.h>

#include <data_bind_method_plan.h>
#include <salts_cmeta_fixed_width.h>

#include "chttp_web_form_plan.http.h"
#include "tinytest.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct WebFormPlanRequest {
  uint32_t left;
  uint32_t right;
  uint32_t scale;
  uint8_t presence;
} WebFormPlanRequest;

typedef struct WebFormPlanResponse {
  uint32_t sum;
} WebFormPlanResponse;

static const cmeta_type_identity WEB_FORM_PLAN_REQUEST_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.web.FormRequest");
static const cmeta_type_identity WEB_FORM_PLAN_RESPONSE_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.web.FormResponse");

static const cmeta_type_desc WEB_FORM_PLAN_REQUEST_TYPE = {
    "WebFormPlanRequest",
    sizeof(WebFormPlanRequest),
    _Alignof(WebFormPlanRequest),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_FORM_PLAN_REQUEST_ID};

static const cmeta_type_desc WEB_FORM_PLAN_RESPONSE_TYPE = {
    "WebFormPlanResponse",
    sizeof(WebFormPlanResponse),
    _Alignof(WebFormPlanResponse),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_FORM_PLAN_RESPONSE_ID};

static const cmeta_type_desc WEB_FORM_PLAN_REQUEST_PTR_TYPE = {
    "const WebFormPlanRequest *",
    sizeof(WebFormPlanRequest *),
    _Alignof(WebFormPlanRequest *),
    CMETA_T_POINTER,
    &WEB_FORM_PLAN_REQUEST_TYPE,
    NULL,
    NULL};

static const cmeta_type_desc WEB_FORM_PLAN_RESPONSE_PTR_TYPE = {
    "WebFormPlanResponse *",
    sizeof(WebFormPlanResponse *),
    _Alignof(WebFormPlanResponse *),
    CMETA_T_POINTER,
    &WEB_FORM_PLAN_RESPONSE_TYPE,
    NULL,
    NULL};

static const cmeta_field_desc WEB_FORM_PLAN_REQUEST_LAYOUT_FIELDS[] = {
    {"left", "uint32_t", offsetof(WebFormPlanRequest, left),
     sizeof(uint32_t), _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"right", "uint32_t", offsetof(WebFormPlanRequest, right),
     sizeof(uint32_t), _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"scale", "uint32_t", offsetof(WebFormPlanRequest, scale),
     sizeof(uint32_t), _Alignof(uint32_t), &cmeta_type_uint32, NULL}};

static const cmeta_struct_desc WEB_FORM_PLAN_REQUEST_LAYOUT = {
    "WebFormPlanRequest",
    sizeof(WebFormPlanRequest),
    _Alignof(WebFormPlanRequest),
    WEB_FORM_PLAN_REQUEST_LAYOUT_FIELDS,
    3u};

static const cmeta_data_field_desc WEB_FORM_PLAN_REQUEST_FIELDS[] = {
    {"test.web.FormRequest.left", "left",
     offsetof(WebFormPlanRequest, left), &cmeta_data_uint32},
    {"test.web.FormRequest.right", "right",
     offsetof(WebFormPlanRequest, right), &cmeta_data_uint32},
    {"test.web.FormRequest.scale", "scale",
     offsetof(WebFormPlanRequest, scale), &cmeta_data_uint32}};

static const cmeta_data_struct_shape WEB_FORM_PLAN_REQUEST_SHAPE = {
    &WEB_FORM_PLAN_REQUEST_LAYOUT,
    WEB_FORM_PLAN_REQUEST_FIELDS,
    3u};

static const cmeta_data_desc WEB_FORM_PLAN_REQUEST_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.web.FormRequest.data",
    .display_name = "FormRequest",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &WEB_FORM_PLAN_REQUEST_TYPE,
    .shape = &WEB_FORM_PLAN_REQUEST_SHAPE};

static const cmeta_field_desc WEB_FORM_PLAN_RESPONSE_LAYOUT_FIELDS[] = {
    {"sum", "uint32_t", offsetof(WebFormPlanResponse, sum),
     sizeof(uint32_t), _Alignof(uint32_t), &cmeta_type_uint32, NULL}};

static const cmeta_struct_desc WEB_FORM_PLAN_RESPONSE_LAYOUT = {
    "WebFormPlanResponse",
    sizeof(WebFormPlanResponse),
    _Alignof(WebFormPlanResponse),
    WEB_FORM_PLAN_RESPONSE_LAYOUT_FIELDS,
    1u};

static const cmeta_data_field_desc WEB_FORM_PLAN_RESPONSE_FIELDS[] = {
    {"test.web.FormResponse.sum", "sum",
     offsetof(WebFormPlanResponse, sum), &cmeta_data_uint32}};

static const cmeta_data_struct_shape WEB_FORM_PLAN_RESPONSE_SHAPE = {
    &WEB_FORM_PLAN_RESPONSE_LAYOUT,
    WEB_FORM_PLAN_RESPONSE_FIELDS,
    1u};

static const cmeta_data_desc WEB_FORM_PLAN_RESPONSE_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.web.FormResponse.data",
    .display_name = "FormResponse",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &WEB_FORM_PLAN_RESPONSE_TYPE,
    .shape = &WEB_FORM_PLAN_RESPONSE_SHAPE};

static const DataBindNativeStateBinding WEB_FORM_PLAN_REQUEST_PRESENCE[] = {
    {sizeof(DataBindNativeStateBinding), "scale",
     offsetof(WebFormPlanRequest, presence), 0u}};

static const DataBindNativeTypeBinding WEB_FORM_PLAN_REQUEST_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "FormRequest",
    .data = &WEB_FORM_PLAN_REQUEST_DATA,
    .presence = WEB_FORM_PLAN_REQUEST_PRESENCE,
    .presence_count = 1u};

static const DataBindNativeTypeBinding WEB_FORM_PLAN_RESPONSE_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "FormResponse",
    .data = &WEB_FORM_PLAN_RESPONSE_DATA};

FunctionDeclAsAbi(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    chttp_web_form_plan_test_operation,
    (const WebFormPlanRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &WEB_FORM_PLAN_REQUEST_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (WebFormPlanResponse *, response,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &WEB_FORM_PLAN_RESPONSE_PTR_TYPE, CMETA_ABI_OBJECT_POINTER));

int chttp_web_form_plan_test_operation(
    const WebFormPlanRequest *request, WebFormPlanResponse *response) {
  if (request == NULL || response == NULL) return -1;
  response->sum = request->left + request->right * request->scale;
  return 0;
}

static chttp_web_status web_form_plan_parse(
    const char *body,
    chttp_web_form_pair *pairs,
    size_t pair_capacity,
    char *bytes,
    size_t byte_capacity,
    chttp_web_form *form,
    chttp_web_error *error) {
  chttp_web_form_parse_options options =
      (chttp_web_form_parse_options)CHTTP_WEB_FORM_PARSE_OPTIONS_INIT;
  options.max_input_bytes = 256u;
  options.max_pairs = pair_capacity;
  options.max_decoded_bytes = byte_capacity;
  options.pair_storage = pairs;
  options.pair_capacity = pair_capacity;
  options.byte_storage = bytes;
  options.byte_capacity = byte_capacity;
  return chttp_web_form_parse(
      body, strlen(body), &options, form, error);
}

static void web_form_plan_frame(
    WebFormPlanRequest *request,
    WebFormPlanResponse *response,
    DataBindBindingCallFrame *frame,
    void **params,
    size_t *param_bytes) {
  params[0] = request;
  params[1] = response;
  param_bytes[0] = sizeof(*request);
  param_bytes[1] = sizeof(*response);
  *frame = (DataBindBindingCallFrame)DATA_BIND_BINDING_CALL_FRAME_INIT;
  frame->request = request;
  frame->request_bytes = sizeof(*request);
  frame->params = params;
  frame->param_bytes = param_bytes;
  frame->param_count = 2u;
}

spec("CHttp::Web generated MethodPlan form binding") {
  it("uses generated wire aliases defaults and canonical validation") {
    static const char schema[] =
        "message FormRequest {"
        " @Min(1) uint32 left;"
        " uint32 right;"
        " optional uint32 scale default 2;"
        "}"
        "message FormResponse { uint32 sum; }"
        "service Calc { Submit: FormRequest -> FormResponse; }";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chttp_web_form_plan_test_operation),
            &WEB_FORM_PLAN_REQUEST_NATIVE,
            &WEB_FORM_PLAN_RESPONSE_NATIVE);
    const DataBindHttpProjectionConfig *projection =
        data_bind_http_projection_artifact_find(
            &databind_chttp_web_form_plan_http_projection,
            "Calc", "Submit");
    DataBindHttpMethodPlan *method_plan = NULL;
    unsigned char workspace[4096];
    DataBindNativeOptions native_options =
        (DataBindNativeOptions)DATA_BIND_NATIVE_OPTIONS_INIT;
    char scalar[64];
    chttp_web_form_plan_options plan_options =
        (chttp_web_form_plan_options)CHTTP_WEB_FORM_PLAN_OPTIONS_INIT;
    chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
    chttp_web_validation_error validation_errors[4];
    char validation_bytes[512];
    chttp_web_form_pair pairs[4];
    char form_bytes[128];
    chttp_web_form form = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    WebFormPlanRequest request = {0};
    WebFormPlanResponse response = {0};
    void *params[2];
    size_t param_bytes[2];
    DataBindBindingCallFrame frame =
        (DataBindBindingCallFrame)DATA_BIND_BINDING_CALL_FRAME_INIT;

    check_not_null(projection);
    check_equal(projection->method, "POST");
    check_equal(projection->route, "/calc");

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_not_null(contract);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Submit", projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);

    native_options.workspace = workspace;
    native_options.workspace_bytes = sizeof(workspace);
    native_options.max_depth = 16u;
    native_options.max_items = 64u;
    native_options.max_owned_bytes = 1024u;
    plan_options.scalar_storage = scalar;
    plan_options.scalar_capacity = sizeof(scalar);

    check_equal(
        chttp_web_validation_init(
            &validation, validation_errors, 4u,
            validation_bytes, sizeof(validation_bytes), &error),
        CHTTP_WEB_OK);

    web_form_plan_frame(
        &request, &response, &frame, params, param_bytes);
    check_equal(
        web_form_plan_parse(
            "lhs=3&rhs=4", pairs, 4u,
            form_bytes, sizeof(form_bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_method_plan(
            &form, method_plan, &native_options, &frame,
            &plan_options, &validation, &diagnostic, &error),
        CHTTP_WEB_OK);
    check_equal(request.left, 3u);
    check_equal(request.right, 4u);
    check_equal(request.scale, 2u);
    check_true(validation.valid);

    request = (WebFormPlanRequest){0};
    response = (WebFormPlanResponse){0};
    web_form_plan_frame(
        &request, &response, &frame, params, param_bytes);
    check_equal(
        web_form_plan_parse(
            "lhs=3&rhs=4&factor=5", pairs, 4u,
            form_bytes, sizeof(form_bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_method_plan(
            &form, method_plan, &native_options, &frame,
            &plan_options, &validation, &diagnostic, &error),
        CHTTP_WEB_OK);
    check_equal(request.scale, 5u);

    request = (WebFormPlanRequest){0};
    response = (WebFormPlanResponse){0};
    web_form_plan_frame(
        &request, &response, &frame, params, param_bytes);
    check_equal(
        web_form_plan_parse(
            "lhs=0&rhs=4", pairs, 4u,
            form_bytes, sizeof(form_bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_method_plan(
            &form, method_plan, &native_options, &frame,
            &plan_options, &validation, &diagnostic, &error),
        CHTTP_WEB_BIND);
    check_false(validation.valid);
    check_equal(
        chttp_web_validation_field_count(&validation, "lhs"),
        (size_t)1u);

    request = (WebFormPlanRequest){0};
    response = (WebFormPlanResponse){0};
    web_form_plan_frame(
        &request, &response, &frame, params, param_bytes);
    check_equal(
        web_form_plan_parse(
            "lhs=3", pairs, 4u,
            form_bytes, sizeof(form_bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_method_plan(
            &form, method_plan, &native_options, &frame,
            &plan_options, &validation, &diagnostic, &error),
        CHTTP_WEB_BIND);
    check_false(validation.valid);
    check_equal(
        chttp_web_validation_field_count(&validation, "rhs"),
        (size_t)1u);

    request = (WebFormPlanRequest){0};
    response = (WebFormPlanResponse){0};
    web_form_plan_frame(
        &request, &response, &frame, params, param_bytes);
    check_equal(
        web_form_plan_parse(
            "left=3&rhs=4", pairs, 4u,
            form_bytes, sizeof(form_bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_method_plan(
            &form, method_plan, &native_options, &frame,
            &plan_options, &validation, &diagnostic, &error),
        CHTTP_WEB_BIND);
    check_equal(
        chttp_web_validation_global_count(&validation),
        (size_t)1u);

    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }
}
