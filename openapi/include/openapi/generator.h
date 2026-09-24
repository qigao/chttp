#ifndef OPENAPI_GENERATOR_H
#define OPENAPI_GENERATOR_H
#include <stddef.h>
#include <openapi/plugin.h>
#include <data_bind.h>
#include <data_bind_method_plan.h>

typedef struct oa_plugin oa_plugin;
typedef struct oa_document oa_document;
typedef struct oa_error { char message[512]; } oa_error;

/* All handles are owned; close/free accept NULL. Calls are synchronous.
 * One handle must not be used concurrently. Error text is caller-owned. */
oa_plugin *oa_plugin_open(const char *path, oa_error *error);
/* Provider is borrowed and must outlive the returned handle. Useful for static
 * registration and embedded consumers; performs identical ABI/query checks. */
oa_plugin *oa_plugin_register(const oa_language_plugin *provider, oa_error *error);
void oa_plugin_close(oa_plugin *plugin);
/* Title and version must be nonempty UTF-8 strings; invalid input returns NULL. */
oa_document *oa_document_create(const char *title, const char *version, oa_error *error);
void oa_document_free(oa_document *document);
/* Transactional: failure leaves the previous document intact. Source is borrowed
 * only during the call. Source must be UTF-8 with no NUL bytes; invalid encoding
 * reports a zero-based byte offset. Maximum 16 MiB/source and 4096 operations/document. */
int oa_document_add(oa_document *document, oa_plugin *plugin,
                    const char *source, size_t length, oa_error *error);

/**
 * Transactionally add operations from one canonical DataBind Service contract
 * plus a generated HTTP projection artifact.
 *
 * The provider reads no legacy HTTP annotations from the DataBind IDL.
 * Unsupported projection/schema shapes fail without mutating the document.
 */
int oa_document_add_databind_http(
    oa_document *document,
    DataBind *contract,
    const DataBindHttpProjectionArtifact *artifact,
    oa_error *error);

/* format is exactly "json" or "yaml". Caller releases successful text through
 * oa_text_free, never a different allocator. No partial output on failure.
 * YAML rejects embedded-NUL strings/keys unsupported by the installed emitter;
 * JSON preserves them. Failed serialization returns NULL and sets length to 0. */
char *oa_document_render(const oa_document *document, const char *format,
                         size_t *length, oa_error *error);
void oa_text_free(char *text);
#endif
