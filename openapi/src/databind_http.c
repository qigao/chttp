#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>

static int db_put(json_value_t *object, const char *key, json_value_t *value) {
  if (value != NULL && json_object_add_checked(object, key, value)) return 1;
  json_free(value);
  return 0;
}

static const DataBindHttpFieldProjection *db_field_mapping(
    const DataBindHttpProjectionConfig *config,
    DataBindBindingDirection direction,
    const char *field_name) {
  size_t i;
  if (config == NULL || field_name == NULL) return NULL;
  for (i = 0u; i < config->field_count; ++i) {
    const DataBindHttpFieldProjection *mapping = &config->fields[i];
    if (mapping->direction == direction &&
        mapping->schema_field != NULL &&
        strcmp(mapping->schema_field, field_name) == 0)
      return mapping;
  }
  return NULL;
}

static int db_type(
    json_value_t *schema, const char *name, int nullable) {
  if (!nullable)
    return db_put(schema, "type", json_create_string(name));
  {
    json_value_t *types = json_create_array();
    if (types == NULL ||
        !json_array_add_checked(types, json_create_string(name)) ||
        !json_array_add_checked(types, json_create_string("null"))) {
      json_free(types);
      return 0;
    }
    return db_put(schema, "type", types);
  }
}

static json_value_t *db_number(const char *text) {
  json_value_t *value;
  if (text == NULL || text[0] == '\0') return NULL;
  value = json_parse(text, strlen(text));
  if (value == NULL || json_type(value) != JSON_NUMBER) {
    json_free(value);
    return NULL;
  }
  return value;
}

static json_value_t *db_default_value(
    const DataBindSchemaField *field) {
  if (field == NULL || !field->has_default ||
      field->default_value == NULL)
    return NULL;

  switch (field->cmeta_kind) {
  case CMETA_DATA_BOOL:
    if (strcmp(field->default_value, "true") == 0)
      return json_create_bool(true);
    if (strcmp(field->default_value, "false") == 0)
      return json_create_bool(false);
    return NULL;
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
    return db_number(field->default_value);
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
  case CMETA_DATA_ENUM:
    return json_create_string(field->default_value);
  default:
    return NULL;
  }
}

static int db_constraints(
    DataBind *contract, const char *type_name, size_t field_index,
    const DataBindSchemaField *field, json_value_t *schema,
    oa_error *error) {
  size_t count;
  size_t i;
  int seen_min = 0, seen_max = 0, seen_size = 0, seen_pattern = 0;

  count = data_bind_schema_field_constraint_count(
      contract, type_name, field_index);
  for (i = 0u; i < count; ++i) {
    DataBindSchemaConstraint constraint =
        DATA_BIND_SCHEMA_CONSTRAINT_INIT;
    if (!data_bind_schema_field_constraint_at(
            contract, type_name, field_index, i, &constraint))
      return oa_fail(error, "%s.%s: invalid reflected constraint",
                     type_name, field->name);

    switch (constraint.kind) {
    case DATA_BIND_SCHEMA_CONSTRAINT_MIN: {
      json_value_t *number;
      if (seen_min++ || constraint.value == NULL)
        return oa_fail(error, "%s.%s: duplicate/invalid minimum",
                       type_name, field->name);
      number = db_number(constraint.value);
      if (number == NULL || !db_put(schema, "minimum", number))
        return oa_fail(error, "%s.%s: invalid minimum",
                       type_name, field->name);
      break;
    }
    case DATA_BIND_SCHEMA_CONSTRAINT_MAX: {
      json_value_t *number;
      if (seen_max++ || constraint.value == NULL)
        return oa_fail(error, "%s.%s: duplicate/invalid maximum",
                       type_name, field->name);
      number = db_number(constraint.value);
      if (number == NULL || !db_put(schema, "maximum", number))
        return oa_fail(error, "%s.%s: invalid maximum",
                       type_name, field->name);
      break;
    }
    case DATA_BIND_SCHEMA_CONSTRAINT_SIZE:
      if (seen_size++ ||
          (field->cmeta_kind != CMETA_DATA_STRING &&
           field->cmeta_kind != CMETA_DATA_BYTES))
        return oa_fail(error, "%s.%s: unsupported size constraint",
                       type_name, field->name);
      if (constraint.has_min &&
          !db_put(schema, "minLength",
                  json_create_uint64(constraint.min_size)))
        return oa_fail(error, "out of memory");
      if (constraint.has_max &&
          !db_put(schema, "maxLength",
                  json_create_uint64(constraint.max_size)))
        return oa_fail(error, "out of memory");
      break;
    case DATA_BIND_SCHEMA_CONSTRAINT_PATTERN:
      if (seen_pattern++ || constraint.pattern == NULL ||
          field->cmeta_kind != CMETA_DATA_STRING ||
          !db_put(schema, "pattern",
                  json_create_string(constraint.pattern)))
        return oa_fail(error, "%s.%s: invalid pattern constraint",
                       type_name, field->name);
      break;
    default:
      return oa_fail(error, "%s.%s: unsupported reflected constraint",
                     type_name, field->name);
    }
  }
  return 1;
}

static json_value_t *db_field_schema(
    DataBind *contract, const char *type_name, size_t field_index,
    DataBindSchemaField *out_field, oa_error *error) {
  DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;
  json_value_t *schema = NULL;
  json_value_t *default_value = NULL;
  const char *type = NULL;

  if (!data_bind_schema_field_at(
          contract, type_name, field_index, &field) ||
      field.name == NULL || !field.has_cmeta_kind) {
    oa_fail(error, "%s[%zu]: field reflection is unavailable",
            type_name, field_index);
    return NULL;
  }

  if (field.is_collection || field.is_composite || field.is_group ||
      field.is_map ||
      field.cmeta_kind == CMETA_DATA_STRUCT ||
      field.cmeta_kind == CMETA_DATA_VARIANT ||
      field.cmeta_kind == CMETA_DATA_SEQUENCE ||
      field.cmeta_kind == CMETA_DATA_SET ||
      field.cmeta_kind == CMETA_DATA_MAP ||
      field.cmeta_kind == CMETA_DATA_CUSTOM) {
    oa_fail(error, "%s.%s: composite/container OpenAPI schema is not admitted by this slice",
            type_name, field.name);
    return NULL;
  }

  switch (field.cmeta_kind) {
  case CMETA_DATA_BOOL: type = "boolean"; break;
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT: type = "integer"; break;
  case CMETA_DATA_FLOAT: type = "number"; break;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
  case CMETA_DATA_ENUM: type = "string"; break;
  default:
    oa_fail(error, "%s.%s: unsupported CMeta data kind",
            type_name, field.name);
    return NULL;
  }

  schema = json_create_object();
  if (schema == NULL || !db_type(schema, type, field.is_nullable))
    goto oom;

  if (field.cmeta_kind == CMETA_DATA_BYTES &&
      !db_put(schema, "format", json_create_string("byte")))
    goto oom;
  if (field.format != NULL && field.format[0] != '\0' &&
      field.cmeta_kind != CMETA_DATA_BYTES &&
      !db_put(schema, "format", json_create_string(field.format)))
    goto oom;

  if (field.cmeta_kind == CMETA_DATA_ENUM) {
    size_t count = data_bind_schema_enum_item_count(
        contract, field.type);
    size_t i;
    json_value_t *values = json_create_array();
    if (values == NULL || count == 0u) {
      json_free(values);
      oa_fail(error, "%s.%s: enum reflection is unavailable",
              type_name, field.name);
      goto fail;
    }
    for (i = 0u; i < count; ++i) {
      DataBindSchemaEnumItem item = DATA_BIND_SCHEMA_ENUM_ITEM_INIT;
      if (!data_bind_schema_enum_item_at(
              contract, field.type, i, &item) ||
          item.name == NULL ||
          !json_array_add_checked(values, json_create_string(item.name))) {
        json_free(values);
        oa_fail(error, "%s.%s: invalid enum reflection",
                type_name, field.name);
        goto fail;
      }
    }
    if (!db_put(schema, "enum", values)) goto oom;
  }

  if (!db_constraints(
          contract, type_name, field_index, &field, schema, error))
    goto fail;

  if (field.has_default) {
    default_value = db_default_value(&field);
    if (default_value == NULL ||
        !db_put(schema, "default", default_value)) {
      default_value = NULL;
      oa_fail(error, "%s.%s: invalid canonical default",
              type_name, field.name);
      goto fail;
    }
    default_value = NULL;
  }

  if (out_field != NULL) *out_field = field;
  return schema;

oom:
  oa_fail(error, "out of memory constructing DataBind OpenAPI schema");
fail:
  json_free(default_value);
  json_free(schema);
  return NULL;
}

static int db_required_add(
    json_value_t *required, const char *name) {
  return json_array_add_checked(required, json_create_string(name));
}

static int db_object_fields(
    DataBind *contract, const char *type_name,
    const DataBindHttpProjectionConfig *config,
    DataBindBindingDirection direction,
    DataBindHttpFieldLocation body_location,
    json_value_t **out_schema,
    oa_error *error) {
  json_value_t *schema = NULL;
  json_value_t *properties = NULL;
  json_value_t *required = NULL;
  size_t field_count;
  size_t i;
  size_t selected = 0u;

  *out_schema = NULL;
  field_count = data_bind_schema_field_count(contract, type_name);
  schema = json_create_object();
  properties = json_create_object();
  required = json_create_array();
  if (schema == NULL || properties == NULL || required == NULL)
    goto oom;

  if (!db_put(schema, "type", json_create_string("object")))
    goto oom;

  for (i = 0u; i < field_count; ++i) {
    DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;
    const DataBindHttpFieldProjection *mapping;
    DataBindHttpFieldLocation location;
    const char *wire;
    json_value_t *field_schema;

    if (!data_bind_schema_field_at(
            contract, type_name, i, &field) ||
        field.name == NULL)
      return oa_fail(error, "%s[%zu]: field reflection failed",
                     type_name, i);

    mapping = db_field_mapping(config, direction, field.name);
    if (mapping != NULL)
      location = mapping->location;
    else
      location = body_location;

    if (location != body_location) continue;

    wire = mapping != NULL && mapping->wire_name != NULL
               ? mapping->wire_name : field.name;
    field_schema = db_field_schema(
        contract, type_name, i, &field, error);
    if (field_schema == NULL) goto fail;
    if (!db_put(properties, wire, field_schema)) goto oom;
    if (!field.is_optional && !db_required_add(required, wire))
      goto oom;
    ++selected;
  }

  if (selected == 0u) {
    json_free(required);
    json_free(properties);
    json_free(schema);
    return 1;
  }

  if (!db_put(schema, "properties", properties)) goto oom;
  properties = NULL;
  if (json_array_size(required) != 0u) {
    if (!db_put(schema, "required", required)) goto oom;
    required = NULL;
  }
  json_free(required);
  *out_schema = schema;
  return 1;

oom:
  oa_fail(error, "out of memory constructing DataBind body schema");
fail:
  json_free(required);
  json_free(properties);
  json_free(schema);
  return 0;
}

static const char *db_parameter_location(DataBindHttpFieldLocation location) {
  switch (location) {
  case DATA_BIND_HTTP_PATH: return "path";
  case DATA_BIND_HTTP_QUERY: return "query";
  case DATA_BIND_HTTP_HEADER: return "header";
  case DATA_BIND_HTTP_COOKIE: return "cookie";
  default: return NULL;
  }
}

static int db_parameters(
    DataBind *contract, const char *type_name,
    const DataBindHttpProjectionConfig *config,
    json_value_t *operation, oa_error *error) {
  json_value_t *parameters = json_create_array();
  size_t count = data_bind_schema_field_count(contract, type_name);
  size_t i;

  if (parameters == NULL) return oa_fail(error, "out of memory");

  for (i = 0u; i < count; ++i) {
    DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;
    const DataBindHttpFieldProjection *mapping;
    const char *location;
    const char *wire;
    json_value_t *parameter = NULL;
    json_value_t *schema = NULL;

    if (!data_bind_schema_field_at(
            contract, type_name, i, &field) ||
        field.name == NULL) {
      json_free(parameters);
      return oa_fail(error, "%s[%zu]: field reflection failed",
                     type_name, i);
    }
    mapping = db_field_mapping(
        config, DATA_BIND_BINDING_INGRESS, field.name);
    if (mapping == NULL) continue;
    location = db_parameter_location(mapping->location);
    if (location == NULL) continue;

    wire = mapping->wire_name != NULL
               ? mapping->wire_name : field.name;
    schema = db_field_schema(
        contract, type_name, i, &field, error);
    if (schema == NULL) {
      json_free(parameters);
      return 0;
    }
    parameter = json_create_object();
    if (parameter == NULL ||
        !db_put(parameter, "name", json_create_string(wire)) ||
        !db_put(parameter, "in", json_create_string(location)) ||
        !db_put(parameter, "required",
                json_create_bool(
                    mapping->location == DATA_BIND_HTTP_PATH
                        ? true : !field.is_optional)) ||
        !db_put(parameter, "schema", schema) ||
        !json_array_add_checked(parameters, parameter)) {
      json_free(schema);
      json_free(parameter);
      json_free(parameters);
      return oa_fail(error, "out of memory constructing parameter");
    }
  }

  if (json_array_size(parameters) == 0u) {
    json_free(parameters);
    return 1;
  }
  if (!db_put(operation, "parameters", parameters)) {
    json_free(parameters);
    return oa_fail(error, "out of memory");
  }
  return 1;
}

static int db_request_body(
    DataBind *contract, const char *type_name,
    const DataBindHttpProjectionConfig *config,
    json_value_t *operation, oa_error *error) {
  json_value_t *schema = NULL;
  json_value_t *media = NULL;
  json_value_t *content = NULL;
  json_value_t *body = NULL;

  if (!db_object_fields(
          contract, type_name, config,
          DATA_BIND_BINDING_INGRESS, DATA_BIND_HTTP_BODY,
          &schema, error))
    return 0;
  if (schema == NULL) return 1;

  media = json_create_object();
  content = json_create_object();
  body = json_create_object();
  if (media == NULL || content == NULL || body == NULL ||
      !db_put(media, "schema", schema) ||
      !db_put(content, "application/json", media) ||
      !db_put(body, "content", content) ||
      !db_put(operation, "requestBody", body)) {
    json_free(schema);
    json_free(media);
    json_free(content);
    json_free(body);
    return oa_fail(error, "out of memory constructing requestBody");
  }
  return 1;
}

static int db_response_headers(
    DataBind *contract, const char *type_name,
    const DataBindHttpProjectionConfig *config,
    json_value_t *response, oa_error *error) {
  json_value_t *headers = json_create_object();
  size_t count = data_bind_schema_field_count(contract, type_name);
  size_t i;

  if (headers == NULL) return oa_fail(error, "out of memory");
  for (i = 0u; i < count; ++i) {
    DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;
    const DataBindHttpFieldProjection *mapping;
    const char *wire;
    json_value_t *header = NULL;
    json_value_t *schema = NULL;

    if (!data_bind_schema_field_at(
            contract, type_name, i, &field) ||
        field.name == NULL) {
      json_free(headers);
      return oa_fail(error, "%s[%zu]: field reflection failed",
                     type_name, i);
    }
    mapping = db_field_mapping(
        config, DATA_BIND_BINDING_EGRESS, field.name);
    if (mapping == NULL ||
        mapping->location != DATA_BIND_HTTP_RESPONSE_HEADER)
      continue;

    wire = mapping->wire_name != NULL
               ? mapping->wire_name : field.name;
    schema = db_field_schema(
        contract, type_name, i, &field, error);
    if (schema == NULL) {
      json_free(headers);
      return 0;
    }
    header = json_create_object();
    if (header == NULL || !db_put(header, "schema", schema) ||
        !db_put(headers, wire, header)) {
      json_free(schema);
      json_free(header);
      json_free(headers);
      return oa_fail(error, "out of memory constructing response header");
    }
  }

  if (json_object_size(headers) == 0u) {
    json_free(headers);
    return 1;
  }
  if (!db_put(response, "headers", headers)) {
    json_free(headers);
    return oa_fail(error, "out of memory");
  }
  return 1;
}

static int db_success_response(
    DataBind *contract, const DataBindServiceOperation *service_operation,
    const DataBindHttpProjectionConfig *config,
    json_value_t *responses, oa_error *error) {
  char status[4];
  json_value_t *response = json_create_object();
  json_value_t *body_schema = NULL;

  if (config->success_status < 100 || config->success_status > 599 ||
      snprintf(status, sizeof(status), "%03d", config->success_status) != 3) {
    json_free(response);
    return oa_fail(error, "%s.%s: invalid success status",
                   service_operation->service_name,
                   service_operation->name);
  }
  if (response == NULL ||
      !db_put(response, "description", json_create_string("Success")))
    goto oom;
  if (!db_response_headers(
          contract, service_operation->response_type,
          config, response, error))
    goto fail;
  if (!db_object_fields(
          contract, service_operation->response_type, config,
          DATA_BIND_BINDING_EGRESS, DATA_BIND_HTTP_RESPONSE_BODY,
          &body_schema, error))
    goto fail;
  if (body_schema != NULL) {
    json_value_t *media = json_create_object();
    json_value_t *content = json_create_object();
    if (media == NULL || content == NULL ||
        !db_put(media, "schema", body_schema) ||
        !db_put(content, "application/json", media) ||
        !db_put(response, "content", content)) {
      json_free(body_schema);
      json_free(media);
      json_free(content);
      goto oom;
    }
  }
  if (!db_put(responses, status, response)) goto oom;
  return 1;

oom:
  oa_fail(error, "out of memory constructing success response");
fail:
  json_free(response);
  return 0;
}

static int db_error_status(
    const DataBindHttpProjectionConfig *config,
    const char *error_type) {
  size_t i;
  for (i = 0u; i < config->error_count; ++i)
    if (config->errors[i].error_type != NULL &&
        strcmp(config->errors[i].error_type, error_type) == 0)
      return config->errors[i].status;
  return 500;
}

static int db_error_responses(
    DataBind *contract, const DataBindServiceOperation *operation,
    const DataBindHttpProjectionConfig *config,
    json_value_t *responses, oa_error *error) {
  size_t i;
  for (i = 0u; i < operation->error_count; ++i) {
    const char *name = data_bind_service_operation_error_at(
        contract, operation->service_name, operation->name, i);
    int status = db_error_status(config, name);
    char code[4];
    char description[192];
    json_value_t *response;

    if (name == NULL || status < 100 || status > 599 ||
        snprintf(code, sizeof(code), "%03d", status) != 3)
      return oa_fail(error, "%s.%s: invalid typed-error mapping",
                     operation->service_name, operation->name);
    if (json_object_get(responses, code) != NULL)
      return oa_fail(error,
                     "%s.%s: response status %s is ambiguous in this OpenAPI slice",
                     operation->service_name, operation->name, code);

    if (snprintf(
            description, sizeof(description),
            "Typed Service error: %s", name) < 0)
      return oa_fail(error, "could not format typed-error description");
    response = json_create_object();
    if (response == NULL ||
        !db_put(response, "description",
                json_create_string(description)) ||
        !db_put(responses, code, response)) {
      json_free(response);
      return oa_fail(error, "out of memory constructing typed-error response");
    }
  }
  return 1;
}

static const char *db_method(const char *method) {
  if (method == NULL || strcmp(method, "POST") == 0) return "post";
  if (strcmp(method, "GET") == 0) return "get";
  if (strcmp(method, "PUT") == 0) return "put";
  if (strcmp(method, "DELETE") == 0) return "delete";
  if (strcmp(method, "OPTIONS") == 0) return "options";
  if (strcmp(method, "HEAD") == 0) return "head";
  if (strcmp(method, "PATCH") == 0) return "patch";
  if (strcmp(method, "TRACE") == 0) return "trace";
  return NULL;
}

static char *db_default_route(
    const char *service, const char *operation) {
  size_t a = strlen(service), b = strlen(operation);
  char *route;
  if (a > SIZE_MAX - b - 3u) return NULL;
  route = (char *)malloc(a + b + 3u);
  if (route == NULL) return NULL;
  route[0] = '/';
  memcpy(route + 1u, service, a);
  route[a + 1u] = '/';
  memcpy(route + a + 2u, operation, b + 1u);
  return route;
}

static size_t db_operation_count(const json_value_t *paths) {
  size_t count = 0u, i;
  for (i = 0u; i < json_object_size(paths); ++i)
    count += json_object_size(json_object_value(paths, i));
  return count;
}

static const char *db_string_field(
    const json_value_t *object, const char *key) {
  const json_value_t *value = json_object_get(object, key);
  if (value == NULL || json_type(value) != JSON_STRING) return NULL;
  return json_string(value);
}

static int db_id_exists(
    const json_value_t *paths, const char *operation_id) {
  size_t i, j;
  for (i = 0u; i < json_object_size(paths); ++i) {
    const json_value_t *path = json_object_value(paths, i);
    for (j = 0u; j < json_object_size(path); ++j) {
      const char *other = db_string_field(
          json_object_value(path, j), "operationId");
      if (other != NULL && strcmp(other, operation_id) == 0)
        return 1;
    }
  }
  return 0;
}

static int db_same_path_template(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a != *b) return 0;
    if (*a == '{') {
      a = strchr(a, '}');
      b = strchr(b, '}');
      if (a == NULL || b == NULL) return 0;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

static int db_append_operation(
    json_value_t *root, DataBind *contract,
    const DataBindHttpProjectionArtifactEntry *entry,
    oa_error *error) {
  DataBindServiceOperation service_operation =
      DATA_BIND_SERVICE_OPERATION_INIT;
  const DataBindHttpProjectionConfig *config;
  const char *method;
  const char *route;
  char *default_route = NULL;
  char operation_id[256];
  json_value_t *paths;
  json_value_t *path_item;
  json_value_t *operation = NULL;
  json_value_t *responses = NULL;
  size_t i;

  if (entry == NULL || entry->size < sizeof(*entry) ||
      entry->service_name == NULL || entry->operation_name == NULL)
    return oa_fail(error, "invalid DataBind HTTP projection entry");

  config = data_bind_http_projection_artifact_find(
      (const DataBindHttpProjectionArtifact *)
          ((const char *)entry -
           offsetof(DataBindHttpProjectionArtifactEntry, size)),
      entry->service_name, entry->operation_name);
  (void)config;
  config = &entry->config;

  if (!data_bind_service_operation_find(
          contract, entry->service_name, entry->operation_name,
          &service_operation))
    return oa_fail(error, "%s.%s: Service operation not found",
                   entry->service_name, entry->operation_name);

  method = db_method(config->method);
  if (method == NULL)
    return oa_fail(error, "%s.%s: HTTP method is not representable in OpenAPI",
                   entry->service_name, entry->operation_name);

  if (config->route != NULL) {
    route = config->route;
  } else {
    default_route = db_default_route(
        entry->service_name, entry->operation_name);
    route = default_route;
  }
  if (route == NULL || route[0] != '/' ||
      strpbrk(route, " \t\r\n?#") != NULL) {
    free(default_route);
    return oa_fail(error, "%s.%s: invalid HTTP route",
                   entry->service_name, entry->operation_name);
  }

  if (snprintf(
          operation_id, sizeof(operation_id), "%s.%s",
          entry->service_name, entry->operation_name) <= 0 ||
      strlen(operation_id) >= sizeof(operation_id) - 1u) {
    free(default_route);
    return oa_fail(error, "operationId is too long");
  }

  paths = json_object_get(root, "paths");
  if (paths == NULL || json_type(paths) != JSON_OBJECT) {
    free(default_route);
    return oa_fail(error, "OpenAPI document has no paths object");
  }
  if (db_operation_count(paths) >= OA_MAX_OPERATIONS) {
    free(default_route);
    return oa_fail(error, "document exceeds %u operations",
                   OA_MAX_OPERATIONS);
  }
  if (db_id_exists(paths, operation_id)) {
    free(default_route);
    return oa_fail(error, "%s: duplicate operationId", operation_id);
  }
  for (i = 0u; i < json_object_size(paths); ++i) {
    const char *other = json_object_key(paths, i);
    if (strcmp(route, other) != 0 &&
        db_same_path_template(route, other)) {
      free(default_route);
      return oa_fail(error,
                     "%s: equivalent route template already exists", route);
    }
  }

  path_item = json_object_get(paths, route);
  if (path_item != NULL && json_object_get(path_item, method) != NULL) {
    free(default_route);
    return oa_fail(error, "%s %s: duplicate route", method, route);
  }

  operation = json_create_object();
  responses = json_create_object();
  if (operation == NULL || responses == NULL ||
      !db_put(operation, "operationId",
              json_create_string(operation_id)) ||
      !db_parameters(
          contract, service_operation.request_type,
          config, operation, error) ||
      !db_request_body(
          contract, service_operation.request_type,
          config, operation, error) ||
      !db_success_response(
          contract, &service_operation, config,
          responses, error) ||
      !db_error_responses(
          contract, &service_operation, config,
          responses, error) ||
      !db_put(operation, "responses", responses)) {
    json_free(operation);
    json_free(responses);
    free(default_route);
    return 0;
  }
  responses = NULL;

  if (path_item == NULL) {
    path_item = json_create_object();
    if (path_item == NULL || !db_put(paths, route, path_item)) {
      json_free(path_item);
      json_free(operation);
      free(default_route);
      return oa_fail(error, "out of memory constructing path");
    }
  }
  if (!db_put(path_item, method, operation)) {
    json_free(operation);
    free(default_route);
    return oa_fail(error, "out of memory constructing operation");
  }

  free(default_route);
  return 1;
}

int oa_document_add_databind_http(
    oa_document *document,
    DataBind *contract,
    const DataBindHttpProjectionArtifact *artifact,
    oa_error *error) {
  json_value_t *next;
  size_t i;

  if (document == NULL || contract == NULL || artifact == NULL ||
      artifact->size < sizeof(*artifact) ||
      artifact->abi_version != DATA_BIND_METHOD_PLAN_ABI_VERSION ||
      (artifact->entry_count != 0u && artifact->entries == NULL))
    return oa_fail(error, "invalid DataBind HTTP OpenAPI arguments");

  next = json_clone(document->root);
  if (next == NULL) return oa_fail(error, "out of memory");

  for (i = 0u; i < artifact->entry_count; ++i) {
    if (!db_append_operation(
            next, contract, &artifact->entries[i], error)) {
      json_free(next);
      return 0;
    }
  }

  json_free(document->root);
  document->root = next;
  return 1;
}
