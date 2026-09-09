#include "internal.h"
#include <cmeta/enum.h>
#include <cflow/stream.h>
#include <cflow/adapters.h>
#include <cyaml/cyaml_json_adapter.h>
#include <ctype.h>
#include <vstr.h>

Enum(OaMethod,
    (OA_GET, 1, "get"), (OA_PUT, 2, "put"), (OA_POST, 3, "post"),
    (OA_DELETE, 4, "delete"), (OA_OPTIONS, 5, "options"),
    (OA_HEAD, 6, "head"), (OA_PATCH, 7, "patch"), (OA_TRACE, 8, "trace")
);

/* Indices use the installed CMeta callable universe. No consumer-only ABI type
 * registrations or pointer captures: records stay borrowed through evaluation. */
typed(filter, value, bool, annotated_index, (int index)) { return index >= 0; }

static int put(json_value_t *object, const char *key, json_value_t *value) {
    if (value && json_object_add_checked(object, key, value)) return 1;
    json_free(value);
    return 0;
}

static const char *string_field(const json_value_t *object, const char *key) {
    const json_value_t *value = json_object_get(object, key);
    if (!value || json_type(value) != JSON_STRING) return NULL;
    const char *text = json_string(value);
    return text && strlen(text) == json_string_len(value) ? text : NULL;
}

oa_document *oa_document_create(const char *title, const char *version, oa_error *error) {
    if (!title || !*title || !version || !*version) { oa_fail(error, "title and version are required"); return NULL; }
    if (!vstr_utf8_valid(vstr_from_cstr(title)) || !vstr_utf8_valid(vstr_from_cstr(version))) {
        oa_fail(error, "title and version must be valid UTF-8"); return NULL;
    }
    oa_document *document = calloc(1, sizeof(*document));
    json_value_t *info = json_create_object();
    if (!document || !info) { free(document); json_free(info); oa_fail(error, "out of memory"); return NULL; }
    document->root = json_create_object();
    if (!put(info, "title", json_create_string(title)) || !put(info, "version", json_create_string(version))) {
        json_free(info); goto fail;
    }
    if (!document->root || !put(document->root, "openapi", json_create_string("3.1.0"))) {
        json_free(info); goto fail;
    }
    if (!put(document->root, "info", info) || !put(document->root, "paths", json_create_object())) goto fail;
    return document;
fail:
    oa_document_free(document); oa_fail(error, "out of memory"); return NULL;
}

void oa_document_free(oa_document *document) {
    if (document) { json_free(document->root); free(document); }
}

static int response_valid(const json_value_t *responses) {
    if (!responses || json_type(responses) != JSON_OBJECT || !json_object_size(responses)) return 0;
    int has_response = 0;
    for (size_t i = 0; i < json_object_size(responses); ++i) {
        const char *code = json_object_key(responses, i);
        if (!code || strlen(code) != json_object_key_len(responses, i)) return 0;
        has_response = 1;
        if (strcmp(code, "default") &&
            !(strlen(code) == 3 && code[0] >= '1' && code[0] <= '5' &&
              ((isdigit((unsigned char)code[1]) && isdigit((unsigned char)code[2])) ||
               (code[1] == 'X' && code[2] == 'X')))) return 0;
        const json_value_t *response = json_object_value(responses, i);
        if (json_type(response) != JSON_OBJECT ||
            (!string_field(response, "description") && !string_field(response, "$ref"))) return 0;
    }
    return has_response;
}

static size_t operation_count(const json_value_t *paths) {
    size_t count = 0;
    for (size_t i = 0; i < json_object_size(paths); ++i)
        count += json_object_size(json_object_value(paths, i));
    return count;
}

static int id_exists(const json_value_t *paths, const char *id) {
    for (size_t i = 0; i < json_object_size(paths); ++i) {
        const json_value_t *path = json_object_value(paths, i);
        for (size_t j = 0; j < json_object_size(path); ++j) {
            const char *other = string_field(json_object_value(path, j), "operationId");
            if (other && !strcmp(other, id)) return 1;
        }
    }
    return 0;
}

/* O(path length * parameter count), O(1) storage; both sides must describe the same inputs. */
static int path_parameters_valid(const char *path, const json_value_t *operation) {
    const json_value_t *params = json_object_get(operation, "parameters");
    for (const char *p = path; *p; ++p) {
        if (*p == '}') return 0;
        if (*p != '{') continue;
        const char *end = strchr(p + 1, '}');
        if (!end || end == p + 1) return 0;
        for (const char *name = p + 1; name < end; ++name)
            if (*name == '{' || *name == '/') return 0;
        int found = 0;
        for (size_t i = 0; i < json_array_size(params); ++i) {
            const json_value_t *param = json_array_get(params, i);
            const char *name = string_field(param, "name");
            if (!strcmp(string_field(param, "in"), "path") &&
                strlen(name) == (size_t)(end - p - 1) && !memcmp(name, p + 1, (size_t)(end - p - 1))) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
        p = end;
    }
    for (size_t i = 0; i < json_array_size(params); ++i) {
        const json_value_t *param = json_array_get(params, i);
        if (strcmp(string_field(param, "in"), "path")) continue;
        const char *name = string_field(param, "name");
        const size_t length = strlen(name);
        int found = 0;
        for (const char *p = strchr(path, '{'); p; p = strchr(p + 1, '{')) {
            const char *end = strchr(p + 1, '}');
            if ((size_t)(end - p - 1) == length && !memcmp(p + 1, name, length)) { found = 1; break; }
        }
        if (!found) return 0;
    }
    return 1;
}

/* Both paths have validated braces. O(path length), O(1) storage. */
static int same_path_template(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        if (*a == '{') { a = strchr(a, '}'); b = strchr(b, '}'); }
        ++a;
        ++b;
    }
    return *a == *b;
}

static int append_record(json_value_t *root, const oa_record *record, oa_error *error) {
    json_value_t *envelope = oa_comment_parse(record, error);
    if (!envelope) return 0;
    int ok = 0;
    const char *path = string_field(envelope, "path");
    const char *method = string_field(envelope, "method");
    json_value_t *operation = json_object_get(envelope, "operation");
    OaMethod method_value;
    if (!path || path[0] != '/' || strpbrk(path, " \t\r\n?#") ||
        !method || !EnumParse(OaMethod, method, &method_value) ||
        strcmp(method, EnumString(OaMethod, method_value)) ||
        !operation || json_type(operation) != JSON_OBJECT) {
        oa_fail(error, "%s: @route requires a supported HTTP method and /path", record->name); goto cleanup;
    }
    if (!response_valid(json_object_get(operation, "responses"))) {
        oa_fail(error, "%s: at least one @response with a valid status and description is required", record->name); goto cleanup;
    }
    if (!path_parameters_valid(path, operation)) {
        oa_fail(error, "%s: path templates and declared path parameters must match", record->name); goto cleanup;
    }
    const char *id = string_field(operation, "operationId");
    if (json_object_get(operation, "operationId") && (!id || !*id)) {
        oa_fail(error, "%s: operationId must be a nonempty string", record->name); goto cleanup;
    }
    if (!id) id = record->name;
    json_value_t *paths = json_object_get(root, "paths");
    for (size_t i = 0; i < json_object_size(paths); ++i) {
        const char *other = json_object_key(paths, i);
        if (strcmp(path, other) && same_path_template(path, other)) {
            oa_fail(error, "%s: equivalent path templates with different parameter names", record->name); goto cleanup;
        }
    }
    json_value_t *path_item = json_object_get(paths, path);
    if ((path_item && json_object_get(path_item, method)) || id_exists(paths, id)) {
        oa_fail(error, "%s: duplicate route or operationId", record->name); goto cleanup;
    }
    if (operation_count(paths) >= OA_MAX_OPERATIONS) {
        oa_fail(error, "document exceeds %u operations", OA_MAX_OPERATIONS); goto cleanup;
    }
    if (!path_item) {
        path_item = json_create_object();
        if (!put(paths, path, path_item)) goto oom;
    }
    json_value_t *copy = json_clone(operation);
    if (!copy) goto oom;
    if (!json_object_get(copy, "operationId") && !put(copy, "operationId", json_create_string(id))) {
        json_free(copy); goto oom;
    }
    if (!put(path_item, method, copy)) goto oom;
    ok = 1;
    goto cleanup;
oom:
    oa_fail(error, "out of memory constructing operation");
cleanup:
    json_free(envelope);
    return ok;
}

int oa_document_add(oa_document *document, oa_plugin *plugin,
                    const char *source, size_t length, oa_error *error) {
    if (!document || !plugin || !source || length > OA_MAX_SOURCE || memchr(source, 0, length))
        return oa_fail(error, "invalid source arguments (maximum 16 MiB, no NUL bytes)");
    size_t invalid_offset = vstr_utf8_invalid_offset(vstr_from_buf(source, length));
    if (invalid_offset != VSTR_NPOS)
        return oa_fail(error, "source is not valid UTF-8 at byte %zu", invalid_offset);
    oa_records records = {0};
    cflow_stream pipeline = {0};
    cflow_result selected = {0};
    json_value_t *next = NULL;
    int *indices = NULL;
    int ok = 0;
    if (!oa_extract(plugin, source, length, &records, error)) return 0;
    if (!records.count) { oa_records_free(&records); return 1; }
    indices = malloc(records.count * sizeof(*indices));
    if (!indices) { oa_fail(error, "out of memory"); goto cleanup; }
    for (size_t i = 0; i < records.count; ++i)
        indices[i] = oa_comment_is_api(records.items[i].doc) ? (int)i : -1;
    if (!cflow_stream_init(&pipeline, &cmeta_type_int) ||
        !pipeline.filter(&pipeline, annotated_index) || !cflow_stream_ok(&pipeline) ||
        !cflow_eval_array(cflow_stream_graph(&pipeline), indices, records.count, &selected)) {
        oa_fail(error, "CFlow annotation selection failed"); goto cleanup;
    }
    next = json_clone(document->root);
    if (!next) { oa_fail(error, "out of memory"); goto cleanup; }
    for (size_t i = 0; i < selected.count; ++i)
        if (!append_record(next, &records.items[((const int *)selected.data)[i]], error)) goto cleanup;
    json_free(document->root);
    document->root = next;
    next = NULL;
    ok = 1;
cleanup:
    json_free(next);
    free(indices);
    cflow_result_destroy(&selected);
    cflow_stream_destroy(&pipeline);
    oa_records_free(&records);
    return ok;
}

/* Explicit tags preserve boolean types and exponent numbers across YAML readers.
 * Spans belong to the same document and remain valid if its source grows. */
static int yaml_scalar_tags(cyaml_doc_t *yaml, cyaml_node_t *node,
                              const json_value_t *value, unsigned depth) {
    if (!node || depth > 256) return 0;
    switch (json_type(value)) {
    case JSON_STRING:
        /* Installed CYaml scalar emission uses C strings. Reject embedded NUL
         * instead of returning successfully truncated YAML. JSON supports it. */
        return strlen(json_string(value)) == json_string_len(value);
    case JSON_BOOL: {
        cyaml_node_t *tag = cyaml_new_cstr(yaml, "!!bool");
        if (!tag) return 0;
        node->tag = tag->span;
        return 1;
    }
    case JSON_NUMBER: {
        size_t size;
        const char *number = json_number_text(value, &size);
        if (!number) return 0;
        if (memchr(number, 'e', size) || memchr(number, 'E', size)) {
            cyaml_node_t *tag = cyaml_new_cstr(yaml, "!!float");
            if (!tag) return 0;
            node->tag = tag->span;
        }
        return 1;
    }
    case JSON_ARRAY:
        for (size_t i = 0; i < json_array_size(value); ++i)
            if (!yaml_scalar_tags(yaml, cyaml_seq_get(node, (uint32_t)i), json_array_get(value, i), depth + 1)) return 0;
        break;
    case JSON_OBJECT:
        for (size_t i = 0; i < json_object_size(value); ++i) {
            if (strlen(json_object_key(value, i)) != json_object_key_len(value, i)) return 0;
            cyaml_pair_t *pair = cyaml_map_at(node, (uint32_t)i);
            if (!pair || !yaml_scalar_tags(yaml, pair->val, json_object_value(value, i), depth + 1)) return 0;
        }
        break;
    default: break;
    }
    return 1;
}

char *oa_document_render(const oa_document *document, const char *format,
                         size_t *length, oa_error *error) {
    if (length) *length = 0;
    if (!document || !format || !length) { oa_fail(error, "invalid render arguments"); return NULL; }
    char *text = NULL;
    if (!strcmp(format, "json")) {
        char *serialized = json_serialize_pretty(document->root, length);
        if (serialized) { text = oa_copy(serialized, *length); json_serialize_free(serialized); }
    } else if (!strcmp(format, "yaml")) {
        cyaml_doc_t *yaml = cyaml_doc_from_json_value(document->root);
        if (yaml) {
            if (yaml_scalar_tags(yaml, cyaml_root(yaml), document->root, 0))
                text = cyaml_emit(yaml, NULL, length);
            cyaml_free(yaml);
        }
    } else { oa_fail(error, "format must be json or yaml"); return NULL; }
    if (!text) { *length = 0; oa_fail(error, "document serialization failed"); }
    return text;
}
void oa_text_free(char *text) { free(text); }
