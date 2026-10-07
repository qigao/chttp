#ifndef CHTTP_CLIENT_RESPONSE_SCOPE_H
#define CHTTP_CLIENT_RESPONSE_SCOPE_H

#include <http_client/http.h>
#include <cmeta/data_reflect.h>
#include <cmeta/scope.h>

CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES((unsigned int *)0, uint32_t *),
    "HTTP response metadata requires the native uint32 carrier");

/* Read-only summary; omitted pointers remain owned by the response. This is
 * not a complete value descriptor and grants no decode/copy authority. */
cmeta_reflect_data(chttp_response, "chttp.response",
    cmeta_data_field(unsigned int, http_major, &cmeta_data_uint32, &cmeta_type_uint32)
    cmeta_data_field(unsigned int, http_minor, &cmeta_data_uint32, &cmeta_type_uint32)
    cmeta_data_field(unsigned int, status_code, &cmeta_data_uint32, &cmeta_type_uint32)
    cmeta_data_field(size_t, header_count, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, body_size, &cmeta_data_size, &cmeta_type_size)
    cmeta_field(int, protocol_keep_alive)
);

/** Initialize non-NULL raw storage; never overwrite a live owning response.
 * Returns CMETA_OK without allocation. Release live storage with destroy(). */
static inline cmeta_status chttp_response_scope_init(chttp_response *response) {
  const chttp_response empty = {0};
  *response = empty;
  return CMETA_OK;
}

/** Transfer ownership into a distinct zero response; source becomes zero.
 * Neither response may be concurrently accessed or copied as an owning value.
 * Views of transferred payloads remain valid until the new owner is destroyed.
 */
static inline void chttp_response_move(
    chttp_response *destination, chttp_response *source) {
  const chttp_response empty = {0};
  *destination = *source;
  *source = empty;
}

/* Only response storage has a no-fail restore. Client stop/drain is fallible
 * and deliberately remains outside this lifecycle. Use cmeta_scope with
 * (chttp_response, name); returning from its body always restores the value.
 * Move into a zero caller-owned response to keep a result beyond that scope.
 */
CMETA_DEFINE_LIFECYCLE(chttp_response, cmeta_reflected_storage(chttp_response),
    chttp_response_scope_init, chttp_response_destroy, chttp_response_move,
    CMETA_LIFECYCLE_INIT_NOFAIL | CMETA_LIFECYCLE_MOVABLE);

#endif
