#ifndef CHTTP_WEBSOCKET_HANDSHAKE_H
#define CHTTP_WEBSOCKET_HANDSHAKE_H

#include <chttp/chttp.h>

#include <stddef.h>

enum {
  CHTTP_WEBSOCKET_KEY_BYTES = 24,
  CHTTP_WEBSOCKET_ACCEPT_BYTES = 28,
  CHTTP_WEBSOCKET_KEY_CAPACITY = CHTTP_WEBSOCKET_KEY_BYTES + 1,
  CHTTP_WEBSOCKET_ACCEPT_CAPACITY = CHTTP_WEBSOCKET_ACCEPT_BYTES + 1
};

int chttp_websocket_accept_compute(const char *key, char *output, size_t output_capacity);
int chttp_websocket_client_key_generate(char *output, size_t output_capacity);
int chttp_websocket_client_handshake_validate(const void *data, size_t size,
                                              const char *expected_accept,
                                              const char *expected_subprotocol,
                                              unsigned int *out_http_status);
int chttp_websocket_server_handshake_validate(const chttp_server_request_view *request,
                                              char *accept, size_t accept_capacity,
                                              unsigned int *out_http_status);

#endif /* CHTTP_WEBSOCKET_HANDSHAKE_H */
