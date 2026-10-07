#include <chttp_web/web.h>

#include <data_bind_method_plan.h>
#include <cmeta_cmeta_fixed_width.h>

#include "chttp_web_form_plan.http.h"
#include "chttp_web_form_plan.service_native.h"
#include "tinytest.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int databind_7_WebForm_4_Calc_6_Submit(
    const FormRequest_t *request, FormResponse_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->sum = request->left + request->right * request->scale;
  return 0;
}

int databind_7_WebForm_4_Calc_6_Values(
    const FormValues_t *request, FormResponse_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->sum = request->count;
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
    FormRequest_t *request,
    FormResponse_t *response,
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
  it("binds generated enum and collections with bounded rollback") {
    static const char schema[] =
        "schema WebForm [version(1)];"
        "enum Side <uint8> { Buy = 1; Sell = 2; }"
        "message FormValues { uint32 count; Side side; list<uint32> tags; }"
        "message FormResponse { uint32 sum; }"
        "service Calc { Values: FormValues -> FormResponse; }";
    static const char *invalid[] = {
        "count=oops&side=Buy&tags=9",
        "count=9&side=Unknown&tags=9",
        "count=9&side=Buy&tags=3&tags=oops",
        "count=9&count=8&side=Buy&tags=9",
        "missing=9&side=Buy&tags=9"};
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindNativeTypeBinding request_binding = {0};
    DataBindNativeTypeBinding response_binding = {0};
    DataBindServiceNativeBinding native = {0};
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindHttpMethodPlan *method_plan = NULL;
    const DataBindHttpProjectionConfig *projection =
        data_bind_http_projection_artifact_find(
            &databind_chttp_web_form_plan_http_projection, "Calc", "Values");
    unsigned char workspace[8192];
    DataBindNativeOptions native_options = DATA_BIND_NATIVE_OPTIONS_INIT;
    char scalar[64];
    chttp_web_form_plan_options options = CHTTP_WEB_FORM_PLAN_OPTIONS_INIT;
    chttp_web_form_pair pairs[4];
    char bytes[128];
    chttp_web_form form = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    FormValues_t request = {0};
    FormResponse_t response = {0};
    void *params[] = {&request, &response};
    size_t param_bytes[] = {sizeof(request), sizeof(response)};
    DataBindBindingCallFrame frame = DATA_BIND_BINDING_CALL_FRAME_INIT;

    check_equal(databind_7_WebForm_4_Calc_6_Values__databind_native_binding(
                    &request_binding, &response_binding, &native, &bind_error),
                DATA_BIND_OK);
    check_equal(data_bind_create_from_text(schema, sizeof(schema) - 1u,
                    &contract, &bind_error), DATA_BIND_OK);
    check_equal(data_bind_http_method_plan_compile_service(contract, "Calc",
                    "Values", projection, &native, &method_plan, &diagnostic),
                DATA_BIND_OK);
    native_options.workspace = workspace;
    native_options.workspace_bytes = sizeof(workspace);
    native_options.max_depth = 16u;
    native_options.max_items = 64u;
    native_options.max_owned_bytes = 1024u;
    options.scalar_storage = scalar;
    options.scalar_capacity = sizeof(scalar);
    frame.request = &request;
    frame.request_bytes = sizeof(request);
    frame.params = params;
    frame.param_bytes = param_bytes;
    frame.param_count = sizeof(params) / sizeof(params[0]);

    check_equal(web_form_plan_parse("tags=3&count=7&side=Sell&tags=5", pairs,
                    4u, bytes, sizeof(bytes), &form, &error), CHTTP_WEB_OK);
    check_equal(chttp_web_form_bind_method_plan(&form, method_plan,
                    &native_options, &frame, &options, NULL, &diagnostic, &error),
                CHTTP_WEB_OK);
    check_equal(request.count, 7u);
    check_equal(request.side, Side_Sell);
    check_equal(FormValues_tags_vec_t_size(&request.tags), (size_t)2u);
    check_equal(*FormValues_tags_vec_t_at(&request.tags, 0u), 3u);
    check_equal(*FormValues_tags_vec_t_at(&request.tags, 1u), 5u);
    FormValues_clear(&request);

    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      check_equal(web_form_plan_parse(invalid[i], pairs, 4u, bytes,
                      sizeof(bytes), &form, &error), CHTTP_WEB_OK);
      check_equal(chttp_web_form_bind_method_plan(&form, method_plan,
                      &native_options, &frame, &options, NULL, &diagnostic, &error),
                  CHTTP_WEB_BIND);
      check_equal(request.count, 0u);
      check_equal(FormValues_tags_vec_t_size(&request.tags), (size_t)0u);
    }

    check_equal(web_form_plan_parse("count=7&side=Sell&tags=3&tags=5", pairs,
                    4u, bytes, sizeof(bytes), &form, &error), CHTTP_WEB_OK);
    native_options.workspace_bytes = 1u;
    check_equal(chttp_web_form_bind_method_plan(&form, method_plan,
                    &native_options, &frame, &options, NULL, &diagnostic, &error),
                CHTTP_WEB_CAPACITY);
    check_equal(request.count, 0u);
    check_equal(FormValues_tags_vec_t_size(&request.tags), (size_t)0u);
    native_options.workspace_bytes = sizeof(workspace);
    native_options.max_items = 1u;
    check_equal(chttp_web_form_bind_method_plan(&form, method_plan,
                    &native_options, &frame, &options, NULL, &diagnostic, &error),
                CHTTP_WEB_CAPACITY);
    check_equal(request.count, 0u);
    check_equal(FormValues_tags_vec_t_size(&request.tags), (size_t)0u);
    native_options.max_items = 64u;
    options.scalar_capacity = 2u;
    check_equal(web_form_plan_parse("count=77&side=Sell&tags=3", pairs,
                    4u, bytes, sizeof(bytes), &form, &error), CHTTP_WEB_OK);
    check_equal(chttp_web_form_bind_method_plan(&form, method_plan,
                    &native_options, &frame, &options, NULL, &diagnostic, &error),
                CHTTP_WEB_CAPACITY);
    check_equal(request.count, 0u);
    check_equal(FormValues_tags_vec_t_size(&request.tags), (size_t)0u);
    FormValues_clear(&request);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }
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
    DataBindNativeTypeBinding request_binding = {0};
    DataBindNativeTypeBinding response_binding = {0};
    DataBindServiceNativeBinding native = {0};
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
    FormRequest_t request = {0};
    FormResponse_t response = {0};
    void *params[2];
    size_t param_bytes[2];
    DataBindBindingCallFrame frame =
        (DataBindBindingCallFrame)DATA_BIND_BINDING_CALL_FRAME_INIT;

    check_equal(
        databind_7_WebForm_4_Calc_6_Submit__databind_native_binding(
            &request_binding, &response_binding, &native, &bind_error),
        DATA_BIND_OK);
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

    request = (FormRequest_t){0};
    response = (FormResponse_t){0};
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

    request = (FormRequest_t){0};
    response = (FormResponse_t){0};
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

    request = (FormRequest_t){0};
    response = (FormResponse_t){0};
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

    request = (FormRequest_t){0};
    response = (FormResponse_t){0};
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

    {
      const char *invalid[] = {
          "lhs=3&lhs=4&rhs=5", "lhs=oops&rhs=4", "lhs=3&rhs=4294967296"};
      for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        request = (FormRequest_t){0};
        web_form_plan_frame(&request, &response, &frame, params, param_bytes);
        check_equal(web_form_plan_parse(invalid[i], pairs, 4u, form_bytes,
                        sizeof(form_bytes), &form, &error), CHTTP_WEB_OK);
        check_equal(chttp_web_form_bind_method_plan(&form, method_plan,
                        &native_options, &frame, &plan_options, &validation,
                        &diagnostic, &error), CHTTP_WEB_BIND);
        check_equal(request.left, 0u);
        check_equal(request.right, 0u);
        check_equal(request.scale, 0u);
      }
    }

    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }
}
