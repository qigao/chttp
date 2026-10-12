#include "chttp_rpc_generation_dso.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdbool.h>

/* Layout must match the test's DataBind native record projections. */
typedef struct chttp_rpc_generation_dso_request {
  uint32_t left, right, scale;
  uint8_t presence;
} chttp_rpc_generation_dso_request;
typedef struct chttp_rpc_generation_dso_response {
  uint32_t sum;
} chttp_rpc_generation_dso_response;

/* This code is compiled only into the fixture shared object. A host-side
 * replacement would not prove that Server pins executed DSO code on stop. */
static bool DATA_BIND_NATIVE_CALL chttp_rpc_generation_dso_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  chttp_rpc_generation_dso_probe *probe =
      (chttp_rpc_generation_dso_probe *)context;
  const chttp_rpc_generation_dso_request *request;
  chttp_rpc_generation_dso_response *response;
  if (return_storage == NULL || params == NULL || param_count != 2u ||
      params[0] == NULL || params[1] == NULL)
    return false;
  if (probe != NULL)
    atomic_fetch_add_explicit(&probe->invoked, 1, memory_order_acq_rel);
  if (probe != NULL &&
      atomic_load_explicit(&probe->enabled, memory_order_acquire)) {
    const uint64_t deadline = cmeta_monotonic_ms() + 3000u;
    atomic_fetch_add_explicit(&probe->entered, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(&probe->release, memory_order_acquire)) {
      if (cmeta_monotonic_ms() >= deadline) return false;
      cmeta_thread_yield();
    }
  }
  request = (const chttp_rpc_generation_dso_request *)params[0];
  response = (chttp_rpc_generation_dso_response *)params[1];
  if (request->left == 99u) {
    *(int *)return_storage = 7;
    return true;
  }
  response->sum = request->left + request->right * request->scale;
  *(int *)return_storage = 0;
  return true;
}

static const chttp_rpc_generation_dso_offer CHTTP_RPC_DSO_OFFER = {
    chttp_rpc_generation_dso_invoke};

const chttp_rpc_generation_dso_offer *
chttp_rpc_generation_dso_get_offer(void) {
  return &CHTTP_RPC_DSO_OFFER;
}

#define CHTTP_RPC_GENERATION_DSO_EXPORTS(X) \
  X(function, chttp_rpc_generation_dso_get_offer, \
    CHTTP_RPC_GENERATION_DSO_EXPORT, CHTTP_RPC_GENERATION_DSO_CONTRACT, \
    CHTTP_RPC_GENERATION_DSO_VERSION, 0)

CMETA_PLUGIN_DECLARE(
    chttp_rpc_generation_dso, "test.chttp.rpc.generation.dso", (1, 0, 0),
    CHTTP_RPC_GENERATION_DSO_EXPORTS, CMETA_PLUGIN_PASSIVE());
