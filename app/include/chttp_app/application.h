#ifndef CHTTP_APP_APPLICATION_H
#define CHTTP_APP_APPLICATION_H

#include <chttp_app/interceptor.h>
#include <salts/component.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_application { void *impl; } chttp_application;

/** Deployment-owned hook context/code must outlive application_close().
 * Names are unique; no_store is reserved for the built-in cache policy. */
typedef struct chttp_application_policy {
  const char *name;
  chttp_service_interceptor_hook hook;
} chttp_application_policy;

typedef struct chttp_application_component_diagnostic {
  salts_component_status status;
  salts_component_failure failure;
} chttp_application_component_diagnostic;

typedef struct chttp_application_options {
  size_t size;
  chttp_server_config server;
  chttp_service_config service;
  const chttp_application_policy *policies;
  size_t policy_count;
  /** Optional READY graph. Borrowed storage/providers remain live until close.
   * Init resolves/starts it, failure rolls it back, close stops it last.
   * This manual graph path does not bind generated service receivers. */
  salts_component_context *components;
  /** Auto-DI graph inputs, mutually exclusive with components. The host copies
   * deployment/selection rows; provider metadata, selection strings and provider
   * contexts and Interface descriptors stay borrowed through close. Config values follow Component's
   * create-only borrow contract. Generated service consumers are appended.
   * At most 128 total components and 4096 dependency edges; no global registry. */
  const salts_component_deployment *providers;
  size_t provider_count;
  const salts_component_selection *selections;
  size_t selection_count;
  /** Optional init-only output, preserving graph failure details after rollback. */
  chttp_application_component_diagnostic *component_diagnostic;
} chttp_application_options;

/** Bounded loopback HTTP defaults; port 0 selects an ephemeral port.
 * route_capacity/method_capacity zero means the generated operation count. */
chttp_application_options chttp_application_options_default(void);

/** Generated immutable adapters; use chttp_app_target() to produce these.
 * Exact typed service calls remain owned and validated by DataBind/CMeta. */
typedef struct chttp_application_operation {
  const char *service;
  const char *operation;
  DataBindStatus (*binding)(DataBindNativeTypeBinding *, DataBindNativeTypeBinding *,
      DataBindServiceNativeBinding *, DataBindError *);
  const DataBindNativeExecution *(*execution)(void);
  const DataBindMessageNativeArtifact *(*request)(void);
  const salts_component_provider_binding *(*component)(void);
  cmeta_status (*bind_execution)(const cmeta_object_ref *, DataBindNativeExecution *);
} chttp_application_operation;

typedef struct chttp_application_definition {
  size_t size;
  DataBindStatus (*codec)(DataBind **, DataBindError *);
  const chttp_application_operation *operations;
  size_t operation_count;
  const DataBindHttpProjectionArtifact *http;
  const char *(*policy_at)(const char *, const char *, size_t);
} chttp_application_definition;

/** Prepare every route before listening. app must be {0}; do not copy live apps.
 * Definition/providers are borrowed through close; options and policy name
 * arrays are only read during init. Hooks retain borrowed contexts.
 * 1..1024 methods, at most 64 named policies and 16 hooks/method. Unknown policy
 * returns SALTS_ENOENT; duplicate/reserved names and malformed metadata return
 * SALTS_EINVAL. Auto-DI missing providers return SALTS_ENOENT; ambiguous/cyclic
 * graphs and provider failures return SALTS_EINVAL, capacity returns SALTS_ENOBUFS.
 * component_diagnostic preserves the precise graph status and failure phase.
 * Other errors propagate from Server/Service. A failed init leaves
 * app zero and unwinds all acquired resources. Components retain their failure
 * details. Only synchronous inline native execution is supported.
 * Example: chttp_schema_example_application_init(&app, &options);
 * chttp_application_start(&app); then chttp_application_close(&app, timeout_ms),
 * including after start failure. The generated header declares the init wrapper. */
int chttp_application_init(chttp_application *app,
    const chttp_application_definition *definition, const chttp_application_options *options);
/** Start a prepared app; propagates Server startup errors. Close is required
 * even on failed start. Repeated start follows the Server's EALREADY contract. */
int chttp_application_start(chttp_application *app);
/** Thread-safe port snapshot; zero for an invalid/unstarted application. */
uint16_t chttp_application_port(const chttp_application *app);
/** Thread-safe Server snapshot. Returns SALTS_EINVAL for NULL/uninitialized
 * arguments. Snapshots must not race close/destroy. */
int chttp_application_get_stats(const chttp_application *app, chttp_server_stats *stats);
/** Control-thread only; never call from a request hook. Stop/join server, release
 * Service/plans/codec, then stop components. A stop timeout preserves the entire
 * app for retry; zero app is already closed. Zero timeout waits without a time
 * limit. Operational errors may accompany successful cleanup (app becomes zero).
 * Serialize all lifecycle operations. */
int chttp_application_close(chttp_application *app, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
