#include "tinytest.h"
#include "chttp_server_runtime.h"

#include <stdatomic.h>

spec("CHTTP deferred generation token") {
  it("rejects a stale generation without changing a reused pending slot") {
    const uint32_t stale_generation = 7u;
    const uint32_t current_generation = stale_generation + 1u;
    const uint_fast64_t current_pending =
        chttp_server_deferred_token(current_generation, CHTTP_SERVER_DEFERRED_PENDING);
    atomic_uint_fast64_t token;

    atomic_init(&token, current_pending);
    check_equal(chttp_server_deferred_claim(&token, stale_generation), SALTS_ENOENT);
    check_equal(atomic_load_explicit(&token, memory_order_acquire), current_pending);
    check_equal(chttp_server_deferred_claim(&token, current_generation), SALTS_OK);
    check_equal(chttp_server_deferred_token_generation(
                    atomic_load_explicit(&token, memory_order_acquire)),
                current_generation);
    check_equal(chttp_server_deferred_token_state(
                    atomic_load_explicit(&token, memory_order_acquire)),
                CHTTP_SERVER_DEFERRED_WRITING);
    check_equal(chttp_server_deferred_claim(&token, current_generation), SALTS_EALREADY);
  }
}
