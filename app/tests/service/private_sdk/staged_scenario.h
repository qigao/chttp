#ifndef CHTTP_PRIVATE_SERVICEHOST_SCENARIO_H
#define CHTTP_PRIVATE_SERVICEHOST_SCENARIO_H

/* Intentionally includes only staged candidate headers and the separately
 * installed release Salts SDK: no CHttp source or generated IDL headers.
 * This is a consumer-side ownership/link proof, not an SDK export. */
#include <salts/configurator.h>
#include <salts/service_host.h>
#include <stdio.h>

typedef struct staged_generation {
  salts_service_host_generation generation;
  salts_configurator_deployment deployments[1];
  salts_configurator_instance instances[1];
  salts_component_dependency dependencies[1];
  size_t activation_order[1];
  salts_service_host_module modules[1];
} staged_generation;

#define STAGED_CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "external candidate consumer %s:%d: %s\n", \
          __FILE__, __LINE__, #expr); return 1; } } while (0)

static int staged_generation_build(
    staged_generation *g, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref plugin) {
  const salts_service_host_generation_storage storage = {
    g->deployments, 1u, g->instances, 1u,
    g->dependencies, 1u, g->activation_order, 1u,
    g->modules, 1u
  };
  const salts_service_host_source source = {
    plugin, "component-provider", NULL, NULL
  };
  return salts_service_host_generation_build(
      &g->generation, id, registry, &storage,
      NULL, 0u, &source, 1u, NULL, 0u) == SALTS_SERVICE_HOST_OK
      ? 0 : 1;
}

/* Two distinct *staged* real Provider DSOs. Each accepts exactly one
 * generation. Scope originals must never move and every module lease remains
 * authoritative until drain. No derived legacy ABI compatibility route. */
static int staged_probe_run(int argc, char **argv) {
  const cmeta_plugin_registry_config config = {2u};
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_ref first = {0}, second = {0};
  staged_generation n = {0}, next = {0};
  salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
  salts_service_host_generation *previous = NULL;
  salts_service_host_scope original = SALTS_SERVICE_HOST_SCOPE_INIT;
  salts_service_host_scope successor = SALTS_SERVICE_HOST_SCOPE_INIT;
  cmeta_plugin_lifecycle_info info = {0};
  bool quiescent = false;

  STAGED_CHECK(argc == 3 && argv[1] != NULL && argv[2] != NULL);
  STAGED_CHECK(cmeta_plugin_registry_init(&registry, &config) ==
               CMETA_PLUGIN_OK);
  STAGED_CHECK(cmeta_plugin_registry_load(&registry, argv[1], &first) ==
               CMETA_PLUGIN_OK);
  STAGED_CHECK(cmeta_plugin_registry_load(&registry, argv[2], &second) ==
               CMETA_PLUGIN_OK);
  STAGED_CHECK(cmeta_plugin_registry_start(&registry, first) ==
               CMETA_PLUGIN_OK);
  STAGED_CHECK(cmeta_plugin_registry_start(&registry, second) ==
               CMETA_PLUGIN_OK);
  STAGED_CHECK(staged_generation_build(
               &n, UINT64_C(1), &registry, first) == 0);
  STAGED_CHECK(staged_generation_build(
               &next, UINT64_C(2), &registry, second) == 0);
  STAGED_CHECK(cmeta_plugin_registry_get_lifecycle(
               &registry, first, &info) == CMETA_PLUGIN_OK &&
               info.active_leases == 1u);
  STAGED_CHECK(salts_service_host_runtime_init(&runtime) ==
               SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(salts_service_host_runtime_publish(
               &runtime, &n.generation, &previous) == SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(previous == NULL);
  STAGED_CHECK(salts_service_host_scope_acquire(
               &runtime, &original) == SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(salts_service_host_scope_generation_id(&original) ==
               UINT64_C(1));

  /* Ownership bound to the original address: a copied live scope must not
   * decrement anything or permit early Plugin unload. */
  salts_service_host_scope illicit_copy = original;
  STAGED_CHECK(salts_service_host_scope_release(&illicit_copy) ==
               SALTS_SERVICE_HOST_INVALID_STATE);

  STAGED_CHECK(salts_service_host_runtime_publish(
               &runtime, &next.generation, &previous) == SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(previous == &n.generation);
  STAGED_CHECK(salts_service_host_generation_drain(
               &runtime, &n.generation) == SALTS_SERVICE_HOST_BUSY);
  STAGED_CHECK(cmeta_plugin_registry_get_lifecycle(
               &registry, first, &info) == CMETA_PLUGIN_OK &&
               info.active_leases == 1u);
  STAGED_CHECK(salts_service_host_scope_acquire(
               &runtime, &successor) == SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(salts_service_host_scope_generation_id(&successor) ==
               UINT64_C(2));
  STAGED_CHECK(salts_service_host_scope_release(&original) ==
               SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(salts_service_host_generation_drain(
               &runtime, &n.generation) == SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(cmeta_plugin_registry_get_lifecycle(
               &registry, first, &info) == CMETA_PLUGIN_OK &&
               info.active_leases == 0u);

  STAGED_CHECK(salts_service_host_runtime_close(
               &runtime, &previous) == SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(previous == &next.generation);
  STAGED_CHECK(salts_service_host_generation_drain(
               &runtime, &next.generation) == SALTS_SERVICE_HOST_BUSY);
  STAGED_CHECK(salts_service_host_scope_release(&successor) ==
               SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(salts_service_host_generation_drain(
               &runtime, &next.generation) == SALTS_SERVICE_HOST_OK);
  STAGED_CHECK(salts_service_host_runtime_destroy(&runtime) ==
               SALTS_SERVICE_HOST_OK);

  STAGED_CHECK(cmeta_plugin_registry_request_stop(&registry, first) ==
               CMETA_PLUGIN_OK);
  STAGED_CHECK(cmeta_plugin_registry_poll_quiescent(
               &registry, first, &quiescent) == CMETA_PLUGIN_OK && quiescent);
  STAGED_CHECK(cmeta_plugin_registry_unload(&registry, first) ==
               CMETA_PLUGIN_OK);
  quiescent = false;
  STAGED_CHECK(cmeta_plugin_registry_request_stop(&registry, second) ==
               CMETA_PLUGIN_OK);
  STAGED_CHECK(cmeta_plugin_registry_poll_quiescent(
               &registry, second, &quiescent) == CMETA_PLUGIN_OK && quiescent);
  STAGED_CHECK(cmeta_plugin_registry_unload(&registry, second) ==
               CMETA_PLUGIN_OK);
  STAGED_CHECK(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);
  puts("external staged ServiceHost C11/C++: DSO N->N+1 / BUSY / scope owner / drain / unload PASS");
  return 0;
}
#endif
