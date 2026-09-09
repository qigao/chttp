#ifndef HTTP_COMMON_RPC_H
#define HTTP_COMMON_RPC_H

#include <cnet/cnet.h>
#include <salts/error_codes.h>
#include <stddef.h>
#include <stdint.h>
#include <cmeta/cmeta.h>
#include <cserde/reader.h>
#include <cserde/writer.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RPC method identity and optional CMeta semantic metadata.
 * The wire method is `service.name` when service is non-NULL, otherwise name.
 * A supplied callable is copied and bound during submit; generators are not
 * accepted by this unary request/response client.
 */
typedef struct crpc_method {
  const char *service;
  const char *name;
  const cmeta_callable *callable;
} crpc_method;

/** HTTP-carried RPC metadata. Content-Type and Accept are owned by CRPC. */
typedef struct crpc_metadata {
  const char *name;
  const char *value;
} crpc_metadata;

/**
 * Emit exactly one JSON-RPC params value as CSerde tokens. The root must be an
 * Array or Map. CRPC owns writer finalization; the callback must not finish it.
 */
typedef cserde_status (*crpc_encode_params_fn)(void *user, cserde_writer *writer);

/** Emits exactly one JSON value. CRPC owns writer finalization. */
typedef cserde_status (*crpc_encode_value_fn)(void *user, cserde_writer *writer);

typedef enum crpc_response_kind {
  CRPC_RESPONSE_RESULT = 1,
  CRPC_RESPONSE_REMOTE_ERROR
} crpc_response_kind;

/** JSON-RPC application error whose views follow the containing response lifetime. */
typedef struct crpc_remote_error {
  int64_t code;
  cserde_slice message;
  cserde_reader *data;
} crpc_remote_error;

/**
 * A protocol-valid JSON-RPC response. The result/data reader is single-pass,
 * callback-scoped, and borrows the parsed response owner.
 */
typedef struct crpc_response_view {
  uint64_t request_id;
  unsigned int http_status;
  crpc_response_kind kind;
  const cmeta_callable *callable;
  union {
    cserde_reader *result;
    crpc_remote_error remote_error;
  } value;
} crpc_response_view;

#ifdef __cplusplus
}
#endif

#endif /* HTTP_COMMON_RPC_H */
