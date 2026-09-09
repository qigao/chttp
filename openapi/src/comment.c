#include "internal.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>

typedef struct comment_builder {
    json_value_t *envelope, *operation, *body_schema;
    json_value_t *text_object;
    const char *text_key;
    const oa_record *record;
    oa_error *error;
    size_t line;
    int explicit_description;
} comment_builder;

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    size_t len = strlen(text);
    while (len && isspace((unsigned char)text[len - 1])) text[--len] = 0;
    return text;
}

static char *word(char **rest) {
    char *start = *rest;
    while (isspace((unsigned char)*start)) ++start;
    if (!*start) { *rest = start; return NULL; }
    char *end = start;
    while (*end && !isspace((unsigned char)*end)) ++end;
    if (*end) *end++ = 0;
    while (isspace((unsigned char)*end)) ++end;
    *rest = end;
    return start;
}

static int tag_is(const char *text, const char *tag) {
    size_t len = strlen(tag);
    return !strncmp(text, tag, len) && (!text[len] || isspace((unsigned char)text[len]));
}

int oa_comment_is_api(const char *text) {
    while (*text) {
        while (*text == ' ' || *text == '\t' || *text == '\r') ++text;
        if (tag_is(text, "@route") || tag_is(text, "@response") || tag_is(text, "@produces") ||
            tag_is(text, "@body") || tag_is(text, "@field") || tag_is(text, "@openapi")) return 1;
        const char *next = strchr(text, '\n');
        if (!next) break;
        text = next + 1;
    }
    return 0;
}

static int fail(comment_builder *b, const char *message) {
    return oa_fail(b->error, "%s: comment line %zu: %s", b->record->name, b->line, message);
}

/* These two helpers always consume the new value, including on failure. */
static int put(json_value_t *object, const char *key, json_value_t *value) {
    if (value && json_object_add_checked(object, key, value)) return 1;
    json_free(value); return 0;
}
static int push(json_value_t *array, json_value_t *value) {
    if (value && json_array_add_checked(array, value)) return 1;
    json_free(value); return 0;
}
static json_value_t *child(json_value_t *object, const char *key, json_type_t type) {
    json_value_t *value = json_object_get(object, key);
    if (value) return json_type(value) == type ? value : NULL;
    value = type == JSON_ARRAY ? json_create_array() : json_create_object();
    return put(object, key, value) ? value : NULL;
}

static int text_field(comment_builder *b, json_value_t *object, const char *key,
                       const char *text, int append) {
    if (!*text) return fail(b, "text argument is required");
    const char *old = json_get_string(object, key);
    if (old && !append) return fail(b, "duplicate text tag");
    char *joined = NULL;
    if (old) {
        size_t length = strlen(old) + strlen(text) + 2;
        joined = malloc(length);
        if (!joined) return fail(b, "out of memory");
        snprintf(joined, length, "%s\n%s", old, text);
        text = joined;
    }
    int ok = put(object, key, json_create_string(text));
    free(joined);
    if (!ok) return fail(b, "out of memory");
    b->text_object = object;
    b->text_key = key;
    return 1;
}

static json_value_t *schema(const char *type) {
    if (!type) return NULL;
    char base[128];
    size_t length = strlen(type);
    if (length >= sizeof(base)) return NULL;
    memcpy(base, type, length + 1);
    unsigned arrays = 0;
    while (length >= 2 && !strcmp(base + length - 2, "[]")) {
        if (++arrays > 8) return NULL;
        base[length -= 2] = 0;
    }
    const char *name = base;
    if (!strcmp(base, "int")) name = "integer";
    else if (!strcmp(base, "char*")) name = "string";
    else if (!strcmp(base, "double") || !strcmp(base, "float")) name = "number";
    else if (!strcmp(base, "bool")) name = "boolean";
    if (strcmp(name, "string") && strcmp(name, "integer") && strcmp(name, "number") &&
        strcmp(name, "boolean") && strcmp(name, "object")) return NULL;
    json_value_t *value = json_create_object();
    if (!value || !put(value, "type", json_create_string(name))) { json_free(value); return NULL; }
    while (arrays--) {
        json_value_t *array = json_create_object();
        if (!array || !put(array, "type", json_create_string("array"))) { json_free(array); json_free(value); return NULL; }
        if (!put(array, "items", value)) { json_free(array); return NULL; }
        value = array;
    }
    return value;
}

static int required_flag(const char *token, int *required) {
    if (!token) return 0;
    *required = !strcmp(token, "required");
    return *required || !strcmp(token, "optional");
}

static int http_token_char(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

static int http_token(const char **cursor) {
    const char *start = *cursor;
    while (http_token_char((unsigned char)**cursor)) ++*cursor;
    return *cursor != start;
}

static int parameter(comment_builder *b, char *args) {
    char *name = word(&args), *location = word(&args), *type = word(&args), *flag = word(&args);
    int required;
    if (!name || !location || !required_flag(flag, &required) || !*args ||
        (strcmp(location, "query") && strcmp(location, "path") && strcmp(location, "header") && strcmp(location, "cookie")))
        return fail(b, "@param expects name location type required|optional description");
    if (!strcmp(location, "path") && !required) return fail(b, "path parameters must be required");
    if (!strcmp(location, "header") || !strcmp(location, "cookie")) {
        const char *end = name;
        if (!http_token(&end) || *end)
            return fail(b, "header and cookie parameter names must be HTTP tokens");
    }
    json_value_t *params = child(b->operation, "parameters", JSON_ARRAY);
    if (!params) return fail(b, "out of memory");
    for (size_t i = 0; i < json_array_size(params); ++i) {
        json_value_t *p = json_array_get(params, i);
        if (!strcmp(json_get_string(p, "name"), name) && !strcmp(json_get_string(p, "in"), location))
            return fail(b, "duplicate parameter");
    }
    json_value_t *p = json_create_object();
    if (!p || !put(p, "name", json_create_string(name)) || !put(p, "in", json_create_string(location)) ||
        !put(p, "required", json_create_bool(required)) || !put(p, "schema", schema(type)) ||
        !text_field(b, p, "description", args, 0)) {
        json_free(p); return fail(b, "invalid parameter type or allocation failure");
    }
    return push(params, p) ? 1 : fail(b, "out of memory");
}

typedef enum constraint_kind {
    CONSTRAINT_NUMBER, CONSTRAINT_POSITIVE, CONSTRAINT_COUNT,
    CONSTRAINT_BOOL, CONSTRAINT_TEXT, CONSTRAINT_ENUM
} constraint_kind;

typedef struct schema_constraint {
    const char *keyword;
    const char *type;
    constraint_kind kind;
} schema_constraint;

static const schema_constraint constraints[] = {
    {"minimum", "number", CONSTRAINT_NUMBER},
    {"maximum", "number", CONSTRAINT_NUMBER},
    {"exclusiveMinimum", "number", CONSTRAINT_NUMBER},
    {"exclusiveMaximum", "number", CONSTRAINT_NUMBER},
    {"multipleOf", "number", CONSTRAINT_POSITIVE},
    {"minLength", "string", CONSTRAINT_COUNT},
    {"maxLength", "string", CONSTRAINT_COUNT},
    {"pattern", "string", CONSTRAINT_TEXT},
    {"format", NULL, CONSTRAINT_TEXT},
    {"minItems", "array", CONSTRAINT_COUNT},
    {"maxItems", "array", CONSTRAINT_COUNT},
    {"uniqueItems", "array", CONSTRAINT_BOOL},
    {"minProperties", "object", CONSTRAINT_COUNT},
    {"maxProperties", "object", CONSTRAINT_COUNT},
    {"additionalProperties", "object", CONSTRAINT_BOOL},
    {"enum", NULL, CONSTRAINT_ENUM}
};

/* Exact integer limit of the double-backed comparisons used by this generator. */
#define OA_MAX_EXACT_INTEGER 9007199254740991.0
#define OA_MAX_ENUM_VALUES 256u

static json_value_t *parameter_schema(comment_builder *b, const char *name, const char *location) {
    json_value_t *params = json_object_get(b->operation, "parameters"), *target = NULL;
    for (size_t i = 0; i < json_array_size(params); ++i) {
        json_value_t *p = json_array_get(params, i);
        if (strcmp(json_get_string(p, "name"), name) ||
            (location && strcmp(json_get_string(p, "in"), location))) continue;
        if (target) { fail(b, "ambiguous parameter name; qualify its location"); return NULL; }
        target = json_object_get(p, "schema");
    }
    return target;
}

/* O(parameters + array depth), O(1) storage; resolve only already-declared schemas. */
static json_value_t *constraint_target(comment_builder *b, char *name) {
    size_t length = strlen(name), arrays = 0;
    while (length >= 2 && !strcmp(name + length - 2, "[]")) {
        ++arrays;
        name[length -= 2] = 0;
    }
    json_value_t *target;
    if (!strcmp(name, "body")) {
        target = b->body_schema;
    } else if (!strncmp(name, "body.", strlen("body."))) {
        target = json_object_get(json_object_get(b->body_schema, "properties"), name + strlen("body."));
    } else {
        char *dot = strchr(name, '.');
        const char *location = NULL;
        if (dot) {
            *dot = 0;
            if (!strcmp(name, "query") || !strcmp(name, "path") ||
                !strcmp(name, "header") || !strcmp(name, "cookie")) location = name;
            else *dot = '.';
        }
        target = parameter_schema(b, location ? dot + 1 : name, location);
    }
    while (target && arrays) {
        target = json_object_get(target, "items");
        --arrays;
    }
    if (!target && (!b->error || !b->error->message[0])) fail(b, "constraint target is not declared");
    return target;
}

static int finite_number(comment_builder *b, const char *text, double *number) {
    char *end;
    errno = 0;
    *number = strtod(text, &end);
    if (end == text || *end || errno || !isfinite(*number)) return fail(b, "expected a finite numeric value");
    return 1;
}

/* Keep the parser-owned decimal token through cloning and both serializers. */
static json_value_t *decimal_value(comment_builder *b, const char *text) {
    json_value_t *value = json_parse(text, strlen(text));
    if (!value || json_type(value) != JSON_NUMBER) {
        json_free(value);
        fail(b, "expected a JSON decimal number");
        return NULL;
    }
    return value;
}

static json_value_t *constraint_value(comment_builder *b, const schema_constraint *rule, const char *text) {
    if (rule->kind == CONSTRAINT_TEXT) return json_create_string(text);
    if (rule->kind == CONSTRAINT_BOOL) {
        if (strcmp(text, "true") && strcmp(text, "false")) { fail(b, "expected true or false"); return NULL; }
        return json_create_bool(!strcmp(text, "true"));
    }
    if (rule->kind == CONSTRAINT_COUNT) {
        for (const char *p = text; *p; ++p)
            if (*p < '0' || *p > '9') { fail(b, "expected a nonnegative decimal integer"); return NULL; }
    }
    double number;
    if (!finite_number(b, text, &number)) return NULL;
    if ((rule->kind == CONSTRAINT_COUNT && number > OA_MAX_EXACT_INTEGER) ||
        (rule->kind == CONSTRAINT_POSITIVE && number <= 0)) {
        fail(b, "constraint value is outside the supported range"); return NULL;
    }
    return rule->kind == CONSTRAINT_COUNT ? json_create_number(number) : decimal_value(b, text);
}

static json_value_t *enum_value(comment_builder *b, const char *type, const char *text) {
    if (!strcmp(type, "string")) return json_create_string(text);
    if (!strcmp(type, "boolean")) {
        if (strcmp(text, "true") && strcmp(text, "false")) { fail(b, "expected a boolean enum member"); return NULL; }
        return json_create_bool(!strcmp(text, "true"));
    }
    if (strcmp(type, "integer") && strcmp(type, "number")) {
        fail(b, "enum supports scalar string, numeric or boolean schemas"); return NULL;
    }
    if (!strcmp(type, "integer")) {
        const char *digits = *text == '-' ? text + 1 : text;
        if (!*digits) { fail(b, "integer enum member requires decimal digits"); return NULL; }
        for (const char *p = digits; *p; ++p)
            if (*p < '0' || *p > '9') { fail(b, "integer enum member requires decimal digits"); return NULL; }
    }
    double number;
    if (!finite_number(b, text, &number)) return NULL;
    if (!strcmp(type, "integer") && fabs(number) > OA_MAX_EXACT_INTEGER) {
        fail(b, "integer enum member exceeds the exact integer range"); return NULL;
    }
    return !strcmp(type, "integer") ? json_create_number(number) : decimal_value(b, text);
}

/* Duplicate checks are O(n) per member, O(n^2) overall, bounded by OA_MAX_ENUM_VALUES. */
static int append_enum(comment_builder *b, json_value_t *target, const char *text) {
    json_value_t *values = child(target, "enum", JSON_ARRAY);
    if (!values) return fail(b, "out of memory");
    if (json_array_size(values) >= OA_MAX_ENUM_VALUES) return fail(b, "enum exceeds 256 members");
    json_value_t *value = enum_value(b, json_get_string(target, "type"), text);
    if (!value) {
        if (!b->error || !b->error->message[0]) fail(b, "out of memory");
        return 0;
    }
    for (size_t i = 0; i < json_array_size(values); ++i) {
        const json_value_t *old = json_array_get(values, i);
        if ((json_type(value) == JSON_STRING && !strcmp(json_string(old), json_string(value))) ||
            (json_type(value) == JSON_NUMBER && json_number(old) == json_number(value)) ||
            (json_type(value) == JSON_BOOL && json_bool(old) == json_bool(value))) {
            json_free(value); return fail(b, "duplicate enum member");
        }
    }
    return push(values, value) ? 1 : fail(b, "out of memory");
}

static int apply_constraint(comment_builder *b, const schema_constraint *rule, char *args) {
    char *name = word(&args);
    if (!name || !*args) return fail(b, "constraint expects target and value");
    json_value_t *target = constraint_target(b, name);
    if (!target) return 0;
    const char *type = json_get_string(target, "type");
    if (!type || (rule->type && strcmp(rule->type, type) &&
        (strcmp(rule->type, "number") || strcmp(type, "integer"))))
        return fail(b, "constraint does not apply to the declared schema type");
    if (rule->kind == CONSTRAINT_ENUM) return append_enum(b, target, args);
    if (json_object_get(target, rule->keyword)) return fail(b, "duplicate schema constraint");
    json_value_t *value = constraint_value(b, rule, args);
    if (!value) {
        if (!b->error || !b->error->message[0]) fail(b, "out of memory");
        return 0;
    }
    return put(target, rule->keyword, value) ? 1 : fail(b, "out of memory");
}

static int response(comment_builder *b, char *args) {
    char *code = word(&args);
    if (!code || !*args) return fail(b, "@response expects status description");
    json_value_t *responses = child(b->operation, "responses", JSON_OBJECT);
    if (!responses) return fail(b, "out of memory");
    if (json_object_get(responses, code)) return fail(b, "duplicate response");
    json_value_t *r = child(responses, code, JSON_OBJECT);
    return r ? text_field(b, r, "description", args, 0) : fail(b, "out of memory");
}

static int media_parameter_value(const char **cursor) {
    if (**cursor != '"') return http_token(cursor);
    ++*cursor;
    while (**cursor && **cursor != '"') {
        if (**cursor == '\\') ++*cursor;
        unsigned char ch = (unsigned char)**cursor;
        if (ch < ' ' || ch == '\x7f') return 0;
        ++*cursor;
    }
    if (**cursor != '"') return 0;
    ++*cursor;
    return 1;
}

/* RFC 9110 sections 5.6.2, 5.6.6 and 8.3.1; O(bytes), O(1) storage.
 * Annotation arguments are whitespace-delimited, so this accepts compact media
 * parameters only. Validation preserves the content key exactly as authored. */
static int media_type_valid(const char *media) {
    if (!media || !http_token(&media) || *media != '/') return 0;
    ++media;
    if (!http_token(&media)) return 0;
    while (*media) {
        if (*media++ != ';') return 0;
        if (!*media || *media == ';') continue;
        if (!http_token(&media) || *media != '=') return 0;
        ++media;
        if (!media_parameter_value(&media)) return 0;
    }
    return 1;
}

static int content_schema(comment_builder *b, json_value_t *owner, const char *media, const char *type) {
    if (!media_type_valid(media))
        return fail(b, "invalid media type (expected type/subtype with compact parameters)");
    json_value_t *content = child(owner, "content", JSON_OBJECT);
    if (!content) return fail(b, "out of memory");
    if (json_object_get(content, media)) return fail(b, "duplicate media type");
    json_value_t *entry = child(content, media, JSON_OBJECT);
    return entry && put(entry, "schema", schema(type)) ? 1 : fail(b, "invalid schema type or allocation failure");
}

static int produces(comment_builder *b, char *args) {
    char *code = word(&args), *media = word(&args), *type = word(&args);
    if (!code || !type || *args) return fail(b, "@produces expects status media-type type");
    json_value_t *r = json_object_get(json_object_get(b->operation, "responses"), code);
    return r ? content_schema(b, r, media, type) : fail(b, "declare @response before @produces");
}

static int body(comment_builder *b, char *args) {
    char *media = word(&args), *type = word(&args), *flag = word(&args);
    int required;
    if (!media || !required_flag(flag, &required) || !*args) return fail(b, "@body expects media-type type required|optional description");
    if (json_object_get(b->operation, "requestBody")) return fail(b, "duplicate body");
    json_value_t *request = child(b->operation, "requestBody", JSON_OBJECT);
    if (!request || !put(request, "required", json_create_bool(required)) ||
        !content_schema(b, request, media, type) || !text_field(b, request, "description", args, 0)) return 0;
    b->body_schema = json_object_get(json_object_get(json_object_get(request, "content"), media), "schema");
    return 1;
}

static int field(comment_builder *b, char *args) {
    char *name = word(&args), *type = word(&args), *flag = word(&args);
    int required;
    if (!name || !required_flag(flag, &required) || !*args) return fail(b, "@field expects name type required|optional description");
    if (!b->body_schema || strcmp(json_get_string(b->body_schema, "type"), "object")) return fail(b, "@field requires a preceding object body");
    json_value_t *properties = child(b->body_schema, "properties", JSON_OBJECT);
    if (!properties) return fail(b, "out of memory");
    if (json_object_get(properties, name)) return fail(b, "duplicate body field");
    json_value_t *value = schema(type);
    if (!put(properties, name, value)) return fail(b, "invalid field type or allocation failure");
    if (!text_field(b, value, "description", args, 0)) return 0;
    if (required && !push(child(b->body_schema, "required", JSON_ARRAY), json_create_string(name))) return fail(b, "out of memory");
    return 1;
}

static int parse_tag(comment_builder *b, char *line) {
    char *args = line, *tag = word(&args);
    b->text_object = NULL;
    b->text_key = NULL;
    if (!strcmp(tag, "@openapi")) return fail(b, "JSON/YAML annotations are unsupported; use @route and documentation tags");
    if (!strcmp(tag, "@route")) {
        char *method = word(&args), *path = word(&args);
        if (!method || !path || *args) return fail(b, "@route expects METHOD /path");
        if (json_object_get(b->envelope, "path")) return fail(b, "duplicate route tag");
        for (char *p = method; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        return put(b->envelope, "method", json_create_string(method)) && put(b->envelope, "path", json_create_string(path));
    }
    if (!strcmp(tag, "@brief") || !strcmp(tag, "@summary")) return text_field(b, b->operation, "summary", args, 0);
    if (!strcmp(tag, "@details") || !strcmp(tag, "@description")) {
        if (b->explicit_description++) return fail(b, "duplicate description tag");
        return text_field(b, b->operation, "description", args, 1);
    }
    if (!strcmp(tag, "@operationId")) {
        char *name = word(&args);
        if (!name || *args || json_object_get(b->operation, "operationId")) return fail(b, "operationId expects one unique identifier");
        return put(b->operation, "operationId", json_create_string(name));
    }
    if (!strcmp(tag, "@tag")) {
        if (!*args) return fail(b, "tag text required");
        json_value_t *tags = child(b->operation, "tags", JSON_ARRAY);
        if (!tags) return fail(b, "out of memory");
        for (size_t i = 0; i < json_array_size(tags); ++i)
            if (!strcmp(json_string(json_array_get(tags, i)), args)) return fail(b, "duplicate tag");
        return push(tags, json_create_string(args));
    }
    if (!strcmp(tag, "@deprecated")) {
        if ((strcmp(args, "true") && strcmp(args, "false")) || json_object_get(b->operation, "deprecated")) return fail(b, "deprecated expects true or false, once");
        return put(b->operation, "deprecated", json_create_bool(!strcmp(args, "true")));
    }
    if (!strcmp(tag, "@param")) return parameter(b, args);
    for (size_t i = 0; i < sizeof(constraints) / sizeof(constraints[0]); ++i)
        if (!strcmp(tag + 1, constraints[i].keyword)) return apply_constraint(b, &constraints[i], args);
    if (!strcmp(tag, "@response")) return response(b, args);
    if (!strcmp(tag, "@produces")) return produces(b, args);
    if (!strcmp(tag, "@body")) return body(b, args);
    if (!strcmp(tag, "@field")) return field(b, args);
    return fail(b, "unsupported documentation tag");
}

json_value_t *oa_comment_parse(const oa_record *record, oa_error *error) {
    comment_builder b = {0};
    if (error) error->message[0] = 0;
    b.record = record; b.error = error;
    char *copy = oa_copy(record->doc, strlen(record->doc));
    b.envelope = json_create_object();
    if (!copy || !b.envelope || !(b.operation = child(b.envelope, "operation", JSON_OBJECT))) {
        oa_fail(error, "out of memory parsing documentation"); goto failure;
    }
    for (char *line = copy; line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        line = trim(line);
        ++b.line;
        if (*line) {
            if (*line == '@') {
                if (!parse_tag(&b, line)) {
                    if (error && !error->message[0]) fail(&b, "out of memory");
                    goto failure;
                }
            } else {
                json_value_t *owner = b.text_object ? b.text_object : b.operation;
                const char *key = b.text_key ? b.text_key : "description";
                if (!text_field(&b, owner, key, line, 1)) goto failure;
            }
        }
        line = next;
    }
    if (!json_object_get(b.envelope, "path")) { fail(&b, "API documentation requires @route"); goto failure; }
    free(copy);
    return b.envelope;
failure:
    free(copy);
    json_free(b.envelope);
    return NULL;
}
