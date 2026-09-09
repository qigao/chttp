#ifndef CHTTP_H2_SERVER_H
#define CHTTP_H2_SERVER_H

#include "chttp_h2_proto.h"

#include <chttp/chttp.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct chttp_server_connection chttp_server_connection;
typedef struct chttp_h2_server_connection chttp_h2_server_connection;
typedef struct chttp_server_websocket_peer chttp_server_websocket_peer;

int chttp_h2_server_config_validate(const chttp_server_config *config,
                                    chttp_h2_proto_config *out_protocol_config);
int chttp_h2_server_connection_init(chttp_h2_server_connection **out_h2,
                                    chttp_server_connection *connection);
int chttp_h2_server_connection_prepare(chttp_h2_server_connection *h2);
void chttp_h2_server_connection_release(chttp_h2_server_connection *h2);
void chttp_h2_server_connection_destroy(chttp_h2_server_connection *h2);
void chttp_h2_server_connection_cancel_file_sources(chttp_h2_server_connection *h2);
int chttp_h2_server_connection_receive(chttp_h2_server_connection *h2, const void *data,
                                       size_t size);
int chttp_h2_server_connection_flush(chttp_h2_server_connection *h2);
int chttp_h2_server_connection_deferred_progress(chttp_h2_server_connection *h2);
bool chttp_h2_server_connection_has_deferred(const chttp_h2_server_connection *h2);
int chttp_h2_server_connection_begin_stop(chttp_h2_server_connection *h2);
bool chttp_h2_server_connection_draining(const chttp_h2_server_connection *h2);
bool chttp_h2_server_connection_stop_ready(const chttp_h2_server_connection *h2);
bool chttp_h2_server_connection_stop_waiting(const chttp_h2_server_connection *h2);
chttp_server_websocket_peer *chttp_h2_server_websocket_peer_find(chttp_h2_server_connection *h2,
                                                                 int32_t stream_id);

#endif /* CHTTP_H2_SERVER_H */
