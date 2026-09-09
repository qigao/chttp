#include "internal.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* Native module operations stay in this host adapter. The installed Salts
 * Platform API has no public dynamic-library service to reuse. */
static void module_close(void *module) {
    if (!module) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)module);
#else
    dlclose(module);
#endif
}

oa_plugin *oa_plugin_register(const oa_language_plugin *provider, oa_error *error) {
    if (!provider || provider->abi_version != OA_PLUGIN_ABI_V1 ||
        provider->struct_size < sizeof(oa_language_plugin)) {
        oa_fail(error, "incompatible language plugin ABI"); return NULL;
    }
    if (!provider->name || !*provider->name || !provider->language || !provider->query) {
        oa_fail(error, "incomplete language plugin descriptor"); return NULL;
    }
    const TSLanguage *language = provider->language();
    if (!language || ts_language_abi_version(language) > TREE_SITTER_LANGUAGE_VERSION ||
        ts_language_abi_version(language) < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION) {
        oa_fail(error, "incompatible Tree-sitter grammar ABI"); return NULL;
    }
    size_t query_length = strlen(provider->query);
    if (query_length > 1024 * 1024) {
        oa_fail(error, "plugin query exceeds 1 MiB"); return NULL;
    }
    uint32_t offset = 0;
    TSQueryError code;
    TSQuery *query = ts_query_new(language, provider->query, (uint32_t)query_length, &offset, &code);
    if (!query) { oa_fail(error, "invalid plugin query at byte %u (error %d)", offset, (int)code); return NULL; }
    oa_plugin *plugin = calloc(1, sizeof(*plugin));
    if (!plugin) { ts_query_delete(query); oa_fail(error, "out of memory"); return NULL; }
    plugin->query = query;
    plugin->language = language;
    plugin->function_id = plugin->name_id = plugin->doc_id = UINT32_MAX;
    for (uint32_t i = 0; i < ts_query_capture_count(query); ++i) {
        uint32_t len;
        const char *name = ts_query_capture_name_for_id(query, i, &len);
        if (len == 8 && !memcmp(name, "function", len)) plugin->function_id = i;
        else if (len == 4 && !memcmp(name, "name", len)) plugin->name_id = i;
        else if (len == 3 && !memcmp(name, "doc", len)) plugin->doc_id = i;
        else { oa_fail(error, "unsupported plugin query capture"); goto fail; }
    }
    if (plugin->function_id == UINT32_MAX || plugin->name_id == UINT32_MAX || plugin->doc_id == UINT32_MAX) {
        oa_fail(error, "query must capture function, name and doc"); goto fail;
    }
    for (uint32_t i = 0; i < ts_query_pattern_count(query); ++i) {
        uint32_t count;
        ts_query_predicates_for_pattern(query, i, &count);
        if (count) { oa_fail(error, "query predicates are not supported by plugin ABI v1"); goto fail; }
    }
    if (provider->name_query) {
        size_t length = strlen(provider->name_query);
        if (length > 1024 * 1024) { oa_fail(error, "name query exceeds 1 MiB"); goto fail; }
        plugin->name_query = ts_query_new(language, provider->name_query, (uint32_t)length, &offset, &code);
        if (!plugin->name_query || ts_query_capture_count(plugin->name_query) != 1) {
            oa_fail(error, "invalid plugin name query"); goto fail;
        }
        uint32_t capture_length;
        const char *capture_name = ts_query_capture_name_for_id(plugin->name_query, 0, &capture_length);
        if (capture_length != 4 || memcmp(capture_name, "name", 4)) {
            oa_fail(error, "name query must capture name"); goto fail;
        }
        for (uint32_t i = 0; i < ts_query_pattern_count(plugin->name_query); ++i) {
            uint32_t count;
            ts_query_predicates_for_pattern(plugin->name_query, i, &count);
            if (count) { oa_fail(error, "name query predicates are unsupported"); goto fail; }
        }
    }
    return plugin;
fail:
    oa_plugin_close(plugin);
    return NULL;
}

oa_plugin *oa_plugin_open(const char *path, oa_error *error) {
    if (!path || !*path) { oa_fail(error, "plugin path is required"); return NULL; }
    void *module;
    oa_plugin_entry entry = NULL;
#ifdef _WIN32
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide = count ? malloc((size_t)count * sizeof(*wide)) : NULL;
    if (!wide) { oa_fail(error, "invalid UTF-8 plugin path or out of memory"); return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count);
    DWORD absolute_count = GetFullPathNameW(wide, 0, NULL, NULL);
    wchar_t *absolute = absolute_count ? malloc((size_t)absolute_count * sizeof(*absolute)) : NULL;
    if (!absolute || !GetFullPathNameW(wide, absolute_count, absolute, NULL)) {
        free(absolute); free(wide); oa_fail(error, "cannot resolve plugin path"); return NULL;
    }
    module = (void *)LoadLibraryExW(absolute, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    free(absolute);
    free(wide);
    if (module) entry = (oa_plugin_entry)GetProcAddress((HMODULE)module, "openapi_language_plugin_v1");
#else
    char *relative = NULL;
    if (!strchr(path, '/')) {
        size_t n = strlen(path);
        relative = malloc(n + 3);
        if (!relative) { oa_fail(error, "out of memory"); return NULL; }
        memcpy(relative, "./", 2); memcpy(relative + 2, path, n + 1);
    }
    module = dlopen(relative ? relative : path, RTLD_NOW | RTLD_LOCAL);
    free(relative);
    if (module) { void *symbol = dlsym(module, "openapi_language_plugin_v1"); memcpy(&entry, &symbol, sizeof(entry)); }
#endif
    if (!module) { oa_fail(error, "cannot load plugin: %s", path); return NULL; }
    if (!entry) { module_close(module); oa_fail(error, "missing openapi_language_plugin_v1 export"); return NULL; }
    oa_plugin *plugin = oa_plugin_register(entry(), error);
    if (!plugin) { module_close(module); return NULL; }
    plugin->module = module;
    return plugin;
}

void oa_plugin_close(oa_plugin *plugin) {
    if (!plugin) return;
    ts_query_delete(plugin->query);
    if (plugin->name_query) ts_query_delete(plugin->name_query);
    module_close(plugin->module);
    free(plugin);
}
