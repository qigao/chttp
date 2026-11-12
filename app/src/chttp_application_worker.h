#ifndef CHTTP_APPLICATION_WORKER_H
#define CHTTP_APPLICATION_WORKER_H
/* Internal buffered Controller prototype for the migration in
 * docs/HTTP_APPLICATION_DESIGN.md. Not installed; not an alternate public API.
 * Only empty Controller declarations (no owner callbacks), no Sessions/assets/
 * custom owner admission are admitted. The completed replacement removes this
 * temporary private mounting surface when public Invocation admission is ready. */
#include <chttp_app/application.h>
#include "chttp_application_work.h"

typedef struct chttp_application_worker_binding {
  const char *component_id;
  const cmeta_data_desc *receiver;
  chttp_method method;
  const char *path;
  chttp_application_work_fn invoke;
  chttp_application_work_completion_fn after_completion;
  const chttp_application_work_chain_hook *hooks;
  size_t hook_count;
} chttp_application_worker_binding;
/* Control-thread only, before the first start attempt. Binding metadata/code
 * and hook contexts are borrowed through Application close. Receiver ownership
 * is resolved from the canonical ACE graph; callers cannot provide a raw Bean. */
int chttp_application_worker_prepare(chttp_application *, const chttp_application_work_config *);
int chttp_application_worker_mount(chttp_application *, const chttp_application_worker_binding *);
int chttp_application_worker_current(const chttp_application *);
#endif
