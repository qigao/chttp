#ifndef OPENAPI_PLUGIN_H
#define OPENAPI_PLUGIN_H
#include <stdint.h>

/* Immutable, borrowed metadata. No allocator or runtime-specific structs cross
 * this boundary. The provider and its language stay live until module unload. */
typedef struct TSLanguage TSLanguage;
typedef struct oa_language_plugin {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *name;
    const TSLanguage *(*language)(void);
    const char *query;
    /* Optional second query, scoped to the main @name capture. Its first @name
     * capture identifies the symbol without language-specific host traversal. */
    const char *name_query;
} oa_language_plugin;

#define OA_PLUGIN_ABI_V1 1u
typedef const oa_language_plugin *(*oa_plugin_entry)(void);

#if defined(_WIN32)
#define OA_PLUGIN_EXPORT __declspec(dllexport)
#else
#define OA_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* Every module exports this exact symbol. Modules are trusted native code. */
OA_PLUGIN_EXPORT const oa_language_plugin *openapi_language_plugin_v1(void);
#endif
