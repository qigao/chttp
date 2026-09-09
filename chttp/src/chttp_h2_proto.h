/**
 * @file chttp_h2_proto.h
 * @brief Private bounded HTTP/2 protocol engine (RFC 9113).
 *
 * Migrated from the legacy HTTP repository commit
 * 38f1e389b3f94909db6cb2482a8cbc16522e7e4f. Owns the wire state machine
 * (preface, SETTINGS, frame dispatch, HPACK, stream/flow-control windows,
 * GOAWAY/PING/RST) and
 * drives the application through callbacks. Supports client and server modes;
 * server mode is used by the public server adapter and in-process test peers.
 */

#ifndef CHTTP_H2_PROTO_H
#define CHTTP_H2_PROTO_H

#include "chttp_h2_hpack.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_h2_proto_s chttp_h2_proto;

typedef enum { CHTTP_H2_PROTO_CLIENT = 0, CHTTP_H2_PROTO_SERVER = 1 } chttp_h2_proto_mode;

enum {
  CHTTP_H2_PROTO_HPACK_FIELD_OVERHEAD_BYTES = 15,
  CHTTP_H2_PROTO_HPACK_PENDING_UPDATE_BYTES = 12
};

/* RFC 9113 §7 error codes (wire values; identical to the codes nghttp2
 * exposes, so the session layer maps them 1:1). */
#define CHTTP_H2_ERR_NO_ERROR 0x0u
#define CHTTP_H2_ERR_PROTOCOL_ERROR 0x1u
#define CHTTP_H2_ERR_INTERNAL_ERROR 0x2u
#define CHTTP_H2_ERR_FLOW_CONTROL_ERROR 0x3u
#define CHTTP_H2_ERR_SETTINGS_TIMEOUT 0x4u
#define CHTTP_H2_ERR_STREAM_CLOSED 0x5u
#define CHTTP_H2_ERR_FRAME_SIZE_ERROR 0x6u
#define CHTTP_H2_ERR_REFUSED_STREAM 0x7u
#define CHTTP_H2_ERR_CANCEL 0x8u
#define CHTTP_H2_ERR_COMPRESSION_ERROR 0x9u
#define CHTTP_H2_ERR_CONNECT_ERROR 0xau
#define CHTTP_H2_ERR_ENHANCE_YOUR_CALM 0xbu
#define CHTTP_H2_ERR_INADEQUATE_SECURITY 0xcu
#define CHTTP_H2_ERR_HTTP_1_1_REQUIRED 0xdu

/* RFC 9113 §6.5.2 SETTINGS identifiers. */
#define CHTTP_H2_SETTING_HEADER_TABLE_SIZE 0x1u
#define CHTTP_H2_SETTING_ENABLE_PUSH 0x2u
#define CHTTP_H2_SETTING_MAX_CONCURRENT_STREAMS 0x3u
#define CHTTP_H2_SETTING_INITIAL_WINDOW_SIZE 0x4u
#define CHTTP_H2_SETTING_MAX_FRAME_SIZE 0x5u
#define CHTTP_H2_SETTING_MAX_HEADER_LIST_SIZE 0x6u
/* RFC 8441 §3: peer allows extended CONNECT (used by server mode to advertise,
 * by client mode to gate :protocol requests). */
#define CHTTP_H2_SETTING_ENABLE_CONNECT_PROTOCOL 0x8u

enum {
  CHTTP_H2_PROTO_DATA_OK = 0,
  /** The current DATA frame was accepted, but later input must wait for an external sink. */
  CHTTP_H2_PROTO_DATA_PAUSE = 1
};

typedef struct chttp_h2_proto_callbacks_s {
  void *user_data;
  /* A header block (request for server, response for client) started. */
  int (*on_begin_headers)(void *ud, int32_t stream_id);
  /* name/value are borrowed views valid during the call. */
  int (*on_header)(void *ud, int32_t stream_id, const char *name, size_t name_len,
                   const char *value, size_t value_len);
  int (*on_end_headers)(void *ud, int32_t stream_id, int end_stream);
  /** Returns DATA_OK, DATA_PAUSE, or a negative value for a connection error. */
  int (*on_data)(void *ud, int32_t stream_id, const uint8_t *data, size_t len);
  int (*on_stream_close)(void *ud, int32_t stream_id, uint32_t error_code);
  void (*on_settings)(void *ud, const uint32_t *ids, const uint32_t *values, size_t count);
  /* Peer acknowledged our SETTINGS (SETTINGS with ACK flag). */
  void (*on_settings_ack)(void *ud);
  /* Peer acknowledged a PING; opaque is borrowed for this callback only. */
  void (*on_ping_ack)(void *ud, const uint8_t opaque[8]);
  /* Peer sent WINDOW_UPDATE (after it was applied). */
  void (*on_window_update)(void *ud, int32_t stream_id, uint32_t increment);
  void (*on_goaway)(void *ud, uint32_t last_stream_id, uint32_t error_code);
  void (*on_rst_received)(void *ud, int32_t stream_id, uint32_t error_code);
  /* Peer sent WINDOW_UPDATE: send windows grew, writer should flush. */
  void (*on_wake_write)(void *ud);
} chttp_h2_proto_callbacks;

/** Every capacity is a positive hard bound; NULL selects conservative defaults. */
typedef struct chttp_h2_proto_config {
  size_t stream_capacity;
  size_t output_buffer_bytes;
  size_t input_buffer_bytes;
  size_t header_block_bytes;
  size_t max_header_list_bytes;
  size_t hpack_dynamic_table_bytes;
  size_t max_hpack_string_bytes;
  size_t max_settings_count;
} chttp_h2_proto_config;

int chttp_h2_proto_config_valid(const chttp_h2_proto_config *config);
chttp_h2_proto *chttp_h2_proto_create(chttp_h2_proto_mode mode, const chttp_h2_proto_config *config,
                                      const chttp_h2_proto_callbacks *callbacks);
void chttp_h2_proto_destroy(chttp_h2_proto *p);

/* Local settings we advertise (must be set before the first send for the
 * client; the server reads them when it starts). */
int chttp_h2_proto_set_local_settings(chttp_h2_proto *p, uint32_t header_table_size,
                                      uint32_t enable_push, uint32_t max_concurrent_streams,
                                      uint32_t initial_window_size, uint32_t max_frame_size,
                                      uint32_t max_header_list_size);

/* Effective peer limits after SETTINGS exchange. */
uint32_t chttp_h2_proto_peer_max_concurrent_streams(chttp_h2_proto *p);
int chttp_h2_proto_settings_acked(const chttp_h2_proto *p);
/* Optional DATA payload cap (0 = use the peer MAX_FRAME_SIZE).  Lets the
 * caller emit smaller DATA frames (e.g. streaming tests). */
void chttp_h2_proto_set_send_chunk(chttp_h2_proto *p, size_t chunk);
uint32_t chttp_h2_proto_peer_initial_window_size(chttp_h2_proto *p);
/* True after the peer's mandatory initial SETTINGS frame was accepted. */
int chttp_h2_proto_peer_settings_received(const chttp_h2_proto *p);
/* Server mode: advertise SETTINGS_ENABLE_CONNECT_PROTOCOL (RFC 8441) so the
 * peer may open extended CONNECT streams. */
void chttp_h2_proto_set_local_enable_connect_protocol(chttp_h2_proto *p, uint32_t enable);
uint32_t chttp_h2_proto_peer_enable_connect_protocol(chttp_h2_proto *p);

/* ── Submit (send side) ───────────────────────────────────────────── */

typedef enum chttp_h2_proto_source_status {
  CHTTP_H2_PROTO_SOURCE_DATA = 0,
  CHTTP_H2_PROTO_SOURCE_WAIT,
  CHTTP_H2_PROTO_SOURCE_EOF,
  CHTTP_H2_PROTO_SOURCE_ERROR
} chttp_h2_proto_source_status;

typedef struct chttp_h2_proto_source_result {
  chttp_h2_proto_source_status status;
  size_t size;
} chttp_h2_proto_source_result;

/* Streaming request/response-body source: fill buf[0..len). DATA requires
 * size in 1..len, WAIT commits no frame and suspends future pulls until
 * resume_source, EOF emits END_STREAM, and ERROR resets the stream. Called
 * from the writer lane; it must not block or re-enter the engine. */
typedef chttp_h2_proto_source_result (*chttp_h2_proto_source_fn)(void *user_data, uint8_t *buf,
                                                                 size_t len);

/* Client: submit a request with optional stream priority (RFC 9113 §5.3) and
 * either a flat borrowed body (body != NULL, source == NULL) or a streaming
 * source (source != NULL, body == NULL).  weight 1..256; 0 = default (16).
 * dep_stream_id < 1 = no dependency.  On success *out_stream_id is set. */
int chttp_h2_proto_submit_request_ex(chttp_h2_proto *p, const chttp_h2_hpack_header *hdrs,
                                     size_t count, const uint8_t *body, size_t body_len,
                                     chttp_h2_proto_source_fn source, void *source_ud,
                                     uint32_t weight, int32_t dep_stream_id, int exclusive,
                                     int32_t *out_stream_id);

/* Client: submit a request.  body/body_len optional; body is borrowed and
 * must stay valid until fully sent.  On success *out_stream_id is set. */
int chttp_h2_proto_submit_request(chttp_h2_proto *p, const chttp_h2_hpack_header *hdrs,
                                  size_t count, const uint8_t *body, size_t body_len,
                                  int32_t *out_stream_id);
/* Client: open a request stream with an explicit send-side state.  With
 * end_stream == 0, later DATA calls carry a long-lived tunnel body. */
int chttp_h2_proto_submit_request_headers(chttp_h2_proto *p, const chttp_h2_hpack_header *hdrs,
                                          size_t count, int end_stream, int32_t *out_stream_id);
/* Server: submit a bare HEADERS block (informational 1xx, or a response whose
 * body/END_STREAM is driven by later submit_data/submit_trailers calls). */
int chttp_h2_proto_submit_headers(chttp_h2_proto *p, int32_t stream_id,
                                  const chttp_h2_hpack_header *hdrs, size_t count, int end_stream);
/* Server: submit a response (HEADERS, optional body, optional END_STREAM). */
int chttp_h2_proto_submit_response(chttp_h2_proto *p, int32_t stream_id,
                                   const chttp_h2_hpack_header *hdrs, size_t count,
                                   const uint8_t *body, size_t body_len);
int chttp_h2_proto_submit_response_ex(chttp_h2_proto *p, int32_t stream_id,
                                      const chttp_h2_hpack_header *hdrs, size_t count,
                                      const uint8_t *body, size_t body_len,
                                      chttp_h2_proto_source_fn source, void *source_ud);
/* Either endpoint: append DATA to an open local send side. */
int chttp_h2_proto_submit_data(chttp_h2_proto *p, int32_t stream_id, const uint8_t *data,
                               size_t len, int end_stream);
/* Server: append trailers to a response. */
int chttp_h2_proto_submit_trailers(chttp_h2_proto *p, int32_t stream_id,
                                   const chttp_h2_hpack_header *hdrs, size_t count);

int chttp_h2_proto_submit_settings(chttp_h2_proto *p);
int chttp_h2_proto_submit_ping(chttp_h2_proto *p, const uint8_t opaque[8]);
int chttp_h2_proto_submit_window_update(chttp_h2_proto *p, int32_t stream_id, uint32_t increment);
int chttp_h2_proto_submit_rst_stream(chttp_h2_proto *p, int32_t stream_id, uint32_t error_code);
int chttp_h2_proto_submit_goaway(chttp_h2_proto *p, uint32_t last_stream_id, uint32_t error_code);

/** Resumes one source suspended by a WAIT result. */
int chttp_h2_proto_resume_source(chttp_h2_proto *p, int32_t stream_id);

/** True while a DATA consumer owns bytes and inbound frame dispatch is suspended. */
int chttp_h2_proto_input_paused(const chttp_h2_proto *p);
/** Releases a matching DATA pause and processes already retained bounded input. */
int chttp_h2_proto_resume_input(chttp_h2_proto *p, int32_t stream_id);

/* ── Byte pumping (non-blocking) ──────────────────────────────────── */

/* Feed received bytes; returns bytes consumed or negative on protocol error. */
ptrdiff_t chttp_h2_proto_recv(chttp_h2_proto *p, const uint8_t *in, size_t in_len);
/* Serialize pending output; *out points into engine-owned storage valid until
 * the next engine call.  Returns bytes or 0 when nothing to send. */
ptrdiff_t chttp_h2_proto_send(chttp_h2_proto *p, const uint8_t **out);
int chttp_h2_proto_want_read(chttp_h2_proto *p);
int chttp_h2_proto_want_write(chttp_h2_proto *p);

/* ── Flow control consumption (receive side) ──────────────────────── */

int chttp_h2_proto_consume_connection(chttp_h2_proto *p, size_t bytes);
int chttp_h2_proto_consume_stream(chttp_h2_proto *p, int32_t stream_id, size_t bytes);

/* ── Stream user data ─────────────────────────────────────────────── */

void *chttp_h2_proto_get_stream_user_data(chttp_h2_proto *p, int32_t stream_id);
/* 1 if the peer signalled END_STREAM on this stream so far. */
int chttp_h2_proto_remote_end_stream(chttp_h2_proto *p, int32_t stream_id);
int chttp_h2_proto_set_stream_user_data(chttp_h2_proto *p, int32_t stream_id, void *ud);
/* 1 while the send path still borrows a DATA body for this stream. */
int chttp_h2_proto_stream_output_pending(chttp_h2_proto *p, int32_t stream_id);

/* ── Lifecycle ────────────────────────────────────────────────────── */

uint32_t chttp_h2_proto_get_last_proc_stream_id(chttp_h2_proto *p);
/* Terminate the session: schedules GOAWAY and closes all streams with the
 * given error code. */
int chttp_h2_proto_terminate(chttp_h2_proto *p, uint32_t error_code);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_H2_PROTO_H */
