/**
 * @file chttp_h2_proto.c
 * @brief Private bounded HTTP/2 protocol engine (RFC 9113).
 *
 * Migrated from the legacy HTTP repository commit
 * 38f1e389b3f94909db6cb2482a8cbc16522e7e4f and adapted to bounded CHTTP storage.
 * Client mode is used by chttp_h2_session.c; server mode by the in-process test
 * peers.  All memory is bounded and checked.
 */

#include "chttp_h2_proto.h"
#include "chttp_h2_frame.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CHTTP_H2_DEFAULT_WINDOW 65535
#define CHTTP_H2_MAX_WINDOW ((int32_t)0x7fffffff)
#define CHTTP_H2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define CHTTP_H2_PREFACE_LEN 24u

enum {
  CHTTP_H2_DEFAULT_STREAM_CAPACITY = 100,
  CHTTP_H2_DEFAULT_OUTPUT_BUFFER_BYTES = 1024 * 1024,
  CHTTP_H2_DEFAULT_INPUT_BUFFER_BYTES = 128 * 1024,
  CHTTP_H2_DEFAULT_HEADER_BLOCK_BYTES = 64 * 1024,
  CHTTP_H2_DEFAULT_HEADER_LIST_BYTES = 64 * 1024,
  CHTTP_H2_DEFAULT_HPACK_TABLE_BYTES = 4096,
  CHTTP_H2_DEFAULT_HPACK_STRING_BYTES = 16 * 1024,
  CHTTP_H2_DEFAULT_MAX_SETTINGS_COUNT = 32,
  CHTTP_H2_DEFAULT_FRAME_BYTES = 16 * 1024,
  CHTTP_H2_MAX_FRAME_BYTES = 0x00ffffff,
  CHTTP_H2_HEADER_LIST_FIELD_OVERHEAD = 32,
  CHTTP_H2_PRIORITY_BYTES = 5
};

/* ── Stream table ─────────────────────────────────────────────────── */

typedef struct chttp_h2_proto_stream_s {
  int32_t id;
  int32_t send_window;
  int32_t recv_window;
  int closed;
  int local_reset;
  int end_stream_sent;
  int remote_end_stream;
  void *user_data;
  /* Pending outbound body (borrowed, must outlive the send). */
  const uint8_t *body;
  size_t body_len;
  size_t body_off;
  int body_eof; /*< emit END_STREAM on the final DATA */

  /* Streaming outbound body source (mutually exclusive with body).  The
   * engine pulls bytes on the send path and emits END_STREAM when the source
   * returns 0; a (size_t)-1 return aborts the stream with RST_STREAM(CANCEL). */
  chttp_h2_proto_source_fn source;
  void *source_ud;
  int source_done;
  int source_waiting;

  /* Deferred trailer HEADERS (RFC 9113 §8.1): encoded at submit time and
   * emitted right after the body's final DATA. */
  chttp_h2_hpack_buffer trailers;
  int trailers_pending;

  /* Receive-side flow control (RFC 9113 §6.9).  consumed accumulates bytes the
   * app has accepted; WINDOW_UPDATE is emitted once it reaches half the
   * advertised window (nghttp2 parity). */
  int32_t consumed;
  int32_t local_window;
} chttp_h2_proto_stream;

/* ── Engine ───────────────────────────────────────────────────────── */

struct chttp_h2_proto_s {
  chttp_h2_proto_mode mode;
  chttp_h2_proto_callbacks cbs;
  chttp_h2_proto_config config;
  chttp_h2_hpack *hpack;
  uint32_t *settings_ids;
  uint32_t *settings_values;

  struct {
    uint32_t header_table_size;
    uint32_t enable_push;
    uint32_t max_concurrent_streams;
    uint32_t initial_window_size;
    uint32_t max_frame_size;
    uint32_t max_header_list_size;
    uint32_t enable_connect_protocol; /* RFC 8441 */
  } local, peer;

  int peer_settings_received;
  int settings_sent;
  int settings_acked;

  chttp_h2_proto_stream *streams;
  size_t stream_count;
  size_t stream_cap;
  int32_t *local_reset_ids;
  size_t local_reset_cursor;
  int32_t next_stream_id;
  int32_t max_peer_stream_id;
  int32_t last_proc_stream_id;
  int goaway_sent;
  int32_t goaway_received_last;
  uint32_t goaway_received_error;
  int draining;

  int32_t conn_send_window;
  int32_t conn_recv_window;
  int32_t conn_consumed;
  int32_t conn_local_window;
  size_t send_chunk; /*< optional DATA payload cap (0 = peer max frame size) */

  chttp_h2_hpack_buffer out;      /* serialized outbound control frames */
  chttp_h2_hpack_buffer inbuf;    /* inbound byte buffer (partial frames) */
  chttp_h2_hpack_buffer in_block; /* accumulated inbound HEADERS+CONTINUATION */
  int in_headers;
  int32_t in_headers_stream;
  int in_headers_end_stream; /* END_STREAM seen on the HEADERS */
  int input_paused;
  int32_t input_paused_stream;
  int input_paused_end_stream;

  int client_preface_done; /* client: preface emitted */
  int server_preface_seen; /* server: client preface consumed */

  int failed;
  uint32_t last_error;
};

static chttp_h2_proto_stream *proto_find(chttp_h2_proto *p, int32_t id) {
  size_t i;
  for (i = 0; i < p->stream_count; i++) {
    if (p->streams[i].id == id) {
      return &p->streams[i];
    }
  }
  return NULL;
}

/* A locally reset stream releases its active slot immediately. Keep only a
 * bounded history so late peer frames can be drained without retaining the
 * per-stream application state. */
static int proto_local_reset_contains(const chttp_h2_proto *p, int32_t id) {
  size_t index;
  if (!p || id <= 0) return 0;
  for (index = 0u; index < p->stream_cap; ++index)
    if (p->local_reset_ids[index] == id) return 1;
  return 0;
}

static void proto_local_reset_record(chttp_h2_proto *p, int32_t id) {
  if (!p || id <= 0 || proto_local_reset_contains(p, id)) return;
  p->local_reset_ids[p->local_reset_cursor] = id;
  p->local_reset_cursor = (p->local_reset_cursor + 1u) % p->stream_cap;
}

static void proto_local_reset_forget(chttp_h2_proto *p, int32_t id) {
  size_t index;
  if (!p || id <= 0) return;
  for (index = 0u; index < p->stream_cap; ++index)
    if (p->local_reset_ids[index] == id) p->local_reset_ids[index] = 0;
}

static chttp_h2_proto_stream *proto_new_stream(chttp_h2_proto *p, int32_t id, void *user_data) {
  chttp_h2_proto_stream *s;
  size_t index;
  for (index = 0u; index < p->stream_count; ++index) {
    if (p->streams[index].closed) {
      chttp_h2_hpack_buffer_destroy(&p->streams[index].trailers);
      s = &p->streams[index];
      goto initialize;
    }
  }
  if (p->stream_count >= p->stream_cap) return NULL;
  s = &p->streams[p->stream_count++];

initialize:
  memset(s, 0, sizeof(*s));
  s->id = id;
  s->send_window = (int32_t)p->peer.initial_window_size;
  s->recv_window = (int32_t)p->local.initial_window_size;
  s->local_window = (int32_t)p->local.initial_window_size;
  s->user_data = user_data;
  if ((p->mode == CHTTP_H2_PROTO_CLIENT ? (id & 1) == 0 : (id & 1) != 0) &&
      id > p->last_proc_stream_id) {
    p->last_proc_stream_id = id;
  }
  return s;
}

static size_t proto_active_local_streams(const chttp_h2_proto *p) {
  size_t active = 0u;
  size_t index;
  for (index = 0u; index < p->stream_count; ++index) {
    const chttp_h2_proto_stream *stream = &p->streams[index];
    const int local =
        p->mode == CHTTP_H2_PROTO_CLIENT ? (stream->id & 1) != 0 : (stream->id & 1) == 0;
    if (!stream->closed && local) ++active;
  }
  return active;
}

static size_t proto_active_streams(const chttp_h2_proto *p) {
  size_t active = 0u;
  size_t index;
  for (index = 0u; index < p->stream_count; ++index) {
    if (!p->streams[index].closed) ++active;
  }
  return active;
}

static void proto_fail(chttp_h2_proto *p, uint32_t error) {
  if (!p->failed) {
    p->failed = 1;
    p->last_error = error;
    /* RFC 9113 §5.4.1: signal the failure with a GOAWAY before the transport
     * goes away (nghttp2 parity; the session flushes it via want_write). */
    (void)chttp_h2_proto_submit_goaway(p, (uint32_t)p->last_proc_stream_id, error);
  }
}

static chttp_h2_proto_config proto_default_config(void) {
  return (chttp_h2_proto_config){.stream_capacity = CHTTP_H2_DEFAULT_STREAM_CAPACITY,
                                 .output_buffer_bytes = CHTTP_H2_DEFAULT_OUTPUT_BUFFER_BYTES,
                                 .input_buffer_bytes = CHTTP_H2_DEFAULT_INPUT_BUFFER_BYTES,
                                 .header_block_bytes = CHTTP_H2_DEFAULT_HEADER_BLOCK_BYTES,
                                 .max_header_list_bytes = CHTTP_H2_DEFAULT_HEADER_LIST_BYTES,
                                 .hpack_dynamic_table_bytes = CHTTP_H2_DEFAULT_HPACK_TABLE_BYTES,
                                 .max_hpack_string_bytes = CHTTP_H2_DEFAULT_HPACK_STRING_BYTES,
                                 .max_settings_count = CHTTP_H2_DEFAULT_MAX_SETTINGS_COUNT};
}

int chttp_h2_proto_config_valid(const chttp_h2_proto_config *config) {
  return config != NULL && config->stream_capacity != 0u &&
         config->stream_capacity <= SIZE_MAX / sizeof(chttp_h2_proto_stream) &&
         config->output_buffer_bytes >= CHTTP_H2_PREFACE_LEN + 2u * CHTTP_H2_FRAME_HEADER_SIZE +
                                            42u + CHTTP_H2_DEFAULT_FRAME_BYTES &&
         config->output_buffer_bytes <= PTRDIFF_MAX &&
         config->input_buffer_bytes >= CHTTP_H2_FRAME_HEADER_SIZE + CHTTP_H2_DEFAULT_FRAME_BYTES &&
         config->input_buffer_bytes <= PTRDIFF_MAX && config->header_block_bytes != 0u &&
         config->header_block_bytes <= PTRDIFF_MAX &&
         config->header_block_bytes <= config->output_buffer_bytes - CHTTP_H2_FRAME_HEADER_SIZE &&
         config->max_header_list_bytes != 0u && config->max_header_list_bytes <= UINT32_MAX &&
         config->hpack_dynamic_table_bytes <= UINT32_MAX && config->max_hpack_string_bytes != 0u &&
         config->max_settings_count != 0u &&
         config->max_settings_count <= SIZE_MAX / sizeof(uint32_t);
}

static size_t proto_initial_capacity(size_t maximum, size_t preferred) {
  return maximum < preferred ? maximum : preferred;
}

chttp_h2_proto *chttp_h2_proto_create(chttp_h2_proto_mode mode, const chttp_h2_proto_config *config,
                                      const chttp_h2_proto_callbacks *callbacks) {
  const chttp_h2_proto_config defaults = proto_default_config();
  const chttp_h2_proto_config *selected = config != NULL ? config : &defaults;
  chttp_h2_hpack_config hpack_config;
  chttp_h2_proto *p;
  if ((mode != CHTTP_H2_PROTO_CLIENT && mode != CHTTP_H2_PROTO_SERVER) ||
      !chttp_h2_proto_config_valid(selected))
    return NULL;
  p = (chttp_h2_proto *)calloc(1, sizeof(*p));
  if (!p) return NULL;
  p->mode = mode;
  p->config = *selected;
  p->stream_cap = selected->stream_capacity;
  p->streams = (chttp_h2_proto_stream *)calloc(p->stream_cap, sizeof(*p->streams));
  p->local_reset_ids = (int32_t *)calloc(p->stream_cap, sizeof(*p->local_reset_ids));
  p->settings_ids = (uint32_t *)calloc(selected->max_settings_count, sizeof(*p->settings_ids));
  p->settings_values =
      (uint32_t *)calloc(selected->max_settings_count, sizeof(*p->settings_values));
  if (!p->streams || !p->local_reset_ids || !p->settings_ids || !p->settings_values) {
    free(p->settings_values);
    free(p->settings_ids);
    free(p->local_reset_ids);
    free(p->streams);
    free(p);
    return NULL;
  }
  if (callbacks) p->cbs = *callbacks;
  hpack_config =
      (chttp_h2_hpack_config){.max_dynamic_table_bytes = selected->hpack_dynamic_table_bytes,
                              .max_header_block_bytes = selected->header_block_bytes,
                              .max_string_bytes = selected->max_hpack_string_bytes};
  p->hpack = chttp_h2_hpack_create(&hpack_config);
  if (!p->hpack || chttp_h2_hpack_encoder_set_max_size(p->hpack, 0u) != 0) {
    chttp_h2_hpack_destroy(p->hpack);
    free(p->settings_values);
    free(p->settings_ids);
    free(p->local_reset_ids);
    free(p->streams);
    free(p);
    return NULL;
  }
  p->local.header_table_size = selected->hpack_dynamic_table_bytes < 4096u
                                   ? (uint32_t)selected->hpack_dynamic_table_bytes
                                   : 4096u;
  p->local.enable_push = 0;
  p->local.max_concurrent_streams =
      selected->stream_capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)selected->stream_capacity;
  p->local.initial_window_size = 65535;
  p->local.max_frame_size = 16384;
  p->local.max_header_list_size = (uint32_t)selected->max_header_list_bytes;
  p->local.enable_connect_protocol = 0;

  p->peer.header_table_size = 4096;
  p->peer.enable_push = 0;
  p->peer.max_concurrent_streams = 0x7fffffff;
  p->peer.initial_window_size = 65535;
  p->peer.max_frame_size = 16384;
  p->peer.max_header_list_size = 0x7fffffff;
  p->peer.enable_connect_protocol = 0;

  p->conn_send_window = CHTTP_H2_DEFAULT_WINDOW;
  p->conn_recv_window = CHTTP_H2_DEFAULT_WINDOW;
  p->conn_local_window = CHTTP_H2_DEFAULT_WINDOW;
  p->next_stream_id = (mode == CHTTP_H2_PROTO_CLIENT) ? 1 : 2;
  p->goaway_received_last = -1;
  if (chttp_h2_hpack_buffer_init(&p->out,
                                 proto_initial_capacity(selected->output_buffer_bytes, 512u),
                                 selected->output_buffer_bytes) != 0 ||
      chttp_h2_hpack_buffer_init(&p->inbuf,
                                 proto_initial_capacity(selected->input_buffer_bytes, 512u),
                                 selected->input_buffer_bytes) != 0 ||
      chttp_h2_hpack_buffer_init(&p->in_block,
                                 proto_initial_capacity(selected->header_block_bytes, 256u),
                                 selected->header_block_bytes) != 0) {
    chttp_h2_proto_destroy(p);
    return NULL;
  }
  return p;
}

void chttp_h2_proto_destroy(chttp_h2_proto *p) {
  if (!p) {
    return;
  }
  chttp_h2_hpack_buffer_destroy(&p->out);
  chttp_h2_hpack_buffer_destroy(&p->inbuf);
  chttp_h2_hpack_buffer_destroy(&p->in_block);
  chttp_h2_hpack_destroy(p->hpack);
  if (p->streams) {
    size_t i;
    for (i = 0; i < p->stream_count; i++) {
      chttp_h2_hpack_buffer_destroy(&p->streams[i].trailers);
    }
  }
  free(p->streams);
  free(p->local_reset_ids);
  free(p->settings_values);
  free(p->settings_ids);
  free(p);
}

void chttp_h2_proto_set_send_chunk(chttp_h2_proto *p, size_t chunk) {
  if (p) {
    p->send_chunk = chunk;
  }
}

int chttp_h2_proto_set_local_settings(chttp_h2_proto *p, uint32_t header_table_size,
                                      uint32_t enable_push, uint32_t max_concurrent_streams,
                                      uint32_t initial_window_size, uint32_t max_frame_size,
                                      uint32_t max_header_list_size) {
  if (!p || p->settings_sent || header_table_size > p->config.hpack_dynamic_table_bytes ||
      enable_push > 1u || max_concurrent_streams == 0u ||
      max_concurrent_streams > p->config.stream_capacity ||
      initial_window_size > (uint32_t)CHTTP_H2_MAX_WINDOW ||
      max_frame_size < CHTTP_H2_DEFAULT_FRAME_BYTES || max_frame_size > CHTTP_H2_MAX_FRAME_BYTES ||
      max_frame_size > p->config.input_buffer_bytes - CHTTP_H2_FRAME_HEADER_SIZE ||
      max_header_list_size == 0u || max_header_list_size > p->config.max_header_list_bytes)
    return -1;
  if (chttp_h2_hpack_decoder_set_max_size(p->hpack, header_table_size) != 0) return -1;
  p->local.header_table_size = header_table_size;
  p->local.enable_push = enable_push;
  p->local.max_concurrent_streams = max_concurrent_streams;
  p->local.initial_window_size = initial_window_size;
  p->local.max_frame_size = max_frame_size;
  p->local.max_header_list_size = max_header_list_size;
  /* The decoder (receiving) must accept the peer's HPACK up to our advertised
   * table size. */
  return 0;
}

uint32_t chttp_h2_proto_peer_max_concurrent_streams(chttp_h2_proto *p) {
  return p ? p->peer.max_concurrent_streams : 0;
}

int chttp_h2_proto_settings_acked(const chttp_h2_proto *p) {
  return p != NULL && p->settings_acked;
}

uint32_t chttp_h2_proto_peer_initial_window_size(chttp_h2_proto *p) {
  return p ? p->peer.initial_window_size : 0;
}

int chttp_h2_proto_peer_settings_received(const chttp_h2_proto *p) {
  return p != NULL && p->peer_settings_received;
}

void chttp_h2_proto_set_local_enable_connect_protocol(chttp_h2_proto *p, uint32_t enable) {
  if (p) {
    p->local.enable_connect_protocol = enable ? 1u : 0u;
  }
}

uint32_t chttp_h2_proto_peer_enable_connect_protocol(chttp_h2_proto *p) {
  return p ? p->peer.enable_connect_protocol : 0;
}

/* ── Output helpers ───────────────────────────────────────────────── */

static int out_frame(chttp_h2_proto *p, uint8_t type, uint8_t flags, int32_t stream_id,
                     const uint8_t *payload, size_t plen) {
  uint8_t hdr[9];
  size_t n = 0;
  if (!p || stream_id < 0 || plen > CHTTP_H2_MAX_FRAME_BYTES || (plen != 0u && !payload) ||
      plen > SIZE_MAX - CHTTP_H2_FRAME_HEADER_SIZE)
    return -1;
  if (chttp_h2_frame_header_encode(hdr, sizeof(hdr), &n, (uint32_t)plen, type, flags,
                                   (uint32_t)stream_id) != 0) {
    return -1;
  }
  if (chttp_h2_hpack_buffer_reserve(&p->out, CHTTP_H2_FRAME_HEADER_SIZE + plen) != 0) {
    return -1;
  }
  memcpy(p->out.data + p->out.size, hdr, CHTTP_H2_FRAME_HEADER_SIZE);
  p->out.size += CHTTP_H2_FRAME_HEADER_SIZE;
  if (plen) {
    memcpy(p->out.data + p->out.size, payload, plen);
    p->out.size += plen;
  }
  return 0;
}

static int proto_header_bounds(const chttp_h2_proto *p, const chttp_h2_hpack_header *headers,
                               size_t count, size_t *out_encoded_bound, size_t *out_wire_bound,
                               int has_priority) {
  size_t encoded = CHTTP_H2_PROTO_HPACK_PENDING_UPDATE_BYTES;
  size_t header_list = 0u;
  size_t payload;
  size_t frame_count;
  size_t index;
  if (!p || !out_encoded_bound || !out_wire_bound || (count != 0u && !headers)) return -1;
  for (index = 0u; index < count; ++index) {
    const chttp_h2_hpack_header *header = &headers[index];
    size_t field_size;
    if (!header->name || !header->value || header->name_size > p->config.max_hpack_string_bytes ||
        header->value_size > p->config.max_hpack_string_bytes ||
        header->name_size > SIZE_MAX - header->value_size)
      return -1;
    field_size = header->name_size + header->value_size;
    if (field_size > SIZE_MAX - CHTTP_H2_PROTO_HPACK_FIELD_OVERHEAD_BYTES ||
        encoded > SIZE_MAX - (field_size + CHTTP_H2_PROTO_HPACK_FIELD_OVERHEAD_BYTES))
      return -1;
    encoded += field_size + CHTTP_H2_PROTO_HPACK_FIELD_OVERHEAD_BYTES;
    if (field_size > SIZE_MAX - CHTTP_H2_HEADER_LIST_FIELD_OVERHEAD ||
        header_list > SIZE_MAX - (field_size + CHTTP_H2_HEADER_LIST_FIELD_OVERHEAD))
      return -1;
    header_list += field_size + CHTTP_H2_HEADER_LIST_FIELD_OVERHEAD;
  }
  if (encoded > p->config.header_block_bytes || header_list > p->peer.max_header_list_size)
    return -1;
  if (encoded > SIZE_MAX - (has_priority ? CHTTP_H2_PRIORITY_BYTES : 0u)) return -1;
  payload = encoded + (has_priority ? CHTTP_H2_PRIORITY_BYTES : 0u);
  frame_count = payload == 0u ? 1u : 1u + (payload - 1u) / p->peer.max_frame_size;
  if (frame_count > SIZE_MAX / CHTTP_H2_FRAME_HEADER_SIZE ||
      payload > SIZE_MAX - frame_count * CHTTP_H2_FRAME_HEADER_SIZE)
    return -1;
  *out_encoded_bound = encoded;
  *out_wire_bound = payload + frame_count * CHTTP_H2_FRAME_HEADER_SIZE;
  return 0;
}

/* Serialize HEADERS (with HPACK block) into the output buffer.  When
 * has_priority is set, a 5-byte priority payload (RFC 9113 §6.2) precedes the
 * HPACK block and CHTTP_H2_FLAG_PRIORITY is set. */
static int out_headers(chttp_h2_proto *p, int32_t stream_id, const chttp_h2_hpack_header *hdrs,
                       size_t count, int end_stream, int has_priority, uint32_t weight,
                       int32_t dep_stream_id, int exclusive) {
  chttp_h2_hpack_buffer blk;
  size_t encoded_bound;
  size_t wire_bound;
  size_t offset = 0u;
  int rc;

  if (proto_header_bounds(p, hdrs, count, &encoded_bound, &wire_bound, has_priority) != 0 ||
      chttp_h2_hpack_buffer_reserve(&p->out, wire_bound) != 0)
    return -1;
  if (chttp_h2_hpack_buffer_init(&blk, encoded_bound < 256u ? encoded_bound : 256u,
                                 p->config.header_block_bytes) != 0)
    return -1;
  rc = chttp_h2_hpack_encode(p->hpack, &blk, hdrs, count);
  if (rc != 0) {
    chttp_h2_hpack_buffer_destroy(&blk);
    proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
    return -1;
  }
  if (has_priority) {
    size_t plen = 0;
    if (chttp_h2_hpack_buffer_reserve(&blk, CHTTP_H2_PRIORITY_BYTES) != 0) {
      chttp_h2_hpack_buffer_destroy(&blk);
      return -1;
    }
    memmove(blk.data + CHTTP_H2_PRIORITY_BYTES, blk.data, blk.size);
    if (chttp_h2_frame_priority_encode(blk.data, CHTTP_H2_PRIORITY_BYTES, &plen, exclusive,
                                       (uint32_t)dep_stream_id, weight) != 0) {
      chttp_h2_hpack_buffer_destroy(&blk);
      return -1;
    }
    blk.size += CHTTP_H2_PRIORITY_BYTES;
  }
  if (blk.size == 0u) {
    uint8_t flags = CHTTP_H2_FLAG_END_HEADERS;
    if (end_stream) flags |= CHTTP_H2_FLAG_END_STREAM;
    if (out_frame(p, CHTTP_H2_FRAME_HEADERS, flags, stream_id, NULL, 0u) != 0) {
      chttp_h2_hpack_buffer_destroy(&blk);
      proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
      return -1;
    }
  } else {
    do {
      size_t chunk = blk.size - offset;
      uint8_t flags = 0u;
      uint8_t type = offset == 0u ? CHTTP_H2_FRAME_HEADERS : CHTTP_H2_FRAME_CONTINUATION;
      if (chunk > p->peer.max_frame_size) chunk = p->peer.max_frame_size;
      if (offset == 0u) {
        if (end_stream) flags |= CHTTP_H2_FLAG_END_STREAM;
        if (has_priority) flags |= CHTTP_H2_FLAG_PRIORITY;
      }
      if (offset + chunk == blk.size) flags |= CHTTP_H2_FLAG_END_HEADERS;
      if (out_frame(p, type, flags, stream_id, blk.data + offset, chunk) != 0) {
        chttp_h2_hpack_buffer_destroy(&blk);
        proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
        return -1;
      }
      offset += chunk;
    } while (offset < blk.size);
  }
  chttp_h2_hpack_buffer_destroy(&blk);
  return 0;
}

static int proto_ensure_preface(chttp_h2_proto *p);

/* ── Submit ───────────────────────────────────────────────────────── */

int chttp_h2_proto_submit_settings(chttp_h2_proto *p) {
  uint32_t ids[7];
  uint32_t vals[7];
  size_t count = 0;
  uint8_t payload[42];
  size_t plen = 0;

  if (!p) {
    return -1;
  }
  if (p->mode == CHTTP_H2_PROTO_CLIENT) {
    ids[count] = CHTTP_H2_SETTING_ENABLE_PUSH;
    vals[count] = p->local.enable_push;
    count++;
  }
  ids[count] = CHTTP_H2_SETTING_MAX_CONCURRENT_STREAMS;
  vals[count] = p->local.max_concurrent_streams;
  count++;
  ids[count] = CHTTP_H2_SETTING_HEADER_TABLE_SIZE;
  vals[count] = p->local.header_table_size;
  count++;
  ids[count] = CHTTP_H2_SETTING_INITIAL_WINDOW_SIZE;
  vals[count] = p->local.initial_window_size;
  count++;
  ids[count] = CHTTP_H2_SETTING_MAX_FRAME_SIZE;
  vals[count] = p->local.max_frame_size;
  count++;
  ids[count] = CHTTP_H2_SETTING_MAX_HEADER_LIST_SIZE;
  vals[count] = p->local.max_header_list_size;
  count++;
  if (p->local.enable_connect_protocol) {
    ids[count] = CHTTP_H2_SETTING_ENABLE_CONNECT_PROTOCOL;
    vals[count] = 1;
    count++;
  }
  if (chttp_h2_frame_settings_encode(payload, sizeof(payload), &plen, ids, vals, count) != 0) {
    return -1;
  }
  if (out_frame(p, CHTTP_H2_FRAME_SETTINGS, 0, 0, payload, plen) != 0) {
    return -1;
  }
  p->settings_sent = 1;
  return 0;
}

int chttp_h2_proto_submit_ping(chttp_h2_proto *p, const uint8_t opaque[8]) {
  if (!p || !opaque || proto_ensure_preface(p) != 0) return -1;
  return out_frame(p, CHTTP_H2_FRAME_PING, 0u, 0, opaque, 8u);
}

int chttp_h2_proto_submit_window_update(chttp_h2_proto *p, int32_t stream_id, uint32_t increment) {
  if (!p) return -1;
  if (proto_ensure_preface(p) != 0) {
    return -1;
  }
  uint8_t payload[4];
  size_t plen = 0;
  if (stream_id < 0 || increment == 0 || increment > (uint32_t)CHTTP_H2_MAX_WINDOW) {
    return -1;
  }
  if (chttp_h2_frame_window_update_encode(payload, sizeof(payload), &plen, increment) != 0) {
    return -1;
  }
  /* A WINDOW_UPDATE restores receive credit; the configured target window
   * stays fixed so consumption thresholds do not grow after every update. */
  if (stream_id == 0) {
    if (p->conn_recv_window > CHTTP_H2_MAX_WINDOW - (int32_t)increment) {
      return -1;
    }
    p->conn_recv_window += (int32_t)increment;
  } else {
    chttp_h2_proto_stream *s = proto_find(p, stream_id);
    if (!s || s->closed) return -1;
    if (s->recv_window > CHTTP_H2_MAX_WINDOW - (int32_t)increment) return -1;
    s->recv_window += (int32_t)increment;
  }
  return out_frame(p, CHTTP_H2_FRAME_WINDOW_UPDATE, 0, stream_id, payload, plen);
}

int chttp_h2_proto_submit_rst_stream(chttp_h2_proto *p, int32_t stream_id, uint32_t error_code) {
  chttp_h2_proto_stream *s;
  if (!p || stream_id <= 0) return -1;
  if (proto_ensure_preface(p) != 0) {
    return -1;
  }
  uint8_t payload[4];
  size_t plen = 0;
  s = proto_find(p, stream_id);
  if (!s || s->closed) return -1;
  if (chttp_h2_frame_rst_encode(payload, sizeof(payload), &plen, error_code) != 0) {
    return -1;
  }
  if (out_frame(p, CHTTP_H2_FRAME_RST_STREAM, 0, stream_id, payload, plen) != 0) {
    return -1;
  }
  if (!s->closed) {
    s->closed = 1;
    s->local_reset = 1;
    proto_local_reset_record(p, stream_id);
    s->body = NULL;
    s->body_len = s->body_off = 0;
    if (s->trailers_pending) {
      s->trailers_pending = 0;
      chttp_h2_hpack_buffer_destroy(&s->trailers);
    }
    /* Local RST converges the stream immediately (nghttp2 fires
     * on_stream_close when the RST frame is flushed; the session layer relies
     * on the synchronous callback for accounting). */
    if (p->cbs.on_stream_close) {
      p->cbs.on_stream_close(p->cbs.user_data, stream_id, error_code);
    }
  }
  return 0;
}

int chttp_h2_proto_submit_goaway(chttp_h2_proto *p, uint32_t last_stream_id, uint32_t error_code) {
  if (!p || last_stream_id > 0x7fffffffu) return -1;
  if (proto_ensure_preface(p) != 0) {
    return -1;
  }
  uint8_t payload[8];
  size_t plen = 0;
  if (chttp_h2_frame_goaway_encode(payload, sizeof(payload), &plen, last_stream_id, error_code) !=
      0) {
    return -1;
  }
  if (out_frame(p, CHTTP_H2_FRAME_GOAWAY, 0, 0, payload, plen) != 0) {
    return -1;
  }
  p->goaway_sent = 1;
  p->draining = 1;
  return 0;
}

/* ── Stream closure ────────────────────────────────────────────────── */

static void proto_maybe_close(chttp_h2_proto *p, chttp_h2_proto_stream *s) {
  if (!s || s->closed) {
    return;
  }
  if (s->end_stream_sent && s->remote_end_stream) {
    s->closed = 1;
    s->body = NULL;
    s->body_len = s->body_off = 0;
    if (p->cbs.on_stream_close) {
      p->cbs.on_stream_close(p->cbs.user_data, s->id, 0);
    }
  }
}

/* ── Submit requests / responses ──────────────────────────────────── */

int chttp_h2_proto_submit_request_ex(chttp_h2_proto *p, const chttp_h2_hpack_header *hdrs,
                                     size_t count, const uint8_t *body, size_t body_len,
                                     chttp_h2_proto_source_fn source, void *source_ud,
                                     uint32_t weight, int32_t dep_stream_id, int exclusive,
                                     int32_t *out_stream_id) {
  chttp_h2_proto_stream *s;
  int32_t id;
  int rc;
  int has_priority;
  int end_stream;
  uint32_t effective_weight;

  if (!p || p->mode != CHTTP_H2_PROTO_CLIENT || p->draining || p->failed || !out_stream_id ||
      (body != NULL && source != NULL) || (body_len != 0u && body == NULL) || weight > 256u ||
      proto_active_local_streams(p) >= p->peer.max_concurrent_streams) {
    return -1;
  }
  *out_stream_id = 0;
  has_priority = (weight != 0) || (dep_stream_id > 0) || exclusive;
  effective_weight = weight != 0u ? weight : 16u;
  id = p->next_stream_id;
  if (p->next_stream_id > 0x7ffffffe || dep_stream_id == id || proto_ensure_preface(p) != 0) {
    return -1;
  }
  p->next_stream_id += 2;
  s = proto_new_stream(p, id, NULL);
  if (!s) {
    return -1;
  }
  if (source != NULL) {
    /* Streaming body: HEADERS never carry END_STREAM; the final DATA does,
     * emitted when the source reports EOF.  content-length, when known, was
     * placed in the headers by the caller. */
    end_stream = 0;
  } else {
    end_stream = (body_len == 0);
  }
  s->end_stream_sent = end_stream;
  rc = out_headers(p, id, hdrs, count, end_stream, has_priority, effective_weight, dep_stream_id,
                   exclusive);
  if (rc != 0) {
    /* No request bytes were committed; drop the orphan stream so the send
     * path never emits a bare HEADERS for it. */
    s->closed = 1;
    s->body = NULL;
    s->source = NULL;
    return -1;
  }
  if (source != NULL) {
    s->source = source;
    s->source_ud = source_ud;
  } else if (body_len > 0) {
    s->body = body;
    s->body_len = body_len;
    s->body_off = 0;
    s->body_eof = 1;
  }
  *out_stream_id = id;
  return 0;
}

int chttp_h2_proto_submit_request(chttp_h2_proto *p, const chttp_h2_hpack_header *hdrs,
                                  size_t count, const uint8_t *body, size_t body_len,
                                  int32_t *out_stream_id) {
  return chttp_h2_proto_submit_request_ex(p, hdrs, count, body, body_len, NULL, NULL, 0, 0, 0,
                                          out_stream_id);
}

int chttp_h2_proto_submit_request_headers(chttp_h2_proto *p, const chttp_h2_hpack_header *hdrs,
                                          size_t count, int end_stream, int32_t *out_stream_id) {
  chttp_h2_proto_stream *stream;
  int32_t stream_id;
  if (p == NULL || p->mode != CHTTP_H2_PROTO_CLIENT || p->draining || p->failed ||
      out_stream_id == NULL || (end_stream != 0 && end_stream != 1) ||
      proto_active_local_streams(p) >= p->peer.max_concurrent_streams)
    return -1;
  *out_stream_id = 0;
  stream_id = p->next_stream_id;
  if (p->next_stream_id > 0x7ffffffe || proto_ensure_preface(p) != 0) return -1;
  p->next_stream_id += 2;
  stream = proto_new_stream(p, stream_id, NULL);
  if (stream == NULL) return -1;
  stream->end_stream_sent = end_stream;
  if (out_headers(p, stream_id, hdrs, count, end_stream, 0, 0, 0, 0) != 0) {
    stream->closed = 1;
    return -1;
  }
  *out_stream_id = stream_id;
  return 0;
}

int chttp_h2_proto_submit_headers(chttp_h2_proto *p, int32_t stream_id,
                                  const chttp_h2_hpack_header *hdrs, size_t count, int end_stream) {
  chttp_h2_proto_stream *s;
  if (!p || p->mode != CHTTP_H2_PROTO_SERVER || (end_stream != 0 && end_stream != 1)) {
    return -1;
  }
  s = proto_find(p, stream_id);
  if (!s || s->closed) {
    return -1;
  }
  if (proto_ensure_preface(p) != 0) return -1;
  if (out_headers(p, stream_id, hdrs, count, end_stream, 0, 0, 0, 0) != 0) {
    return -1;
  }
  if (end_stream) {
    s->end_stream_sent = 1;
    proto_maybe_close(p, s);
  }
  return 0;
}

int chttp_h2_proto_submit_response_ex(chttp_h2_proto *p, int32_t stream_id,
                                      const chttp_h2_hpack_header *hdrs, size_t count,
                                      const uint8_t *body, size_t body_len,
                                      chttp_h2_proto_source_fn source, void *source_ud) {
  chttp_h2_proto_stream *s;
  int rc;

  if (!p || p->mode != CHTTP_H2_PROTO_SERVER || (body != NULL && source != NULL) ||
      (body_len != 0u && body == NULL)) {
    return -1;
  }
  s = proto_find(p, stream_id);
  if (!s || s->closed) {
    return -1;
  }
  if (proto_ensure_preface(p) != 0) return -1;
  s->end_stream_sent = body_len == 0u && source == NULL;
  rc = out_headers(p, stream_id, hdrs, count, s->end_stream_sent, 0, 0, 0, 0);
  if (rc != 0) {
    return -1;
  }
  if (source != NULL) {
    s->source = source;
    s->source_ud = source_ud;
  } else if (body_len > 0) {
    s->body = body;
    s->body_len = body_len;
    s->body_off = 0;
    s->body_eof = 1;
  } else {
    proto_maybe_close(p, s);
  }
  return 0;
}

int chttp_h2_proto_submit_response(chttp_h2_proto *p, int32_t stream_id,
                                   const chttp_h2_hpack_header *hdrs, size_t count,
                                   const uint8_t *body, size_t body_len) {
  return chttp_h2_proto_submit_response_ex(p, stream_id, hdrs, count, body, body_len, NULL, NULL);
}

int chttp_h2_proto_submit_data(chttp_h2_proto *p, int32_t stream_id, const uint8_t *data,
                               size_t len, int end_stream) {
  chttp_h2_proto_stream *s;
  if (!p || (len > 0 && !data) || (len == 0 && !end_stream) ||
      (end_stream != 0 && end_stream != 1)) {
    return -1;
  }
  s = proto_find(p, stream_id);
  if (!s || s->closed) {
    return -1;
  }
  if (s->body != NULL || s->source != NULL || s->end_stream_sent) {
    return -1; /* a body is already streaming */
  }
  if (proto_ensure_preface(p) != 0) return -1;
  s->body = data ? data : (const uint8_t *)"";
  s->body_len = len;
  s->body_off = 0;
  s->body_eof = end_stream ? 1 : 0;
  return 0;
}

int chttp_h2_proto_submit_trailers(chttp_h2_proto *p, int32_t stream_id,
                                   const chttp_h2_hpack_header *hdrs, size_t count) {
  chttp_h2_proto_stream *s;
  if (!p || p->mode != CHTTP_H2_PROTO_SERVER) {
    return -1;
  }
  s = proto_find(p, stream_id);
  if (!s || s->closed) {
    return -1;
  }
  if (proto_ensure_preface(p) != 0) return -1;
  if (s->trailers_pending) {
    return -1; /* a trailer block is already queued */
  }
  if (s->body != NULL && (s->body_off < s->body_len || s->body_eof)) {
    /* Body still streaming: encode now (so the caller's array may go out of
     * scope) and emit the HEADERS right after the final DATA. */
    if (chttp_h2_hpack_buffer_init(&s->trailers,
                                   proto_initial_capacity(p->config.header_block_bytes, 64u),
                                   p->config.header_block_bytes) != 0) {
      return -1;
    }
    if (chttp_h2_hpack_encode(p->hpack, &s->trailers, hdrs, count) != 0) {
      chttp_h2_hpack_buffer_destroy(&s->trailers);
      return -1;
    }
    s->trailers_pending = 1;
    return 0;
  }
  s->end_stream_sent = 1;
  s->body_eof = 0;
  {
    int rc = out_headers(p, stream_id, hdrs, count, 1, 0, 0, 0, 0);
    if (rc != 0) {
      return -1;
    }
  }
  proto_maybe_close(p, s);
  return 0;
}

/* ── Send path ────────────────────────────────────────────────────── */

static int proto_any_pending_body(chttp_h2_proto *p) {
  size_t i;
  if (p->failed) {
    return 0; /* after a protocol error only control frames (e.g. GOAWAY) go out */
  }
  for (i = 0; i < p->stream_count; i++) {
    chttp_h2_proto_stream *s = &p->streams[i];
    if (s->closed) {
      continue;
    }
    if (s->body != NULL && (s->body_off < s->body_len || s->body_eof)) {
      return 1;
    }
    if (s->source != NULL && !s->source_done && !s->source_waiting) {
      return 1;
    }
  }
  return 0;
}

static void proto_emit_data(chttp_h2_proto *p, chttp_h2_proto_stream *s) {
  for (;;) {
    size_t chunk;
    uint8_t flags = 0;
    uint8_t *payload;
    size_t max = p->peer.max_frame_size;
    size_t output_remaining;

    if (s->closed) return;
    if (p->out.size > p->config.output_buffer_bytes) {
      proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
      return;
    }
    output_remaining = p->config.output_buffer_bytes - p->out.size;

    /* An empty DATA frame is the wire-level END_STREAM used by streaming
     * response helpers. It does not consume flow-control window. */
    if (s->source == NULL && s->body != NULL && s->body_len == 0 && s->body_off == 0 &&
        s->body_eof) {
      if (output_remaining < CHTTP_H2_FRAME_HEADER_SIZE) return;
      if (out_frame(p, CHTTP_H2_FRAME_DATA, CHTTP_H2_FLAG_END_STREAM, s->id, NULL, 0) != 0) {
        proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
        return;
      }
      s->body = NULL;
      s->body_len = s->body_off = 0;
      s->body_eof = 0;
      s->end_stream_sent = 1;
      proto_maybe_close(p, s);
      return;
    }

    if (p->conn_send_window <= 0 || s->send_window <= 0) return;
    if (output_remaining <= CHTTP_H2_FRAME_HEADER_SIZE) return;
    output_remaining -= CHTTP_H2_FRAME_HEADER_SIZE;
    if (max > output_remaining) max = output_remaining;

    if (s->source != NULL) {
      /* Streaming source: pull bytes into the frame payload and emit DATA
       * gated by the flow-control windows.  A 0 return is EOF (empty DATA
       * with END_STREAM); (size_t)-1 aborts the stream (RST_STREAM). */
      chttp_h2_proto_source_result source_result;
      size_t n;
      if (s->source_done) return;
      chunk = max;
      if (p->send_chunk && chunk > p->send_chunk) chunk = p->send_chunk;
      if (chunk > (size_t)p->conn_send_window) chunk = (size_t)p->conn_send_window;
      if (chunk > (size_t)s->send_window) chunk = (size_t)s->send_window;
      if (chunk == 0) return;
      if (chttp_h2_hpack_buffer_reserve(&p->out, 9 + chunk) != 0) {
        proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
        return;
      }
      payload = p->out.data + p->out.size + 9;
      source_result = s->source(s->source_ud, payload, chunk);
      n = source_result.size;
      if (source_result.status == CHTTP_H2_PROTO_SOURCE_WAIT) {
        if (n != 0u) {
          s->source = NULL;
          s->source_ud = NULL;
          proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
          return;
        }
        s->source_waiting = 1;
        return;
      }
      if (source_result.status == CHTTP_H2_PROTO_SOURCE_ERROR) {
        /* Abort: nothing was committed to p->out yet; RST the stream. */
        s->source = NULL;
        s->source_ud = NULL;
        (void)chttp_h2_proto_submit_rst_stream(p, s->id, CHTTP_H2_ERR_CANCEL);
        return;
      }
      if (source_result.status == CHTTP_H2_PROTO_SOURCE_DATA && (n == 0u || n > chunk)) {
        s->source = NULL;
        s->source_ud = NULL;
        proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
        return;
      }
      if (source_result.status == CHTTP_H2_PROTO_SOURCE_EOF) {
        if (n != 0u) {
          s->source = NULL;
          s->source_ud = NULL;
          proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
          return;
        }
        flags = CHTTP_H2_FLAG_END_STREAM;
        s->source = NULL;
        s->source_ud = NULL;
        s->source_done = 1;
      } else if (source_result.status != CHTTP_H2_PROTO_SOURCE_DATA) {
        s->source = NULL;
        s->source_ud = NULL;
        proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
        return;
      }
      if (chttp_h2_frame_header_encode(p->out.data + p->out.size, 9, &(size_t){0}, (uint32_t)n,
                                       CHTTP_H2_FRAME_DATA, flags, (uint32_t)s->id) != 0) {
        proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
        return;
      }
      p->out.size += 9 + n;
      if (source_result.status == CHTTP_H2_PROTO_SOURCE_EOF) {
        s->end_stream_sent = 1;
        proto_maybe_close(p, s);
        return;
      }
      p->conn_send_window -= (int32_t)n;
      s->send_window -= (int32_t)n;
      continue; /* more source bytes may fit the remaining window */
    }

    /* Flat body mode. */
    if (s->body == NULL || !(s->body_off < s->body_len || s->body_eof)) return;
    chunk = s->body_len - s->body_off;
    if (chunk > max) chunk = max;
    if (p->send_chunk && chunk > p->send_chunk) chunk = p->send_chunk;
    if (chunk > (size_t)p->conn_send_window) chunk = (size_t)p->conn_send_window;
    if (chunk > (size_t)s->send_window) chunk = (size_t)s->send_window;
    if (chunk == 0) return;
    /* END_STREAM rides the last DATA only when no trailer block is pending;
     * otherwise it is carried by the trailer HEADERS. */
    if (s->body_off + chunk == s->body_len && s->body_eof && !s->trailers_pending) {
      flags |= CHTTP_H2_FLAG_END_STREAM;
    }
    if (chttp_h2_hpack_buffer_reserve(&p->out, 9 + chunk) != 0) {
      proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
      return;
    }
    if (chttp_h2_frame_header_encode(p->out.data + p->out.size, 9, &(size_t){0}, (uint32_t)chunk,
                                     CHTTP_H2_FRAME_DATA, flags, (uint32_t)s->id) != 0) {
      proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
      return;
    }
    p->out.size += 9;
    payload = p->out.data + p->out.size;
    memcpy(payload, s->body + s->body_off, chunk);
    p->out.size += chunk;
    p->conn_send_window -= (int32_t)chunk;
    s->send_window -= (int32_t)chunk;
    s->body_off += chunk;
    if (s->body_off == s->body_len) {
      if (s->body_eof) {
        if (s->trailers_pending) {
          /* The final DATA carried no END_STREAM; emit the trailer HEADERS. */
          s->trailers_pending = 0;
          if (out_frame(p, CHTTP_H2_FRAME_HEADERS,
                        CHTTP_H2_FLAG_END_HEADERS | CHTTP_H2_FLAG_END_STREAM, s->id,
                        s->trailers.data, s->trailers.size) != 0) {
            proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
            return;
          }
          chttp_h2_hpack_buffer_destroy(&s->trailers);
          s->end_stream_sent = 1;
          s->body = NULL;
          s->body_len = s->body_off = 0;
          s->body_eof = 0;
        } else if (flags & CHTTP_H2_FLAG_END_STREAM) {
          s->end_stream_sent = 1;
          s->body = NULL;
          s->body_len = s->body_off = 0;
          s->body_eof = 0;
        } else {
          /* Body exhausted but END_STREAM could not ride the last DATA (the
           * window ran out exactly at the boundary): leave a zero-length body
           * pending so a final empty DATA(END_STREAM) is emitted once the
           * window reopens. */
          s->body = (const uint8_t *)"";
          s->body_len = 0;
          s->body_off = 0;
          s->body_eof = 1;
          s->end_stream_sent = 0;
        }
      } else {
        s->body = NULL;
        s->body_len = s->body_off = 0;
      }
    }
    proto_maybe_close(p, s);
  }
}

/* Ensure the connection preface / our SETTINGS are queued before any other
 * outbound frame.  The client writes the preface once, the server responds
 * with SETTINGS after it has seen the client preface. */
static int proto_ensure_preface(chttp_h2_proto *p) {
  if (!p) return -1;
  if (p->mode == CHTTP_H2_PROTO_CLIENT && !p->client_preface_done) {
    if (chttp_h2_hpack_buffer_reserve(&p->out, CHTTP_H2_PREFACE_LEN + CHTTP_H2_FRAME_HEADER_SIZE +
                                                   42u) != 0) {
      return -1;
    }
    memcpy(p->out.data + p->out.size, CHTTP_H2_PREFACE, CHTTP_H2_PREFACE_LEN);
    p->out.size += CHTTP_H2_PREFACE_LEN;
    p->client_preface_done = 1;
    return chttp_h2_proto_submit_settings(p);
  }
  if (p->mode == CHTTP_H2_PROTO_SERVER && p->server_preface_seen && !p->settings_sent) {
    return chttp_h2_proto_submit_settings(p);
  }
  return 0;
}

ptrdiff_t chttp_h2_proto_send(chttp_h2_proto *p, const uint8_t **out) {
  size_t n;
  size_t i;
  if (!p || !out) {
    return -1;
  }
  *out = NULL;
  if (proto_ensure_preface(p) != 0) {
    return -1;
  }
  /* Emit pending DATA frames gated by flow-control windows (skipped once the
   * session failed: only the GOAWAY/control frames remain). */
  if (!p->failed) {
    for (i = 0; i < p->stream_count; i++) {
      proto_emit_data(p, &p->streams[i]);
    }
  }
  n = p->out.size;
  *out = p->out.data;
  p->out.size = 0; /* consumed; buffer data stays valid until the next call */
  return (ptrdiff_t)n;
}

int chttp_h2_proto_want_write(chttp_h2_proto *p) {
  return p && (p->out.size > 0 || proto_any_pending_body(p));
}

int chttp_h2_proto_want_read(chttp_h2_proto *p) {
  /* Like nghttp2: a failed session keeps wanting reads until its GOAWAY has
   * been flushed; the writer/reader tasks then observe want_read == 0 and
   * tear the connection down. */
  return p && (!p->failed || p->out.size > 0);
}

/* ── Receive path ─────────────────────────────────────────────────── */

static uint32_t proto_apply_peer_settings(chttp_h2_proto *p, const uint32_t *ids,
                                          const uint32_t *vals, size_t count) {
  size_t i;
  for (i = 0; i < count; i++) {
    switch (ids[i]) {
    case CHTTP_H2_SETTING_HEADER_TABLE_SIZE:
      p->peer.header_table_size = vals[i];
      break;
    case CHTTP_H2_SETTING_ENABLE_PUSH:
      if (p->mode == CHTTP_H2_PROTO_CLIENT || vals[i] > 1u) return CHTTP_H2_ERR_PROTOCOL_ERROR;
      p->peer.enable_push = vals[i];
      break;
    case CHTTP_H2_SETTING_ENABLE_CONNECT_PROTOCOL:
      if (vals[i] > 1u || (p->peer.enable_connect_protocol == 1u && vals[i] == 0u))
        return CHTTP_H2_ERR_PROTOCOL_ERROR;
      p->peer.enable_connect_protocol = vals[i];
      break;
    case CHTTP_H2_SETTING_MAX_CONCURRENT_STREAMS:
      p->peer.max_concurrent_streams = vals[i];
      break;
    case CHTTP_H2_SETTING_INITIAL_WINDOW_SIZE: {
      const int64_t delta = (int64_t)vals[i] - p->peer.initial_window_size;
      size_t stream_index;
      if (vals[i] > (uint32_t)CHTTP_H2_MAX_WINDOW) return CHTTP_H2_ERR_FLOW_CONTROL_ERROR;
      for (stream_index = 0u; stream_index < p->stream_count; ++stream_index) {
        chttp_h2_proto_stream *stream = &p->streams[stream_index];
        const int64_t updated = (int64_t)stream->send_window + delta;
        if (!stream->closed && (updated < INT32_MIN || updated > CHTTP_H2_MAX_WINDOW))
          return CHTTP_H2_ERR_FLOW_CONTROL_ERROR;
      }
      for (stream_index = 0u; stream_index < p->stream_count; ++stream_index) {
        chttp_h2_proto_stream *stream = &p->streams[stream_index];
        if (!stream->closed) stream->send_window = (int32_t)((int64_t)stream->send_window + delta);
      }
      p->peer.initial_window_size = vals[i];
      break;
    }
    case CHTTP_H2_SETTING_MAX_FRAME_SIZE:
      if (vals[i] < CHTTP_H2_DEFAULT_FRAME_BYTES || vals[i] > CHTTP_H2_MAX_FRAME_BYTES)
        return CHTTP_H2_ERR_PROTOCOL_ERROR;
      p->peer.max_frame_size = vals[i];
      break;
    case CHTTP_H2_SETTING_MAX_HEADER_LIST_SIZE:
      p->peer.max_header_list_size = vals[i];
      break;
    default:
      break;
    }
  }
  return CHTTP_H2_ERR_NO_ERROR;
}

typedef struct chttp_h2_header_context_s {
  chttp_h2_proto *p;
  int32_t stream_id;
  int ignore;
  int callback_failed;
} chttp_h2_header_context;

static int chttp_h2_header_decode(void *ud, const char *name, size_t name_len, const char *value,
                                  size_t value_len) {
  chttp_h2_header_context *ctx = (chttp_h2_header_context *)ud;
  if (ctx->ignore || !ctx->p->cbs.on_header) {
    return 0;
  }
  if (ctx->p->cbs.on_header(ctx->p->cbs.user_data, ctx->stream_id, name, name_len, value,
                            value_len) != 0) {
    ctx->callback_failed = 1;
    ctx->ignore = 1;
  }
  return 0;
}

static void proto_finish_headers(chttp_h2_proto *p) {
  size_t consumed = 0;
  int rc;
  int initially_ignored;
  uint32_t stream_error = CHTTP_H2_ERR_NO_ERROR;
  chttp_h2_proto_stream *s;
  chttp_h2_header_context ctx;

  ctx.p = p;
  ctx.stream_id = p->in_headers_stream;
  s = proto_find(p, p->in_headers_stream);
  initially_ignored = (s != NULL && s->closed && s->local_reset) ||
                      proto_local_reset_contains(p, p->in_headers_stream);
  ctx.ignore = initially_ignored;
  ctx.callback_failed = 0;
  if (!ctx.ignore && p->cbs.on_begin_headers &&
      p->cbs.on_begin_headers(p->cbs.user_data, p->in_headers_stream) != 0) {
    ctx.callback_failed = 1;
    ctx.ignore = 1;
  }
  rc = chttp_h2_hpack_decode(p->hpack, p->in_block.data, p->in_block.size, &consumed,
                             chttp_h2_header_decode, &ctx, p->local.max_header_list_size);
  if (rc == -2) {
    stream_error = CHTTP_H2_ERR_ENHANCE_YOUR_CALM;
  } else if (rc == -3) {
    proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
  } else if (rc != 0) {
    proto_fail(p, CHTTP_H2_ERR_COMPRESSION_ERROR);
  }
  if (s && p->in_headers_end_stream) {
    s->remote_end_stream = 1;
    if (s->local_reset) {
      s->local_reset = 0;
      proto_local_reset_forget(p, p->in_headers_stream);
    }
  } else if (p->in_headers_end_stream && initially_ignored) {
    proto_local_reset_forget(p, p->in_headers_stream);
  }
  if (!p->failed && !initially_ignored && stream_error == CHTTP_H2_ERR_NO_ERROR &&
      ctx.callback_failed)
    stream_error = CHTTP_H2_ERR_PROTOCOL_ERROR;
  if (!p->failed && !initially_ignored && stream_error == CHTTP_H2_ERR_NO_ERROR && !ctx.ignore &&
      p->cbs.on_end_headers &&
      p->cbs.on_end_headers(p->cbs.user_data, p->in_headers_stream, p->in_headers_end_stream) != 0)
    stream_error = CHTTP_H2_ERR_PROTOCOL_ERROR;
  if (!p->failed && !initially_ignored && stream_error != CHTTP_H2_ERR_NO_ERROR && s &&
      !s->closed) {
    if (chttp_h2_proto_submit_rst_stream(p, p->in_headers_stream, stream_error) != 0) {
      proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
    } else if (p->in_headers_end_stream) {
      s->local_reset = 0;
      proto_local_reset_forget(p, p->in_headers_stream);
    }
  }
  if (!p->failed && !initially_ignored && stream_error == CHTTP_H2_ERR_NO_ERROR && s &&
      !s->closed) {
    proto_maybe_close(p, s);
  }
  p->in_headers = 0;
  p->in_block.size = 0;
}

static void proto_dispatch(chttp_h2_proto *p, const uint8_t *payload,
                           const chttp_h2_frame_header *hdr) {
  chttp_h2_proto_stream *s;
  if (p->in_headers && hdr->type != CHTTP_H2_FRAME_CONTINUATION) {
    proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
    return;
  }
  if (!p->peer_settings_received &&
      (hdr->type != CHTTP_H2_FRAME_SETTINGS || (hdr->flags & CHTTP_H2_FLAG_ACK) != 0u)) {
    proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
    return;
  }
  switch (hdr->type) {
  case CHTTP_H2_FRAME_SETTINGS:
    if (hdr->stream_id != 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR); /* PROTOCOL_ERROR */
      return;
    }
    if (hdr->flags & CHTTP_H2_FLAG_ACK) {
      if (hdr->length != 0u) {
        proto_fail(p, CHTTP_H2_ERR_FRAME_SIZE_ERROR);
        return;
      }
      p->settings_acked = 1;
      if (p->cbs.on_settings_ack) {
        p->cbs.on_settings_ack(p->cbs.user_data);
      }
      return;
    }
    {
      size_t count = 0;
      if (chttp_h2_frame_settings_parse(payload, hdr->length, p->settings_ids, p->settings_values,
                                        &count, p->config.max_settings_count) != 0) {
        proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
        return;
      }
      {
        const uint32_t settings_error =
            proto_apply_peer_settings(p, p->settings_ids, p->settings_values, count);
        if (settings_error != CHTTP_H2_ERR_NO_ERROR) {
          proto_fail(p, settings_error);
          return;
        }
      }
      p->peer_settings_received = 1;
      if (p->cbs.on_settings) {
        p->cbs.on_settings(p->cbs.user_data, p->settings_ids, p->settings_values, count);
      }
      /* ACK */
      if (out_frame(p, CHTTP_H2_FRAME_SETTINGS, CHTTP_H2_FLAG_ACK, 0, NULL, 0) != 0) {
        proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      }
    }
    return;

  case CHTTP_H2_FRAME_WINDOW_UPDATE: {
    uint32_t inc;
    if (hdr->length != 4u) {
      proto_fail(p, CHTTP_H2_ERR_FRAME_SIZE_ERROR);
      return;
    }
    inc = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
          ((uint32_t)payload[2] << 8) | (uint32_t)payload[3];
    inc &= 0x7fffffffu;
    if (inc == 0) {
      proto_fail(p, CHTTP_H2_ERR_FLOW_CONTROL_ERROR); /* FLOW_CONTROL_ERROR */
      return;
    }
    if (hdr->stream_id == 0) {
      if (p->conn_send_window > CHTTP_H2_MAX_WINDOW - (int32_t)inc) {
        proto_fail(p, CHTTP_H2_ERR_FLOW_CONTROL_ERROR);
        return;
      }
      p->conn_send_window += (int32_t)inc;
    } else {
      s = proto_find(p, (int32_t)hdr->stream_id);
      if (!s) {
        if (proto_local_reset_contains(p, (int32_t)hdr->stream_id)) return;
        proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
        return;
      }
      /* RFC 9113 section 5.1 permits WINDOW_UPDATE to race with the peer's
       * observation of END_STREAM. A known closed stream therefore ignores
       * the late credit instead of turning a stream race into connection
       * failure. */
      if (s->closed) return;
      if (s->send_window > CHTTP_H2_MAX_WINDOW - (int32_t)inc) {
        proto_fail(p, CHTTP_H2_ERR_FLOW_CONTROL_ERROR);
        return;
      }
      s->send_window += (int32_t)inc;
    }
    if (p->cbs.on_window_update) {
      p->cbs.on_window_update(p->cbs.user_data, (int32_t)hdr->stream_id, inc);
    }
    if (p->cbs.on_wake_write) {
      p->cbs.on_wake_write(p->cbs.user_data);
    }
    return;
  }

  case CHTTP_H2_FRAME_RST_STREAM:
    if (hdr->stream_id == 0 || hdr->length != 4u) {
      proto_fail(p,
                 hdr->stream_id == 0 ? CHTTP_H2_ERR_PROTOCOL_ERROR : CHTTP_H2_ERR_FRAME_SIZE_ERROR);
      return;
    }
    s = proto_find(p, (int32_t)hdr->stream_id);
    if (!s) {
      if (proto_local_reset_contains(p, (int32_t)hdr->stream_id)) {
        proto_local_reset_forget(p, (int32_t)hdr->stream_id);
        return;
      }
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    if (!s->closed) {
      s->closed = 1;
      s->body = NULL;
      s->body_len = s->body_off = 0;
      if (s->trailers_pending) {
        s->trailers_pending = 0;
        chttp_h2_hpack_buffer_destroy(&s->trailers);
      }
      if (p->cbs.on_stream_close) {
        p->cbs.on_stream_close(p->cbs.user_data, (int32_t)hdr->stream_id,
                               ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                   ((uint32_t)payload[2] << 8) | (uint32_t)payload[3]);
      }
    }
    s->local_reset = 0;
    proto_local_reset_forget(p, (int32_t)hdr->stream_id);
    if (p->cbs.on_rst_received) {
      p->cbs.on_rst_received(p->cbs.user_data, (int32_t)hdr->stream_id,
                             ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                 ((uint32_t)payload[2] << 8) | (uint32_t)payload[3]);
    }
    return;

  case CHTTP_H2_FRAME_GOAWAY: {
    uint32_t last, err;
    size_t i;
    if (hdr->stream_id != 0 || hdr->length < 8) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    if (chttp_h2_frame_goaway_parse(payload, hdr->length, &last, &err) != 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    p->goaway_received_last = (int32_t)last;
    p->goaway_received_error = err;
    p->draining = 1;
    if (p->cbs.on_goaway) {
      p->cbs.on_goaway(p->cbs.user_data, last, err);
    }
    /* RFC 9113 §6.8: streams we initiated above the peer's Last-Stream-ID
     * were not processed; close them with REFUSED_STREAM (nghttp2 parity). */
    for (i = 0; i < p->stream_count; i++) {
      chttp_h2_proto_stream *st = &p->streams[i];
      int mine = (p->mode == CHTTP_H2_PROTO_CLIENT) ? (st->id & 1) : !(st->id & 1);
      if (!st->closed && mine && st->id > (int32_t)last) {
        st->closed = 1;
        st->body = NULL;
        st->body_len = st->body_off = 0;
        if (st->trailers_pending) {
          st->trailers_pending = 0;
          chttp_h2_hpack_buffer_destroy(&st->trailers);
        }
        if (p->cbs.on_stream_close) {
          p->cbs.on_stream_close(p->cbs.user_data, st->id, CHTTP_H2_ERR_REFUSED_STREAM);
        }
      }
    }
    return;
  }

  case CHTTP_H2_FRAME_PING:
    if (hdr->stream_id != 0 || hdr->length != 8) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    if ((hdr->flags & CHTTP_H2_FLAG_ACK) == 0) {
      if (out_frame(p, CHTTP_H2_FRAME_PING, CHTTP_H2_FLAG_ACK, 0, payload, 8) != 0) {
        proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      }
    } else if (p->cbs.on_ping_ack) {
      p->cbs.on_ping_ack(p->cbs.user_data, payload);
    }
    return;

  case CHTTP_H2_FRAME_DATA: {
    const uint8_t *data = NULL;
    size_t data_len = 0u;
    size_t hidden_flow_bytes;
    int data_status = CHTTP_H2_PROTO_DATA_OK;
    int reset_stream;
    if (hdr->stream_id == 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    s = proto_find(p, (int32_t)hdr->stream_id);
    reset_stream = proto_local_reset_contains(p, (int32_t)hdr->stream_id);
    if (chttp_h2_frame_data_payload(payload, hdr->length, hdr->flags, &data, &data_len) != 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    if ((int32_t)hdr->length > p->conn_recv_window) {
      proto_fail(p, CHTTP_H2_ERR_FLOW_CONTROL_ERROR);
      return;
    }
    p->conn_recv_window -= (int32_t)hdr->length;
    if (!s) {
      if (!reset_stream) {
        proto_fail(p, CHTTP_H2_ERR_STREAM_CLOSED);
        return;
      }
      if (hdr->length != 0u && chttp_h2_proto_consume_connection(p, hdr->length) != 0) {
        proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
        return;
      }
      if (hdr->flags & CHTTP_H2_FLAG_END_STREAM)
        proto_local_reset_forget(p, (int32_t)hdr->stream_id);
      return;
    }
    if ((s->closed && !s->local_reset) || (s->remote_end_stream && !s->local_reset)) {
      proto_fail(p, CHTTP_H2_ERR_STREAM_CLOSED);
      return;
    }
    /* RFC 9113 section 5.4.2 requires frames already in flight after our
     * RST_STREAM to be ignored. DATA still consumes connection flow-control
     * credit, so restore that credit without surfacing a canceled response. */
    if (s->local_reset) {
      if (hdr->length != 0u && chttp_h2_proto_consume_connection(p, hdr->length) != 0) {
        proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
        return;
      }
      if (hdr->flags & CHTTP_H2_FLAG_END_STREAM) {
        s->remote_end_stream = 1;
        s->local_reset = 0;
        proto_local_reset_forget(p, (int32_t)hdr->stream_id);
      }
      return;
    }
    if ((int32_t)hdr->length > s->recv_window) {
      proto_fail(p, CHTTP_H2_ERR_FLOW_CONTROL_ERROR);
      return;
    }
    s->recv_window -= (int32_t)hdr->length;
    hidden_flow_bytes = hdr->length - data_len;
    if (hidden_flow_bytes != 0u &&
        (chttp_h2_proto_consume_stream(p, (int32_t)hdr->stream_id, hidden_flow_bytes) != 0 ||
         chttp_h2_proto_consume_connection(p, hidden_flow_bytes) != 0)) {
      proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
      return;
    }
    if (hdr->flags & CHTTP_H2_FLAG_END_STREAM) s->remote_end_stream = 1;
    if (p->cbs.on_data)
      data_status = p->cbs.on_data(p->cbs.user_data, (int32_t)hdr->stream_id, data, data_len);
    if (data_status == CHTTP_H2_PROTO_DATA_PAUSE) {
      p->input_paused = 1;
      p->input_paused_stream = (int32_t)hdr->stream_id;
      p->input_paused_end_stream = (hdr->flags & CHTTP_H2_FLAG_END_STREAM) != 0u;
      return;
    }
    if (data_status != CHTTP_H2_PROTO_DATA_OK) {
      proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
      return;
    }
    if (hdr->flags & CHTTP_H2_FLAG_END_STREAM) {
      if (s->local_reset) {
        s->local_reset = 0;
        proto_local_reset_forget(p, (int32_t)hdr->stream_id);
      }
      proto_maybe_close(p, s);
    }
    return;
  }

  case CHTTP_H2_FRAME_HEADERS: {
    const uint8_t *block;
    size_t block_len;
    int has_pri;
    int reset_stream;
    if (hdr->stream_id == 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    if (p->in_headers) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR); /* previous block missing CONTINUATION */
      return;
    }
    if (chttp_h2_frame_headers_payload(payload, hdr->length, hdr->flags, &block, &block_len,
                                       &has_pri, NULL, NULL) != 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    reset_stream = proto_local_reset_contains(p, (int32_t)hdr->stream_id);
    /* Server: register a new stream for an inbound request. */
    if (p->mode == CHTTP_H2_PROTO_SERVER) {
      s = proto_find(p, (int32_t)hdr->stream_id);
      if (!s && !reset_stream) {
        if ((hdr->stream_id & 1u) == 0u || hdr->stream_id <= (uint32_t)p->max_peer_stream_id) {
          proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
          return;
        }
        if (proto_active_streams(p) >= p->local.max_concurrent_streams) {
          proto_fail(p, CHTTP_H2_ERR_ENHANCE_YOUR_CALM);
          return;
        }
        if (proto_new_stream(p, (int32_t)hdr->stream_id, NULL) == NULL) {
          proto_fail(p, CHTTP_H2_ERR_ENHANCE_YOUR_CALM);
          return;
        }
        p->max_peer_stream_id = (int32_t)hdr->stream_id;
        s = proto_find(p, (int32_t)hdr->stream_id);
      }
    } else {
      s = proto_find(p, (int32_t)hdr->stream_id);
    }
    if (!reset_stream &&
        (!s || (s->closed && !s->local_reset) || (s->remote_end_stream && !s->local_reset))) {
      proto_fail(p, CHTTP_H2_ERR_STREAM_CLOSED);
      return;
    }
    p->in_headers = 1;
    p->in_headers_stream = (int32_t)hdr->stream_id;
    p->in_headers_end_stream = (hdr->flags & CHTTP_H2_FLAG_END_STREAM) != 0;
    p->in_block.size = 0;
    if (chttp_h2_hpack_buffer_reserve(&p->in_block, block_len) != 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    memcpy(p->in_block.data, block, block_len);
    p->in_block.size = block_len;
    if (hdr->flags & CHTTP_H2_FLAG_END_HEADERS) {
      proto_finish_headers(p);
    }
    return;
  }

  case CHTTP_H2_FRAME_CONTINUATION:
    if (hdr->stream_id == 0 || !p->in_headers || (int32_t)hdr->stream_id != p->in_headers_stream) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    if (chttp_h2_hpack_buffer_reserve(&p->in_block, hdr->length) != 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return;
    }
    memcpy(p->in_block.data + p->in_block.size, payload, hdr->length);
    p->in_block.size += hdr->length;
    if (hdr->flags & CHTTP_H2_FLAG_END_HEADERS) {
      proto_finish_headers(p);
    }
    return;

  case CHTTP_H2_FRAME_PRIORITY:
    if (hdr->stream_id == 0 || hdr->length != CHTTP_H2_PRIORITY_BYTES) {
      proto_fail(p,
                 hdr->stream_id == 0 ? CHTTP_H2_ERR_PROTOCOL_ERROR : CHTTP_H2_ERR_FRAME_SIZE_ERROR);
    }
    return; /* priority is advisory; ignored (RFC 9113 §5.3) */

  case CHTTP_H2_FRAME_PUSH_PROMISE:
    proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR); /* ENABLE_PUSH=0: rejected */
    return;

  default:
    return; /* unknown frame types are ignored */
  }
}

ptrdiff_t chttp_h2_proto_recv(chttp_h2_proto *p, const uint8_t *in, size_t in_len) {
  size_t pos = 0;
  if (!p || (!in && in_len)) {
    return -1;
  }
  if (in_len && chttp_h2_hpack_buffer_reserve(&p->inbuf, in_len) != 0) {
    return -1;
  }
  if (in_len) {
    memcpy(p->inbuf.data + p->inbuf.size, in, in_len);
    p->inbuf.size += in_len;
  }
  if (p->input_paused) return (ptrdiff_t)in_len;
  /* Server: consume and verify the client preface. */
  if (p->mode == CHTTP_H2_PROTO_SERVER && !p->server_preface_seen) {
    size_t avail = p->inbuf.size;
    size_t need = CHTTP_H2_PREFACE_LEN;
    if (avail > need) avail = need;
    if (memcmp(p->inbuf.data, CHTTP_H2_PREFACE, avail) != 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR);
      return -1;
    }
    if (avail < need) {
      return (ptrdiff_t)in_len; /* wait for the full preface */
    }
    p->server_preface_seen = 1;
    pos += need;
    /* The server connection preface starts with its SETTINGS frame. Queue it
     * before dispatching the client's SETTINGS so that the generated ACK can
     * never become the first server frame on the wire. */
    if (proto_ensure_preface(p) != 0) {
      proto_fail(p, CHTTP_H2_ERR_INTERNAL_ERROR);
      return -1;
    }
  }
  while (!p->failed && !p->input_paused && p->inbuf.size - pos >= CHTTP_H2_FRAME_HEADER_SIZE) {
    chttp_h2_frame_header hdr;
    size_t used = 0;
    if (chttp_h2_frame_header_decode(p->inbuf.data + pos, p->inbuf.size - pos, &used, &hdr,
                                     p->local.max_frame_size) != 0) {
      proto_fail(p, CHTTP_H2_ERR_PROTOCOL_ERROR); /* FRAME_SIZE_ERROR */
      break;
    }
    if (p->inbuf.size - pos - used < hdr.length) {
      break; /* partial frame: wait for more bytes */
    }
    proto_dispatch(p, p->inbuf.data + pos + used, &hdr);
    pos += used + hdr.length;
  }
  if (pos > 0 && pos <= p->inbuf.size) {
    memmove(p->inbuf.data, p->inbuf.data + pos, p->inbuf.size - pos);
    p->inbuf.size -= pos;
  }
  /* A negative return signals the failure to the caller (nghttp2 parity) so
   * the session layer can fail fast; the GOAWAY is queued in p->out and the
   * writer flushes it before tearing down. */
  return p->failed ? -1 : (ptrdiff_t)in_len;
}

/* ── Flow control consumption ─────────────────────────────────────── */

int chttp_h2_proto_consume_connection(chttp_h2_proto *p, size_t bytes) {
  if (!p || bytes == 0 || bytes > (size_t)CHTTP_H2_MAX_WINDOW) {
    return -1;
  }
  if (p->conn_consumed > CHTTP_H2_MAX_WINDOW - (int32_t)bytes) {
    return -1;
  }
  p->conn_consumed += (int32_t)bytes;
  /* Replenish only once half the advertised window has been consumed
   * (nghttp2 parity), keeping WINDOW_UPDATE traffic bounded. */
  if (p->conn_consumed > 0 && p->conn_consumed >= p->conn_local_window / 2) {
    int32_t inc = p->conn_consumed;
    p->conn_consumed = 0;
    if (chttp_h2_proto_submit_window_update(p, 0, (uint32_t)inc) != 0) {
      return -1;
    }
  }
  return 0;
}

int chttp_h2_proto_consume_stream(chttp_h2_proto *p, int32_t stream_id, size_t bytes) {
  chttp_h2_proto_stream *s;
  if (!p || bytes == 0 || bytes > (size_t)CHTTP_H2_MAX_WINDOW) {
    return -1;
  }
  s = proto_find(p, stream_id);
  if (!s || s->closed) {
    return -1; /* late consume on a detached/closed stream: ignore */
  }
  /* Return the stream window for every consumed byte.  A half-window
   * threshold is unsafe here: local_window grows with each WINDOW_UPDATE, so
   * the threshold would grow too and let recv_window dip below the next DATA
   * frame size, causing a spurious FLOW_CONTROL_ERROR on the peer.  The
   * connection window keeps a threshold because it is large (6.5 MiB) and
   * never gets that close to a frame boundary. */
  if (chttp_h2_proto_submit_window_update(p, stream_id, (uint32_t)bytes) != 0) {
    return -1;
  }
  return 0;
}

/* ── Stream user data ─────────────────────────────────────────────── */

void *chttp_h2_proto_get_stream_user_data(chttp_h2_proto *p, int32_t stream_id) {
  chttp_h2_proto_stream *s;
  if (!p) {
    return NULL;
  }
  s = proto_find(p, stream_id);
  return s ? s->user_data : NULL;
}

int chttp_h2_proto_remote_end_stream(chttp_h2_proto *p, int32_t stream_id) {
  chttp_h2_proto_stream *s;
  if (!p) {
    return 0;
  }
  s = proto_find(p, stream_id);
  return s ? s->remote_end_stream : 0;
}

int chttp_h2_proto_set_stream_user_data(chttp_h2_proto *p, int32_t stream_id, void *ud) {
  chttp_h2_proto_stream *s;
  if (!p) {
    return -1;
  }
  s = proto_find(p, stream_id);
  if (!s) {
    return -1;
  }
  s->user_data = ud;
  return 0;
}

int chttp_h2_proto_stream_output_pending(chttp_h2_proto *p, int32_t stream_id) {
  chttp_h2_proto_stream *s;

  if (!p) return 0;
  s = proto_find(p, stream_id);
  if (!s || s->closed) return 0;
  return (s->body != NULL && (s->body_off < s->body_len || s->body_eof)) ||
         (s->source != NULL && !s->source_done);
}

int chttp_h2_proto_resume_source(chttp_h2_proto *p, int32_t stream_id) {
  chttp_h2_proto_stream *s;
  if (!p) return -1;
  s = proto_find(p, stream_id);
  if (!s || s->closed || s->source == NULL || !s->source_waiting || s->source_done) return -1;
  s->source_waiting = 0;
  if (p->cbs.on_wake_write) p->cbs.on_wake_write(p->cbs.user_data);
  return 0;
}

int chttp_h2_proto_input_paused(const chttp_h2_proto *p) { return p != NULL && p->input_paused; }

int chttp_h2_proto_resume_input(chttp_h2_proto *p, int32_t stream_id) {
  chttp_h2_proto_stream *stream;
  int end_stream;
  if (p == NULL || stream_id <= 0 || !p->input_paused || p->input_paused_stream != stream_id)
    return -1;
  stream = proto_find(p, stream_id);
  end_stream = p->input_paused_end_stream;
  p->input_paused = 0;
  p->input_paused_stream = 0;
  p->input_paused_end_stream = 0;
  if (end_stream && stream != NULL && !stream->closed) proto_maybe_close(p, stream);
  if (p->failed) return -1;
  return chttp_h2_proto_recv(p, NULL, 0u) < 0 ? -1 : 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

uint32_t chttp_h2_proto_get_last_proc_stream_id(chttp_h2_proto *p) {
  return p ? (uint32_t)p->last_proc_stream_id : 0;
}

int chttp_h2_proto_terminate(chttp_h2_proto *p, uint32_t error_code) {
  size_t i;
  if (!p) {
    return -1;
  }
  if (chttp_h2_proto_submit_goaway(p, (uint32_t)p->last_proc_stream_id, error_code) != 0) {
    return -1;
  }
  /* Close all open streams with the error; the app is responsible for the
   * request-level classification. */
  for (i = 0; i < p->stream_count; i++) {
    chttp_h2_proto_stream *s = &p->streams[i];
    if (!s->closed) {
      s->closed = 1;
      s->body = NULL;
      s->body_len = s->body_off = 0;
      if (p->cbs.on_stream_close) {
        p->cbs.on_stream_close(p->cbs.user_data, s->id, error_code);
      }
    }
  }
  return 0;
}
