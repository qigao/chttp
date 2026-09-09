#include "internal.h"
#include <ctype.h>

/* Strip comment delimiters and conventional leading '*' decoration, preserving
 * documentation text and newlines. Parsing the source as C prevents strings that
 * merely look like comments from becoming API declarations. */
static char *comment_text(const char *source, TSNode doc) {
    uint32_t start = ts_node_start_byte(doc), end = ts_node_end_byte(doc);
    if (end - start < 2) return NULL;
    int block = source[start + 1] == '*';
    start += 2;
    if (block) { if (end < start + 2) return NULL; end -= 2; }
    char *text = malloc((size_t)(end - start) + 1);
    if (!text) return NULL;
    size_t out = 0;
    int line_start = 1;
    for (uint32_t i = start; i < end; ++i) {
        char ch = source[i];
        if (line_start) {
            if (ch == ' ' || ch == '\t' || ch == '\r') continue;
            if (block && ch == '*') { line_start = 0; continue; }
            line_start = 0;
        }
        text[out++] = ch;
        if (ch == '\n') line_start = 1;
    }
    text[out] = 0;
    return text;
}

void oa_records_free(oa_records *records) {
    for (size_t i = 0; i < records->count; ++i) {
        free(records->items[i].name);
        free(records->items[i].doc);
    }
    free(records->items);
    memset(records, 0, sizeof(*records));
}

static int resolve_name(const oa_plugin *plugin, TSNode scope, TSNode *name, oa_error *error) {
    if (!plugin->name_query) { *name = scope; return 1; }
    TSQueryCursor *cursor = ts_query_cursor_new();
    if (!cursor) return oa_fail(error, "out of memory");
    ts_query_cursor_set_match_limit(cursor, OA_MAX_OPERATIONS);
    ts_query_cursor_exec(cursor, plugin->name_query, scope);
    TSQueryMatch match;
    uint32_t capture_index;
    int found = ts_query_cursor_next_capture(cursor, &match, &capture_index);
    if (found) *name = match.captures[capture_index].node;
    ts_query_cursor_delete(cursor);
    return found ? 1 : oa_fail(error, "function name query produced no symbol");
}

int oa_extract(oa_plugin *plugin, const char *source, size_t length,
               oa_records *records, oa_error *error) {
    TSParser *parser = ts_parser_new();
    TSTree *tree = NULL;
    TSQueryCursor *cursor = NULL;
    int ok = 0;
    if (!parser || !ts_parser_set_language(parser, plugin->language)) {
        oa_fail(error, "cannot initialize language parser"); goto cleanup;
    }
    tree = ts_parser_parse_string(parser, NULL, source, (uint32_t)length);
    if (!tree || ts_node_has_error(ts_tree_root_node(tree))) {
        oa_fail(error, "source contains syntax errors"); goto cleanup;
    }
    cursor = ts_query_cursor_new();
    if (!cursor) { oa_fail(error, "out of memory"); goto cleanup; }
    ts_query_cursor_set_match_limit(cursor, OA_MAX_OPERATIONS);
    ts_query_cursor_exec(cursor, plugin->query, ts_tree_root_node(tree));
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        TSNode function = {0}, name = {0}, doc = {0};
        for (uint16_t i = 0; i < match.capture_count; ++i) {
            TSQueryCapture capture = match.captures[i];
            if (capture.index == plugin->function_id) function = capture.node;
            else if (capture.index == plugin->name_id) name = capture.node;
            else if (capture.index == plugin->doc_id) doc = capture.node;
        }
        if (ts_node_is_null(function) || ts_node_is_null(name) || ts_node_is_null(doc)) {
            oa_fail(error, "query match lacks required captures"); goto cleanup;
        }
        uint32_t doc_end = ts_node_end_byte(doc), function_start = ts_node_start_byte(function);
        if (doc_end > function_start) { oa_fail(error, "documentation must precede function"); goto cleanup; }
        int adjacent = 1;
        for (uint32_t i = doc_end; i < function_start; ++i)
            if (!isspace((unsigned char)source[i])) { adjacent = 0; break; }
        if (!adjacent) continue;
        if (!resolve_name(plugin, name, &name, error)) goto cleanup;
        if (records->count == OA_MAX_OPERATIONS) { oa_fail(error, "too many documented functions"); goto cleanup; }
        oa_record item = {0};
        item.name = oa_copy(source + ts_node_start_byte(name), ts_node_end_byte(name) - ts_node_start_byte(name));
        item.doc = comment_text(source, doc);
        oa_record *items = realloc(records->items, (records->count + 1) * sizeof(*items));
        if (items) records->items = items;
        if (!item.name || !item.doc || !items) {
            free(item.name); free(item.doc); oa_fail(error, "out of memory"); goto cleanup;
        }
        records->items[records->count++] = item;
    }
    if (ts_query_cursor_did_exceed_match_limit(cursor)) { oa_fail(error, "query match limit exceeded"); goto cleanup; }
    ok = 1;
cleanup:
    if (cursor) ts_query_cursor_delete(cursor);
    if (tree) ts_tree_delete(tree);
    if (parser) ts_parser_delete(parser);
    if (!ok) oa_records_free(records);
    return ok;
}
