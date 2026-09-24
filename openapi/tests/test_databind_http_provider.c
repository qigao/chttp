#include <openapi/generator.h>

#include "openapi_databind.http.h"

#include <cyaml/cyaml_json_adapter.h>
#include <json_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { \
  if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
    goto fail; \
  } \
} while (0)

static int same_value(const json_value_t *a, const json_value_t *b) {
  size_t i;
  if (a == NULL || b == NULL || json_type(a) != json_type(b)) return 0;
  switch (json_type(a)) {
  case JSON_NULL:
    return 1;
  case JSON_BOOL:
    return json_bool(a) == json_bool(b);
  case JSON_NUMBER:
    return json_number(a) == json_number(b);
  case JSON_STRING:
    return json_string_len(a) == json_string_len(b) &&
           memcmp(json_string(a), json_string(b), json_string_len(a)) == 0;
  case JSON_ARRAY:
    if (json_array_size(a) != json_array_size(b)) return 0;
    for (i = 0u; i < json_array_size(a); ++i)
      if (!same_value(json_array_get(a, i), json_array_get(b, i)))
        return 0;
    return 1;
  case JSON_OBJECT:
    if (json_object_size(a) != json_object_size(b)) return 0;
    for (i = 0u; i < json_object_size(a); ++i) {
      const json_value_t *other =
          json_object_get_v(b, json_object_key_v(a, i));
      if (!same_value(json_object_value(a, i), other)) return 0;
    }
    return 1;
  }
  return 0;
}

static const json_value_t *parameter(
    const json_value_t *operation,
    const char *name,
    const char *location) {
  const json_value_t *parameters = json_object_get(operation, "parameters");
  size_t i;
  if (parameters == NULL || json_type(parameters) != JSON_ARRAY) return NULL;
  for (i = 0u; i < json_array_size(parameters); ++i) {
    const json_value_t *item = json_array_get(parameters, i);
    const char *item_name = json_get_string(item, "name");
    const char *item_location = json_get_string(item, "in");
    if (item_name != NULL && item_location != NULL &&
        strcmp(item_name, name) == 0 &&
        strcmp(item_location, location) == 0)
      return item;
  }
  return NULL;
}

static int array_has_string(const json_value_t *array, const char *expected) {
  size_t i;
  if (array == NULL || json_type(array) != JSON_ARRAY) return 0;
  for (i = 0u; i < json_array_size(array); ++i) {
    const json_value_t *value = json_array_get(array, i);
    if (value != NULL && json_type(value) == JSON_STRING &&
        strcmp(json_string(value), expected) == 0)
      return 1;
  }
  return 0;
}

static int type_has(const json_value_t *schema, const char *expected) {
  const json_value_t *type = json_object_get(schema, "type");
  if (type == NULL) return 0;
  if (json_type(type) == JSON_STRING)
    return strcmp(json_string(type), expected) == 0;
  return array_has_string(type, expected);
}

static int generated_provider(oa_error *error) {
  static const char schema[] =
      "message AddRequest {"
      " @Min(1) uint32 left;"
      " optional uint32 right default 2;"
      " optional nullable @Pattern(\"^[a-z]+$\") string tag;"
      " string note;"
      "}"
      "message AddResponse {"
      " uint32 sum;"
      " optional string trace;"
      "}"
      "message CalcError { string detail; }"
      "service Calc {"
      " Add: AddRequest -> AddResponse throws CalcError;"
      "}";

  DataBind *contract = NULL;
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  oa_document *document = NULL;
  char *json = NULL;
  char *yaml = NULL;
  size_t json_size = 0u;
  size_t yaml_size = 0u;
  json_value_t *root = NULL;
  cyaml_doc_t *yaml_doc = NULL;
  json_value_t *yaml_root = NULL;
  const json_value_t *paths;
  const json_value_t *operation;
  const json_value_t *left;
  const json_value_t *right;
  const json_value_t *tag;
  const json_value_t *left_schema;
  const json_value_t *right_schema;
  const json_value_t *tag_schema;
  const json_value_t *request_body;
  const json_value_t *request_schema;
  const json_value_t *properties;
  const json_value_t *responses;
  const json_value_t *success;
  const json_value_t *success_schema;
  const json_value_t *success_headers;
  const json_value_t *error_response;
  int ok = 0;

  REQUIRE(data_bind_create_from_text(
              schema, sizeof(schema) - 1u,
              &contract, &bind_error) == DATA_BIND_OK);

  document = oa_document_create("DataBind HTTP", "1.0", error);
  REQUIRE(document != NULL);
  REQUIRE(oa_document_add_databind_http(
      document, contract,
      &databind_openapi_databind_http_projection, error));

  json = oa_document_render(document, "json", &json_size, error);
  REQUIRE(json != NULL);
  root = json_parse(json, json_size);
  REQUIRE(root != NULL);

  paths = json_object_get(root, "paths");
  REQUIRE(paths != NULL);
  operation = json_object_get(json_object_get(paths, "/calc/{left}"), "post");
  REQUIRE(operation != NULL);
  REQUIRE(strcmp(json_get_string(operation, "operationId"), "Calc.Add") == 0);

  left = parameter(operation, "left", "path");
  right = parameter(operation, "right", "query");
  tag = parameter(operation, "X-Tag", "header");
  REQUIRE(left != NULL && right != NULL && tag != NULL);

  REQUIRE(json_bool(json_object_get(left, "required")));
  REQUIRE(!json_bool(json_object_get(right, "required")));
  REQUIRE(!json_bool(json_object_get(tag, "required")));

  left_schema = json_object_get(left, "schema");
  right_schema = json_object_get(right, "schema");
  tag_schema = json_object_get(tag, "schema");
  REQUIRE(left_schema != NULL && right_schema != NULL && tag_schema != NULL);
  REQUIRE(type_has(left_schema, "integer"));
  REQUIRE(json_number(json_object_get(left_schema, "minimum")) == 1.0);
  REQUIRE(type_has(right_schema, "integer"));
  REQUIRE(json_number(json_object_get(right_schema, "default")) == 2.0);
  REQUIRE(type_has(tag_schema, "string"));
  REQUIRE(type_has(tag_schema, "null"));
  REQUIRE(strcmp(json_get_string(tag_schema, "pattern"), "^[a-z]+$") == 0);

  request_body = json_object_get(operation, "requestBody");
  REQUIRE(request_body != NULL);
  REQUIRE(json_bool(json_object_get(request_body, "required")));
  request_schema = json_object_get(
      json_object_get(
          json_object_get(request_body, "content"),
          "application/json"),
      "schema");
  REQUIRE(request_schema != NULL);
  properties = json_object_get(request_schema, "properties");
  REQUIRE(properties != NULL);
  REQUIRE(json_object_get(properties, "note") != NULL);
  REQUIRE(array_has_string(json_object_get(request_schema, "required"), "note"));

  responses = json_object_get(operation, "responses");
  REQUIRE(responses != NULL);
  success = json_object_get(responses, "201");
  REQUIRE(success != NULL);
  success_headers = json_object_get(success, "headers");
  REQUIRE(success_headers != NULL);
  REQUIRE(json_object_get(success_headers, "X-Trace") != NULL);
  success_schema = json_object_get(
      json_object_get(
          json_object_get(success, "content"),
          "application/json"),
      "schema");
  REQUIRE(success_schema != NULL);
  properties = json_object_get(success_schema, "properties");
  REQUIRE(properties != NULL && json_object_get(properties, "sum") != NULL);
  REQUIRE(array_has_string(json_object_get(success_schema, "required"), "sum"));

  error_response = json_object_get(responses, "422");
  REQUIRE(error_response != NULL);
  REQUIRE(strcmp(
      json_get_string(error_response, "description"),
      "Typed Service error: CalcError") == 0);

  yaml = oa_document_render(document, "yaml", &yaml_size, error);
  REQUIRE(yaml != NULL);
  yaml_doc = cyaml_parse(yaml, yaml_size, NULL, NULL);
  REQUIRE(yaml_doc != NULL);
  yaml_root = json_value_from_cyaml(yaml_doc);
  REQUIRE(yaml_root != NULL && same_value(root, yaml_root));

  ok = 1;

fail:
  json_free(yaml_root);
  if (yaml_doc != NULL) cyaml_free(yaml_doc);
  json_free(root);
  oa_text_free(yaml);
  oa_text_free(json);
  oa_document_free(document);
  data_bind_free(contract);
  return ok;
}

static int transactional_failure(oa_error *error) {
  static const char schema[] =
      "message Nested { string value; }"
      "message BadRequest { Nested nested; }"
      "message BadResponse { uint32 ok; }"
      "service Bad { Do: BadRequest -> BadResponse; }";

  DataBind *contract = NULL;
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  DataBindHttpProjectionConfig config =
      DATA_BIND_HTTP_PROJECTION_CONFIG_INIT;
  DataBindHttpProjectionArtifactEntry entry =
      DATA_BIND_HTTP_PROJECTION_ARTIFACT_ENTRY_INIT;
  DataBindHttpProjectionArtifact artifact =
      DATA_BIND_HTTP_PROJECTION_ARTIFACT_INIT;
  oa_document *document = NULL;
  char *before = NULL;
  char *after = NULL;
  size_t length = 0u;
  int ok = 0;

  REQUIRE(data_bind_create_from_text(
              schema, sizeof(schema) - 1u,
              &contract, &bind_error) == DATA_BIND_OK);

  config.method = "POST";
  config.route = "/bad";
  entry.service_name = "Bad";
  entry.operation_name = "Do";
  entry.config = config;
  artifact.entries = &entry;
  artifact.entry_count = 1u;

  document = oa_document_create("Transactional", "1.0", error);
  REQUIRE(document != NULL);
  before = oa_document_render(document, "json", &length, error);
  REQUIRE(before != NULL);

  error->message[0] = '\0';
  REQUIRE(!oa_document_add_databind_http(
      document, contract, &artifact, error));
  REQUIRE(error->message[0] != '\0');
  REQUIRE(strstr(error->message, "composite/container") != NULL);

  after = oa_document_render(document, "json", &length, error);
  REQUIRE(after != NULL);
  REQUIRE(strcmp(before, after) == 0);

  ok = 1;

fail:
  oa_text_free(after);
  oa_text_free(before);
  oa_document_free(document);
  data_bind_free(contract);
  return ok;
}

int main(void) {
  oa_error error = {{0}};

  if (!generated_provider(&error)) {
    fprintf(stderr, "generated provider failed: %s\n", error.message);
    return 1;
  }
  error.message[0] = '\0';
  if (!transactional_failure(&error)) {
    fprintf(stderr, "transactional failure test failed: %s\n", error.message);
    return 1;
  }

  puts("DataBind HTTP OpenAPI provider passed");
  return 0;
}
