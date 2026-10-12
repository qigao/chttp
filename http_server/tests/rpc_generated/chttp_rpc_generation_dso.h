#ifndef CHTTP_RPC_GENERATION_DSO_H
#define CHTTP_RPC_GENERATION_DSO_H

#include <salts/plugin_decl.h>
#include <data_bind_native_binding.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* One shared probe ABI across the host test and the actual loaded DSO. */
typedef struct chttp_rpc_generation_dso_probe {
  atomic_int enabled;
  atomic_int entered;
  atomic_int release;
} chttp_rpc_generation_dso_probe;

typedef struct chttp_rpc_generation_dso_offer {
  DataBindNativeInvokeFn invoke;
} chttp_rpc_generation_dso_offer;

#define CHTTP_RPC_GENERATION_DSO_EXPORT "chttp.rpc.dso.invoke"
#define CHTTP_RPC_GENERATION_DSO_CONTRACT "test.chttp.rpc.dso.invoke.v1"
enum { CHTTP_RPC_GENERATION_DSO_VERSION = 1 };

static const cmeta_type_identity CHTTP_RPC_DSO_OFFER_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.rpc.dso.offer.v1");
static const cmeta_type_identity CHTTP_RPC_DSO_OFFER_CONST =
    CMETA_TYPE_ID_CONST_INIT(&CHTTP_RPC_DSO_OFFER_ID);
static const cmeta_type_identity CHTTP_RPC_DSO_OFFER_POINTER_ID =
    CMETA_TYPE_ID_POINTER_INIT(&CHTTP_RPC_DSO_OFFER_CONST);
static const cmeta_type_desc CHTTP_RPC_DSO_OFFER_TYPE = {
    "const chttp_rpc_generation_dso_offer",
    sizeof(chttp_rpc_generation_dso_offer),
    CMETA_ALIGNOF(chttp_rpc_generation_dso_offer),
    CMETA_T_OBJECT, NULL, NULL, &CHTTP_RPC_DSO_OFFER_CONST};
static const cmeta_type_desc CHTTP_RPC_DSO_OFFER_POINTER_TYPE = {
    "const chttp_rpc_generation_dso_offer *",
    sizeof(const chttp_rpc_generation_dso_offer *),
    CMETA_ALIGNOF(const chttp_rpc_generation_dso_offer *),
    CMETA_T_POINTER, &CHTTP_RPC_DSO_OFFER_TYPE, NULL,
    &CHTTP_RPC_DSO_OFFER_POINTER_ID};

/* A reflected function export is the only way for the test to obtain the
 * provider's callback address. The registry's borrow is kept until mount. */
Function0InvokeDeclAsAbiResult(
    value, const chttp_rpc_generation_dso_offer *,
    &CHTTP_RPC_DSO_OFFER_POINTER_TYPE, CMETA_ABI_OBJECT_POINTER,
    CMETA_RESULT_BORROWED, chttp_rpc_generation_dso_get_offer);

#endif
