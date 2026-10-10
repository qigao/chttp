#ifndef CHTTP_WEBSOCKET_SERVER_INTERNAL_H
#define CHTTP_WEBSOCKET_SERVER_INTERNAL_H

#include <http_server/http.h>
#include <cnet/websocket.h>
#include <cnet/websocket_transport.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct chttp_server_impl chttp_server_impl;
typedef struct chttp_server_request_state chttp_server_request_state;
typedef struct chttp_server_route_record chttp_server_route_record;

typedef enum chttp_server_websocket_phase {
  CHTTP_SERVER_WEBSOCKET_NONE = 0,
  CHTTP_SERVER_WEBSOCKET_HANDSHAKE,
  CHTTP_SERVER_WEBSOCKET_OPEN
} chttp_server_websocket_phase;

typedef int (*chttp_server_websocket_write_fn)(void *transport, const uint8_t *data, size_t size);

typedef struct chttp_server_websocket_peer {
  cnet_websocket engine;
  cnet_websocket *session;
  cnet_websocket_transport bridge;
  mem_buffer_t *output_retained;
  unsigned char *output;
  size_t output_capacity;
  unsigned char *opening_data;
  size_t opening_capacity;
  size_t opening_size;
  int opening_kind;
  uint16_t opening_code;
  bool opening_pending;
  bool dedicated;
  bool receive_paused;
  bool receive_pending;
  bool tag_pending;
  bool commands_blocked;
  size_t tag_size;
  chttp_websocket handle;
  chttp_server_impl *server;
  chttp_server_route_record *route;
  cnet_connection connection;
  uint32_t server_slot;
  uint32_t server_generation;
  int32_t stream_id;
  chttp_server_websocket_write_fn write;
  void *transport;
  chttp_server_websocket_phase phase;
} chttp_server_websocket_peer;

int chttp_server_websocket_peer_init(chttp_server_websocket_peer *peer, chttp_server_impl *server,
                                     chttp_server_route_record *route, cnet_connection connection,
                                     uint32_t server_slot, uint32_t server_generation,
                                     int32_t stream_id,
                                     chttp_server_websocket_write_fn write, void *transport);
void chttp_server_websocket_peer_reset(chttp_server_websocket_peer *peer);
void chttp_server_websocket_peer_open(chttp_server_websocket_peer *peer);
int chttp_server_websocket_peer_feed(chttp_server_websocket_peer *peer, const void *data,
                                     size_t size);
int chttp_server_websocket_peer_flush(chttp_server_websocket_peer *peer);
void chttp_server_websocket_peer_transport_closed(chttp_server_websocket_peer *peer);
bool chttp_server_websocket_peer_terminal(const chttp_server_websocket_peer *peer);

int chttp_server_websocket_route_open(chttp_server_websocket_peer *peer,
                                      chttp_server_request_state *state,
                                      chttp_server_route_record *route,
                                      const chttp_server_request_view *request);

#endif /* CHTTP_WEBSOCKET_SERVER_INTERNAL_H */
