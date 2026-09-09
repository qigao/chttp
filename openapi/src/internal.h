#ifndef OA_INTERNAL_H
#define OA_INTERNAL_H
#include <openapi/generator.h>
#include <tree_sitter/api.h>
#include <json_parser.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OA_MAX_SOURCE (16u * 1024u * 1024u)
#define OA_MAX_OPERATIONS 4096u
struct oa_plugin {
    void *module;
    const TSLanguage *language;
    TSQuery *query;
    TSQuery *name_query;
    uint32_t function_id, name_id, doc_id;
};
struct oa_document { json_value_t *root; };
typedef struct oa_record { char *name; char *doc; } oa_record;
typedef struct oa_records { oa_record *items; size_t count; } oa_records;

static inline int oa_fail(oa_error *error, const char *format, ...) {
    if (error) {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return 0;
}
static inline char *oa_copy(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy) { memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}
int oa_extract(oa_plugin *plugin, const char *source, size_t length,
               oa_records *records, oa_error *error);
void oa_records_free(oa_records *records);
int oa_comment_is_api(const char *text);
json_value_t *oa_comment_parse(const oa_record *record, oa_error *error);
#endif
