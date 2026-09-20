#include <chttp_web/web.h>

#include "tinytest.h"

#include <data_bind.h>
#include <tbe_typed.h>
#include <salts_cmeta_fixed_width.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

TBE_TYPED_VEC_DEFINE(web_form_u32_vec, uint32_t)

typedef struct web_form_model {
  uint32_t count;
  uint8_t side;
  web_form_u32_vec tags;
} web_form_model;

static const TbeTypedField WEB_FORM_FIELDS[] = {
    {.name = "count",
     .kind = TBE_TYPED_U32,
     .wire_kind = TBE_TYPED_U32,
     .offset = offsetof(web_form_model, count)},
    {.name = "side",
     .kind = TBE_TYPED_ENUM,
     .wire_kind = TBE_TYPED_U8,
     .offset = offsetof(web_form_model, side)},
    {.name = "tags",
     .kind = TBE_TYPED_LIST,
     .wire_kind = TBE_TYPED_LIST,
     .offset = offsetof(web_form_model, tags),
     .element_kind = TBE_TYPED_U32,
     .element_wire_kind = TBE_TYPED_U32,
     .element_size = sizeof(uint32_t)}};

static const TbeTypedType WEB_FORM_TYPE = {
    .name = "FormModel",
    .size = sizeof(web_form_model),
    .fields = WEB_FORM_FIELDS,
    .field_count = sizeof(WEB_FORM_FIELDS) / sizeof(WEB_FORM_FIELDS[0])};


typedef struct web_form_descriptor_model {
  uint32_t count;
} web_form_descriptor_model;

TBE_TYPED_DEFINE_STRUCT(
    WEB_FORM_DESCRIPTOR_OVERLAY, web_form_descriptor_model, "ScalarForm",
    TBE_TYPED_FIELD(web_form_descriptor_model, count, "count",
                    TBE_TYPED_U32, TBE_TYPED_REQUIRED));

static const cmeta_type_identity WEB_FORM_DESCRIPTOR_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.form.ScalarForm");
static const cmeta_type_desc WEB_FORM_DESCRIPTOR_TYPE = {
    "web_form_descriptor_model",
    sizeof(web_form_descriptor_model),
    _Alignof(web_form_descriptor_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_FORM_DESCRIPTOR_ID};
static const cmeta_field_desc WEB_FORM_DESCRIPTOR_LAYOUT_FIELDS[] = {{
    "count", "uint32_t", offsetof(web_form_descriptor_model, count),
    sizeof(uint32_t), _Alignof(uint32_t), &salts_uint32_cmeta_type, NULL}};
static const cmeta_struct_desc WEB_FORM_DESCRIPTOR_LAYOUT = {
    "web_form_descriptor_model",
    sizeof(web_form_descriptor_model),
    _Alignof(web_form_descriptor_model),
    WEB_FORM_DESCRIPTOR_LAYOUT_FIELDS,
    1u};
static const cmeta_data_field_desc WEB_FORM_DESCRIPTOR_FIELDS[] = {{
    "chttp.web.form.ScalarForm.count", "count",
    offsetof(web_form_descriptor_model, count), &salts_uint32_cmeta_data}};
static const cmeta_data_struct_shape WEB_FORM_DESCRIPTOR_SHAPE = {
    &WEB_FORM_DESCRIPTOR_LAYOUT, WEB_FORM_DESCRIPTOR_FIELDS, 1u};
static const cmeta_data_desc WEB_FORM_DESCRIPTOR_DATA = {
    sizeof(cmeta_data_desc),
    CMETA_DATA_DESC_ABI_VERSION,
    "chttp.web.form.ScalarForm.data",
    "ScalarForm",
    CMETA_DATA_STRUCT,
    &WEB_FORM_DESCRIPTOR_TYPE,
    &WEB_FORM_DESCRIPTOR_SHAPE,
    NULL,
    NULL,
    NULL};
static const TbeTypedDescriptor WEB_FORM_DESCRIPTOR =
    TBE_TYPED_DESCRIPTOR_INIT(
        &WEB_FORM_DESCRIPTOR_OVERLAY, &WEB_FORM_DESCRIPTOR_DATA);

static chttp_web_status parse_form(
    const char *body, size_t max_input, size_t max_pairs,
    size_t max_decoded, chttp_web_form_pair *pairs, size_t pair_capacity,
    char *bytes, size_t byte_capacity, chttp_web_form *form,
    chttp_web_error *error) {
  chttp_web_form_parse_options options =
      (chttp_web_form_parse_options)CHTTP_WEB_FORM_PARSE_OPTIONS_INIT;
  options.max_input_bytes = max_input;
  options.max_pairs = max_pairs;
  options.max_decoded_bytes = max_decoded;
  options.pair_storage = pairs;
  options.pair_capacity = pair_capacity;
  options.byte_storage = bytes;
  options.byte_capacity = byte_capacity;
  return chttp_web_form_parse(
      body, body ? strlen(body) : 0u, &options, form, error);
}

static int view_equals(chttp_web_string_view value, const char *expected) {
  const size_t size = strlen(expected);
  return value.size == size &&
         (size == 0u || memcmp(value.data, expected, size) == 0);
}

spec("CHttp::Web bounded form parsing and DataBind binding") {
  it("percent-decodes plus, escapes, repeated keys, and exact bounds") {
    static const char body[] =
        "name=Alice+Smith&tag=A%26B%3DC&tag=two";
    chttp_web_form_pair pairs[3];
    char bytes[30];
    chttp_web_form form = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    const size_t decoded = 29u;

    check_equal(
        parse_form(body, sizeof(body) - 1u, 3u, decoded,
                   pairs, 3u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(form.pair_count, 3u);
    check_equal(form.decoded_bytes, decoded);
    check_true(view_equals(form.pairs[0].name, "name"));
    check_true(view_equals(form.pairs[0].value, "Alice Smith"));
    check_equal(chttp_web_form_count(&form, "tag"), 2u);
    check_true(view_equals(
        chttp_web_form_get(&form, "tag", 0u)->value, "A&B=C"));
    check_true(view_equals(
        chttp_web_form_get(&form, "tag", 1u)->value, "two"));

    form = (chttp_web_form){0};
    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form(body, sizeof(body) - 1u, 3u, decoded - 1u,
                   pairs, 3u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_CAPACITY);
    check_equal(form.pair_count, 0u);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form(body, sizeof(body) - 2u, 3u, decoded,
                   pairs, 3u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_CAPACITY);
  }

  it("rejects malformed encoding, empty segments, and one-pair-over") {
    chttp_web_form_pair pairs[2];
    char bytes[32];
    chttp_web_form form = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        parse_form("a=%GG", 5u, 2u, 32u,
                   pairs, 2u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_FORM);
    check_equal(form.pair_count, 0u);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form("a=1&&b=2", 8u, 2u, 32u,
                   pairs, 2u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_FORM);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form("a=1&b=2", 7u, 1u, 32u,
                   pairs, 2u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_CAPACITY);
  }

  it("binds scalar enum and repeated collection values transactionally") {
    static const char schema[] =
        "enum Side <uint8> { Buy = 1; Sell = 2; } "
        "message FormModel { uint32 count; Side side; list<uint32> tags; }";
    static const char valid[] =
        "count=7&side=Sell&tags=3&tags=5";
    static const char bad_number[] =
        "count=oops&side=Buy&tags=9";
    chttp_web_form_pair pairs[4];
    char bytes[64];
    char json[128];
    chttp_web_form form = {0};
    chttp_web_form_bind_options bind_options =
        (chttp_web_form_bind_options)CHTTP_WEB_FORM_BIND_OPTIONS_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBind *codec = NULL;
    web_form_model model;
    uint32_t before0;
    uint32_t before1;

    bind_options.json_storage = json;
    bind_options.json_capacity = sizeof(json);

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &codec, &bind_error),
        DATA_BIND_OK);
    check_not_null(codec);
    check_equal(
        tbe_typed_init(&WEB_FORM_TYPE, &model, &bind_error),
        DATA_BIND_OK);

    check_equal(
        parse_form(valid, sizeof(valid) - 1u, 4u, sizeof(bytes),
                   pairs, 4u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_typed(
            &form, codec, "FormModel", &WEB_FORM_TYPE, &model,
            &bind_options, &error),
        CHTTP_WEB_OK);
    check_equal(model.count, 7u);
    check_equal(model.side, 2u);
    check_equal(web_form_u32_vec_size(&model.tags), 2u);
    check_equal(*web_form_u32_vec_at(&model.tags, 0u), 3u);
    check_equal(*web_form_u32_vec_at(&model.tags, 1u), 5u);

    before0 = *web_form_u32_vec_at(&model.tags, 0u);
    before1 = *web_form_u32_vec_at(&model.tags, 1u);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form(bad_number, sizeof(bad_number) - 1u, 4u, sizeof(bytes),
                   pairs, 4u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_typed(
            &form, codec, "FormModel", &WEB_FORM_TYPE, &model,
            &bind_options, &error),
        CHTTP_WEB_BIND);
    check_equal(model.count, 7u);
    check_equal(model.side, 2u);
    check_equal(web_form_u32_vec_size(&model.tags), 2u);
    check_equal(*web_form_u32_vec_at(&model.tags, 0u), before0);
    check_equal(*web_form_u32_vec_at(&model.tags, 1u), before1);

    tbe_typed_clear(&WEB_FORM_TYPE, &model);
    data_bind_free(codec);
  }

  it("binds through the canonical CMeta/DataBind descriptor transactionally") {
    static const char schema[] = "message ScalarForm { uint32 count; }";
    chttp_web_form_pair pairs[1];
    char bytes[32];
    char json[64];
    chttp_web_form form = {0};
    chttp_web_form_bind_options bind_options =
        (chttp_web_form_bind_options)CHTTP_WEB_FORM_BIND_OPTIONS_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBind *codec = NULL;
    web_form_descriptor_model model;

    bind_options.json_storage = json;
    bind_options.json_capacity = sizeof(json);

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &codec, &bind_error),
        DATA_BIND_OK);
    check_not_null(codec);
    check_equal(
        tbe_typed_descriptor_init(
            &WEB_FORM_DESCRIPTOR, &model, &bind_error),
        DATA_BIND_OK);

    check_equal(
        parse_form("count=11", 8u, 1u, sizeof(bytes),
                   pairs, 1u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_descriptor(
            &form, codec, "ScalarForm", &WEB_FORM_DESCRIPTOR, &model,
            &bind_options, &error),
        CHTTP_WEB_OK);
    check_equal(model.count, 11u);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form("count=oops", 10u, 1u, sizeof(bytes),
                   pairs, 1u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_descriptor(
            &form, codec, "ScalarForm", &WEB_FORM_DESCRIPTOR, &model,
            &bind_options, &error),
        CHTTP_WEB_BIND);
    check_equal(model.count, 11u);

    check_equal(
        tbe_typed_descriptor_clear(
            &WEB_FORM_DESCRIPTOR, &model, &bind_error),
        DATA_BIND_OK);
    data_bind_free(codec);
  }

  it("rejects duplicate scalar, unknown field and JSON bridge overflow without mutation") {
    static const char schema[] =
        "enum Side <uint8> { Buy = 1; Sell = 2; } "
        "message FormModel { uint32 count; Side side; list<uint32> tags; }";
    chttp_web_form_pair pairs[4];
    char bytes[64];
    char json[128];
    chttp_web_form form = {0};
    chttp_web_form_bind_options bind_options =
        (chttp_web_form_bind_options)CHTTP_WEB_FORM_BIND_OPTIONS_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBind *codec = NULL;
    web_form_model model;

    bind_options.json_storage = json;
    bind_options.json_capacity = sizeof(json);
    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &codec, &bind_error),
        DATA_BIND_OK);
    check_equal(
        tbe_typed_init(&WEB_FORM_TYPE, &model, &bind_error),
        DATA_BIND_OK);

    check_equal(
        parse_form("count=1&count=2", 15u, 4u, 64u,
                   pairs, 4u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_typed(
            &form, codec, "FormModel", &WEB_FORM_TYPE, &model,
            &bind_options, &error),
        CHTTP_WEB_BIND);
    check_equal(model.count, 0u);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form("missing=1", 9u, 4u, 64u,
                   pairs, 4u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_form_bind_typed(
            &form, codec, "FormModel", &WEB_FORM_TYPE, &model,
            &bind_options, &error),
        CHTTP_WEB_BIND);
    check_equal(model.count, 0u);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form("count=9&side=Buy&tags=1", 25u, 4u, 64u,
                   pairs, 4u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    bind_options.json_capacity = 8u;
    check_equal(
        chttp_web_form_bind_typed(
            &form, codec, "FormModel", &WEB_FORM_TYPE, &model,
            &bind_options, &error),
        CHTTP_WEB_CAPACITY);
    check_equal(model.count, 0u);

    tbe_typed_clear(&WEB_FORM_TYPE, &model);
    data_bind_free(codec);
  }
}
